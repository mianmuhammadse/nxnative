// BC images in the Vulkan layer on a GPU without BC (A15 / A16 ...), forced here with DM_TEST_NO_BC=1 so the Mac runs it: Metal aborts the process on a BC pixel format there, and
// a Vulkan game made for the Switch asks for BC images first thing (Until Then, Godot: BC3 at its first texture). The layer stores BC1-3 as ASTC 4x4 and decompresses the rest.
//   · the image is created in the stand-in format (ASTC 4x4 LDR / sRGB, R8, RG8, RGBA8, RGBA16F), also as an array and with mip levels
//   · vkCmdCopyBufferToImage converts the blocks on the CPU: the texture's blocks / texels read back from the GPU equal the NVN layer's reference conversion of the same bytes
//   · views of a BC image (also the sRGB one of a UNORM image) are made in the stand-in format
//   · reading a BC image back into a buffer is refused with one red line, not a wrong picture
#include "../vk/hle_vk.m"
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
void dm_vi_layer_crop(int c[4]) { c[0] = c[1] = c[2] = c[3] = 0; }
int dm_opmode(void) { return 1; }
static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static VkDevice dev; static VkQueue queue; static VkCommandPool pool; static VkDeviceMemory mem; static VkDeviceSize mem_top;

static VkBuffer new_buf(VkDeviceSize size) {
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer b; h_vkCreateBuffer(dev, &ci, NULL, &b); h_vkBindBufferMemory(dev, b, mem, mem_top);
    mem_top += (size + 255) & ~(VkDeviceSize)255;
    memset((uint8_t *)mem_buf(mem).contents + b->off, 0, size);
    return b;
}
static uint8_t *bytes(VkBuffer b) { return (uint8_t *)mem_buf(b->mem).contents + b->off; }
static VkCommandBuffer new_cb(void) {
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; h_vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    h_vkBeginCommandBuffer(cb, &bi);
    return cb;
}
static void run(VkCommandBuffer cb) {
    h_vkEndCommandBuffer(cb);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    h_vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); h_vkQueueWaitIdle(queue);
}
static VkImage new_img(VkFormat fmt, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, VkImageCreateFlags flags) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {w, h, 1}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkImage im = NULL; VkResult r = h_vkCreateImage(dev, &ci, NULL, &im); CHECK(r == VK_SUCCESS && im && im->tex, "image format %d %ux%u created", fmt, w, h);
    return im;
}
static void upload(VkImage im, const uint8_t *data, size_t n, uint32_t w, uint32_t h, uint32_t level, uint32_t layers) {   // one region: the whole level of `layers` layers, the layers back to back
    VkBuffer b = new_buf(n); memcpy(bytes(b), data, n);
    VkCommandBuffer cb = new_cb();
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers}, .imageExtent = {w, h, 1}};
    h_vkCmdCopyBufferToImage(cb, b, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r); run(cb);
}
// The texture's own bytes (blocks for ASTC, texels otherwise) from the GPU: a blit of the level and slice into a shared buffer
static void texture_bytes(id<MTLTexture> t, uint32_t level, uint32_t slice, uint32_t w, uint32_t h, size_t row, size_t total, uint8_t *out) {
    id<MTLBuffer> b = [g_dev newBufferWithLength:total options:MTLResourceStorageModeShared]; memset(b.contents, 0xEE, total);
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:t sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:total];
    [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
    memcpy(out, b.contents, total);
}
static uint32_t rng = 12345;
static uint8_t rnd(void) { rng = rng * 1664525u + 1013904223u; return (uint8_t)(rng >> 24); }

// A format's case: the Vulkan format, its NVN number, the block size in bytes, the expected stand-in pixel format and whether it is ASTC (else texels of `bpp` bytes)
typedef struct { VkFormat vk; int nvn, bb; MTLPixelFormat pf; int astc, bpp; const char *name; } bc_case;

// One BC image of w×h texels: random blocks uploaded, the texture read back, compared with the reference conversion of the same blocks
static void check_format(const bc_case *c, uint32_t w, uint32_t h) {
    VkImage im = new_img(c->vk, w, h, 1, 1, 0); if (!im || !im->tex) return;
    CHECK(im->tex.pixelFormat == c->pf, "%s: stand-in pixel format %lu (got %lu)", c->name, (unsigned long)c->pf, (unsigned long)im->tex.pixelFormat);
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4; size_t n = (size_t)bw * bh * c->bb; uint8_t *src = malloc(n);
    for (size_t i = 0; i < n; i++) src[i] = rnd();
    upload(im, src, n, w, h, 0, 1);
    size_t row = c->astc ? (size_t)bw * 16 : (size_t)w * c->bpp, total = c->astc ? row * bh : row * h;
    uint8_t *want = calloc(1, total), *got = calloc(1, total);
    if (c->astc) nvn_bc2astc((uint32_t)c->nvn, src, (size_t)bw * c->bb, w, h, want, row); else nvn_bc_decode((uint32_t)c->nvn, src, (size_t)bw * c->bb, w, h, want, row);
    texture_bytes(im->tex, 0, 0, w, h, row, total, got);
    CHECK(!memcmp(want, got, total), "%s %ux%u: the texture holds the reference conversion of the uploaded blocks", c->name, w, h);
    free(src); free(want); free(got);
}

int main(void) {
    setenv("DM_TEST_NO_BC", "1", 1);
    dev = &g_device; queue = &g_queue;
    @autoreleasepool {
        dev_init();
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; h_vkCreateCommandPool(dev, &pci, NULL, &pool);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 4 << 20, .memoryTypeIndex = 1}; h_vkAllocateMemory(dev, &mai, NULL, &mem);
        CHECK(mtl_no_bc(), "the test switch DM_TEST_NO_BC forces the no-BC path");

        const bc_case cases[] = {
            {VK_FORMAT_BC1_RGB_UNORM_BLOCK, 0x41, 8, MTLPixelFormatASTC_4x4_LDR, 1, 0, "BC1 RGB"},
            {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 0x42, 8, MTLPixelFormatASTC_4x4_LDR, 1, 0, "BC1 RGBA"},
            {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 0x46, 8, MTLPixelFormatASTC_4x4_sRGB, 1, 0, "BC1 sRGB"},
            {VK_FORMAT_BC2_UNORM_BLOCK, 0x43, 16, MTLPixelFormatASTC_4x4_LDR, 1, 0, "BC2"},
            {VK_FORMAT_BC3_UNORM_BLOCK, 0x44, 16, MTLPixelFormatASTC_4x4_LDR, 1, 0, "BC3"},
            {VK_FORMAT_BC3_SRGB_BLOCK, 0x48, 16, MTLPixelFormatASTC_4x4_sRGB, 1, 0, "BC3 sRGB"},
            {VK_FORMAT_BC4_UNORM_BLOCK, 0x49, 8, MTLPixelFormatR8Unorm, 0, 1, "BC4"},
            {VK_FORMAT_BC4_SNORM_BLOCK, 0x4a, 8, MTLPixelFormatR8Snorm, 0, 1, "BC4 snorm"},
            {VK_FORMAT_BC5_UNORM_BLOCK, 0x4b, 16, MTLPixelFormatRG8Unorm, 0, 2, "BC5"},
            {VK_FORMAT_BC7_UNORM_BLOCK, 0x4d, 16, MTLPixelFormatRGBA8Unorm, 0, 4, "BC7"},
            {VK_FORMAT_BC7_SRGB_BLOCK, 0x4e, 16, MTLPixelFormatRGBA8Unorm_sRGB, 0, 4, "BC7 sRGB"},
            {VK_FORMAT_BC6H_UFLOAT_BLOCK, 0x50, 16, MTLPixelFormatRGBA16Float, 0, 8, "BC6H"},
        };
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) check_format(&cases[i], 8, 8);
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) check_format(&cases[i], 6, 6);   // a size that is not a multiple of the block: the edge blocks are partial

        // ---- A mip chain: the second level (4x4) and a third (2x2) of a BC3 image ----
        {
            VkImage im = new_img(VK_FORMAT_BC3_UNORM_BLOCK, 8, 8, 3, 1, 0);
            uint8_t l1[16], l2[16]; for (int i = 0; i < 16; i++) { l1[i] = rnd(); l2[i] = rnd(); }
            upload(im, l1, 16, 4, 4, 1, 1); upload(im, l2, 16, 2, 2, 2, 1);
            uint8_t want[16], got[16];
            nvn_bc2astc(0x44, l1, 16, 4, 4, want, 16); texture_bytes(im->tex, 1, 0, 4, 4, 16, 16, got);
            CHECK(!memcmp(want, got, 16), "BC3 mip level 1 (4x4) holds the converted block");
            nvn_bc2astc(0x44, l2, 16, 2, 2, want, 16); texture_bytes(im->tex, 2, 0, 2, 2, 16, 16, got);
            CHECK(!memcmp(want, got, 16), "BC3 mip level 2 (2x2, a partial block) holds the converted block");
        }

        // ---- An array of two layers uploaded in one region ----
        {
            VkImage im = new_img(VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 1, 2, 0);
            uint8_t src[32]; for (int i = 0; i < 32; i++) src[i] = rnd();
            upload(im, src, 32, 4, 4, 0, 2);
            uint8_t want[16], got[16];
            for (int l = 0; l < 2; l++) {
                nvn_bc2astc(0x44, src + l * 16, 16, 4, 4, want, 16); texture_bytes(im->tex, 0, (uint32_t)l, 4, 4, 16, 16, got);
                CHECK(!memcmp(want, got, 16), "BC3 array layer %d holds its own converted block", l);
            }
        }

        // ---- Views: the BC format's own, and the sRGB one of a UNORM image (a mutable-format image) ----
        {
            VkImage im = new_img(VK_FORMAT_BC3_UNORM_BLOCK, 8, 8, 1, 1, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
            VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_BC3_UNORM_BLOCK,
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            VkImageView v = NULL; h_vkCreateImageView(dev, &vi, NULL, &v);
            CHECK(v && v->tex && v->tex.pixelFormat == MTLPixelFormatASTC_4x4_LDR, "a BC3 view of a BC3 image is the ASTC 4x4 texture");
            vi.format = VK_FORMAT_BC3_SRGB_BLOCK; VkImageView vs = NULL; h_vkCreateImageView(dev, &vi, NULL, &vs);
            CHECK(vs && vs->tex && vs->tex.pixelFormat == MTLPixelFormatASTC_4x4_sRGB, "the sRGB view of a BC3 image is the ASTC 4x4 sRGB texture");
            VkImage im7 = new_img(VK_FORMAT_BC7_UNORM_BLOCK, 8, 8, 1, 1, 0);
            vi.image = im7; vi.format = VK_FORMAT_BC7_UNORM_BLOCK; VkImageView v7 = NULL; h_vkCreateImageView(dev, &vi, NULL, &v7);
            CHECK(v7 && v7->tex && v7->tex.pixelFormat == MTLPixelFormatRGBA8Unorm, "a BC7 view is the RGBA8 texture");
        }

        // ---- A format that is not BC is untouched ----
        {
            VkImage im = new_img(VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 1, 0);
            CHECK(im && !im->bcm && im->tex.pixelFormat == MTLPixelFormatRGBA8Unorm, "an RGBA8 image is created as before");
        }

        // ---- Reading a BC image back is refused (one red line), not a wrong picture ----
        {
            VkImage im = new_img(VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 1, 1, 0);
            VkBuffer b = new_buf(64); memset(bytes(b), 0x5A, 64);
            VkCommandBuffer cb = new_cb(); VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}};
            h_vkCmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &r); run(cb);
            CHECK(bytes(b)[0] == 0x5A, "a BC image read back into a buffer leaves the buffer alone");
        }
    }
    fprintf(stderr, "t_vk_bc: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
