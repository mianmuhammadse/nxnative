// Identity of a packaged (block-linear) texture across runs, computed from the bytes the game stores: used by the BC3 keep table (dm/games/<id>/bc3_keep_rgba8.tsv), which names the BC3 textures
// that stay RGBA8 on GPUs without BC because ASTC would lose too much alpha. The benchmark (tests/bench_bc2astc_dread.c) and the host compute the key with this one function.
#pragma once
#include <stdint.h>

// 64-bit FNV-1a over the first min(size, 64 KB) bytes of the storage, then mixed with the total size, NVN format, width, height and level count
static inline uint64_t nvn_texkey(const void *storage, uint64_t size, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels) {
    const uint8_t *p = storage; uint64_t n = size < 65536 ? size : 65536, x = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < n; i++) { x ^= p[i]; x *= 0x100000001b3ull; }
    uint64_t m[5] = {size, fmt, w, h, levels};
    for (int i = 0; i < 5; i++) { x ^= m[i]; x *= 0x100000001b3ull; x ^= x >> 29; }
    return x;
}
