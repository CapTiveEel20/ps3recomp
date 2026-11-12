/*
 * rsx_vertex_cache: the converted-vertex cache of the live renderer.
 *
 *  1. Coverage (the safety property): for random fetch plans (every vertex
 *     type, strides, offsets, frequency dividers, 28-bit base wrap, partly
 *     unmapped memory) and random references, every guest byte whose change
 *     alters rsx_vertex_fetch_many's output lies inside the ranges
 *     rsx_vcache_ranges_from_plan returns, and changing it changes the
 *     content hash. So a draw whose content hash still matches converts to
 *     exactly the stored bytes. Plans the cache cannot validate (an attribute
 *     without a contiguous mapping, inline streams) must return 0 ranges.
 *  2. Range merging against a brute-force byte map.
 *  3. Table: lookup/create, load limit -> full, reset.
 *  4. Policy: new -> FILL, same -> HIT, one change -> FILL, two in a row ->
 *     BYPASS with a growing backoff, skipped until it expires.
 *  5. Arena: aligned offsets, full -> 0 and a reset request.
 *
 * Build (from the repo root):
 *   clang -std=gnu17 -O2 -I include -I libs/video -o test_vertex_cache \
 *     libs/video/tests/test_vertex_cache.c libs/video/rsx_vertex_cache.c \
 *     libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c \
 *     libs/video/rsx_texture_layout.c
 */
#include "rsx_vertex_cache.h"
#include "rsx_vertex_formats.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails <= 20) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}

enum { GUEST = 1 << 15 };
static uint8_t guest[2][GUEST];
static uint32_t mapped_end[2];

static const u8* guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
    const u32 loc = location & 1u;
    if ((u64)offset + min_bytes > mapped_end[loc]) return NULL;
    return guest[loc] + offset;
}

static void random_plan(rsx_vertex_fetch_plan* plan)
{
    memset(plan, 0, sizeof *plan);
    rsx_vertex_layout_plan_init(&plan->layout, (rnd() & 0xFFFFu) | 1u);
    plan->base_offset = (rnd() % 4 == 0) ? rnd() % 256u : 0;
    if (rnd() % 8 == 0) plan->base_offset += (1u + rnd() % 15u) << 28;
    plan->divider_mask = rnd() & 0xFFFFu;
    plan->guest_ptr = guest_ptr;
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        rsx_vertex_fetch_attr* f = &plan->attr[a];
        const u32 r = rnd() % 16;
        f->desc.type = r < 6 ? RSX_VTX_TYPE_FLOAT : r < 8 ? 0 : 1 + rnd() % 7;
        f->desc.size = (rnd() % 8 == 0) ? rnd() % 6 : 1 + rnd() % 4;
        f->elem_size = rsx_vertex_attrib_size(f->desc.type, f->desc.size);
        f->desc.stride = (rnd() % 4 == 0) ? 0 : f->elem_size + (rnd() % 3) * 4u;
        f->stride = f->desc.stride ? f->desc.stride : f->elem_size;
        f->desc.frequency = (rnd() % 4 == 0) ? rnd() % 4 : 0;
        f->desc.offset = rnd() % 2048u;
        f->desc.location = rnd() & 1u;
        for (u32 c = 0; c < 4; c++) f->default_value[c] = (float)(rnd() % 7);
    }
}

static int in_ranges(const rsx_vcache_range* r, u32 n, u32 loc, u32 off)
{
    for (u32 i = 0; i < n; i++)
        if ((r[i].location & 1u) == loc && off >= r[i].offset &&
            (u64)off < (u64)r[i].offset + r[i].bytes)
            return 1;
    return 0;
}

static void test_coverage(void)
{
    enum { MAXREF = 96 };
    rsx_vertex_ref refs[MAXREF];
    static u8 out0[MAXREF * 256], out1[MAXREF * 256];
    unsigned plans_cached = 0, plans_rejected = 0, bytes_checked = 0,
             bytes_relevant = 0;
    for (int iter = 0; iter < 1500; iter++) {
        for (u32 l = 0; l < 2; l++) {
            for (u32 i = 0; i < GUEST; i++) guest[l][i] = (u8)rnd();
            mapped_end[l] = rnd() % 3 == 0 ? rnd() % GUEST : GUEST;
        }
        rsx_vertex_fetch_plan plan;
        random_plan(&plan);
        const u32 count = 1 + rnd() % MAXREF;
        const u32 base_index = rnd() % 4 == 0 ? rnd() % 64 : 0;
        const u32 lo = rnd() % 200;
        for (u32 i = 0; i < count; i++) {
            refs[i].vertex_id = lo + rnd() % (1 + rnd() % 64);
            refs[i].base_index = base_index;
        }
        rsx_vertex_fetch_plan_prepare(&plan, refs, count);

        int validatable = 0, needs_guest = 0;
        for (u32 s = 0; s < plan.layout.count; s++) {
            const rsx_vertex_fetch_attr* f = &plan.attr[plan.layout.attrs[s]];
            if (!f->desc.type || !f->desc.size) continue;
            needs_guest = 1;
            if (!f->stable_base) { validatable = -1; break; }
            validatable = 1;
        }
        rsx_vcache_range ranges[RSX_VCACHE_MAX_RANGES];
        const u32 n = rsx_vcache_ranges_from_plan(&plan, ranges);
        CHECK((n != 0) == (validatable == 1 && needs_guest),
              "iter %d: ranges %u but validatable %d", iter, n, validatable);
        if (!n) { plans_rejected++; continue; }
        plans_cached++;
        for (u32 i = 1; i < n; i++)
            CHECK(ranges[i - 1].location < ranges[i].location ||
                  (ranges[i - 1].location == ranges[i].location &&
                   (u64)ranges[i - 1].offset + ranges[i - 1].bytes < ranges[i].offset),
                  "iter %d: ranges not sorted/disjoint", iter);

        const size_t out_bytes = (size_t)count * plan.layout.stride;
        const int ok0 = rsx_vertex_fetch_many(&plan, refs, count, out0);
        u64 h0 = 0;
        CHECK(rsx_vcache_hash_ranges(ranges, n, guest_ptr, NULL, &h0, NULL),
              "iter %d: hash failed", iter);

        /* Flip candidate bytes: random ones, and every byte near a range edge. */
        for (u32 probe = 0; probe < 160; probe++) {
            u32 loc, off;
            if (probe < 64) {
                loc = rnd() & 1u; off = rnd() % GUEST;
            } else {
                const rsx_vcache_range* r = &ranges[rnd() % n];
                loc = r->location & 1u;
                const s64 edge = (rnd() & 1) ? (s64)r->offset
                                             : (s64)r->offset + r->bytes;
                const s64 at = edge + (s64)(rnd() % 17) - 8;
                if (at < 0 || at >= GUEST) continue;
                off = (u32)at;
            }
            if (off >= mapped_end[loc]) continue;
            bytes_checked++;
            const u8 old = guest[loc][off];
            guest[loc][off] = (u8)(old ^ (1u << (rnd() % 8)));
            const int ok1 = rsx_vertex_fetch_many(&plan, refs, count, out1);
            const int changed = ok0 != ok1 ||
                (ok0 && memcmp(out0, out1, out_bytes) != 0);
            const int inside = in_ranges(ranges, n, loc, off);
            if (changed) {
                bytes_relevant++;
                CHECK(inside, "iter %d: byte %u:%u changes output, outside ranges",
                      iter, loc, off);
            }
            if (inside) {
                u64 h1 = 0;
                CHECK(rsx_vcache_hash_ranges(ranges, n, guest_ptr, NULL, &h1, NULL) &&
                      h1 != h0, "iter %d: byte %u:%u inside, hash unchanged",
                      iter, loc, off);
            }
            guest[loc][off] = old;
        }
    }
    printf("coverage: %u plans cached, %u rejected, %u bytes flipped (%u changed output)\n",
           plans_cached, plans_rejected, bytes_checked, bytes_relevant);
    CHECK(plans_cached > 300 && plans_rejected > 50, "too few cases of a kind");
}

static void test_inline_rejected(void)
{
    static u8 stream[256];
    rsx_vertex_fetch_plan plan;
    for (u32 l = 0; l < 2; l++) mapped_end[l] = GUEST;
    random_plan(&plan);
    plan.attr[0].desc.type = RSX_VTX_TYPE_FLOAT; plan.attr[0].desc.size = 4;
    plan.inline_data = stream; plan.inline_bytes = sizeof stream; plan.inline_stride = 16;
    rsx_vertex_ref r = {0, 0};
    rsx_vertex_fetch_plan_prepare(&plan, &r, 1);
    rsx_vcache_range ranges[RSX_VCACHE_MAX_RANGES];
    CHECK(rsx_vcache_ranges_from_plan(&plan, ranges) == 0, "inline stream must bypass");
}

static void test_merge(void)
{
    for (int iter = 0; iter < 3000; iter++) {
        rsx_vcache_range r[RSX_VCACHE_MAX_RANGES];
        static u8 want[2][600], got[2][600];
        memset(want, 0, sizeof want); memset(got, 0, sizeof got);
        const u32 n = 1 + rnd() % RSX_VCACHE_MAX_RANGES;
        for (u32 i = 0; i < n; i++) {
            r[i].location = rnd() & 1u;
            r[i].offset = rnd() % 500;
            r[i].bytes = 1 + rnd() % 60;
            for (u32 b = 0; b < r[i].bytes; b++) want[r[i].location][r[i].offset + b] = 1;
        }
        const u32 m = rsx_vcache_merge_ranges(r, n);
        CHECK(m >= 1 && m <= n, "merge count %u of %u", m, n);
        for (u32 i = 0; i < m; i++) {
            for (u32 b = 0; b < r[i].bytes; b++) got[r[i].location][r[i].offset + b] = 1;
            if (i)
                CHECK(r[i - 1].location < r[i].location ||
                      (u64)r[i - 1].offset + r[i - 1].bytes < r[i].offset,
                      "iter %d: merged ranges touch or overlap", iter);
        }
        CHECK(!memcmp(want, got, sizeof want), "iter %d: merged coverage differs", iter);
    }
}

static void test_table(void)
{
    rsx_vcache c;
    CHECK(rsx_vcache_init(&c, 64, 1 << 20, 1 << 20), "init");
    CHECK(c.slot_capacity == 64, "capacity %u", c.slot_capacity);
    rsx_vcache_entry* first = rsx_vcache_lookup(&c, 0x1234);
    CHECK(first && first->identity == 0x1234, "create");
    CHECK(rsx_vcache_lookup(&c, 0x1234) == first, "same identity, same entry");
    CHECK(rsx_vcache_lookup(&c, 0) != NULL, "identity 0 maps to a valid key");
    u32 created = 2;
    for (u64 id = 100; id < 200; id++)
        if (rsx_vcache_lookup(&c, id * 0x9E3779B97F4A7C15ull)) created++;
    CHECK(created == 48 && c.full, "load limit 3/4: created %u full %d", created, c.full);
    CHECK(rsx_vcache_lookup(&c, 0x1234) == first, "existing entries still found when full");
    const u32 gen = c.generation;
    CHECK(rsx_vcache_reset_if_full(&c) && !c.full && c.slot_used == 0 &&
          c.generation == gen + 1 && c.stats.resets == 1, "reset");
    CHECK(rsx_vcache_lookup(&c, 0x1234)->generation == 0, "reset forgets entries");
    rsx_vcache_destroy(&c);
}

static void test_policy(void)
{
    rsx_vcache c;
    rsx_vcache_init(&c, 64, 1 << 20, 1 << 20);
    rsx_vcache_entry* e = rsx_vcache_lookup(&c, 77);
    rsx_vcache_range r = {1, 0x1000, 64};
    u32 vo, io;
    u32 frame = 10;

    CHECK(!rsx_vcache_entry_skipped(&c, e, frame), "new entry not skipped");
    CHECK(!rsx_vcache_entry_wants_vertex_hash(&c, e, 5), "nothing to compare yet");
    CHECK(rsx_vcache_decide(&c, e, frame, 5, 0, 0) == RSX_VCACHE_FILL, "new -> FILL");
    CHECK(rsx_vcache_alloc(&c, 100, 12, &vo, &io), "alloc");
    rsx_vcache_commit(&c, e, 5, 50, &r, 1, vo, 100, io, 12, 3, 4, 3, 1);

    frame++;
    CHECK(rsx_vcache_entry_wants_vertex_hash(&c, e, 5), "same index -> hash vertices");
    CHECK(!rsx_vcache_entry_wants_vertex_hash(&c, e, 6), "index changed -> no vertex hash");
    CHECK(rsx_vcache_decide(&c, e, frame, 5, 1, 50) == RSX_VCACHE_HIT, "same -> HIT");
    CHECK(rsx_vcache_decide(&c, e, frame, 5, 0, 50) != RSX_VCACHE_HIT, "unhashable -> no HIT");
    e->streak = 0;
    CHECK(rsx_vcache_decide(&c, e, frame, 5, 1, 51) == RSX_VCACHE_FILL, "1 change -> FILL");
    rsx_vcache_commit(&c, e, 5, 51, &r, 1, vo, 100, io, 12, 3, 4, 3, 1);
    CHECK(rsx_vcache_decide(&c, e, ++frame, 5, 1, 51) == RSX_VCACHE_HIT, "settled -> HIT");
    CHECK(rsx_vcache_decide(&c, e, ++frame, 5, 1, 52) == RSX_VCACHE_FILL, "change 1");
    CHECK(rsx_vcache_decide(&c, e, ++frame, 5, 1, 53) == RSX_VCACHE_BYPASS, "change 2 -> BYPASS");
    CHECK(e->retry_frame == frame + 16, "backoff 16 (got %u)", e->retry_frame - frame);
    CHECK(rsx_vcache_entry_skipped(&c, e, frame + 15), "skipped during backoff");
    CHECK(!rsx_vcache_entry_skipped(&c, e, frame + 16), "retried after backoff");
    frame += 16;
    CHECK(rsx_vcache_decide(&c, e, frame, 5, 1, 54) == RSX_VCACHE_BYPASS, "still changing");
    CHECK(e->retry_frame == frame + 32, "backoff doubles (got %u)", e->retry_frame - frame);
    frame += 32;
    CHECK(rsx_vcache_decide(&c, e, frame, 5, 1, 51) == RSX_VCACHE_HIT,
          "content back to the stored bytes -> HIT");
    CHECK(e->streak == 0, "streak cleared on hit");
    CHECK(c.stats.hits == 3 && c.stats.fills == 2 && c.stats.volatile_skips == 1,
          "stats hits %llu fills %llu skips %llu", (unsigned long long)c.stats.hits,
          (unsigned long long)c.stats.fills, (unsigned long long)c.stats.volatile_skips);

    rsx_vcache_reset(&c);
    e = rsx_vcache_lookup(&c, 77);
    CHECK(rsx_vcache_decide(&c, e, frame, 5, 1, 51) == RSX_VCACHE_FILL,
          "after reset the old arena data is never a hit");
    rsx_vcache_destroy(&c);
}

static void test_arena(void)
{
    rsx_vcache c;
    rsx_vcache_init(&c, 64, 1000, 600);
    u32 vo = 1, io = 1;
    CHECK(rsx_vcache_alloc(&c, 10, 0, &vo, &io) && vo == 0 && io == 0, "first at 0");
    CHECK(rsx_vcache_alloc(&c, 300, 100, &vo, &io) && vo == 256 && io == 0, "aligned (vo %u io %u)", vo, io);
    CHECK(rsx_vcache_alloc(&c, 10, 10, &vo, &io) && vo == 768 && io == 256, "aligned 2 (vo %u io %u)", vo, io);
    CHECK(!rsx_vcache_alloc(&c, 300, 10, &vo, &io) && c.full, "vb full");
    CHECK(c.vb_used == 778 && c.ib_used == 266, "failed alloc consumes nothing");
    rsx_vcache_reset(&c);
    CHECK(!rsx_vcache_alloc(&c, 10, 700, &vo, &io) && c.full && c.vb_used == 0, "ib full");
    rsx_vcache_destroy(&c);
}

int main(void)
{
    test_merge();
    test_table();
    test_policy();
    test_arena();
    test_inline_rejected();
    test_coverage();
    printf(fails ? "test_vertex_cache: %d FAILED\n" : "test_vertex_cache: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
