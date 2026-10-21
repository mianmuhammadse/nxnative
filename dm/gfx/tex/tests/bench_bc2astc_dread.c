// Benchmark of the BC1/BC2/BC3 → ASTC 4x4 transcoder (bc2astc_core.h) on a real game: every BC1/BC3 block of Metroid Dread's textures goes through the C transcoder, is decoded with ARM's astcenc and
// compared with bcdec. Reports the share of blocks per layout and format, bytes per format (native BC / decompressed to RGBA8 / ASTC), RGB and alpha error overall, per texture (median, percentiles, worst 1%).
// The .bctex parsing is Dread-specific (MercurySteam's container: "MTXT" + gzip; inside, an NVN texture header with the format at +0xc8, the data size at +0xac, the block height at +0x118 and the block-linear
// data at +0x154); any other game needs its own reader. No game data lives in the repository: the extracted RomFS is an argument.
// Usage: bench_bc2astc_dread <RomFS folder> <astcenc binary> <output folder>   (writes report.txt and textures.csv there; the folder also holds the scratch files of the astcenc calls)
//   make -C dm/gfx/tex/tests bench ROMFS=<RomFS folder> ASTCENC=<astcenc> OUT=<folder>
#define _DARWIN_C_SOURCE
#define BCDEC_IMPLEMENTATION
#include "../../../third_party/bcdec/bcdec.h"
#include "../nvn_deswizzle.h"
#include "../nvn_bc2astc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *astcenc, *tmpdir;
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static uint8_t *inflate_file(const char *path, size_t *outn) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *in = malloc(n); if (fread(in, 1, n, f) != (size_t)n) { fclose(f); free(in); return 0; } fclose(f);
    if (n < 12 || memcmp(in, "MTXT", 4)) { free(in); return 0; }
    z_stream z; memset(&z, 0, sizeof z); if (inflateInit2(&z, 31) != Z_OK) { free(in); return 0; }
    size_t cap = n * 8 + 4096, len = 0; uint8_t *out = malloc(cap);
    z.next_in = in + 8; z.avail_in = (uInt)(n - 8);
    for (;;) {
        if (len == cap) { cap *= 2; out = realloc(out, cap); }
        z.next_out = out + len; z.avail_out = (uInt)(cap - len);
        int r = inflate(&z, Z_NO_FLUSH); len = cap - z.avail_out;
        if (r == Z_STREAM_END) break; if (r != Z_OK && r != Z_BUF_ERROR) { free(out); out = 0; break; }
        if (r == Z_BUF_ERROR && z.avail_out) { free(out); out = 0; break; }
    }
    inflateEnd(&z); free(in); *outn = len; return out;
}

// ASTC blocks → RGBA8 through astcenc (TGA output)
static int astc_decode(const uint8_t *blocks, int nbx, int nby, uint8_t *rgba) {
    char in[512], out[512], cmd[1400]; snprintf(in, sizeof in, "%s/in.astc", tmpdir); snprintf(out, sizeof out, "%s/out.tga", tmpdir);
    FILE *f = fopen(in, "wb"); int w = nbx * 4, h = nby * 4;
    uint8_t hd[16] = {0x13, 0xAB, 0xA1, 0x5C, 4, 4, 1, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)h, (uint8_t)(h >> 8), (uint8_t)(h >> 16), 1, 0, 0};
    fwrite(hd, 1, 16, f); fwrite(blocks, 16, (size_t)nbx * nby, f); fclose(f);
    snprintf(cmd, sizeof cmd, "\"%s\" -ds \"%s\" \"%s\" >/dev/null 2>&1", astcenc, in, out);   // sRGB or linear is irrelevant for the error of the stored bytes; -ds compares gamma-encoded bytes like bcdec's
    if (system(cmd)) return -1;
    f = fopen(out, "rb"); if (!f) return -2; fseek(f, 0, SEEK_END); long fs = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(fs); if (fread(d, 1, fs, f) != (size_t)fs) { fclose(f); free(d); return -3; } fclose(f);
    int type = d[2], top = (d[17] >> 5) & 1; if ((type != 2 && type != 10) || d[16] != 32) { free(d); return -4; }
    const uint8_t *p = d + 18 + d[0]; size_t npx = (size_t)w * h, i = 0; uint8_t *lin = malloc(npx * 4);
    while (i < npx) {
        if (type == 2) { memcpy(lin + i * 4, p, 4); p += 4; i++; continue; }
        int hh = *p++, cnt = (hh & 127) + 1;
        for (int k = 0; k < cnt && i < npx; k++, i++) { memcpy(lin + i * 4, p, 4); if (!(hh & 128)) p += 4; }
        if (hh & 128) p += 4;
    }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { const uint8_t *s = lin + ((size_t)(top ? y : h - 1 - y) * w + x) * 4; uint8_t *o = rgba + ((size_t)y * w + x) * 4; o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = s[3]; }
    free(lin); free(d); return 0;
}

typedef struct { char name[160]; int fmt; long blocks, lay[5], l3_swapped; double se_a, se_rgb, se_a_l3; long na, nrgb, na_l3; int maxa, maxrgb, maxa_l3; double psnr_a, psnr_a_l3; } tex_t;
typedef struct { long files, blocks, lay[5]; double bc_bytes, unp_bytes, astc_bytes; } fmt_t;
static double psnr(double se, long n) { return se == 0 ? 99.0 : 10.0 * log10(255.0 * 255.0 / (se / n)); }
static int cmp_psnr(const void *a, const void *b) { double x = ((const tex_t *)a)->psnr_a, y = ((const tex_t *)b)->psnr_a; return x < y ? -1 : x > y; }

static char **files; static size_t nfiles, capfiles;
static void walk(const char *dir) {   // every *.bctex below dir, sorted for reproducible reports
    DIR *d = opendir(dir); if (!d) return; struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st; if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) walk(p);
        else { size_t n = strlen(p); if (n > 6 && !strcmp(p + n - 6, ".bctex")) { if (nfiles == capfiles) { capfiles = capfiles ? capfiles * 2 : 1024; files = realloc(files, capfiles * sizeof *files); } files[nfiles++] = strdup(p); } }
    }
    closedir(d);
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <RomFS folder> <astcenc binary> <output folder>\n", argv[0]); return 2; }
    astcenc = argv[2]; tmpdir = argv[3];
    walk(argv[1]); qsort(files, nfiles, sizeof *files, cmp_str);
    if (!nfiles) { fprintf(stderr, "no .bctex files below %s\n", argv[1]); return 2; }
    char rep[1100], csv[1100]; snprintf(rep, sizeof rep, "%s/report.txt", tmpdir); snprintf(csv, sizeof csv, "%s/textures.csv", tmpdir);
    if (!freopen(rep, "w", stdout)) { fprintf(stderr, "cannot write %s\n", rep); return 2; }
    char path[1024]; size_t fi = 0;
    static fmt_t F[256]; tex_t *T = calloc(8000, sizeof(tex_t)); int nt = 0;
    long parsed = 0, unparsed = 0, size_ok = 0, size_auto = 0, size_bad = 0;
    double total_other = 0;
    while (fi < nfiles) {
        snprintf(path, sizeof path, "%s", files[fi++]);
        size_t n; uint8_t *r = inflate_file(path, &n);
        if (!r || n < 0x160 || rd32(r + 0x88) != 0x4e764248) { unparsed++; free(r); continue; }
        uint32_t W = rd32(r + 8), H = rd32(r + 12), mips = rd32(r + 16), dsz = rd32(r + 0xac), fmt = rd32(r + 0xc8), blk = rd32(r + 0x118);
        if (0x154 + (size_t)dsz > n || fmt > 255 || !mips || mips > 16) { unparsed++; free(r); continue; }
        parsed++;
        int kind = 0; uint32_t pf; if (nvn_bc_astc_fmt(fmt, &pf, &kind)) {} 
        int bb = 0, bw = 0, bh = 0; bb = nvn_fmt_bpb((int)fmt, &bw, &bh);
        fmt_t *e = &F[fmt]; e->files++;
        // bytes: on the Switch (and with native BC), after the current no-BC decompression, with ASTC for BC1-3
        double bcb = dsz, unpf = 1, astcf = 1;
        if (fmt >= 0x41 && fmt <= 0x4e && bb) { double bpt = bb / 16.0; (void)bpt; }
        switch (fmt) { case 0x41: case 0x42: case 0x45: case 0x46: unpf = 8; astcf = 2; break; case 0x43: case 0x44: case 0x47: case 0x48: unpf = 4; astcf = 1; break;
                       case 0x49: case 0x4a: unpf = 2; astcf = 2; break; case 0x4b: case 0x4c: unpf = 2; astcf = 2; break; case 0x4d: case 0x4e: unpf = 4; astcf = 4; break; case 0x4f: case 0x50: unpf = 8; astcf = 8; break; default: unpf = 1; astcf = 1; }
        // BC4 (8 bytes → R8 = 8 per 16 texels... factor 2), BC5 (16 → RG8 32: 2); BC7 4; BC6H 8
        e->bc_bytes += bcb; e->unp_bytes += bcb * unpf; e->astc_bytes += bcb * astcf;
        if (!(fmt == 0x42 || fmt == 0x44 || fmt == 0x41 || fmt == 0x43 || fmt == 0x45 || fmt == 0x46 || fmt == 0x47 || fmt == 0x48)) { if (!bb) total_other += dsz; free(r); continue; }
        // deswizzle every level, collect blocks
        nvn_texdesc d = {1, (int)fmt, 0, 0, W, H, 1, mips, 0, 0};
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        d.pkg_block_h = (int)blk + 1; nvn_texlayout L2; nvn_tex_layout(&d, &L2);
        if (L2.size == dsz) size_ok++; else { d.pkg_block_h = 0; if (L.size == dsz) size_auto++; else { size_bad++; } }
        if (L2.size != dsz && L.size != dsz) { free(r); continue; }
        long cap = 0; for (unsigned l = 0; l < mips; l++) cap += ((W >> l ? W >> l : 1) + 3) / 4 * (((H >> l ? H >> l : 1) + 3) / 4);
        uint8_t *bcs = malloc((size_t)cap * 16), *astc = malloc((size_t)cap * 16), *lays = malloc((size_t)cap); long nb = 0;
        for (unsigned l = 0; l < mips; l++) {
            uint32_t rb, rows, slices, bpb; uint64_t base; unsigned th, td;
            if (nvn_deswizzle_geom(&d, l, 0, &rb, &rows, &slices, &base, &th, &td, &bpb)) continue;
            uint8_t *lin = malloc((size_t)rb * rows);
            if (nvn_deswizzle(&d, l, 0, r + 0x154, lin, rb)) { free(lin); continue; }
            for (uint32_t i = 0; i < rows * (rb / bpb); i++) memcpy(bcs + (size_t)(nb + i) * 16, lin + (size_t)i * bpb, bpb);
            nb += rows * (rb / bpb); free(lin);
        }
        tex_t *t = &T[nt++]; const char *sl = strrchr(path, '/'); snprintf(t->name, sizeof t->name, "%s", sl ? sl + 1 : path); t->fmt = (int)fmt; t->blocks = nb;
        for (long i = 0; i < nb; i++) { uint32_t o = nvn_bc2astc_block(kind, bcs + (size_t)i * 16, astc + (size_t)i * 16); lays[i] = (uint8_t)(o & 0xff); t->lay[o & 0xff]++; if ((o & 0x100) && (o & 0xff) == 4) t->l3_swapped++; e->lay[o & 0xff]++; }
        e->blocks += nb;
        int nbx = nb < 64 ? (int)nb : 64, nby = (int)((nb + nbx - 1) / nbx);
        uint8_t *pad = calloc((size_t)nbx * nby, 16); memcpy(pad, astc, (size_t)nb * 16); for (long i = nb; i < (long)nbx * nby; i++) nvn_astc_zero_block(pad + i * 16);
        uint8_t *img = malloc((size_t)nbx * nby * 16 * 16);
        if (astc_decode(pad, nbx, nby, img)) { printf("astcenc failed on %s\n", path); return 1; }
        for (long i = 0; i < nb; i++) {
            uint8_t ref[16][4]; if (kind == 1) bcdec_bc1(bcs + i * 16, ref, 16); else if (kind == 2) bcdec_bc2(bcs + i * 16, ref, 16); else bcdec_bc3(bcs + i * 16, ref, 16);
            uint32_t lay = lays[i];
            int bx = (int)(i % nbx), by = (int)(i / nbx);
            for (int p = 0; p < 16; p++) {
                const uint8_t *g = img + (((size_t)(by * 4 + p / 4) * nbx * 4) + bx * 4 + (p & 3)) * 4;
                int skip = kind == 1 && ref[p][3] == 0;
                for (int k = 0; k < 4; k++) {
                    int df = abs((int)g[k] - (int)ref[p][k]);
                    if (k < 3) { if (skip) continue; t->se_rgb += (double)df * df; t->nrgb++; if (df > t->maxrgb) t->maxrgb = df; }
                    else { t->se_a += (double)df * df; t->na++; if (df > t->maxa) t->maxa = df; if (lay == 4) { t->se_a_l3 += (double)df * df; t->na_l3++; if (df > t->maxa_l3) t->maxa_l3 = df; } }
                }
            }
        }
        t->psnr_a = psnr(t->se_a, t->na); t->psnr_a_l3 = t->na_l3 ? psnr(t->se_a_l3, t->na_l3) : 99.0;
        free(bcs); free(astc); free(lays); free(pad); free(img); free(r);
    }
    printf("files parsed %ld, unparsed %ld; layout size check: block height from header %ld, automatic %ld, mismatch %ld\n\n", parsed, unparsed, size_ok, size_auto, size_bad);
    printf("%-8s %6s %12s  %-44s | %10s %10s %10s\n", "format", "files", "blocks", "layout share % L0 / L1a / L1b / L2 / L3", "BC MB", "no-BC MB", "ASTC MB");
    const char *nm[256] = {0}; nm[0x41] = "BC1 RGB"; nm[0x42] = "BC1"; nm[0x43] = "BC2"; nm[0x44] = "BC3"; nm[0x4b] = "BC5"; nm[0x50] = "BC6H"; nm[0x4d] = "BC7"; nm[0x25] = "RGBA8"; nm[0x6d] = "fmt 0x6d"; nm[0x01] = "R8";
    double tb = 0, tu = 0, ta = 0, tub1 = 0;
    for (int f = 0; f < 256; f++) if (F[f].files) {
        fmt_t *e = &F[f]; char lab[24]; snprintf(lab, sizeof lab, "%s (0x%02x)", nm[f] ? nm[f] : "?", f);
        printf("%-8s %6ld %12ld  ", lab, e->files, e->blocks);
        if (e->blocks) printf("%5.1f /%5.1f /%5.1f /%5.1f /%5.1f             ", 100.0 * e->lay[0] / e->blocks, 100.0 * e->lay[1] / e->blocks, 100.0 * e->lay[2] / e->blocks, 100.0 * e->lay[3] / e->blocks, 100.0 * e->lay[4] / e->blocks); else printf("%-44s", "-");
        printf("| %10.1f %10.1f %10.1f\n", e->bc_bytes / 1e6, e->unp_bytes / 1e6, e->astc_bytes / 1e6);
        tb += e->bc_bytes; tu += e->unp_bytes; ta += e->astc_bytes;
        int is1 = f == 0x41 || f == 0x42 || f == 0x45 || f == 0x46; tub1 += is1 ? e->astc_bytes : e->unp_bytes;
    }
    printf("%-8s %6s %12s  %-44s | %10.1f %10.1f %10.1f   (bc1-only ASTC: %.1f)\n", "total", "", "", "", tb / 1e6, tu / 1e6, ta / 1e6, tub1 / 1e6);
    // alpha error on real blocks
    for (int kind = 2; kind <= 3; kind++) {
        int want = kind == 2 ? 0x43 : 0x44;
        double se = 0, sel3 = 0; long n = 0, nl3 = 0; int mx = 0, mxl3 = 0, cnt = 0; long l3b = 0, allb = 0;
        for (int i = 0; i < nt; i++) if (T[i].fmt == want || (kind == 3 && 0)) { se += T[i].se_a; n += T[i].na; sel3 += T[i].se_a_l3; nl3 += T[i].na_l3; if (T[i].maxa > mx) mx = T[i].maxa; if (T[i].maxa_l3 > mxl3) mxl3 = T[i].maxa_l3; cnt++; l3b += T[i].lay[4]; allb += T[i].blocks; }
        if (!cnt) { printf("\nBC%d: no textures\n", kind); continue; }
        printf("\nBC%d alpha on real blocks: %d textures, %ld blocks (%ld in L3 = %.1f%%): all blocks PSNR %.2f dB, max err %d; L3 blocks only PSNR %.2f dB, max err %d\n", kind, cnt, allb, l3b, 100.0 * l3b / allb, psnr(se, n), mx, nl3 ? psnr(sel3, nl3) : 99.0, mxl3);
    }
    { FILE *cf = fopen(csv, "w"); fprintf(cf, "name,fmt,blocks,l3_blocks,alpha_psnr,alpha_max,rgb_psnr,rgb_max\n"); for (int i = 0; i < nt; i++) fprintf(cf, "%s,0x%02x,%ld,%ld,%.3f,%d,%.3f,%d\n", T[i].name, T[i].fmt, T[i].blocks, T[i].lay[4], T[i].psnr_a, T[i].maxa, psnr(T[i].se_rgb, T[i].nrgb), T[i].maxrgb); fclose(cf); }
    // worst 1% of BC3 textures by alpha PSNR
    tex_t *b3 = malloc(nt * sizeof *b3); int n3 = 0; for (int i = 0; i < nt; i++) if (T[i].fmt == 0x44) b3[n3++] = T[i];
    qsort(b3, n3, sizeof *b3, cmp_psnr);
    int worst = (n3 + 99) / 100; printf("\nworst 1%% of the %d BC3 textures by alpha PSNR (%d textures):\n", n3, worst);
    printf("  %-44s %8s %9s %8s %8s %9s\n", "texture", "blocks", "L3 share", "alphaPSNR", "maxA", "rgbPSNR");
    for (int i = 0; i < worst; i++) printf("  %-44s %8ld %8.1f%% %9.2f %8d %9.2f\n", b3[i].name, b3[i].blocks, 100.0 * b3[i].lay[4] / b3[i].blocks, b3[i].psnr_a, b3[i].maxa, psnr(b3[i].se_rgb, b3[i].nrgb));
    double pcts[] = {0.01, 0.05, 0.10, 0.50}; printf("\nBC3 per-texture alpha PSNR percentiles (low end): ");
    for (int k = 0; k < 4; k++) printf(" p%.0f=%.1f", pcts[k] * 100, b3[(int)(pcts[k] * n3)].psnr_a); printf("\n");
    int exact = 0; for (int i = 0; i < n3; i++) if (b3[i].maxa == 0) exact++; printf("BC3 textures with exact alpha: %d of %d\n", exact, n3);
    // RGB overall
    for (int pass = 0; pass < 2; pass++) { int want = pass ? 0x44 : 0x42; double se = 0; long n = 0; int mx = 0; for (int i = 0; i < nt; i++) if (T[i].fmt == want) { se += T[i].se_rgb; n += T[i].nrgb; if (T[i].maxrgb > mx) mx = T[i].maxrgb; } if (n) printf("%s RGB on real blocks: PSNR %.2f dB, max err %d\n", pass ? "BC3" : "BC1", psnr(se, n), mx); }
    return 0;
}
