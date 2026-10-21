// Upload batch order (up_flush): a texture zeroed and uploaded in the same batch must end with the uploaded content, whichever was queued first. Without BC, BC1–3 textures are ASTC 4x4:
// the test queues the content copies BEFORE the zero fill (the worst order) and flushes through the real up_flush, then reads the texture back (ASTC blocks, byte for byte).
//   A  content copy queued, then zero fill, same batch  → the content
//   B  zero fill alone                                  → the constant (0,0,0,0) ASTC block (an all-zero block would be invalid)
//   C  a `bca` transcoding job + the copy of its output, then zero fill, same batch → the transcoded blocks (kernel dispatch before the copy, copy after the fill)
// Usage: t_up_order
#include "../nvn/nvn_mtl.m"
int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]) { (void)p; (void)stage; memset(key, 0, 8); return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

// symbols of hle_nvn.c, which this test does not link: no sparse pools, every address readable
int nvn_host_readable(uint64_t a, uint64_t n) { (void)a; (void)n; return 1; }
uint64_t nvn_new_serial(void) { static uint64_t n = 90000; return ++n; }
int nvn_sparse_pool(const void *pool) { (void)pool; return 0; }
uint64_t nvn_sparse_gen(void) { return 0; }
uint64_t nvn_sparse_copy(const void *pool, void *dst, uint64_t a, uint64_t n) { (void)pool; (void)dst; (void)a; (void)n; return 0; }
int nvn_sparse_since(const void *pool, uint64_t a, uint64_t n, uint64_t gen, uint64_t stamp) { (void)pool; (void)a; (void)n; (void)gen; (void)stamp; return 0; }
void nvn_sparse_protect(const void *pool, uint64_t a, uint64_t n) { (void)pool; (void)a; (void)n; }
int nvn_sparse_mapped(const void *pool, uint64_t a, uint64_t n) { (void)pool; (void)a; (void)n; return 1; }

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
        up_flush(); wait_uploads();
        uint8_t got[64]; read_back(t, got);
        CHECK(!memcmp(got, want, 64), "A: texture zeroed after the content was queued does not hold the content");
    }
    {   // B: zero fill alone
        id<MTLTexture> t = new_astc();
        tex_zero_fill(t, 1); up_flush(); wait_uploads();
        uint8_t got[64]; read_back(t, got);
        int ok = 1; for (int b = 0; b < 4; b++) ok &= !memcmp(got + b * 16, zero, 16);
        CHECK(ok, "B: zero fill does not leave the constant ASTC block");
    }
    {   // C: GPU transcoding job (BC3 blocks in a buffer → ASTC blocks in the same buffer) + the copy of its output + zero fill, one batch
        id<MTLTexture> t = new_astc();
        uint8_t stage[64 + 64]; memset(stage, 0xCD, sizeof stage); memcpy(stage, bc[0], 16); memcpy(stage + 16, bc[1], 16); memcpy(stage + 32, bc[2], 16); memcpy(stage + 48, bc[3], 16);   // 2x2 BC3 blocks, row pitch 32, then 64 bytes of output
        id<MTLBuffer> stg = [g_dev newBufferWithBytes:stage length:sizeof stage options:MTLResourceStorageModeShared]; res_add(stg);
        up_begin();
        size_t qoff; id<MTLBuffer> q = up_param_alloc(&qoff);
        uint32_t *pp = (uint32_t *)((uint8_t *)q.contents + qoff); pp[0] = 0; pp[1] = 32; pp[2] = 64; pp[3] = 32; pp[4] = 2; pp[5] = 2; pp[6] = 3;
        if (g_nup_asts == g_up_astcap) { g_up_astcap = g_up_astcap ? g_up_astcap * 2 : 16; g_up_asts = realloc(g_up_asts, g_up_astcap * sizeof *g_up_asts); }
        g_up_asts[g_nup_asts++] = (up_ast){stg, q, qoff, 2, 2};
        up_blit_add(stg, 64, 32, 64, MTLSizeMake(8, 8, 1), t, 0, 0); [g_up_texs addObject:t];
        tex_zero_fill(t, 1);
        up_flush(); wait_uploads();
        uint8_t got[64]; read_back(t, got);
        CHECK(!memcmp(got, want, 64), "C: transcoded blocks do not end up in the texture (dispatch, fill and copy out of order)");
    }
    printf("up_flush order: %s\n", failed ? "FAILED" : "ok (content queued before the zero fill still wins; kernel output copied after the dispatch)");
    return failed != 0;
}
