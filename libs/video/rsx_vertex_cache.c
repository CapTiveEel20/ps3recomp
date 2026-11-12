/*
 * ps3recomp - converted-vertex cache for the live RSX renderer.
 * See rsx_vertex_cache.h for what is cached and why it is safe.
 */
#include "rsx_vertex_cache.h"
#include "rsx_texture_layout.h"   /* rsx_mutation_hash */

#include <stdlib.h>
#include <string.h>

#define RSX_VCACHE_P 1099511628211ull

int rsx_vcache_init(rsx_vcache* cache, u32 slot_capacity,
                    u32 vb_capacity, u32 ib_capacity)
{
    if (!cache)
        return 0;
    memset(cache, 0, sizeof(*cache));
    u32 capacity = 16;
    while (capacity < slot_capacity && capacity < (1u << 24))
        capacity <<= 1;
    cache->slots = (rsx_vcache_entry*)calloc(capacity, sizeof(rsx_vcache_entry));
    if (!cache->slots)
        return 0;
    cache->spare = (rsx_vcache_entry*)calloc(capacity, sizeof(rsx_vcache_entry));
    if (!cache->spare) {
        free(cache->slots);
        cache->slots = NULL;
        return 0;
    }
    cache->slot_capacity = capacity;
    cache->generation = 1;
    cache->vb_capacity = vb_capacity;
    cache->ib_capacity = ib_capacity;
    return 1;
}

void rsx_vcache_destroy(rsx_vcache* cache)
{
    if (!cache)
        return;
    free(cache->slots);
    free(cache->spare);
    memset(cache, 0, sizeof(*cache));
}

void rsx_vcache_reset(rsx_vcache* cache)
{
    if (!cache || !cache->slots)
        return;
    memset(cache->slots, 0, (size_t)cache->slot_capacity * sizeof(rsx_vcache_entry));
    cache->slot_used = 0;
    if (++cache->generation == 0)
        cache->generation = 1;
    cache->vb_used = 0;
    cache->ib_used = 0;
    cache->full = 0;
    cache->arena_full = 0;
    cache->stats.resets++;
}

int rsx_vcache_reset_if_full(rsx_vcache* cache)
{
    if (!cache || !cache->full)
        return 0;
    rsx_vcache_reset(cache);
    return 1;
}

rsx_vcache_entry* rsx_vcache_lookup(rsx_vcache* cache, u64 identity)
{
    if (!cache || !cache->slots)
        return NULL;
    if (!identity)
        identity = 1;
    const u32 mask = cache->slot_capacity - 1u;
    u32 at = (u32)(identity ^ (identity >> 29) ^ (identity >> 47)) & mask;
    for (u32 probe = 0; probe < cache->slot_capacity; probe++) {
        rsx_vcache_entry* entry = &cache->slots[at];
        if (entry->identity == identity)
            return entry;
        if (!entry->identity) {
            if ((u64)(cache->slot_used + 1u) * 4u > (u64)cache->slot_capacity * 3u) {
                cache->full = 1;
                return NULL;
            }
            memset(entry, 0, sizeof(*entry));
            entry->identity = identity;
            cache->slot_used++;
            return entry;
        }
        at = (at + 1u) & mask;
    }
    cache->full = 1;
    return NULL;
}

int rsx_vcache_entry_seen(rsx_vcache_entry* entry, u32 frame)
{
    if (!entry)
        return 0;
    const u32 before = entry->last_frame;
    entry->last_frame = frame + 1u ? frame + 1u : 1u;
    return before != 0;
}

static int rsx_vcache_entry_valid(const rsx_vcache* cache,
                                  const rsx_vcache_entry* entry);

u32 rsx_vcache_sweep(rsx_vcache* cache, u32 frame, u32 keep_filled, u32 keep_other)
{
    if (!cache || !cache->slots || !cache->spare)
        return 0;
    const u32 mask = cache->slot_capacity - 1u;
    rsx_vcache_entry* to = cache->spare;   /* all zero: see below */
    u32 kept = 0;
    for (u32 i = 0; i < cache->slot_capacity; i++) {
        const rsx_vcache_entry* entry = &cache->slots[i];
        if (!entry->identity)
            continue;
        /* Frames since the last sighting; never seen counts as very old. */
        const u32 age = entry->last_frame ? frame + 1u - entry->last_frame : UINT32_MAX;
        const u32 keep = rsx_vcache_entry_valid(cache, entry) ? keep_filled : keep_other;
        const int backing_off = entry->retry_frame - frame - 1u < 0x7FFFFFFFu;   /* retry_frame > frame */
        if (age > keep && !backing_off)
            continue;
        u32 at = (u32)(entry->identity ^ (entry->identity >> 29) ^
                       (entry->identity >> 47)) & mask;
        while (to[at].identity)
            at = (at + 1u) & mask;
        to[at] = *entry;
        kept++;
    }
    const u32 dropped = cache->slot_used - kept;
    memset(cache->slots, 0, (size_t)cache->slot_capacity * sizeof(rsx_vcache_entry));
    cache->spare = cache->slots;   /* zeroed, ready for the next sweep */
    cache->slots = to;
    cache->slot_used = kept;
    if (!cache->arena_full)
        cache->full = 0;
    cache->stats.sweeps++;
    cache->stats.evicted += dropped;
    return dropped;
}

int rsx_vcache_frame_end(rsx_vcache* cache, u32 frame, int sweep_enabled)
{
    if (!cache || !cache->slots)
        return 0;
    if (!sweep_enabled || cache->arena_full)
        return cache->full != 0;
    if (!cache->full && (u64)cache->slot_used * 2u <= cache->slot_capacity)
        return 0;
    rsx_vcache_sweep(cache, frame, 3000u, 8u);
    if ((u64)cache->slot_used * 2u > cache->slot_capacity)
        rsx_vcache_sweep(cache, frame, 1u, 1u);
    if ((u64)cache->slot_used * 2u > cache->slot_capacity)
        cache->full = 1;   /* that many draws in two frames: start over */
    return cache->full != 0;
}

static int rsx_vcache_range_less(const rsx_vcache_range* a,
                                 const rsx_vcache_range* b)
{
    if (a->location != b->location)
        return a->location < b->location;
    return a->offset < b->offset;
}

u32 rsx_vcache_merge_ranges(rsx_vcache_range* ranges, u32 count)
{
    if (!ranges || count < 2)
        return count;
    for (u32 i = 1; i < count; i++) {   /* insertion sort: count <= 16 */
        const rsx_vcache_range key = ranges[i];
        u32 j = i;
        while (j && rsx_vcache_range_less(&key, &ranges[j - 1])) {
            ranges[j] = ranges[j - 1];
            j--;
        }
        ranges[j] = key;
    }
    u32 out = 0;
    for (u32 i = 1; i < count; i++) {
        rsx_vcache_range* current = &ranges[out];
        const rsx_vcache_range* next = &ranges[i];
        const u64 current_end = (u64)current->offset + current->bytes;
        if (next->location == current->location &&
            (u64)next->offset <= current_end) {
            const u64 next_end = (u64)next->offset + next->bytes;
            if (next_end > current_end)
                current->bytes = (u32)(next_end - current->offset);
            continue;
        }
        ranges[++out] = *next;
    }
    return out + 1u;
}

u32 rsx_vcache_ranges_from_plan(const rsx_vertex_fetch_plan* plan,
                                rsx_vcache_range* out)
{
    if (!plan || !out || plan->inline_data)
        return 0;
    u32 count = 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        if (!fetch->desc.type || !fetch->desc.size)
            continue;   /* constant register: part of the identity */
        if (!fetch->stable_base || !fetch->elem_size ||
            fetch->stable_last < fetch->stable_first)
            return 0;
        const u64 prefix = rsx_dsp_resolve_vertex_offset(
            plan->base_offset, fetch->desc.offset);
        const u64 start = prefix + (u64)fetch->stable_first * fetch->stride;
        const u64 end = prefix + (u64)fetch->stable_last * fetch->stride +
                        fetch->elem_size;
        if (start > UINT32_MAX || end > 0x100000000ull || end <= start ||
            count >= RSX_VCACHE_MAX_RANGES)
            return 0;
        out[count].location = fetch->desc.location;
        out[count].offset = (u32)start;
        out[count].bytes = (u32)(end - start);
        count++;
    }
    return rsx_vcache_merge_ranges(out, count);
}

int rsx_vcache_hash_ranges(const rsx_vcache_range* ranges, u32 count,
                           rsx_vertex_guest_ptr_fn guest_ptr, void* user,
                           u64* hash, u64* hashed_bytes)
{
    if (!ranges || !count || !guest_ptr || !hash)
        return 0;
    u64 h = 0x84222325CBF29CE4ull;
    for (u32 i = 0; i < count; i++) {
        const u8* bytes = guest_ptr(user, ranges[i].location,
                                    ranges[i].offset, ranges[i].bytes);
        if (!bytes)
            return 0;
        h = (h ^ ranges[i].location) * RSX_VCACHE_P;
        h = (h ^ ranges[i].offset) * RSX_VCACHE_P;
        h = (h ^ rsx_mutation_hash(bytes, ranges[i].bytes)) * RSX_VCACHE_P;
        if (hashed_bytes)
            *hashed_bytes += ranges[i].bytes;
    }
    *hash = h;
    return 1;
}

static int rsx_vcache_entry_valid(const rsx_vcache* cache,
                                  const rsx_vcache_entry* entry)
{
    return entry->generation == cache->generation && entry->n_ranges;
}

int rsx_vcache_entry_wants_vertex_hash(const rsx_vcache* cache,
                                       const rsx_vcache_entry* entry,
                                       u64 index_hash)
{
    return cache && entry && rsx_vcache_entry_valid(cache, entry) &&
           entry->index_hash == index_hash;
}

int rsx_vcache_entry_skipped(rsx_vcache* cache, const rsx_vcache_entry* entry,
                             u32 frame)
{
    if (!entry || frame >= entry->retry_frame)
        return 0;
    if (cache)
        cache->stats.volatile_skips++;
    return 1;
}

void rsx_vcache_entry_defer(rsx_vcache_entry* entry, u32 frame, u32 frames)
{
    if (entry)
        entry->retry_frame = frame + frames;
}

rsx_vcache_action rsx_vcache_decide(rsx_vcache* cache, rsx_vcache_entry* entry,
                                    u32 frame, u64 index_hash,
                                    int vertex_hash_ok, u64 vertex_hash)
{
    if (!cache || !entry)
        return RSX_VCACHE_BYPASS;
    if (!rsx_vcache_entry_valid(cache, entry))
        return RSX_VCACHE_FILL;   /* never filled in this generation */
    if (entry->index_hash == index_hash && vertex_hash_ok &&
        entry->vertex_hash == vertex_hash) {
        entry->streak = 0;
        cache->stats.hits++;
        return RSX_VCACHE_HIT;
    }
    /* The bytes behind this draw changed since it was stored. Once is an
     * update; twice in a row is a buffer rewritten every frame (animation,
     * text): stop hashing and storing it for a while, longer each time. */
    cache->stats.changed++;
    if (entry->streak < 255)
        entry->streak++;
    if (entry->streak >= 2) {
        const u32 shift = entry->streak - 2u < 6u ? entry->streak - 2u : 6u;
        entry->retry_frame = frame + (16u << shift);
        return RSX_VCACHE_BYPASS;
    }
    return RSX_VCACHE_FILL;
}

static int rsx_vcache_take(u32* used, u32 capacity, u32 bytes, u32* offset)
{
    const u64 start = ((u64)*used + RSX_VCACHE_ALIGN - 1u) &
                      ~(u64)(RSX_VCACHE_ALIGN - 1u);
    if (start + bytes > capacity)
        return 0;
    *offset = (u32)start;
    *used = (u32)(start + bytes);
    return 1;
}

int rsx_vcache_alloc(rsx_vcache* cache, u32 vb_bytes, u32 ib_bytes,
                     u32* vb_offset, u32* ib_offset)
{
    if (!cache || !vb_offset || !ib_offset)
        return 0;
    u32 vb_used = cache->vb_used, ib_used = cache->ib_used;
    if (!rsx_vcache_take(&vb_used, cache->vb_capacity, vb_bytes, vb_offset) ||
        !rsx_vcache_take(&ib_used, cache->ib_capacity, ib_bytes, ib_offset)) {
        cache->full = 1;
        cache->arena_full = 1;
        return 0;
    }
    cache->vb_used = vb_used;
    cache->ib_used = ib_used;
    return 1;
}

void rsx_vcache_commit(rsx_vcache* cache, rsx_vcache_entry* entry,
                       u64 index_hash, u64 vertex_hash,
                       const rsx_vcache_range* ranges, u32 n_ranges,
                       u32 vb_offset, u32 vb_bytes, u32 ib_offset, u32 ib_bytes,
                       u32 draw_count, u32 vertex_count, u32 source_refs,
                       int indexed)
{
    if (!cache || !entry || !ranges || !n_ranges ||
        n_ranges > RSX_VCACHE_MAX_RANGES)
        return;
    entry->index_hash = index_hash;
    entry->vertex_hash = vertex_hash;
    memcpy(entry->ranges, ranges, n_ranges * sizeof(*ranges));
    entry->n_ranges = (u8)n_ranges;
    entry->vb_offset = vb_offset;
    entry->vb_bytes = vb_bytes;
    entry->ib_offset = ib_offset;
    entry->ib_bytes = ib_bytes;
    entry->draw_count = draw_count;
    entry->vertex_count = vertex_count;
    entry->source_refs = source_refs;
    entry->indexed = (u8)(indexed != 0);
    entry->generation = cache->generation;
    cache->stats.fills++;
    cache->stats.filled_bytes += (u64)vb_bytes + ib_bytes;
}
