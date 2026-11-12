/*
 * rsx_vertex_remap_build's dense path against its hash path.
 *
 * Random reference lists -- indexed meshes (narrow id range, one base_index),
 * sparse ids, mixed base_index, ids near the 32-bit top, single refs -- go
 * through both paths; the unique count, the rewritten refs prefix and the
 * occurrence map must be identical. The same remap object is reused across
 * calls and sizes, as the renderer does. Then a timing of a typical mesh.
 *
 * Build (from the repo root):
 *   clang -std=gnu17 -O2 -I include -I libs/video -o test_vertex_remap_dense \
 *     libs/video/tests/test_vertex_remap_dense.c libs/video/rsx_vertex_compact.c \
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

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}

static void make_refs(rsx_vertex_ref* refs, u32 count, int shape)
{
    const u32 base = rnd() % 3 ? rnd() % 1000 : 0;
    const u32 low = shape == 3 ? 0xFFFFFFFFu - (rnd() % 5000) - 5000u
                               : rnd() % 100000;
    const u32 range = shape == 1 ? 1u + rnd() % (count * 64u + 1u)   /* sparse */
                                 : 1u + rnd() % (count + 1u);        /* mesh */
    for (u32 i = 0; i < count; i++) {
        refs[i].vertex_id = low + rnd() % range;
        refs[i].base_index = base;
        if (shape == 2 && rnd() % 50 == 0)                           /* mixed */
            refs[i].base_index = base + 1u;
    }
}

int main(void)
{
    enum { MAX = 20000 };
    rsx_vertex_ref* input = malloc(MAX * sizeof *input);
    rsx_vertex_ref* a = malloc(MAX * sizeof *a);
    rsx_vertex_ref* b = malloc(MAX * sizeof *b);
    u32* map_hash = malloc(MAX * sizeof *map_hash);
    rsx_vertex_remap dense = {0}, hash = {0};
    unsigned cases = 0, dense_shaped = 0;

    for (int iter = 0; iter < 4000; iter++) {
        const u32 count = iter % 10 == 0 ? 1u + rnd() % 4u : 1u + rnd() % MAX;
        const int shape = rnd() % 4;
        make_refs(input, count, shape);
        memcpy(a, input, count * sizeof *a);
        memcpy(b, input, count * sizeof *b);
        u32 unique_dense = 0, unique_hash = 0;
        rsx_vertex_remap_set_dense(0);
        const int ok_hash = rsx_vertex_remap_build(&hash, b, count, &unique_hash);
        for (u32 i = 0; i < count; i++) map_hash[i] = rsx_vertex_remap_index(&hash, i);
        rsx_vertex_remap_set_dense(1);
        const int ok_dense = rsx_vertex_remap_build(&dense, a, count, &unique_dense);
        cases++;
        if (shape == 0) dense_shaped++;
        CHECK(ok_dense == ok_hash, "iter %d ok %d vs %d", iter, ok_dense, ok_hash);
        CHECK(unique_dense == unique_hash, "iter %d shape %d unique %u vs %u",
              iter, shape, unique_dense, unique_hash);
        CHECK(memcmp(a, b, unique_hash * sizeof *a) == 0, "iter %d refs differ", iter);
        int map_ok = 1;
        for (u32 i = 0; i < count; i++)
            if (rsx_vertex_remap_index(&dense, i) != map_hash[i]) { map_ok = 0; break; }
        CHECK(map_ok, "iter %d occurrence map differs", iter);
    }
    printf("test_vertex_remap_dense: %u cases, %d failure(s)\n", cases, fails);

    /* Timing: a 6000-index mesh over 2000 vertices, 2000 times per path. */
    const u32 count = 6000;
    for (u32 i = 0; i < count; i++) {
        input[i].vertex_id = 5000u + rnd() % 2000u;
        input[i].base_index = 0;
    }
    for (int mode = 0; mode < 2; mode++) {
        rsx_vertex_remap_set_dense(mode);
        rsx_vertex_remap* r = mode ? &dense : &hash;
        u32 unique = 0;
        const clock_t t0 = clock();
        for (int rep = 0; rep < 2000; rep++) {
            memcpy(a, input, count * sizeof *a);
            rsx_vertex_remap_build(r, a, count, &unique);
        }
        const double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
        printf("  %s path: %.1f us per 6000-ref draw (unique %u)\n",
               mode ? "dense" : "hash ", ms * 1000.0 / 2000.0, unique);
    }
    rsx_vertex_remap_destroy(&dense);
    rsx_vertex_remap_destroy(&hash);
    free(input); free(a); free(b); free(map_hash);
    return fails ? 1 : 0;
}
