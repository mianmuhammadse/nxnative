// A Vulkan game's shaders in several libraries (vk_shaders.metallib, vk_shaders.1.metallib, ...: vk_shaders.py links at most DM_VK_LIB_SHADERS to a library, because one link of 3,359 Godot
// shaders took 28 GB of memory): the layer loads every library of the folder and makes each function from the library that holds it (lib_of), also one with specialization constants.
//   · five tiny libraries are built with the Metal compiler, two functions in each of the first four and one in the last, and loaded through DM_MSL_DIR as the host does
//   · every function is found in its own library, the first library is g_lib, a name no library has resolves to nothing, and a gap in the numbering ends the search
#include "../vk/hle_vk.m"
#include <stdarg.h>
#include <sys/stat.h>
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

static int sh(const char *fmt, ...) {   // a shell command, 0 on success
    char cmd[2048]; va_list ap; va_start(ap, fmt); vsnprintf(cmd, sizeof cmd, fmt, ap); va_end(ap);
    return system(cmd);
}

int main(void) {
    char dir[] = "/tmp/t_vk_libs.XXXXXX"; if (!mkdtemp(dir)) { fprintf(stderr, "no temporary folder\n"); return 1; }
    char vk[1100]; snprintf(vk, sizeof vk, "%s/vk", dir); mkdir(vk, 0755);
    // library i holds vk_k<2i> and vk_k<2i+1> (the last one only vk_k8); the files go to <dir>/vk, a gap after number 4 (vk_shaders.6.metallib exists, .5 does not)
    for (int i = 0; i < 5; i++) {
        char src[1200]; snprintf(src, sizeof src, "%s/l%d.metal", dir, i); FILE *f = fopen(src, "w");
        fprintf(f, "#include <metal_stdlib>\nusing namespace metal;\n");
        for (int k = 2 * i; k < 2 * i + 2 && k < 9; k++) fprintf(f, "vertex float4 vk_k%d(uint v [[vertex_id]]) { return float4(float(v + %d)); }\n", k, k);
        fclose(f);
        char nm[40]; if (i) snprintf(nm, sizeof nm, "vk_shaders.%d.metallib", i); else snprintf(nm, sizeof nm, "vk_shaders.metallib");
        int rc = sh("cd %s && xcrun -sdk macosx metal -c l%d.metal -o l%d.air 2>/dev/null && xcrun -sdk macosx metallib l%d.air -o vk/%s 2>/dev/null", dir, i, i, i, nm);
        CHECK(rc == 0, "library %d is built", i);
    }
    sh("cd %s/vk && cp vk_shaders.4.metallib vk_shaders.6.metallib", dir);   // beyond the gap: not loaded
    sh("echo '# test table' > %s/vk/vk_shaders.tsv", dir);
    setenv("DM_MSL_DIR", dir, 1);
    @autoreleasepool {
        dev_init();
        CHECK(g_lib != nil && g_nlibs == 5, "five libraries are loaded, the one past the gap is not (got %d)", g_nlibs);
        CHECK(g_fnlib.count == 9, "nine functions are known (got %lu)", (unsigned long)g_fnlib.count);
        for (int k = 0; k < 9; k++) {
            NSString *n = [NSString stringWithFormat:@"vk_k%d", k];
            id<MTLLibrary> l = lib_of(n);
            CHECK(l != nil && [l newFunctionWithName:n] != nil, "%s is made from the library that holds it", n.UTF8String);
            CHECK((k < 2) == (l == g_lib), "%s %s the first library", n.UTF8String, k < 2 ? "is in" : "is not in");
        }
        CHECK([lib_of(@"vk_k0") newFunctionWithName:@"vk_k8"] == nil, "a function is not found in another library");
        CHECK([lib_of(@"vk_nothing") newFunctionWithName:@"vk_nothing"] == nil, "a name no library has resolves to nothing");
    }
    sh("rm -rf %s", dir);
    fprintf(stderr, "t_vk_libs: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
