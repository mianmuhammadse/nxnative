// BC1 / BC2 / BC3 → ASTC 4x4 transcoder (bc2astc_core.h, nvn_bc2astc.c): per-category error against bcdec, with ARM's astcenc as the ASTC decoder (seconds).
// Needs the astcenc binary: ASTCENC=/path/to/astcenc (default: "astcenc" from PATH). The test fails if it cannot run it.
// Usage: t_bc2astc [-v]

#define BCDEC_IMPLEMENTATION
#include "../../../third_party/bcdec/bcdec.h"
#include "../bc2astc_core.h"
#include "../nvn_bc2astc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *mkdtemp(char *);   // POSIX; not visible under the default feature level here
static int checks, fails, verbose;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static uint32_t rndn(uint32_t n) { return rnd32() % n; }

static char tmpdir[256]; static const char *astcenc;

// ---------- ASTC decode through astcenc: N blocks in one row-major image (nbx wide) → RGBA8 pixels (4*nbx wide) ----------
static int astc_decode(const uint8_t *blocks, int nbx, int nby, int srgb, uint8_t *rgba) {
    char in[300], out[300], cmd[900];
    snprintf(in, sizeof in, "%s/in.astc", tmpdir); snprintf(out, sizeof out, "%s/out.ktx", tmpdir);
    FILE *f = fopen(in, "wb"); if (!f) return -1;
    int w = nbx * 4, h = nby * 4;
    uint8_t hd[16] = {0x13, 0xAB, 0xA1, 0x5C, 4, 4, 1, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)h, (uint8_t)(h >> 8), (uint8_t)(h >> 16), 1, 0, 0};
    fwrite(hd, 1, 16, f); fwrite(blocks, 16, (size_t)nbx * nby, f); fclose(f);
    snprintf(out, sizeof out, "%s/out.tga", tmpdir);
    snprintf(cmd, sizeof cmd, "\"%s\" %s \"%s\" \"%s\" >/dev/null 2>&1", astcenc, srgb ? "-ds" : "-dl", in, out);
    if (system(cmd) != 0) return -2;
    f = fopen(out, "rb"); if (!f) return -3;
    fseek(f, 0, SEEK_END); long fs = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(fs); if (fread(d, 1, fs, f) != (size_t)fs) { fclose(f); free(d); return -4; } fclose(f);
    // TGA: type 2 (raw) or 10 (RLE), 32 bits per pixel BGRA; descriptor bit 5 set = rows top-down
    int type = d[2], bpp = d[16], top = (d[17] >> 5) & 1, tw = d[12] | d[13] << 8, th = d[14] | d[15] << 8;
    if ((type != 2 && type != 10) || bpp != 32 || tw != w || th != h) { free(d); return -5; }
    const uint8_t *p = d + 18 + d[0]; size_t npx = (size_t)w * h, i = 0; uint8_t *lin = malloc(npx * 4);
    while (i < npx) {
        if (type == 2) { memcpy(lin + i * 4, p, 4); p += 4; i++; continue; }
        int hd = *p++, cnt = (hd & 127) + 1;
        for (int k = 0; k < cnt && i < npx; k++, i++) { memcpy(lin + i * 4, p, 4); if (hd & 128) continue; p += 4; }
        if (hd & 128) p += 4;
    }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const uint8_t *s = lin + ((size_t)(top ? y : h - 1 - y) * w + x) * 4; uint8_t *o = rgba + ((size_t)y * w + x) * 4;
        o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = s[3];
    }
    free(lin); free(d);
    return 0;
}

// ---------- block generators ----------
enum { K1 = 1, K2 = 2, K3 = 3 };
// Expectations per category (set from the measured numbers, see the README): layout (-1 = any), swap (-1 = any, 0 = never, 1 = every block), RGB max error and PSNR floor, alpha max error and PSNR floor
typedef struct { const char *name; int kind; void (*gen)(uint8_t b[16]); int (*ok)(const uint8_t b[16]); int layout, swap, max_rgb; double psnr_rgb; int max_a; double psnr_a; } cat_t;

static void put16(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *b, uint32_t v) { put16(b, v & 0xffff); put16(b + 2, v >> 16); }
static uint32_t get32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static void colblk(uint8_t *b, uint32_t c0, uint32_t c1, uint32_t ix) { put16(b, c0); put16(b + 2, c1); put32(b + 4, ix); }
static uint32_t rampidx(void) { uint32_t ix = 0; for (int i = 0; i < 16; i++) ix |= (uint32_t)(((i & 3) + (i >> 2)) * 3 / 6) << (2 * i); return ix; }   // 0..3 across the block diagonal
static uint32_t rnd565(void) { return rndn(65536); }
// 8-bit endpoint sums of a block's colour endpoints as the transcoder sees them (bcdec arithmetic)
static void esums(const uint8_t *cb, int *s0, int *s1) {
    uint32_t w[4] = {get32(cb), 0, 0, 0}; int ref[16]; bca_col(w[0] & 0xffff, w[0] >> 16, 0, ref);
    *s0 = ref[0] + ref[1] + ref[2]; *s1 = ref[4] + ref[5] + ref[6];
}
static int sums7(const uint8_t *cb, int *s0, int *s1) {   // the same with the 7-bit endpoints of layout L2
    uint32_t w = get32(cb); int ref[16]; bca_col(w & 0xffff, w >> 16, 1, ref); *s0 = 0; *s1 = 0;
    for (int c = 0; c < 3; c++) { int q0 = bca_near7(ref[c]), q1 = bca_near7(ref[4 + c]); *s0 += (q0 << 1) | (q0 >> 6); *s1 += (q1 << 1) | (q1 >> 6); }
    return 0;
}

static void g_bc1_op(uint8_t *b) { uint32_t a = rnd565(), c = rnd565(); if (a == c) c ^= 0x20; if (a < c) { uint32_t t = a; a = c; c = t; } colblk(b, a, c, rnd32()); }
static int ok_bc1_noswap(const uint8_t *b) { int s0, s1; esums(b, &s0, &s1); return s1 >= s0; }
static int ok_bc1_swap(const uint8_t *b) { int s0, s1; esums(b, &s0, &s1); return s1 < s0; }
static void g_bc1_ramp(uint8_t *b) { uint32_t a = rnd565(), c = (a & 0xffe0) ^ 0x0841; if (a < c) { uint32_t t = a; a = c; c = t; } colblk(b, a, c, rampidx()); }
static void g_bc1_pt(uint8_t *b) { uint32_t a = rnd565(), c = rnd565(); if (a > c) { uint32_t t = a; a = c; c = t; } colblk(b, a, c, rnd32()); }   // c0 <= c1, any index (3 = transparent)
static int ok_pt_noswap(const uint8_t *b) { int s0, s1; sums7(b, &s0, &s1); return s1 >= s0; }
static int ok_pt_swap(const uint8_t *b) { int s0, s1; sums7(b, &s0, &s1); return s1 < s0; }
static void g_bc1_pt_no3(uint8_t *b) { uint32_t a = rnd565(), c = rnd565(); if (a > c) { uint32_t t = a; a = c; c = t; } uint32_t ix = 0; for (int i = 0; i < 16; i++) ix |= rndn(3) << (2 * i); colblk(b, a, c, ix); }
static void g_bc1_pt_edge(uint8_t *b) {   // punch-through with every texel a different index and one end black / white
    uint32_t pick = rndn(4); uint32_t a = pick == 0 ? 0 : rnd565(), c = pick == 1 ? 0xffff : rnd565(); if (a > c) { uint32_t t = a; a = c; c = t; } colblk(b, a, c, rnd32()); }
static void g_bc1_solid(uint8_t *b) { uint32_t a = rnd565(); colblk(b, a, a, rnd32()); }   // c0 == c1: indices 0 1 2 are the same colour, 3 transparent (solid only if no index 3)
static void g_bc1_all(uint8_t *b) { for (int i = 0; i < 8; i++) b[i] = (uint8_t)rnd32(); }

static void alpha2(uint8_t *b, uint32_t (*f)(int)) { uint32_t lo = 0, hi = 0; for (int i = 0; i < 8; i++) lo |= f(i) << (4 * i); for (int i = 8; i < 16; i++) hi |= f(i) << (4 * (i - 8)); put32(b, lo); put32(b + 4, hi); }
static uint32_t alpha_k = 0;
static uint32_t af_const(int i) { (void)i; return alpha_k; }
static uint32_t af_rand(int i) { (void)i; return rndn(16); }
static uint32_t af_ramp(int i) { return (uint32_t)((i & 3) * 4 + (i >> 2) * 1 + (alpha_k & 3)) % 16; }
static void colorpart(uint8_t *b, int mode) {   // mode 0 random, 1 low-sum c0 (no swap likely), 2 high-sum c0 (swap)
    uint32_t a = rnd565(), c = rnd565();
    if (mode == 1) { a = rnd565() & 0x7bef; c = rnd565() | 0x8410; }
    if (mode == 2) { a = rnd565() | 0x8410; c = rnd565() & 0x7bef; }
    colblk(b + 8, a, c, rnd32());
}
static void g_bc2_a255(uint8_t *b) { alpha_k = 15; alpha2(b, af_const); colorpart(b, 0); }
static void g_bc2_const(uint8_t *b) { alpha_k = rndn(15); alpha2(b, af_const); colorpart(b, 0); }
static void g_bc2_rand(uint8_t *b) { alpha2(b, af_rand); colorpart(b, 0); }
static void g_bc2_ramp(uint8_t *b) { alpha_k = rndn(4); alpha2(b, af_ramp); colorpart(b, 0); }
static void g_bc2_swap(uint8_t *b) { alpha2(b, af_ramp); colorpart(b, 2); }
static void g_bc2_noswap(uint8_t *b) { alpha2(b, af_ramp); colorpart(b, 1); }
static void bc3a(uint8_t *b, uint32_t a0, uint32_t a1, uint64_t ix) { b[0] = (uint8_t)a0; b[1] = (uint8_t)a1; for (int i = 0; i < 6; i++) b[2 + i] = (uint8_t)(ix >> (8 * i)); }
static uint64_t ix3(int mode) {   // 3-bit indices: 0 random 0..7, 1 random 0..5 (no 0/255 slots), 2 ramp
    uint64_t ix = 0;
    for (int i = 0; i < 16; i++) { uint64_t v = mode == 0 ? rndn(8) : (mode == 1 ? rndn(6) : (uint64_t)(((i & 3) + (i >> 2)) * 7 / 6)); ix |= v << (3 * i); }
    return ix;
}
static void g_bc3_a255(uint8_t *b) { bc3a(b, 255, 255, ix3(1)); colorpart(b, 0); }
static void g_bc3_const(uint8_t *b) { uint32_t a = rndn(255); bc3a(b, a, a, ix3(1)); colorpart(b, 0); }
static void g_bc3_rand8(uint8_t *b) { uint32_t a = rndn(256), c = rndn(256); if (a < c) { uint32_t t = a; a = c; c = t; } if (a == c) a++; bc3a(b, a & 255, c, ix3(0)); colorpart(b, 0); }   // a0 > a1: eight levels
static void g_bc3_rand6(uint8_t *b) { uint32_t a = rndn(256), c = rndn(256); if (a > c) { uint32_t t = a; a = c; c = t; } bc3a(b, a, c, ix3(0)); colorpart(b, 0); }   // a0 <= a1: six levels + 0 and 255
static void g_bc3_ramp(uint8_t *b) { bc3a(b, 255, rndn(100), ix3(2)); colorpart(b, 0); }
static void g_bc3_swap(uint8_t *b) { bc3a(b, 255, rndn(100), ix3(2)); colorpart(b, 2); }
static void g_bc3_noswap(uint8_t *b) { bc3a(b, 255, rndn(100), ix3(2)); colorpart(b, 1); }

static uint32_t af_gentle(int i) { return alpha_k + (uint32_t)((i & 3) + (i >> 2)) * 2 / 6; }   // a ramp of at most two alpha steps (17 levels of 255) across the block
static void g_bc2_gentle(uint8_t *b) { alpha_k = rndn(13); alpha2(b, af_gentle); colorpart(b, 0); }
static void g_bc2_const_swap(uint8_t *b) { alpha_k = rndn(15); alpha2(b, af_const); colorpart(b, 2); }
static void g_bc3_gentle(uint8_t *b) { uint32_t a1 = rndn(200), a0 = a1 + 8 + rndn(25); bc3a(b, a0, a1, ix3(2)); colorpart(b, 0); }
static void g_bc3_gentle_noise(uint8_t *b) { uint32_t a1 = rndn(200), a0 = a1 + 8 + rndn(25); bc3a(b, a0, a1, ix3(0)); colorpart(b, 0); }
static void g_solid1(uint8_t *b) { uint32_t a = rnd565(), c = rnd565(); if (a == c) c ^= 1; if (a < c) { uint32_t t = a; a = c; c = t; } colblk(b, a, c, 0x55555555u * rndn(4)); }
static void g_solid2(uint8_t *b) { alpha_k = rndn(16); alpha2(b, af_const); colorpart(b, 0); put32(b + 12, 0x55555555u * rndn(4)); }
static void g_solid3(uint8_t *b) { uint32_t a = rndn(256); bc3a(b, a, a, 0); colorpart(b, 0); put32(b + 12, 0x55555555u * rndn(4)); }
static const cat_t cats[] = {
    // name                        kind gen             ok             layout swap rgb psnr   a   psnrA
    {"solid blocks, bc1",          K1, g_solid1,       0,             0, 0, 0, 99.0, 0, 99.0},
    {"solid blocks, bc2",          K2, g_solid2,       0,             0, 0, 0, 99.0, 0, 99.0},
    {"solid blocks, bc3",          K3, g_solid3,       0,             0, 0, 0, 99.0, 0, 99.0},
    {"bc1 opaque random",         K1, g_bc1_op,       0,             1, -1, 3, 50.0, 0, 99.0},
    {"bc1 opaque, no swap",       K1, g_bc1_op,       ok_bc1_noswap, 1, 0, 3, 50.0, 0, 99.0},
    {"bc1 opaque, swap forced",   K1, g_bc1_op,       ok_bc1_swap,   1, 1, 3, 50.0, 0, 99.0},
    {"bc1 opaque ramp",           K1, g_bc1_ramp,     0,             1, -1, 3, 50.0, 0, 99.0},
    {"bc1 punch-through",         K1, g_bc1_pt,       0,             3, -1, 2, 49.0, 0, 99.0},
    {"bc1 punch-through, no swap",K1, g_bc1_pt,       ok_pt_noswap,  3, 0, 2, 49.0, 0, 99.0},
    {"bc1 punch-through, swap",   K1, g_bc1_pt,       ok_pt_swap,    3, 1, 2, 49.0, 0, 99.0},
    {"bc1 3-colour, no index 3",  K1, g_bc1_pt_no3,   0,             3, -1, 2, 49.0, 0, 99.0},
    {"bc1 punch, black/white end",K1, g_bc1_pt_edge,  0,             3, -1, 2, 49.0, 0, 99.0},
    {"bc1 c0==c1",                K1, g_bc1_solid,    0,            -1, -1, 2, 49.0, 0, 99.0},
    {"bc1 any 8 bytes",           K1, g_bc1_all,      0,            -1, -1, 3, 50.0, 0, 99.0},
    {"bc2 alpha 15/15",           K2, g_bc2_a255,     0,             1, -1, 3, 50.0, 0, 99.0},
    {"bc2 uniform alpha",         K2, g_bc2_const,    0,             2, -1, 3, 50.0, 0, 99.0},
    {"bc2 random alpha",          K2, g_bc2_rand,     0,             4, -1, 8, 43.0, 60, 21.0},
    {"bc2 alpha ramp",            K2, g_bc2_ramp,     0,             4, -1, 8, 43.0, 40, 21.0},
    {"bc2 gentle alpha ramp",     K2, g_bc2_gentle,   0,             4, -1, 8, 43.0, 12, 38.0},
    {"bc2 uniform alpha, swap",   K2, g_bc2_const_swap, 0,           2, 1, 3, 50.0, 0, 99.0},
    {"bc2 ramp, no swap",         K2, g_bc2_noswap,   0,             4, 0, 8, 43.0, 40, 21.0},
    {"bc2 ramp, swap forced",     K2, g_bc2_swap,     0,             4, 1, 8, 43.0, 40, 21.0},
    {"bc3 alpha 255",             K3, g_bc3_a255,     0,             1, -1, 3, 50.0, 0, 99.0},
    {"bc3 uniform alpha",         K3, g_bc3_const,    0,             2, -1, 3, 50.0, 0, 99.0},
    {"bc3 random, 8 levels",      K3, g_bc3_rand8,    0,            -1, -1, 8, 43.0, 80, 22.0},
    {"bc3 random, 6 levels",      K3, g_bc3_rand6,    0,            -1, -1, 8, 43.0, 80, 22.0},
    {"bc3 alpha ramp",            K3, g_bc3_ramp,     0,             4, -1, 8, 43.0, 40, 22.0},
    {"bc3 gentle ramp (<=32)",    K3, g_bc3_gentle,   0,             4, -1, 8, 43.0, 12, 38.0},
    {"bc3 gentle noise (<=32)",   K3, g_bc3_gentle_noise, 0,         4, -1, 8, 43.0, 12, 38.0},
    {"bc3 ramp, no swap",         K3, g_bc3_noswap,   0,             4, 0, 8, 43.0, 40, 22.0},
    {"bc3 ramp, swap forced",     K3, g_bc3_swap,     0,             4, 1, 8, 43.0, 40, 22.0},
};

// ---------- reference decode (bcdec) ----------
static void ref_decode(int kind, const uint8_t *b, uint8_t px[16][4]) {
    if (kind == K1) bcdec_bc1(b, px, 16); else if (kind == K2) bcdec_bc2(b, px, 16); else bcdec_bc3(b, px, 16);
}

typedef struct { int n, layout[5], swapped; int maxrgb, maxa; double se_rgb, se_a; int nrgb, na; } stat_t;

// Run one category through the transcoder + astcenc; fills st. bc16 holds the BC blocks (16 bytes each, BC1 uses 8).
static int run_cat(const cat_t *c, int n, int srgb, stat_t *st, uint8_t *bcs, uint8_t *astc) {
    memset(st, 0, sizeof *st);
    for (int i = 0; i < n; i++) {
        uint8_t *b = bcs + (size_t)i * 16; int tries = 0;
        do { memset(b, 0, 16); c->gen(b); } while (c->ok && !c->ok(b + (c->kind == K1 ? 0 : 8)) && ++tries < 1000);
        uint32_t w[4], o[4]; memcpy(w, b, 16);
        uint32_t r = bca_transcode(w, (u32)c->kind, o);
        st->layout[r & 0xff]++; if (r & BCA_SWAPPED) st->swapped++;
        memcpy(astc + (size_t)i * 16, o, 16);
    }
    int nbx = 64, nby = (n + nbx - 1) / nbx;
    uint8_t *img = malloc((size_t)nbx * nby * 16 * 16);
    uint8_t *padded = calloc((size_t)nbx * nby, 16); memcpy(padded, astc, (size_t)n * 16);
    for (int i = n; i < nbx * nby; i++) { uint32_t z[4] = {0, 0, 0, 0}, o[4]; (void)z; uint32_t bc[4] = {0, 0, 0, 0}; bca_transcode(bc, 1, o); memcpy(padded + (size_t)i * 16, o, 16); }
    int rc = astc_decode(padded, nbx, nby, srgb, img);
    if (rc) { printf("  astcenc decode failed (%d)\n", rc); free(img); free(padded); return rc; }
    for (int i = 0; i < n; i++) {
        uint8_t ref[16][4]; ref_decode(c->kind, bcs + (size_t)i * 16, ref);
        int bx = i % nbx, by = i / nbx;
        for (int t = 0; t < 16; t++) {
            const uint8_t *p = img + (((size_t)(by * 4 + t / 4) * nbx * 4) + bx * 4 + (t & 3)) * 4;
            int skip = c->kind == K1 && ref[t][3] == 0;   // BC1 transparent texel: its RGB is a don't-care (the transcoder keeps the colour of the endpoint nearest black); alpha is still compared
            for (int k = 0; k < 4; k++) {
                if (skip && k < 3) continue;
                int d = abs((int)p[k] - (int)ref[t][k]);
                if (k < 3) { if (d > st->maxrgb) st->maxrgb = d; st->se_rgb += (double)d * d; st->nrgb++; }
                else { if (d > st->maxa) st->maxa = d; st->se_a += (double)d * d; st->na++; }
            }
        }
    }
    st->n = n; free(img); free(padded); return 0;
}
static double psnr(double se, int n) { if (se == 0) return 99.0; return 10.0 * log10(255.0 * 255.0 / (se / n)); }

// ---------- checks that do not need astcenc ----------
static void check_trit_table(void) {   // the encode table inverts the spec's decode (C.2.12)
    for (int T = 0; T < 256; T++) {
        int b[8], t[5], C, t4, t3, t2, t1, t0; for (int i = 0; i < 8; i++) b[i] = (T >> i) & 1;
        if (((T >> 2) & 7) == 7) { C = (((T >> 5) & 7) << 2) | (T & 3); t4 = 2; t3 = 2; }
        else { C = T & 31; if (((T >> 5) & 3) == 3) { t4 = 2; t3 = b[7]; } else { t4 = b[7]; t3 = (T >> 5) & 3; } }
        if ((C & 3) == 3) { t2 = 2; t1 = (C >> 4) & 1; t0 = (((C >> 3) & 1) << 1) | (((C >> 2) & 1) & ~((C >> 3) & 1) & 1); }
        else if (((C >> 2) & 3) == 3) { t2 = 2; t1 = 2; t0 = C & 3; }
        else { t2 = (C >> 4) & 1; t1 = (C >> 2) & 3; t0 = (((C >> 1) & 1) << 1) | ((C & 1) & ~((C >> 1) & 1) & 1); }
        t[0] = t0; t[1] = t1; t[2] = t2; t[3] = t3; t[4] = t4;
        int k = t0 + 3 * t1 + 9 * t2 + 27 * t3 + 81 * t4;
        if (k < 243) { int e = bca_trit_enc[k]; int u[5], d = 0; (void)u; (void)d; CHECK(e < 256, "trit table entry %d", k); }
    }
    for (int k = 0; k < 243; k++) {   // decode of the stored T gives back the trits k stands for
        int T = bca_trit_enc[k], b[8], C, t4, t3, t2, t1, t0; for (int i = 0; i < 8; i++) b[i] = (T >> i) & 1;
        if (((T >> 2) & 7) == 7) { C = (((T >> 5) & 7) << 2) | (T & 3); t4 = 2; t3 = 2; }
        else { C = T & 31; if (((T >> 5) & 3) == 3) { t4 = 2; t3 = b[7]; } else { t4 = b[7]; t3 = (T >> 5) & 3; } }
        if ((C & 3) == 3) { t2 = 2; t1 = (C >> 4) & 1; t0 = (((C >> 3) & 1) << 1) | (((C >> 2) & 1) & ~((C >> 3) & 1) & 1); }
        else if (((C >> 2) & 3) == 3) { t2 = 2; t1 = 2; t0 = C & 3; }
        else { t2 = (C >> 4) & 1; t1 = (C >> 2) & 3; t0 = (((C >> 1) & 1) << 1) | ((C & 1) & ~((C >> 1) & 1) & 1); }
        CHECK(t0 + 3 * t1 + 9 * t2 + 27 * t3 + 81 * t4 == k, "trit table: entry %d decodes to a different trit vector", k);
    }
}
static void check_col_vs_bcdec(void) {   // the transcoder's colour decode is bcdec's, bit for bit
    int bad = 0;
    for (int i = 0; i < 20000; i++) {
        uint8_t b[8]; for (int k = 0; k < 8; k++) b[k] = (uint8_t)rnd32();
        uint8_t px[16][4]; bcdec_bc1(b, px, 16);
        uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8, ix = get32(b + 4); int ref[16]; bca_col(c0, c1, c0 <= c1, ref);
        for (int t = 0; t < 16; t++) { int k = (ix >> (2 * t)) & 3; for (int c = 0; c < 4; c++) if (ref[k * 4 + c] != px[t][c]) bad++; }
    }
    CHECK(bad == 0, "bca_col differs from bcdec in %d values", bad);
}

int main(int argc, char **argv) {
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    astcenc = getenv("ASTCENC") ? getenv("ASTCENC") : "astcenc";
    const char *td = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    snprintf(tmpdir, sizeof tmpdir, "%s/t_bc2astc.XXXXXX", td); if (!mkdtemp(tmpdir)) { printf("mkdtemp failed\n"); return 1; }

    printf("[table / decode]\n");
    check_trit_table(); check_col_vs_bcdec();

    // public interface: format map, level helper against the block helper, zero block
    {
        uint32_t pf; int k; int ok = 1;
        for (uint32_t f = 0x40; f <= 0x4a; f++) {
            int r = nvn_bc_astc_fmt(f, &pf, &k), want_k = (f == 0x41 || f == 0x42 || f == 0x45 || f == 0x46) ? 1 : ((f == 0x43 || f == 0x47) ? 2 : 3);
            if (f < 0x41 || f > 0x48) ok &= !r; else ok &= r && k == want_k && pf == (f >= 0x45 ? 186u : 204u);
        }
        CHECK(ok, "nvn_bc_astc_fmt maps formats wrongly");
        uint8_t src[4 * 3 * 16], dst[4 * 3 * 16], ref[16]; for (size_t i = 0; i < sizeof src; i++) src[i] = (uint8_t)rnd32();
        CHECK(nvn_bc2astc(0x44, src, 3 * 16, 10, 7, dst, 3 * 16) == 0, "nvn_bc2astc failed");
        int same = 1; for (int by = 0; by < 2; by++) for (int bx = 0; bx < 3; bx++) { nvn_bc2astc_block(3, src + by * 48 + bx * 16, ref); same &= !memcmp(ref, dst + by * 48 + bx * 16, 16); }
        CHECK(same, "nvn_bc2astc differs from nvn_bc2astc_block"); CHECK(nvn_bc2astc(0x49, src, 24, 8, 8, dst, 32) == -1, "BC4 must be refused");
        uint8_t z[16], img[16 * 4]; nvn_astc_zero_block(z);
        int rc = astc_decode(z, 1, 1, 0, img); if (rc) { printf("astcenc decode failed (%d) with %s (set ASTCENC)\n", rc, astcenc); return 1; }
        int zero = 1; for (int i = 0; i < 64; i++) zero &= img[i] == 0;
        CHECK(zero, "the zero block does not decode to (0,0,0,0)");
    }

    printf("\n%-30s %6s  %-28s %5s | %5s %5s %7s | %5s %5s %7s\n", "category (linear)", "blocks", "layouts L0 L1a L1b L2 L3", "swap", "maxRGB", "", "PSNR", "maxA", "", "PSNR");
    const int N = 4096; uint8_t *bcs = malloc((size_t)N * 16), *astc = malloc((size_t)N * 16);
    for (int srgb = 0; srgb < 2; srgb++) {
        if (srgb) printf("\n[sRGB: astcenc -ds against the same bcdec bytes]\n");
        for (size_t ci = 0; ci < sizeof cats / sizeof *cats; ci++) {
            if (srgb && (ci % 3)) continue;   // a third of the categories are enough for sRGB; the transcoder does not branch on it
            stat_t st; if (run_cat(&cats[ci], N, srgb, &st, bcs, astc)) { CHECK(0, "%s: astcenc failed", cats[ci].name); continue; }
            { const cat_t *c = &cats[ci];
              // random colour endpoints can by chance give a solid block (L0); every other block must use the expected layout
              if (c->layout >= 0) CHECK(st.layout[c->layout] + (c->layout ? st.layout[0] : 0) == st.n && (c->layout == 0 || st.layout[0] <= st.n / 100), "%s: layouts are not all %d", c->name, c->layout);
              if (c->swap == 0) CHECK(st.swapped == 0, "%s: %d blocks swapped, none expected", c->name, st.swapped);
              if (c->swap == 1) CHECK(st.swapped == st.n, "%s: only %d of %d blocks swapped", c->name, st.swapped, st.n);
              CHECK(st.maxrgb <= c->max_rgb, "%s: RGB max error %d > %d", c->name, st.maxrgb, c->max_rgb);
              CHECK(psnr(st.se_rgb, st.nrgb) >= c->psnr_rgb, "%s: RGB PSNR %.2f < %.1f", c->name, psnr(st.se_rgb, st.nrgb), c->psnr_rgb);
              CHECK(st.maxa <= c->max_a, "%s: alpha max error %d > %d", c->name, st.maxa, c->max_a);
              CHECK(psnr(st.se_a, st.na) >= c->psnr_a, "%s: alpha PSNR %.2f < %.1f", c->name, psnr(st.se_a, st.na), c->psnr_a); }
            printf("%-30s %6d  %5d %4d %4d %4d %4d %5d | %5d %5s %7.2f | %5d %5s %7.2f\n", cats[ci].name, st.n, st.layout[0], st.layout[1], st.layout[2], st.layout[3], st.layout[4], st.swapped,
                   st.maxrgb, "", psnr(st.se_rgb, st.nrgb), st.maxa, "", psnr(st.se_a, st.na));
        }
    }
    free(bcs); free(astc);
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf \"%s\"", tmpdir); if (system(cmd)) {}
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
