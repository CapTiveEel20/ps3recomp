/*
 * rsx_vertex_remap_from_span against rsx_vertex_remap_build.
 *
 * For random index lists (meshes, sparse ids, ids near the 32-bit top, tiny
 * draws) both paths must make every occurrence name the same vertex
 * (vertex_id, base_index), so the drawn triangles are identical. The span
 * path must take exactly the draws whose id span fits max_span, leave a
 * rejected draw untouched, and fetch the whole span in order. The same remap
 * and refs buffers are reused across calls, as the renderer does. Then a
 * timing of a typical mesh, old path (refs + de-dup) against the span path.
 *
 * Build (from the repo root):
 *   clang -std=gnu17 -O2 -I include -I libs/video -o test_vertex_remap_span \
 *     libs/video/tests/test_vertex_remap_span.c libs/video/rsx_vertex_compact.c \
 *     libs/video/rsx_dispatch.c
 */
#include "rsx_vertex_compact.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails <= 20) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}

static void make_ids(u32* ids, u32 count, int shape)
{
    const u32 low = shape == 2 ? 0xFFFFFFFFu - (rnd() % 5000) - 5000u
                               : rnd() % 100000;
    const u32 range = shape == 1 ? 1u + rnd() % (count * 8u + 1u)    /* sparse */
                                 : 1u + rnd() % (count / 3u + 1u);   /* mesh */
    for (u32 i = 0; i < count; i++)
        ids[i] = low + rnd() % range;
}

static double now_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

int main(void)
{
    enum { MAX = 30000 };
    u32* ids = malloc(MAX * sizeof *ids);
    rsx_vertex_ref* old_refs = malloc(MAX * sizeof *old_refs);
    rsx_vertex_remap old_map = {0}, span_map = {0};
    rsx_vertex_ref* span_refs = NULL;
    u32 span_cap = 0;
    unsigned taken = 0, rejected = 0;

    for (int iter = 0; iter < 4000; iter++) {
        const u32 count = iter % 10 == 0 ? 2u + rnd() % 4u : 2u + rnd() % (MAX - 2);
        const int shape = (int)(rnd() % 3);
        const u32 base = rnd() % 3 ? rnd() % 1000 : 0;
        make_ids(ids, count, shape);
        u32 lo = ids[0], hi = ids[0];
        for (u32 i = 1; i < count; i++) {
            if (ids[i] < lo) lo = ids[i];
            if (ids[i] > hi) hi = ids[i];
        }
        const u64 span = (u64)hi - lo + 1u;
        const u32 max_span = iter % 2 ? count : count * 2u;

        /* Old path: one ref per occurrence, then de-dup. */
        for (u32 i = 0; i < count; i++) {
            old_refs[i].vertex_id = ids[i];
            old_refs[i].base_index = base;
        }
        u32 old_unique = 0;
        CHECK(rsx_vertex_remap_build(&old_map, old_refs, count, &old_unique),
              "remap_build failed (count %u)", count);

        /* Span path: raw ids straight into the occurrence map. */
        u32* occ = rsx_vertex_remap_reserve(&span_map, count);
        CHECK(occ != NULL, "reserve failed");
        if (!occ) break;
        memcpy(occ, ids, count * sizeof *ids);
        u32 span_unique = 0xDEADu;
        const int ok = rsx_vertex_remap_from_span(&span_map, count, base, max_span,
                                                  &span_refs, &span_cap, &span_unique);
        CHECK(ok == (span <= max_span), "taken=%d span=%llu max=%u", ok,
              (unsigned long long)span, max_span);
        if (!ok) {
            rejected++;
            CHECK(span_unique == 0xDEADu, "rejected draw wrote unique_count");
            CHECK(memcmp(occ, ids, count * sizeof *ids) == 0,
                  "rejected draw changed the occurrence map");
            continue;
        }
        taken++;
        CHECK(span_unique == span, "unique %u != span %llu", span_unique,
              (unsigned long long)span);
        CHECK(span_unique >= old_unique, "span %u < unique %u", span_unique, old_unique);
        for (u32 i = 0; i < span_unique; i++)
            CHECK(span_refs[i].vertex_id == lo + i && span_refs[i].base_index == base,
                  "span ref %u = (%u,%u)", i, span_refs[i].vertex_id, span_refs[i].base_index);
        for (u32 o = 0; o < count; o++) {
            const u32 a = span_map.occurrence_to_unique[o];
            const u32 b = old_map.occurrence_to_unique[o];
            CHECK(a < span_unique && b < old_unique, "occurrence %u out of range", o);
            if (a >= span_unique || b >= old_unique) break;
            CHECK(span_refs[a].vertex_id == old_refs[b].vertex_id &&
                  span_refs[a].base_index == old_refs[b].base_index,
                  "occurrence %u: span (%u,%u) old (%u,%u)", o,
                  span_refs[a].vertex_id, span_refs[a].base_index,
                  old_refs[b].vertex_id, old_refs[b].base_index);
        }
    }
    printf("cases: %u taken by the span path, %u rejected (span too wide)\n", taken, rejected);
    CHECK(taken > 1000 && rejected > 300, "shape mix too narrow");

    /* Timing: a 6000-reference mesh over 1200 vertices, 2000 draws. */
    {
        enum { N = 6000, DRAWS = 2000 };
        rsx_vertex_ref* refs = malloc(N * sizeof *refs);
        u32 sink = 0;
        double t0 = now_s();
        for (int d = 0; d < DRAWS; d++) {
            for (u32 i = 0; i < N; i++) {
                refs[i].vertex_id = 500u + (i * 7u) % 1200u;
                refs[i].base_index = 0;
            }
            u32 u = 0;
            rsx_vertex_remap_build(&old_map, refs, N, &u);
            sink += u;
        }
        double t1 = now_s();
        for (int d = 0; d < DRAWS; d++) {
            u32* occ = rsx_vertex_remap_reserve(&span_map, N);
            for (u32 i = 0; i < N; i++)
                occ[i] = 500u + (i * 7u) % 1200u;
            u32 u = 0;
            rsx_vertex_remap_from_span(&span_map, N, 0, N, &span_refs, &span_cap, &u);
            sink += u;
        }
        double t2 = now_s();
        printf("timing (%d draws x %d refs): old %.2f us/draw, span %.2f us/draw (sink %u)\n",
               DRAWS, N, (t1 - t0) * 1e6 / DRAWS, (t2 - t1) * 1e6 / DRAWS, sink);
        free(refs);
    }

    rsx_vertex_remap_destroy(&old_map);
    rsx_vertex_remap_destroy(&span_map);
    free(span_refs);
    free(old_refs);
    free(ids);
    printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
