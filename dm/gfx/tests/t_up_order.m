// Upload batch order (up_flush): a texture zeroed and uploaded in the same batch must end with the uploaded content, whichever was queued first. Without BC, BC1–3 textures are ASTC 4x4:
// the test queues the content copies BEFORE the zero fill (the worst order) and flushes through the real up_flush, then reads the texture back (ASTC blocks, byte for byte).
//   A  content copy queued, then zero fill, same batch  → the content
//   B  zero fill alone                                  → the constant (0,0,0,0) ASTC block (an all-zero block would be invalid)
//   C  a large `bca` transcoding job + the copy of its output, then zero fill, same batch → the transcoded blocks (kernel finished before the copy starts, copy after the fill)
// Besides the data, the test counts the intra-encoder barriers up_flush encodes (g_up_ebar) and asserts the exact number: A 1, B 0, C 2. Whether an unordered copy overtakes its kernel depends on what else the GPU runs
// (an idle GPU often runs them in order anyway, and the bug seen in Metroid Dread needed a busy one), so the data check alone cannot be trusted to fail without the barriers.
// Usage: t_up_order
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int failed;
#define CHECK(c, ...) do { if (!(c)) { failed++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static id<MTLTexture> new_astc(void) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatASTC_4x4_LDR width:8 height:8 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d]; res_add(t); return t;
}
static void wait_uploads(void) { for (int i = 0; i < 2000 && atomic_load(&g_up_pending); i++) usleep(1000); }
static void read_back(id<MTLTexture> t, uint8_t out[64]) {   // 2x2 ASTC blocks = 64 bytes
    id<MTLBuffer> dst = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; res_add(dst);
    id read = blit();
    [read copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 8, 1) toBuffer:dst destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:64];
    enc_b_end(g_blit); g_blit = nil;
    mtl_sub_commit(); mtl_drain();
    memcpy(out, dst.contents, 64);
    mtl_sub_begin();
}

int main(void) {
    setenv("DM_TEST_NO_BC", "1", 1);
    mtl_init(); mtl_sub_begin();
    // four BC3 blocks with varying alpha, and what they transcode to
    uint8_t bc[4][16], want[64]; uint32_t s = 12345;
    for (int b = 0; b < 4; b++) { for (int i = 0; i < 16; i++) { s = s * 1664525u + 1013904223u; bc[b][i] = (uint8_t)(s >> 24); } nvn_bc2astc_block(3, bc[b], want + b * 16); }
    uint8_t zero[16]; nvn_astc_zero_block(zero);
    for (int b = 0; b < 4; b++) CHECK(memcmp(want + b * 16, zero, 16), "test block %d transcodes to the zero block", b);

    {   // A: content queued first, zero fill second, one batch
        id<MTLTexture> t = new_astc();
        id<MTLBuffer> src = [g_dev newBufferWithBytes:want length:64 options:MTLResourceStorageModeShared]; res_add(src);
        up_begin(); up_blit_add(src, 0, 32, 64, MTLSizeMake(8, 8, 1), t, 0, 0); [g_up_texs addObject:t];
        tex_zero_fill(t, 1);   // queues the fill (the host has a command buffer open, so this does not flush by itself)
        uint64_t eb0 = g_up_ebar; up_flush(); wait_uploads();
        CHECK(g_up_ebar - eb0 == 1, "A: %llu intra-encoder barriers, expected 1 (zero fill, barrier, content copy)", (unsigned long long)(g_up_ebar - eb0));
        uint8_t got[64]; read_back(t, got);
        CHECK(!memcmp(got, want, 64), "A: texture zeroed after the content was queued does not hold the content");
    }
    {   // B: zero fill alone
        id<MTLTexture> t = new_astc();
        tex_zero_fill(t, 1); uint64_t eb0 = g_up_ebar; up_flush(); wait_uploads();
        CHECK(g_up_ebar == eb0, "B: a lone zero fill needs no barrier, %llu encoded", (unsigned long long)(g_up_ebar - eb0));
        uint8_t got[64]; read_back(t, got);
        int ok = 1; for (int b = 0; b < 4; b++) ok &= !memcmp(got + b * 16, zero, 16);
        CHECK(ok, "B: zero fill does not leave the constant ASTC block");
    }
    {   // C: GPU transcoding job (BC3 blocks in a buffer → ASTC blocks in the same buffer) + the copy of its output + zero fill, one batch. Large (512x512 blocks): a copy that is not ordered after
        // the kernel starts before the kernel has written its blocks and reads zeros, which a handful of blocks never shows
        const uint32_t NB = 512, N = NB * NB; const size_t BYTES = (size_t)N * 16;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatASTC_4x4_LDR width:NB * 4 height:NB * 4 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; d.storageMode = MTLStorageModePrivate; id<MTLTexture> t = [g_dev newTextureWithDescriptor:d]; res_add(t);
        uint8_t *stage = calloc(1, BYTES * 2), *wantbig = malloc(BYTES); uint32_t s2 = 777;
        for (uint32_t i = 0; i < N; i++) { uint8_t *b3 = stage + (size_t)i * 16; for (int k = 0; k < 16; k++) { s2 = s2 * 1664525u + 1013904223u; b3[k] = (uint8_t)(s2 >> 24); } nvn_bc2astc_block(3, b3, wantbig + (size_t)i * 16); }
        id<MTLBuffer> stg = [g_dev newBufferWithBytes:stage length:BYTES * 2 options:MTLResourceStorageModeShared]; res_add(stg);
        up_begin();
        size_t qoff; id<MTLBuffer> q = up_param_alloc(&qoff);
        uint32_t *pp = (uint32_t *)((uint8_t *)q.contents + qoff); pp[0] = 0; pp[1] = NB * 16; pp[2] = (uint32_t)BYTES; pp[3] = NB * 16; pp[4] = NB; pp[5] = NB; pp[6] = 3;
        if (g_nup_asts == g_up_astcap) { g_up_astcap = g_up_astcap ? g_up_astcap * 2 : 16; g_up_asts = realloc(g_up_asts, g_up_astcap * sizeof *g_up_asts); }
        g_up_asts[g_nup_asts++] = (up_ast){stg, q, qoff, NB, NB};
        up_blit_add(stg, BYTES, NB * 16, BYTES, MTLSizeMake(NB * 4, NB * 4, 1), t, 0, 0); [g_up_texs addObject:t];
        tex_zero_fill(t, 1);
        uint64_t eb0 = g_up_ebar; up_flush(); wait_uploads();
        CHECK(g_up_ebar - eb0 == 2, "C: %llu intra-encoder barriers, expected 2 (zero fill → kernel, kernel → copy); a queue barrier does not order commands inside one encoder", (unsigned long long)(g_up_ebar - eb0));
        id<MTLBuffer> dst = [g_dev newBufferWithLength:BYTES options:MTLResourceStorageModeShared]; res_add(dst);
        id read = blit();
        [read copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(NB * 4, NB * 4, 1) toBuffer:dst destinationOffset:0 destinationBytesPerRow:NB * 16 destinationBytesPerImage:BYTES];
        enc_b_end(g_blit); g_blit = nil; mtl_sub_commit(); mtl_drain(); mtl_sub_begin();
        size_t bad = 0; for (uint32_t i = 0; i < N; i++) bad += memcmp((uint8_t *)dst.contents + (size_t)i * 16, wantbig + (size_t)i * 16, 16) != 0;
        CHECK(bad == 0, "C: %zu of %u transcoded blocks do not end up in the texture (the copy ran before the kernel finished, or fill and copy out of order)", bad, N);
        free(stage); free(wantbig);
    }
    printf("up_flush order: %s\n", failed ? "FAILED" : "ok (content queued before the zero fill still wins; kernel output copied after the dispatch)");
    return failed != 0;
}
