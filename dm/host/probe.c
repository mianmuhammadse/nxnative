// Probes DM_PROBE: when the debugger needs a system authorization prompt on the Mac, use this to print registers and a backtrace at a guest address.
#include "dm.h"
#include <TargetConditionals.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#if !TARGET_OS_IPHONE
#include <mach/mach_vm.h>   // the write watch (DM_PROBE_WATCH): iOS's SDK has no mach_vm.h
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
void crash_set_trap_hook(int (*fn)(ucontext_t *uc));
#if !TARGET_OS_IPHONE
// Syntax module:offset[@x0 offset]. Put a brk at the guest address; on hit print x0..x3 (and the readable strings they point to) and a backtrace, then execute the original instruction from a trampoline slot and jump back.
// The trampoline borrows x16: place probes only at function entries or at points where x16 is dead; the original instruction must not be PC-relative (adr/adrp/b/bl/ldr literal/cbz...).
#define MAX_PROBE 16
typedef void (*probe_fn)(ucontext_t *uc);
static struct { uint64_t at; uint32_t *slot; unsigned hits; char name[40]; long dump, dpost; probe_fn fn; int freg; long foff; uint64_t fval; int hreg; } g_probe[MAX_PROBE];   // dpost: @*xN+off+dpost

// !xN: treat xN as a sead::ExpHeap and print its free and used blocks (block size at block +0x18, alignment pad at +0x12). Settings (docs/agents/05-debugging.md):
// DM_PROBE_HEAP=<name>,<start>,<size>,<free list>,<used list> layout by sead version (hex; node offset at list +0x14), default SMBW 0,28,30,b0,c8 (no name), BotW 28,38,40,e0,f8;
// DM_PROBE_HEAPNAME=<text> only heaps whose name contains it; DM_PROBE_HEAPWORDS=1 each used block's first two words; DM_PROBE_HEAPFRAME=<vtable offset in main> heaps of that class
// are frame heaps (head +0xe0, tail +0xe8); DM_PROBE_HEAPRAW=1 the heap object's first 0x130 bytes
static struct { int init, words, raw; long lay[5], frame_vt; const char *want; } g_hw = { .lay = { 0, 0x28, 0x30, 0xb0, 0xc8 } };
static void probe_heapcfg(void) {
    if (g_hw.init) return; g_hw.init = 1;
    const char *e = getenv("DM_PROBE_HEAP"); if (e) sscanf(e, "%lx,%lx,%lx,%lx,%lx", &g_hw.lay[0], &g_hw.lay[1], &g_hw.lay[2], &g_hw.lay[3], &g_hw.lay[4]);
    e = getenv("DM_PROBE_HEAPFRAME"); g_hw.frame_vt = e ? strtol(e, 0, 16) : -1;
    g_hw.want = getenv("DM_PROBE_HEAPNAME"); g_hw.words = getenv("DM_PROBE_HEAPWORDS") != NULL; g_hw.raw = getenv("DM_PROBE_HEAPRAW") != NULL;
}
static int rd64(uint64_t a, uint64_t *v) { vm_size_t g = 0; return !vm_read_overwrite(mach_task_self(), a, 8, (vm_address_t)v, &g) && g == 8; }
static void probe_word(uint64_t v) {   // A word as module+offset when it points into a module (a vtable names the object)
    module_t *m = v > 0x10000 ? module_of((void *)v) : NULL;
    if (m) fprintf(stderr, "%s+%llx", m->name, (unsigned long long)(v - (uint64_t)m->base)); else fprintf(stderr, "%llx", (unsigned long long)v);
}
static void probe_heapname(uint64_t h, char *nm, size_t n) {   // sead heaps are named (resource heaps after their file)
    uint64_t np = 0; nm[0] = 0;
    if (g_hw.lay[0] && rd64(h + g_hw.lay[0], &np) && np) { vm_size_t g = 0; vm_read_overwrite(mach_task_self(), np, n - 1, (vm_address_t)nm, &g); nm[g < n ? g : n - 1] = 0; for (int k = 0; nm[k]; k++) if (nm[k] < 0x20 || nm[k] > 0x7e) { nm[k] = 0; break; } }
}
static void probe_heapwalk(uint64_t h) {
    char nm[128]; probe_heapname(h, nm, sizeof nm);
    uint64_t start = 0, size = 0, vt = 0; rd64(h + g_hw.lay[1], &start); rd64(h + g_hw.lay[2], &size); rd64(h, &vt);
    module_t *vm = vt ? module_of((void *)vt) : NULL;
    if (g_hw.frame_vt > 0 && vm && !strcmp(vm->name, "main") && vt - (uint64_t)vm->base == (uint64_t)g_hw.frame_vt) {
        uint64_t hd = 0, tl = 0; rd64(h + 0xe0, &hd); rd64(h + 0xe8, &tl);
        fprintf(stderr, "[dm]   FrameHeap %#llx \"%s\" size %#llx head +%#llx tail +%#llx free %#llx\n", (unsigned long long)h, nm, (unsigned long long)size, (unsigned long long)(hd - start), (unsigned long long)(tl - start), (unsigned long long)(tl - hd));
        return;
    }
    fprintf(stderr, "[dm]   ExpHeap %#llx \"%s\" start %#llx size %#llx\n", (unsigned long long)h, nm, (unsigned long long)start, (unsigned long long)size);
    if (g_hw.raw) { fprintf(stderr, "[dm]   heap words:"); for (int k = 0; k < 0x130; k += 8) { uint64_t v = 0; rd64(h + k, &v); fprintf(stderr, "%s+%x=", k % 64 ? " " : "\n[dm]     ", k); probe_word(v); } fputc('\n', stderr); }
    static const char *L[2] = { "free", "used" };
    for (int k = 0; k < 2; k++) {
        uint64_t list = h + g_hw.lay[3 + k], offw = 0, node = 0, sum = 0, pads = 0; int n = 0; rd64(list + 0x14, &offw); int32_t off = (int32_t)offw;
        rd64(list + 8, &node);
        fprintf(stderr, "[dm]   %s:", L[k]);
        while (node && node != list && n < 4000) {
            uint64_t blk = node - off, bsz = 0, w2 = 0; rd64(blk + 0x18, &bsz); rd64(blk + 0x10, &w2);
            unsigned pad = (unsigned)(w2 >> 16) & 0xffff;
            if (g_hw.words && k) {
                uint64_t a = 0, b = 0; rd64(blk + 0x20, &a); rd64(blk + 0x28, &b);
                fprintf(stderr, "\n[dm]     %llx+%llx pad %x: ", (unsigned long long)(blk - start), (unsigned long long)bsz, pad); probe_word(a); fputc(' ', stderr); probe_word(b);
            } else {
                fprintf(stderr, "%s%llx+%llx", n % 8 ? " " : "\n[dm]     ", (unsigned long long)(blk - start), (unsigned long long)bsz);
                if (pad) fprintf(stderr, "p%x", pad);
            }
            sum += bsz; pads += pad; n++;
            if (!rd64(node + 8, &node)) break;
        }
        fprintf(stderr, "\n[dm]   %s %d blocks %#llx bytes, alignment pads %#llx\n", L[k], n, (unsigned long long)sum, (unsigned long long)pads);
    }
}   // dump: on hit print 16 bytes at [x0+dump] (-1 = do not print); fn: dedicated handler (replaces the default print)
static int g_nprobe;
static uint64_t g_probe_frame_first, g_probe_frame_last = UINT64_MAX;
static unsigned g_probe_limit;
uint64_t dm_guest_frame(void);
static void probe_str(uint64_t p) {
    char buf[80]; vm_size_t got = 0;
    if (p < 0x1000 || vm_read_overwrite(mach_task_self(), p, sizeof buf - 1, (vm_address_t)buf, &got) || !got) return;
    buf[got] = 0; int n = 0; while (buf[n] >= 0x20 && buf[n] < 0x7f) n++;
    if (n >= 3 && (buf[n] == 0 || n == (int)got)) { fprintf(stderr, " \"%.*s\"", n, buf); return; }
    // Game string object {char* pointer, u64 length, u64 capacity|top bit} (the path argument of 0xdf67f0 FJSONUtils has exactly this layout)
    if (got >= 24) {
        uint64_t ptr, len; memcpy(&ptr, buf, 8); memcpy(&len, buf + 8, 8);
        char t[96]; got = 0;
        if (len >= 3 && len < sizeof t && !vm_read_overwrite(mach_task_self(), ptr, len, (vm_address_t)t, &got) && got == len) {
            int k = 0; while (k < (int)len && t[k] >= 0x20 && t[k] < 0x7f) k++;
            if (k == (int)len) fprintf(stderr, " S\"%.*s\"", (int)len, t);
        }
    }
}
// DM_PROBE_DUMP=module:offset:length[:file]: on the first hit of any probe, write that range of guest memory to a file (default run/platform/probe_dump.bin)
static void probe_dump_once(void) {
    static int done; const char *e = getenv("DM_PROBE_DUMP"); if (done || !e) return; done = 1;
    char spec[512]; strlcpy(spec, e, sizeof spec);
    char *mod = strtok(spec, ":"), *o = strtok(NULL, ":"), *n = strtok(NULL, ":"), *f = strtok(NULL, ":");
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (mod && !strcmp(g_mod[i].name, mod)) m = &g_mod[i];
    if (!m || !o || !n) { LOG("DM_PROBE_DUMP format: module:offset:length[:file]"); return; }
    char def[512]; snprintf(def, sizeof def, "%s%s/run/platform/probe_dump.bin", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work");
    FILE *fp = fopen(f ? f : def, "wb"); if (!fp) return;
    fwrite(m->base + strtoull(o, 0, 16), 1, strtoull(n, 0, 16), fp); fclose(fp);
    LOG("probe dump %s+%s length %s → %s", mod, o, n, f ? f : def);
}
// DM_PROBE_RING=<file>: probe hits are recorded, not printed, in a ring of the last RING_N (probe, thread, time, x0–x23, lr), written to the file when the process dies of a signal:
// printing every hit slowed the thread enough to hide a race (Unravel Two: the reference-count loops main+0x694f28 / 0x695290 log 4,000 increments a second; with every hit printed
// none of five runs crashed). File: a header {'RING', count, next} then the entries (struct ring_ent)
#define RING_N (1u << 18)
typedef struct { uint64_t t; uint32_t probe, thr; uint64_t x[24], lr; } ring_ent;
static ring_ent *g_ring; static _Atomic uint32_t g_ring_next; static const char *g_ring_path;
static void ring_write(void) {
    if (!g_ring) return;
    int fd = open(g_ring_path, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return;
    uint32_t h[4] = {0x474e4952, RING_N, atomic_load(&g_ring_next), (uint32_t)sizeof(ring_ent)};
    write(fd, h, sizeof h); write(fd, g_ring, sizeof(ring_ent) * RING_N); close(fd);
    fprintf(stderr, "[dm] probe ring: %u hits, written to %s\n", h[2], g_ring_path);
}
// DM_HWWATCH=@x<N>+<offset>+<length> (hex): an ARM hardware watchpoint on the bytes at register xN + offset of the first reported DM_PROBE hit (its filters pick the object), in every
// thread: no cost until a store reaches them, so a race that any probe or page watch hides still happens. Once armed, every probe is removed (its traps would slow the thread). A store
// that hits prints the thread, pc, lr, the store address and size, the 8 bytes there before it and a backtrace; the thread's watchpoint is then off for the one instruction, which runs again
// (an exclusive store fails once and its loop retries), and a host thread arms every thread again every 2 ms (new threads too). Regions: up to 4 aligned power-of-two blocks covering the
// bytes, so stores just beside them hit as well and print their address. Unravel Two: the boot-time material blocks whose pointers gain 2^32 a minute later
#if !TARGET_OS_IPHONE
static int g_hw_reg = -1, g_hw_keep = -1; static uint64_t g_hw_off, g_hw_len; static _Atomic int g_hw_on;   // g_hw_keep: DM_HWWATCH_KEEP, the DM_PROBE entry (0-based) left in place, reporting only hits whose x1 lies in the watched regions (a free of the watched object)
static uint64_t g_hw_lo, g_hw_hi; static arm_debug_state64_t g_hw_ds; static uint32_t g_hw_lsc = 2;   // WCR LSC: 2 stores, 3 loads and stores (DM_HWWATCH_RW=1)
static void hw_regions(uint64_t a, uint64_t len) {   // cover [a, a + len) with at most 4 aligned power-of-two regions (WCR MASK), the smallest that fit
    memset(&g_hw_ds, 0, sizeof g_hw_ds);
    for (int m = 3; m < 31; m++) {
        uint64_t sz = 1ull << m, lo = a & ~(sz - 1), hi = (a + len + sz - 1) & ~(sz - 1);
        if ((hi - lo) / sz > 4) continue;
        int k = 0; for (uint64_t r = lo; r < hi; r += sz, k++) { g_hw_ds.__wvr[k] = r; g_hw_ds.__wcr[k] = 1 | (2u << 1) | (g_hw_lsc << 3) | (m > 3 ? ((uint32_t)m << 24) | (0xffu << 5) : 0xffu << 5); }
        g_hw_lo = lo; g_hw_hi = hi; return;
    }
    die("DM_HWWATCH: no region cover");
}
static void *hw_rearm(void *p) {
    (void)p; thread_act_t self = mach_thread_self();
    for (;;) {
        thread_act_array_t ths; mach_msg_type_number_t n = 0;
        if (task_threads(mach_task_self(), &ths, &n) == KERN_SUCCESS) {
            for (mach_msg_type_number_t i = 0; i < n; i++) { if (ths[i] != self) thread_set_state(ths[i], ARM_DEBUG_STATE64, (thread_state_t)&g_hw_ds, ARM_DEBUG_STATE64_COUNT); mach_port_deallocate(mach_task_self(), ths[i]); }
            vm_deallocate(mach_task_self(), (vm_address_t)ths, n * sizeof *ths);
        }
        usleep(2000);
    }
    return NULL;
}
static void probes_remove(void) {   // put every probed instruction back
    for (int i = 0; i < g_nprobe; i++) {
        if (i == g_hw_keep) continue;
        uint32_t *w = (uint32_t *)g_probe[i].at; uint64_t pg = (uint64_t)w & ~0x3FFFull;
        if (mprotect((void *)pg, 0x4000, PROT_READ | PROT_WRITE)) continue;
        *w = g_probe[i].slot[0]; mprotect((void *)pg, 0x4000, PROT_READ | PROT_EXEC); sys_icache_invalidate(w, 4);
    }
}
static void watch_where(const char *tag, uint64_t a);
static int watch_decode(ucontext_t *uc, uint32_t in, uint64_t *ea, unsigned *len, uint8_t *data, int *wb, uint64_t *wbv);
static int hw_hit(ucontext_t *uc) {   // a watchpoint exception: the store's address is FAR
    if (!atomic_load(&g_hw_on)) return 0;
    uint64_t far = uc->uc_mcontext->__es.__far, pc = uc->uc_mcontext->__ss.__pc;
    if (far < g_hw_lo || far >= g_hw_hi) return 0;
    arm_debug_state64_t off; memset(&off, 0, sizeof off);   // this thread's watchpoints off first (the handler reads the watched bytes); the access runs when the handler returns, hw_rearm arms the thread again
    { thread_act_t me = mach_thread_self(); thread_set_state(me, ARM_DEBUG_STATE64, (thread_state_t)&off, ARM_DEBUG_STATE64_COUNT); mach_port_deallocate(mach_task_self(), me); }
    uint32_t in = *(uint32_t *)pc; uint64_t ea = far, old = 0; unsigned len = 0; uint8_t data[64]; int wb; uint64_t wbv;
    int dec = watch_decode(uc, in, &ea, &len, data, &wb, &wbv);
    memcpy(&old, (void *)(far & ~7ull), 8);
    uint64_t nv = 0; if (dec && len <= 8) memcpy(&nv, data, len);
    flockfile(stderr);
    { extern uint64_t g_t0_ns; uint64_t now_ns(void); uint32_t thr_cur_handle(void);
      fprintf(stderr, "[dm] HWWATCH [%.4fs] T=%x %s at %#llx (%s+%#llx) instruction %08x %s%u bytes %#llx, 8 bytes at %#llx before: %#llx", (now_ns() - g_t0_ns) / 1e9, thr_cur_handle(), dec ? "store" : "access",
              (unsigned long long)ea, far >= g_hw_lo + 0 && ea >= g_hw_lo ? "region" : "?", (unsigned long long)(ea - g_hw_lo), in, dec ? "" : "(not decoded) ", len, (unsigned long long)nv,
              (unsigned long long)(far & ~7ull), (unsigned long long)old); }
    watch_where("pc", pc); watch_where("lr", uc->uc_mcontext->__ss.__lr); fputc('\n', stderr);
    dm_backtrace(uc->uc_mcontext->__ss.__fp);
    funlockfile(stderr);
    return 1;
}
static int g_hw_multi; static uint64_t g_hw_at[4]; static int g_hw_nat;   // DM_HWWATCH_MULTI=<n ≤ 4>: one 8-byte watchpoint each on the first n distinct addresses (length ≤ 8, within one doubleword)
static void hw_arm_multi(void) {
    memset(&g_hw_ds, 0, sizeof g_hw_ds); g_hw_lo = UINT64_MAX; g_hw_hi = 0;
    for (int k = 0; k < g_hw_nat; k++) {
        uint64_t a = g_hw_at[k], dw = a & ~7ull; uint32_t bas = ((1u << g_hw_len) - 1) << (a & 7);
        g_hw_ds.__wvr[k] = dw; g_hw_ds.__wcr[k] = 1 | (2u << 1) | (g_hw_lsc << 3) | ((bas & 0xff) << 5);
        if (dw < g_hw_lo) g_hw_lo = dw; if (dw + 8 > g_hw_hi) g_hw_hi = dw + 8;
        LOG("HWWATCH %d: %#llx + %llu", k, (unsigned long long)a, (unsigned long long)g_hw_len);
    }
    probes_remove(); atomic_store(&g_hw_on, 1);
    pthread_t t; pthread_create(&t, NULL, hw_rearm, NULL); pthread_detach(t);
}
static void hw_arm(uint64_t a) {
    hw_regions(a, g_hw_len);
    probes_remove();
    atomic_store(&g_hw_on, 1);
    LOG("HWWATCH armed: %#llx + %#llx, regions %#llx..%#llx (probes removed)", (unsigned long long)a, (unsigned long long)g_hw_len, (unsigned long long)g_hw_lo, (unsigned long long)g_hw_hi);
    pthread_t t; pthread_create(&t, NULL, hw_rearm, NULL); pthread_detach(t);
}
#endif
static int probe_hit(ucontext_t *uc) {   // Registered by probe_install with the SIGTRAP branch of crash.c
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
#if !TARGET_OS_IPHONE
    if (hw_hit(uc)) return 1;
#endif
    for (int i = 0; i < g_nprobe; i++) if (g_probe[i].at == pc) {
        if (g_probe[i].fn) { g_probe[i].hits++; g_probe[i].fn(uc); uc->uc_mcontext->__ss.__pc = (uint64_t)g_probe[i].slot; return 1; }
        uint64_t frame = dm_guest_frame();
        if (frame < g_probe_frame_first || frame > g_probe_frame_last || (g_probe_limit && g_probe[i].hits >= g_probe_limit)) {
            uc->uc_mcontext->__ss.__pc = (uint64_t)g_probe[i].slot; return 1;
        }
        if (g_probe[i].freg >= 0) {   // ?xN=value / ?[xN+offset]=value: pass through silently when it does not match
            uint64_t v = uc->uc_mcontext->__ss.__x[g_probe[i].freg]; vm_size_t g = 0;
            if (g_probe[i].foff >= 0 && vm_read_overwrite(mach_task_self(), v + g_probe[i].foff, 8, (vm_address_t)&v, &g)) v = ~g_probe[i].fval;
            if (v != g_probe[i].fval) { uc->uc_mcontext->__ss.__pc = (uint64_t)g_probe[i].slot; return 1; }
        }
        if (g_probe[i].hreg >= 0 && g_hw.want) {   // !xN with DM_PROBE_HEAPNAME: report only hits on heaps of that name
            char nm[128]; probe_heapname(uc->uc_mcontext->__ss.__x[g_probe[i].hreg], nm, sizeof nm);
            if (!strstr(nm, g_hw.want)) { uc->uc_mcontext->__ss.__pc = (uint64_t)g_probe[i].slot; return 1; }
        }
#if !TARGET_OS_IPHONE
        if (i == g_hw_keep) {   // DM_HWWATCH_KEEP: only hits on the watched bytes, printed in full
            uint64_t x1 = uc->uc_mcontext->__ss.__x[1];
            if (!atomic_load(&g_hw_on) || x1 < g_hw_lo || x1 >= g_hw_hi) { uc->uc_mcontext->__ss.__pc = (uint64_t)g_probe[i].slot; return 1; }
        } else
        if (g_hw_reg >= 0) {   // DM_HWWATCH: arm on this hit and run on (the probes are removed)
            static _Atomic int once; uint64_t a = uc->uc_mcontext->__ss.__x[g_hw_reg] + g_hw_off;
            if (g_hw_multi) {   // collect the first n distinct addresses (probe hits are serialized enough for a diagnostic)
                static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&mu);
                int seen = 0; for (int k = 0; k < g_hw_nat; k++) seen |= g_hw_at[k] == a;
                if (!seen && g_hw_nat < g_hw_multi && !atomic_load(&once)) { g_hw_at[g_hw_nat++] = a; if (g_hw_nat == g_hw_multi) { atomic_store(&once, 1); hw_arm_multi(); } }
                pthread_mutex_unlock(&mu);
            } else
            if (!atomic_exchange(&once, 1)) hw_arm(a);
            uc->uc_mcontext->__ss.__pc = (uint64_t)g_probe[i].slot; return 1;
        }
#endif
        if (g_ring) {
            extern uint64_t g_t0_ns; uint64_t now_ns(void); uint32_t thr_cur_handle(void);
            ring_ent *r = &g_ring[atomic_fetch_add(&g_ring_next, 1) & (RING_N - 1)];
            r->t = now_ns() - g_t0_ns; r->probe = (uint32_t)i; r->thr = thr_cur_handle(); r->lr = uc->uc_mcontext->__ss.__lr;
            for (int k = 0; k < 24; k++) r->x[k] = uc->uc_mcontext->__ss.__x[k];
            g_probe[i].hits++; uc->uc_mcontext->__ss.__pc = (uint64_t)g_probe[i].slot; return 1;
        }
        probe_dump_once();
        flockfile(stderr);   // One block per hit when threads hit at once
        { extern uint64_t g_t0_ns; uint64_t now_ns(void); uint32_t thr_cur_handle(void); fprintf(stderr, "[dm] PROBE %s #%u [%.4fs] F=%llu T=%x", g_probe[i].name, ++g_probe[i].hits, (now_ns() - g_t0_ns) / 1e9, (unsigned long long)frame, thr_cur_handle()); }
        for (int r = 0; r < 8; r++) { uint64_t v = uc->uc_mcontext->__ss.__x[r]; fprintf(stderr, " x%d=%#llx", r, v); probe_str(v); }
        for (int r = 19; r < 24; r++) fprintf(stderr, " x%d=%#llx", r, uc->uc_mcontext->__ss.__x[r]);
        if (getenv("DM_PROBE_ALL")) for (int r = 8; r < 19; r++) fprintf(stderr, " x%d=%#llx", r, uc->uc_mcontext->__ss.__x[r]);   // also print x8–x18
        fprintf(stderr, " lr=%#llx", uc->uc_mcontext->__ss.__lr);
        { uint64_t x8 = uc->uc_mcontext->__ss.__x[8]; Dl_info di; fprintf(stderr, " x8=%#llx", x8);   // x8: indirect call targets are often here (probe placed before blr x8)
          if (dladdr((void *)x8, &di) && di.dli_sname) fprintf(stderr, " (%s+%#lx)", di.dli_sname, (unsigned long)(x8 - (uint64_t)di.dli_saddr)); }
        if (g_probe[i].dump >= 0) {
            uint8_t b[16]; vm_size_t got = 0;
            int dr = (g_probe[i].dump >> 25) & 31;                                    // @xN+offset: based on xN
            uint64_t at = uc->uc_mcontext->__ss.__x[dr] + (g_probe[i].dump & 0xffffff);
            if (g_probe[i].dump & 0x1000000) { uint64_t p = 0; vm_size_t g = 0; if (!vm_read_overwrite(mach_task_self(), at, 8, (vm_address_t)&p, &g) && p) at = p + g_probe[i].dpost; else at = 0; }   // @*offset[+post]: dereference first, then dump (post bytes further on)
            if (at && !vm_read_overwrite(mach_task_self(), at, 16, (vm_address_t)b, &got) && got == 16) {
                fprintf(stderr, " [%sx%d+%#lx]", (g_probe[i].dump & 0x1000000) ? "*" : "", dr, g_probe[i].dump & 0xffffff);
                if (g_probe[i].dpost) fprintf(stderr, "+%#lx", g_probe[i].dpost);
                fputc('=', stderr); for (int k = 0; k < 16; k++) fprintf(stderr, "%02x", b[k]); }
        }
        fputc('\n', stderr);
        { static long xd = -2; if (xd == -2) { const char *e = getenv("DM_PROBE_X2"); xd = e ? strtol(e, 0, 16) : -1; }   // DM_PROBE_X2=bytes: also print the memory x2 points to (to read object fields)
          if (xd > 0) { uint64_t at = uc->uc_mcontext->__ss.__x[2]; uint8_t b[512]; vm_size_t got = 0; long n = xd > 512 ? 512 : xd;
            if (!vm_read_overwrite(mach_task_self(), at, n, (vm_address_t)b, &got)) { fprintf(stderr, "[dm]   [x2]:"); for (long k = 0; k < (long)got; k += 8) { uint64_t v; memcpy(&v, b + k, 8); fprintf(stderr, "%s%llx", k % 32 ? " " : "\n[dm]   ", (unsigned long long)v); } fputc('\n', stderr); } } }
        if (g_probe[i].hreg >= 0) probe_heapwalk(uc->uc_mcontext->__ss.__x[g_probe[i].hreg]);
        dm_backtrace(uc->uc_mcontext->__ss.__fp);
        funlockfile(stderr);
        uc->uc_mcontext->__ss.__pc = (uint64_t)g_probe[i].slot;
        return 1;
    }
    return 0;
}
static uint32_t *g_slots; static int g_slots_sealed;
static void probe_add(module_t *m, uint64_t off, long dump, probe_fn fn) {
    if (g_nprobe >= MAX_PROBE) die("too many probes");
    if (!g_slots) g_slots = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint64_t at = (uint64_t)m->base + off;
    uint32_t *w = (uint32_t *)at, *sl = g_slots + g_nprobe * 8;
    sl[0] = *w;                                  // Original instruction
    sl[1] = 0x58000050;                          // ldr x16, #8
    sl[2] = 0xD61F0200;                          // br x16
    *(uint64_t *)(sl + 3) = at + 4;
    uint64_t pg = at & ~0x3FFFull;
    if (mprotect((void *)pg, 0x4000, PROT_READ | PROT_WRITE)) die("probe mprotect");
    *w = 0xD43E0000;                             // brk #0xf000
    mprotect((void *)pg, 0x4000, PROT_READ | PROT_EXEC);
    sys_icache_invalidate(w, 4);
    g_probe[g_nprobe].at = at; g_probe[g_nprobe].slot = sl; g_probe[g_nprobe].dump = dump; g_probe[g_nprobe].fn = fn; g_probe[g_nprobe].freg = -1; g_probe[g_nprobe].hreg = -1;
    snprintf(g_probe[g_nprobe].name, sizeof g_probe[0].name, "%s+%#llx", m->name, off);
    LOG("probe %s (original instruction %08x)", g_probe[g_nprobe].name, sl[0]);
    g_nprobe++;
}

// DM_PROBE_WATCH=<address>[+<length>] (hex guest address, length 8 by default): who writes these bytes, without a debugger. Their host page is write-protected for good once it is writable,
// and every store that faults on it is carried out by the fault handler through a writable alias of the page (STR / STUR / STP of general and SIMD registers in their immediate, pre- and
// post-index forms, register-offset STR, DC ZVA), so a burst of stores to a busy page cannot slip past. A store reaching the watched bytes prints the thread, pc, lr, the store address,
// the old and new values and a backtrace. Any other instruction (atomics, exclusives) is logged once per pc and the page is unprotected for it, then protected again a millisecond later
// (a store in that millisecond goes unseen). STEINS;GATE RE:BOOT: the engine object's page takes bursts of stores every frame, and a watch that unprotected the page for each fault missed
// the one that matters; this one found it at once (main+0x389e90, a setter its start-up code calls with 0: the game renders 1280x720 docked by its own choice)
static uint64_t g_wat, g_wlen, g_wpg, g_wpsz; static uint8_t *g_walias; static _Atomic int g_warmed;
static void watch_where(const char *tag, uint64_t a) {
    module_t *m = module_of((void *)a); Dl_info di;
    if (m) fprintf(stderr, " %s=%s+%#llx", tag, m->name, (unsigned long long)(a - (uint64_t)m->base));
    else if (dladdr((void *)a, &di) && di.dli_sname) fprintf(stderr, " %s=%s+%#lx", tag, di.dli_sname, (unsigned long)(a - (uint64_t)di.dli_saddr));
    else fprintf(stderr, " %s=%#llx", tag, (unsigned long long)a);
}
static uint64_t watch_val(void) { uint64_t v = 0; memcpy(&v, (void *)g_wat, g_wlen < 8 ? g_wlen : 8); return v; }
// Decode the store at pc: its address, size and data (up to 64 bytes); the base register write-back (wb: register number, value) for pre- and post-index forms. 0 when not a decodable store
static int watch_decode(ucontext_t *uc, uint32_t in, uint64_t *ea, unsigned *len, uint8_t *data, int *wb, uint64_t *wbv) {
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss; _STRUCT_ARM_NEON_STATE64 *ns = &uc->uc_mcontext->__ns;
#define XN(r) ((r) == 31 ? ss->__sp : (r) == 29 ? ss->__fp : (r) == 30 ? ss->__lr : ss->__x[r])        // base register: 31 = sp
#define XT(r) ((r) == 31 ? 0 : (r) == 29 ? ss->__fp : (r) == 30 ? ss->__lr : ss->__x[r])                // data register: 31 = zero
    unsigned rt = in & 31, rn = (in >> 5) & 31, v = (in >> 26) & 1; *wb = -1;
    if ((in & 0xffffffe0) == 0xd50b7420) {                        // DC ZVA: zero the 64-byte block
        *ea = XT(rt) & ~63ull; *len = 64; memset(data, 0, 64); return 1; }
    if ((in & 0x3a000000) == 0x28000000) {                                                               // STP / STNP: opc 101 V 0 type L imm7
        if (in & (1u << 22)) return 0;                                                                   // LDP
        unsigned opc = in >> 30, type = (in >> 23) & 3, sz = v ? 4u << opc : (opc & 2 ? 8 : 4);
        if ((!v && opc == 1) || opc == 3) return 0;                                                      // STGP / LDPSW, reserved
        int64_t imm = ((int64_t)(int32_t)(in << 10) >> 25) * (int64_t)sz; uint64_t base = XN(rn);
        *ea = type == 1 ? base : base + imm; *len = 2 * sz;
        unsigned r2 = (in >> 10) & 31;
        if (v) { memcpy(data, &ns->__v[rt], sz); memcpy(data + sz, &ns->__v[r2], sz); }
        else { uint64_t a = XT(rt), b = XT(r2); memcpy(data, &a, sz); memcpy(data + sz, &b, sz); }
        if (type == 1 || type == 3) { *wb = (int)rn; *wbv = base + imm; }
        return 1;
    }
    if ((in & 0x3b000000) == 0x39000000 || (in & 0x3b200000) == 0x38000000 || (in & 0x3b200c00) == 0x38200800) {   // STR (unsigned offset) / STUR, STR pre / post / STR (register)
        unsigned size = in >> 30, opc = (in >> 22) & 3, sz;
        if (v) { if (opc == 0) sz = 1u << size; else if (opc == 2 && size == 0) sz = 16; else return 0; }
        else { if (opc != 0) return 0; sz = 1u << size; }
        uint64_t base = XN(rn), addr;
        if ((in & 0x3b000000) == 0x39000000) addr = base + (uint64_t)((in >> 10) & 0xfff) * sz;          // unsigned offset
        else if (!(in & (1u << 21))) {                                                                   // imm9: unscaled, post (01), pre (11)
            int64_t imm = (int64_t)(int32_t)(in << 11) >> 23; unsigned k = (in >> 10) & 3;
            if (k == 2) return 0;                                                                        // STTR
            addr = k == 1 ? base : base + imm;
            if (k == 1 || k == 3) { *wb = (int)rn; *wbv = base + imm; }
        } else {                                                                                         // register offset: option (15:13), S (12)
            unsigned rm = (in >> 16) & 31, opt = (in >> 13) & 7, s = (in >> 12) & 1; uint64_t x = XT(rm);
            if (opt == 2) x = (uint32_t)x; else if (opt == 6) x = (uint64_t)(int64_t)(int32_t)x; else if (opt != 3 && opt != 7) return 0;
            addr = base + (s ? x << __builtin_ctz(sz) : x);
        }
        *ea = addr; *len = sz;
        if (v) memcpy(data, &ns->__v[rt], sz); else { uint64_t a = XT(rt); memcpy(data, &a, sz); }
        return 1;
    }
    return 0;
#undef XN
#undef XT
}
static int watch_fault(void *addr, int is_write, void *ucv) {
    uint64_t a = (uint64_t)addr;
    if (!is_write || !g_wat || a < g_wpg || a >= g_wpg + g_wpsz) return 0;
    ucontext_t *uc = ucv; uint64_t pc = uc->uc_mcontext->__ss.__pc, ea = 0, wbv = 0; unsigned len = 0; int wb = -1; uint8_t data[64];
    if (g_walias && watch_decode(uc, *(uint32_t *)pc, &ea, &len, data, &wb, &wbv)) {
        int hit = ea < g_wat + g_wlen && ea + len > g_wat; uint64_t old = hit ? watch_val() : 0;
        for (unsigned i = 0; i < len; i++) {   // bytes in the watched page go through the alias; a store crossing into the next page writes the rest directly
            uint64_t b = ea + i;
            if (b >= g_wpg && b < g_wpg + g_wpsz) g_walias[b - g_wpg] = data[i]; else *(volatile uint8_t *)b = data[i];
        }
        if (wb >= 0) { if (wb == 31) uc->uc_mcontext->__ss.__sp = wbv; else if (wb == 29) uc->uc_mcontext->__ss.__fp = wbv; else if (wb == 30) uc->uc_mcontext->__ss.__lr = wbv; else uc->uc_mcontext->__ss.__x[wb] = wbv; }
        uc->uc_mcontext->__ss.__pc = pc + 4;
        if (hit) {
            flockfile(stderr);
            { extern uint64_t g_t0_ns; uint64_t now_ns(void); uint32_t thr_cur_handle(void);
              fprintf(stderr, "[dm] WATCH %#llx [%.4fs] T=%x store of %u bytes at %#llx: %#llx → %#llx", (unsigned long long)g_wat, (now_ns() - g_t0_ns) / 1e9, thr_cur_handle(), len, (unsigned long long)ea, (unsigned long long)old, (unsigned long long)watch_val()); }
            watch_where("pc", pc); watch_where("lr", uc->uc_mcontext->__ss.__lr); fputc('\n', stderr);
            dm_backtrace(uc->uc_mcontext->__ss.__fp);
            funlockfile(stderr);
        }
        return 1;
    }
    // Exclusive and atomic stores (STXR / STLXR / STLR / STXP, CAS, SWP, LD<op>) address [Xn] with no offset: one that reaches the watched bytes is reported like a store, every time
    // (its new value lands when it runs on the unprotected page). Tears of the Kingdom's memory-shortage flag is set by such a store, and the page it lies on takes many of them
    { uint32_t in = *(uint32_t *)pc; int ex = (in & 0x3f000000) == 0x08000000, at = (in & 0x3f200c00) == 0x38200000;   // exclusive class (CAS / CASP among them); LD<op> / SWP
      if (ex || at) {
          _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss; unsigned rn = (in >> 5) & 31, sz = 1u << (in >> 30);
          uint64_t base = rn == 31 ? ss->__sp : rn == 29 ? ss->__fp : rn == 30 ? ss->__lr : ss->__x[rn];
          if ((in & 0xbfa00000) == 0x88200000) sz *= 2;   // STXP / STLXP: a register pair
          if (base < g_wat + g_wlen && base + sz > g_wat) {
              flockfile(stderr);
              { extern uint64_t g_t0_ns; uint64_t now_ns(void); uint32_t thr_cur_handle(void);
                fprintf(stderr, "[dm] WATCH %#llx [%.4fs] T=%x %s store of %u bytes at %#llx (instruction %08x, not emulated), value before %#llx", (unsigned long long)g_wat, (now_ns() - g_t0_ns) / 1e9, thr_cur_handle(),
                        ex ? "exclusive" : "atomic", sz, (unsigned long long)base, in, (unsigned long long)watch_val()); }
              watch_where("pc", pc); watch_where("lr", ss->__lr); fputc('\n', stderr);
              dm_backtrace(ss->__fp);
              funlockfile(stderr);
          }
      } }
    { static uint64_t seen[16]; static _Atomic int ns; int k = 0, n = atomic_load(&ns);   // not a store form the handler carries out: let it run on an unprotected page
      while (k < n && k < 16 && seen[k] != pc) k++;
      if (k == n && n < 16) { seen[atomic_fetch_add(&ns, 1) & 15] = pc; flockfile(stderr); fprintf(stderr, "[dm] WATCH: instruction %08x not emulated, page unprotected for it:", *(uint32_t *)pc); watch_where("pc", pc); fputc('\n', stderr); funlockfile(stderr); } }
    mprotect((void *)g_wpg, g_wpsz, PROT_READ | PROT_WRITE); atomic_store(&g_warmed, 0);
    return 1;
}
static uint64_t g_wrel; static int g_wheap;   // heap+<offset>: the address is that far past the guest heap's base, resolved once the game opens its heap
static void *watch_arm(void *p) {
    (void)p;
    for (;;) {
        if (g_wheap && !g_wat) {
            uint64_t hb = 0; dm_guest_heap_range(&hb, NULL); if (!hb) { usleep(1000); continue; }
            g_wpg = (hb + g_wrel) & ~(g_wpsz - 1); g_wat = hb + g_wrel;
            LOG("write watch on heap %#llx + %#llx = %#llx", (unsigned long long)hb, (unsigned long long)g_wrel, (unsigned long long)g_wat);
        }
        if (g_wat && !atomic_load(&g_warmed)) {
            mach_vm_address_t ra = g_wpg; mach_vm_size_t rs = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
            if (!mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &obj) && ra <= g_wpg && (bi.protection & VM_PROT_WRITE)) {
                if (!g_walias) {   // a second, writable mapping of the same page, for the handler's stores
                    mach_vm_address_t al = 0; vm_prot_t cur, max;
                    if (mach_vm_remap(mach_task_self(), &al, g_wpsz, 0, VM_FLAGS_ANYWHERE, mach_task_self(), g_wpg, FALSE, &cur, &max, VM_INHERIT_NONE) == KERN_SUCCESS) g_walias = (uint8_t *)al;
                    LOG("WATCH %#llx armed, value %#llx%s", (unsigned long long)g_wat, (unsigned long long)watch_val(), g_walias ? "" : " ([RED] no alias mapping: every store unprotects the page)");
                }
                atomic_store(&g_warmed, 1); mprotect((void *)g_wpg, g_wpsz, PROT_READ); }
        }
        usleep(1000);
    }
    return NULL;
}
// DM_PROBE_POLL=<address>[+<length>] (hex guest address, up to 8 bytes, 4 by default): when even the watch above misses a write (a page that atomics keep unprotected), a host thread spins on
// the bytes and, on each change, prints old → new and dumps every guest thread with its backtrace (svc_thread_dump, the first 8 changes): the writer is still inside the code that wrote.
// Tears of the Kingdom's memory-shortage flag (a word its frame check compares with 1 and clears) lies on such a page. A suffix =<value> (hex) snapshots and dumps only the changes to that value,
// for a word that changes thousands of times before the bad store (Mario vs. Donkey Kong: a heap block's list link, rewritten by every allocation beside it, until 0x3f landed in it)
static uint64_t g_poll, g_plen, g_pval; static int g_pwant, g_pheap;
// All threads but this one stopped at once, then their pc, lr and 8 frame-pointer frames read and printed after they run again: the writer is caught a few microseconds after its store
static void poll_snap(void) {
    enum { MT = 256, NF = 8 }; static uint64_t fr[MT][NF + 2]; static thread_act_t keep[MT];
    thread_act_array_t ths; mach_msg_type_number_t nth = 0, ns = 0; thread_act_t self = mach_thread_self();
    if (task_threads(mach_task_self(), &ths, &nth) != KERN_SUCCESS) return;
    for (mach_msg_type_number_t i = 0; i < nth && ns < MT; i++) if (ths[i] != self && thread_suspend(ths[i]) == KERN_SUCCESS) keep[ns++] = ths[i];
    for (mach_msg_type_number_t i = 0; i < ns; i++) {
        arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT; memset(fr[i], 0, sizeof fr[i]);
        if (thread_get_state(keep[i], ARM_THREAD_STATE64, (thread_state_t)&st, &n) != KERN_SUCCESS) continue;
        fr[i][0] = __darwin_arm_thread_state64_get_pc(st); fr[i][1] = __darwin_arm_thread_state64_get_lr(st);
        uint64_t fp = __darwin_arm_thread_state64_get_fp(st), w[2]; vm_size_t g;
        for (int k = 0; k < NF && fp > 0x100000000ull && !vm_read_overwrite(mach_task_self(), fp, 16, (vm_address_t)w, &g) && g == 16; k++) { fr[i][2 + k] = w[1]; fp = w[0]; }
    }
    for (mach_msg_type_number_t i = 0; i < ns; i++) thread_resume(keep[i]);
    for (mach_msg_type_number_t i = 0; i < nth; i++) mach_port_deallocate(mach_task_self(), ths[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)ths, nth * sizeof *ths); mach_port_deallocate(mach_task_self(), self);
    flockfile(stderr);
    for (mach_msg_type_number_t i = 0; i < ns; i++) {   // threads with guest code anywhere in their first frames
        int guest = 0; for (int k = 0; k < NF + 2; k++) guest |= module_of((void *)fr[i][k]) != NULL;
        if (!guest) continue;
        fprintf(stderr, "[dm] POLL thread %u:", i); watch_where("pc", fr[i][0]); watch_where("lr", fr[i][1]);
        for (int k = 2; k < NF + 2 && fr[i][k]; k++) watch_where("<", fr[i][k]);
        fputc('\n', stderr);
    }
    funlockfile(stderr);
}
static void *poll_run(void *p) {
    (void)p; void svc_thread_dump(void); extern uint64_t g_t0_ns; uint64_t now_ns(void); vm_size_t got;
    uint64_t old = 0;   // read through the kernel: the address is not readable until the game maps it (the heap before SetHeapSize)
    if (g_pheap) { uint64_t hb = 0; while (dm_guest_heap_range(&hb, NULL), !hb) usleep(1000); g_poll += hb; }   // heap+<offset>
    while (vm_read_overwrite(mach_task_self(), g_poll, g_plen, (vm_address_t)&old, &got) || got != g_plen) usleep(1000);
    LOG("POLL %#llx + %llu armed, value %#llx", (unsigned long long)g_poll, (unsigned long long)g_plen, (unsigned long long)old);
    for (int n = 0;;) {
        uint64_t v = 0;
        if (vm_read_overwrite(mach_task_self(), g_poll, g_plen, (vm_address_t)&v, &got) || v == old) continue;
        int hit = !g_pwant || v == g_pval;
        if (hit && n < 8) poll_snap();
        fprintf(stderr, "[dm] POLL %#llx [%.4fs] %#llx → %#llx\n", (unsigned long long)g_poll, (now_ns() - g_t0_ns) / 1e9, (unsigned long long)old, (unsigned long long)v);
        old = v;
        if (hit && n++ < 2) svc_thread_dump();
    }
    return NULL;
}
static void watch_install(void) {
    { const char *e = getenv("DM_PROBE_POLL");
      if (e) { char *end; g_pheap = !strncmp(e, "heap+", 5); if (g_pheap) e += 5; g_poll = strtoull(e, &end, 16); g_plen = *end == '+' ? strtoull(end + 1, &end, 16) : 4;
               if (*end == '=') { g_pwant = 1; g_pval = strtoull(end + 1, 0, 16); }
               if ((!g_poll && !g_pheap) || !g_plen || g_plen > 8) die("DM_PROBE_POLL format: <address>[+<length up to 8>][=<value>] (hex)");
               pthread_t t; pthread_create(&t, NULL, poll_run, NULL); pthread_detach(t); } }
    const char *e = getenv("DM_PROBE_WATCH"); if (!e) return;
    char *end; g_wheap = !strncmp(e, "heap+", 5);
    uint64_t at = strtoull(e + (g_wheap ? 5 : 0), &end, 16); g_wlen = *end == '+' ? strtoull(end + 1, 0, 16) : 8;
    if ((!at && !g_wheap) || !g_wlen) die("DM_PROBE_WATCH format: [heap+]<address>[+<length>] (hex)");
    g_wpsz = (uint64_t)getpagesize();
    if ((at & (g_wpsz - 1)) + g_wlen > g_wpsz) die("DM_PROBE_WATCH: the watched bytes must lie in one host page");
    if (g_wheap) { g_wrel = at; g_wpg = 0; } else { g_wat = at; g_wpg = g_wat & ~(g_wpsz - 1); }
    dm_fault_hook_register(watch_fault);
    pthread_t t; pthread_create(&t, NULL, watch_arm, NULL); pthread_detach(t);
    if (g_wheap) LOG("write watch on heap + %#llx + %#llx (resolved once the game opens its heap)", (unsigned long long)g_wrel, (unsigned long long)g_wlen);
    else LOG("write watch on %#llx + %#llx (host page %#llx)", (unsigned long long)g_wat, (unsigned long long)g_wlen, (unsigned long long)g_wpg);
}

#endif

// DM_RECORD=<file> with DM_RECORD_SITES=<module>:<offset>/<register>,… (at most 8): a register's value at those instructions, recorded at native speed with no trap. The instruction
// becomes a branch to a stub just below the module that takes a slot of a ring of 2^20 with one LDADD, stores the register and CNTVCT (shifted left 4, low bits the site's index),
// runs the original instruction and branches back; the ring is written to the file when the process dies of a signal ({'RECR', count, next, 16} then the entries). The stub uses x16 and
// x17: sites must have both dead, and the instruction must not be PC-relative. For races that any trap hides (Unravel Two: the reference-count increments of main+0x694f28 / 0x695290)
#define REC_N (1u << 20)
static uint64_t *g_rec; static _Atomic uint64_t g_rec_next; static const char *g_rec_path;
static void record_write(void) {
    if (!g_rec) return;
    int fd = open(g_rec_path, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return;
    uint32_t h[4] = {0x52434552, REC_N, (uint32_t)atomic_load(&g_rec_next), 16};
    write(fd, h, sizeof h); write(fd, g_rec, 16ull * REC_N); close(fd);
    fprintf(stderr, "[dm] record: %llu entries, written to %s\n", (unsigned long long)atomic_load(&g_rec_next), g_rec_path);
}
static void record_install(void) {
    void crash_set_fatal_hook(void (*)(void));
    if (!(g_rec_path = getenv("DM_RECORD"))) return;
    const char *e = getenv("DM_RECORD_SITES"); if (!e) die("DM_RECORD needs DM_RECORD_SITES=<module>:<offset>/<register>,...");
    if (!(g_rec = calloc(REC_N, 16))) die("DM_RECORD: no memory for the ring");
    char spec[512]; strlcpy(spec, e, sizeof spec); int idx = 0; uint32_t *stubs = NULL; uint64_t near = 0;
    for (char *t = strtok(spec, ","); t && idx < 8; t = strtok(NULL, ","), idx++) {
        char *c = strchr(t, ':'), *sl = strchr(t, '/'); if (!c || !sl) die("DM_RECORD_SITES format: <module>:<offset>/<register>");
        *c = 0; module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, t)) m = &g_mod[i];
        if (!m) die("DM_RECORD_SITES: no such module %s", t);
        uint64_t at = (uint64_t)m->base + strtoull(c + 1, 0, 16); char *e2; int reg = (int)strtol(sl + 1, &e2, 10) & 31, reg2 = *e2 == '+' ? (int)strtol(e2 + 1, 0, 10) & 31 : -1;   // /<reg>[+<reg2>]: reg2 replaces the time stamp
        if (!stubs) {   // one page just below the first site's module, within the reach of a B (±128 MB)
            for (int64_t d = 0x4000; d < 0x7f00000 && !stubs; d += 0x4000)   // nearest free page either side of the site
                for (int sgn = -1; sgn <= 1 && !stubs; sgn += 2) {
                    mach_vm_address_t a = ((at & ~0x3fffull) + (uint64_t)(sgn * d));
                    if (mach_vm_allocate(mach_task_self(), &a, 0x4000, VM_FLAGS_FIXED) == KERN_SUCCESS) { stubs = (uint32_t *)a; near = a; }
                }
            if (!stubs) die("DM_RECORD: no page within branch reach of %s", m->name);
            LOG("DM_RECORD: stubs at %#llx", (unsigned long long)near);
        }
        uint32_t *st = stubs + idx * 20, *w = (uint32_t *)at; int64_t d = (int64_t)((uint64_t)st - at);
        if (d < -(1ll << 27) || d >= (1ll << 27)) die("DM_RECORD: site %s+%s out of branch reach of the stubs", t, c + 1);
        static const uint32_t tpl[14] = {0x580001d0, 0xd2800031, 0xf8310211, 0x92404e31, 0x58000190, 0x8b111210, 0xf9000209, 0xd53be051, 0xd37cee31, 0xb2400231, 0xf9000611, 0xd503201f, 0x580000d0, 0xd61f0200};
        memcpy(st, tpl, sizeof tpl);
        st[6] = 0xf9000200 | (uint32_t)reg;                                            // str x<reg>, [x16]
        st[9] = idx ? 0xb2400231 | ((uint32_t)(idx == 1 ? 0 : 0) << 10) : 0xd503201f;   // orr x17, x17, #1 for site 1; site 0 leaves the low bits 0
        if (idx > 1) { st[9] = 0xd503201f; }                                            // sites 2+ share tag 0 (record their register's value only)
        if (reg2 >= 0) { st[7] = 0xf9000600 | (uint32_t)reg2; st[8] = st[9] = st[10] = 0xd503201f; }   // str x<reg2>, [x16, #8]
        st[11] = *w;                                                                    // the original instruction
        uint64_t *lit = (uint64_t *)(st + 14); lit[0] = (uint64_t)&g_rec_next; lit[1] = (uint64_t)g_rec; lit[2] = at + 4;
        LOG("DM_RECORD: site %d %s+%s, register x%d (instruction %08x)", idx, t, c + 1, reg, *w);
        (void)near;
    }
    mprotect(stubs, 0x4000, PROT_READ | PROT_EXEC); sys_icache_invalidate(stubs, 0x4000);
    // the branches go in last, once every stub is executable
    strlcpy(spec, e, sizeof spec); idx = 0;
    for (char *t = strtok(spec, ","); t && idx < 8; t = strtok(NULL, ","), idx++) {
        char *c = strchr(t, ':'); *c = 0; module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, t)) m = &g_mod[i];
        uint64_t at = (uint64_t)m->base + strtoull(c + 1, 0, 16); uint32_t *w = (uint32_t *)at, *st = stubs + idx * 20;
        uint64_t pg = at & ~0x3FFFull;
        if (mprotect((void *)pg, 0x4000, PROT_READ | PROT_WRITE)) die("DM_RECORD mprotect");
        *w = 0x14000000 | (uint32_t)((((int64_t)st - (int64_t)at) >> 2) & 0x3ffffff);
        mprotect((void *)pg, 0x4000, PROT_READ | PROT_EXEC); sys_icache_invalidate(w, 4);
    }
    crash_set_fatal_hook(record_write);
    LOG("DM_RECORD: %d sites, ring of %u written to %s at a crash", idx, REC_N, g_rec_path);
}
void probe_install(void) {
#if !TARGET_OS_IPHONE
    crash_set_trap_hook(probe_hit);
    { const char *e = getenv("DM_HWWATCH"); char *end;
      if (e) { if (e[0] != '@' || e[1] != 'x') die("DM_HWWATCH format: @x<N>+<offset>+<length> (hex)");
               g_hw_reg = (int)strtol(e + 2, &end, 10); g_hw_off = *end == '+' ? strtoull(end + 1, &end, 16) : 0; g_hw_len = *end == '+' ? strtoull(end + 1, &end, 16) : 8;
               if (g_hw_reg < 0 || g_hw_reg > 30 || !g_hw_len) die("DM_HWWATCH format: @x<N>+<offset>+<length> (hex)");
               { const char *k = getenv("DM_HWWATCH_KEEP"); if (k) g_hw_keep = atoi(k); }
               { const char *rw = getenv("DM_HWWATCH_RW"); if (rw && atoi(rw)) g_hw_lsc = 3; }
               { const char *mu = getenv("DM_HWWATCH_MULTI"); if (mu) { g_hw_multi = atoi(mu); if (g_hw_multi < 1 || g_hw_multi > 4 || g_hw_len > 8) die("DM_HWWATCH_MULTI: 1 to 4 addresses of at most 8 bytes"); } }
               LOG("DM_HWWATCH: hardware watchpoint on x%d + %#llx + %#llx of the first reported probe hit", g_hw_reg, (unsigned long long)g_hw_off, (unsigned long long)g_hw_len); } }
    if ((g_ring_path = getenv("DM_PROBE_RING"))) { void crash_set_fatal_hook(void (*)(void));
        if (!(g_ring = calloc(RING_N, sizeof *g_ring))) die("DM_PROBE_RING: no memory for the ring");
        crash_set_fatal_hook(ring_write); LOG("DM_PROBE_RING: probe hits recorded in a ring of %u, written to %s at a crash", RING_N, g_ring_path); }
    const char *frames = getenv("DM_PROBE_FRAMES");
    if (frames) {
        char *end; g_probe_frame_first = strtoull(frames, &end, 10);
        g_probe_frame_last = *end == '-' ? strtoull(end + 1, NULL, 10) : g_probe_frame_first;
    }
    const char *limit = getenv("DM_PROBE_LIMIT");
    if (limit) g_probe_limit = (unsigned)strtoul(limit, NULL, 10);
    const char *e = getenv("DM_PROBE");
    if (e) {
        char spec[512]; strlcpy(spec, e, sizeof spec);
        for (char *t = strtok(spec, ","); t; t = strtok(NULL, ",")) {
            char *c = strchr(t, ':'); if (!c) die("DM_PROBE format: module:offset");
            *c = 0; module_t *m = NULL;
            for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, t)) m = &g_mod[i];
            if (!m) die("DM_PROBE: no such module %s", t);
            char *at_ = strchr(c + 1, '@');
            long dump = -1, post = 0;
            if (at_) {   // @offset / @*offset / @xN+offset / @*xN+offset / @*xN+offset+post (the dereferenced pointer plus post: @*x0+48+8 = an al::LiveActor's position, al::getTrans)
                const char *q = at_ + 1; long deref = 0, reg = 0; char *e3;
                if (*q == '*') { deref = 0x1000000; q++; }
                if (*q == 'x') { char *e2; reg = strtol(q + 1, &e2, 10) & 31; q = *e2 == '+' ? e2 + 1 : e2; }
                dump = deref | (reg << 25) | (strtol(q, &e3, 16) & 0xffffff);
                if (deref && *e3 == '+') post = strtol(e3 + 1, 0, 16);
            }
            char *qm = strchr(c + 1, '?'), *bang = strchr(c + 1, '!');
            probe_add(m, strtoull(c + 1, 0, 16), dump, NULL);
            g_probe[g_nprobe - 1].dpost = post;
            if (bang && bang[1] == 'x') { g_probe[g_nprobe - 1].hreg = (int)strtol(bang + 2, 0, 10) & 31; probe_heapcfg(); }
            if (qm) {   // Filter: ?x0=706c400000 or ?[x0+38]=781548 (hex)
                const char *q = qm + 1; int br = *q == '[';
                if (br) q++;
                if (*q != 'x') die("DM_PROBE filter format: ?xN=value or ?[xN+offset]=value");
                char *e2; g_probe[g_nprobe - 1].freg = (int)strtol(q + 1, &e2, 10) & 31; g_probe[g_nprobe - 1].foff = -1;
                if (br) { g_probe[g_nprobe - 1].foff = *e2 == '+' ? strtol(e2 + 1, &e2, 16) : 0; if (*e2 == ']') e2++; }
                if (*e2 != '=') die("DM_PROBE filter missing =");
                g_probe[g_nprobe - 1].fval = strtoull(e2 + 1, 0, 16);
            }
        }
    }
    if (g_slots && !g_slots_sealed) { mprotect(g_slots, 0x4000, PROT_READ | PROT_EXEC); sys_icache_invalidate(g_slots, 0x4000); g_slots_sealed = 1; }
    record_install();
    watch_install();
#endif
}
