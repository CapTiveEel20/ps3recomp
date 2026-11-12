/*
 * ps3recomp - converted-vertex cache for the live RSX renderer
 *
 * Every draw used to decode its guest index buffer, convert every referenced
 * vertex from big-endian RSX formats to float4s, build a triangle-list index
 * buffer and copy both into the upload ring -- every frame, although in a
 * typical scene almost none of those bytes change between frames
 * (Civilization Revolution: ~2.4M index references and ~750K converted
 * vertices per frame, ~31% of the renderer thread).
 *
 * This cache keeps the converted vertices and indices of a draw in a GPU
 * arena and reuses them while the guest bytes they came from are unchanged.
 * A draw is looked up by its IDENTITY (everything that decides how bytes are
 * read: formats, strides, offsets, constant attribute values, index-buffer
 * location and batches, primitive), and an entry is valid only while the
 * CONTENT hashes still match: the raw index bytes, and the exact vertex byte
 * ranges the conversion read. Content is compared on every use, so a guest
 * write by any agent (PPU, SPU DMA, RSX) is seen without any write tracking.
 *
 * This file is D3D12-independent: the renderer owns the GPU buffers and only
 * asks the cache for arena offsets.
 */
#ifndef PS3RECOMP_RSX_VERTEX_CACHE_H
#define PS3RECOMP_RSX_VERTEX_CACHE_H

#include "rsx_vertex_compact.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_VCACHE_MAX_RANGES 16u
#define RSX_VCACHE_ALIGN 256u

/* One run of guest bytes a draw's conversion reads. */
typedef struct rsx_vcache_range {
    u32 location;
    u32 offset;
    u32 bytes;
} rsx_vcache_range;

typedef struct rsx_vcache_entry {
    u64 identity;            /* 0 = free slot */
    u64 index_hash;
    u64 vertex_hash;
    u32 generation;          /* arena generation of vb/ib; 0 = never filled */
    u32 vb_offset, vb_bytes;
    u32 ib_offset, ib_bytes;
    u32 draw_count;          /* indices when indexed, else vertices */
    u32 vertex_count;        /* converted vertices in the arena */
    u32 source_refs;
    u32 retry_frame;         /* volatile/uncacheable: bypass before this frame */
    u32 last_frame;          /* frame + 1 of the last sighting; 0 = never seen */
    u8  indexed;
    u8  n_ranges;
    u8  streak;              /* consecutive content changes */
    u8  pad;
    rsx_vcache_range ranges[RSX_VCACHE_MAX_RANGES];
} rsx_vcache_entry;

typedef struct rsx_vcache_stats {
    u64 hits, fills, changed, volatile_skips, bypass, hashed_bytes,
        filled_bytes, resets, first_seen, sweeps, evicted;
} rsx_vcache_stats;

typedef struct rsx_vcache {
    rsx_vcache_entry* slots;
    rsx_vcache_entry* spare;   /* second table, swapped in by rsx_vcache_sweep */
    u32 slot_capacity;       /* power of two */
    u32 slot_used;
    u32 generation;          /* starts at 1 */
    u32 vb_capacity, ib_capacity;
    u32 vb_used, ib_used;
    int full;                /* arena/table full: sweep or reset at the next frame end */
    int arena_full;          /* the arena ran out: only a reset frees it */
    rsx_vcache_stats stats;
} rsx_vcache;

typedef enum rsx_vcache_action {
    RSX_VCACHE_BYPASS = 0,   /* convert into the ordinary ring, store nothing */
    RSX_VCACHE_HIT = 1,      /* bind the entry's arena data, convert nothing */
    RSX_VCACHE_FILL = 2      /* convert into the arena and record the entry */
} rsx_vcache_action;

int  rsx_vcache_init(rsx_vcache* cache, u32 slot_capacity,
                     u32 vb_capacity, u32 ib_capacity);
void rsx_vcache_destroy(rsx_vcache* cache);

/* Forget every entry and restart both arenas. Only call when the GPU no longer
 * reads any arena data (after a full fence wait). */
void rsx_vcache_reset(rsx_vcache* cache);

/* Reset when the cache asked for it (arena or table full); 1 = it did. */
int  rsx_vcache_reset_if_full(rsx_vcache* cache);

/* Entry for `identity` (never 0), created if absent and the table is below
 * 3/4 load; NULL when it cannot be created (the cache then asks for a reset). */
rsx_vcache_entry* rsx_vcache_lookup(rsx_vcache* cache, u64 identity);

/* Record that `entry`'s draw was issued in `frame`. Returns 1 when it had been
 * seen before. A draw whose identity never repeats (geometry the title writes
 * at a new address every frame) would fill the arena and the table with
 * entries nothing ever hits, so the renderer stores a draw only from its
 * second sighting on. */
int rsx_vcache_entry_seen(rsx_vcache_entry* entry, u32 frame);

/* Drop table entries nothing used lately, keeping every other entry and all
 * arena data (so no GPU-idle point is needed, only that the caller holds no
 * entry pointer). Kept: entries with arena data seen within `keep_filled`
 * frames, entries still in a volatile/uncacheable backoff, and other entries
 * seen within `keep_other` frames. Clears the table-full request. Returns the
 * number of entries dropped. */
u32 rsx_vcache_sweep(rsx_vcache* cache, u32 frame, u32 keep_filled, u32 keep_other);

/* Frame-end housekeeping. Sweeps when the table is over half full (or asked
 * for room), first gently, then down to the entries of the last two frames.
 * Returns 1 when only a full reset helps (arena full, or the table is still
 * over half full after sweeping): the caller waits for the GPU and calls
 * rsx_vcache_reset. sweep_enabled = 0 keeps the old reset-only behaviour. */
int rsx_vcache_frame_end(rsx_vcache* cache, u32 frame, int sweep_enabled);

/* Sort by (location, offset) and merge overlapping or touching ranges of one
 * location in place. Returns the new count. */
u32 rsx_vcache_merge_ranges(rsx_vcache_range* ranges, u32 count);

/* The guest byte ranges rsx_vertex_fetch_many reads for this prepared plan,
 * merged. Returns the count, or 0 when some read attribute has no single
 * contiguous mapping (stable_base) or the ranges do not fit: such a draw
 * cannot be validated by hashing and must bypass the cache. A plan reading
 * nothing from guest memory (only constant attributes) returns 0 too. */
u32 rsx_vcache_ranges_from_plan(const rsx_vertex_fetch_plan* plan,
                                rsx_vcache_range* out);

/* Hash the bytes of `ranges` (re-resolved through guest_ptr, so a changed
 * mapping is followed). 0 when a range no longer maps contiguously. */
int  rsx_vcache_hash_ranges(const rsx_vcache_range* ranges, u32 count,
                            rsx_vertex_guest_ptr_fn guest_ptr, void* user,
                            u64* hash, u64* hashed_bytes);

/* Decide what a draw does with its entry, given the index-content hash just
 * computed. HIT requires the vertex-content hash too: the caller passes
 * vertex_hash_ok = 1 and vertex_hash when the entry's stored ranges hashed
 * successfully (call rsx_vcache_entry_wants_vertex_hash first). Updates the
 * change streak and the volatile backoff. */
int rsx_vcache_entry_wants_vertex_hash(const rsx_vcache* cache,
                                       const rsx_vcache_entry* entry,
                                       u64 index_hash);
rsx_vcache_action rsx_vcache_decide(rsx_vcache* cache, rsx_vcache_entry* entry,
                                    u32 frame, u64 index_hash,
                                    int vertex_hash_ok, u64 vertex_hash);

/* Volatile/uncacheable gate, checked before any hashing. 1 = bypass now. */
int rsx_vcache_entry_skipped(rsx_vcache* cache, const rsx_vcache_entry* entry,
                             u32 frame);

/* Mark an entry uncacheable for a while (no hashable ranges, unmapped
 * index buffer, ...). */
void rsx_vcache_entry_defer(rsx_vcache_entry* entry, u32 frame, u32 frames);

/* Arena space for a FILL (RSX_VCACHE_ALIGN-aligned offsets). 0 = no room:
 * the cache asks for a reset and the draw must bypass. */
int rsx_vcache_alloc(rsx_vcache* cache, u32 vb_bytes, u32 ib_bytes,
                     u32* vb_offset, u32* ib_offset);

/* Record a completed FILL. */
void rsx_vcache_commit(rsx_vcache* cache, rsx_vcache_entry* entry,
                       u64 index_hash, u64 vertex_hash,
                       const rsx_vcache_range* ranges, u32 n_ranges,
                       u32 vb_offset, u32 vb_bytes, u32 ib_offset, u32 ib_bytes,
                       u32 draw_count, u32 vertex_count, u32 source_refs,
                       int indexed);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_VERTEX_CACHE_H */
