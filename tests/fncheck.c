/* Function-level check of the translation: each translated function (of LARush.exe and the DLLs
   translated with it) is run on random inputs twice, as the original machine code in the Unicorn
   emulator and as the translated C, and the results (registers, x87 stack, SSE and MMX
   registers, every byte written) must be identical.

     fncheck --game DIR [--seeds N] [--only ADDR] [--verbose]
     fncheck --capture FILE [--verbose]
     fncheck --capture FILE --ab

   With --ab the captured state runs natively twice, as the translated function and with the
   decompiled replacements switched in (runtime/decomp/), and every byte either run changed is
   compared, with the registers: the game's own reference, its FPU modes (single precision,
   truncation) included, which the emulator does not model.

   With --capture it replays a state saved at a function's entry in a real game
   (runtime/capture.c): all of memory and the registers, both ways, compared the same way.

   Functions that call into Windows/DirectX, fault on the random inputs, or run too long are
   skipped (they are covered by whole-program runs instead). The native run happens in a
   forked child so a crash only fails that one test. */
#define _GNU_SOURCE
#include <signal.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unicorn/unicorn.h>
#include "runtime.h"
#include "image.h"
#include "options.h"
#include <immintrin.h>
#include <zlib.h>

#define SENTINEL 0x7efe0000u
#define MAX_INSNS 3000000

static uc_engine *uc;
static uint32_t scratch, stack_top, teb, gdt_base;
static CPU *cpu;

/* ------------------------------------------------------------------ write journal */
typedef struct { uint32_t addr; uint8_t old; } Undo;
static Undo *journal;
static size_t njournal, capjournal;

static void hook_write(uc_engine *u, uc_mem_type type, uint64_t addr, int size, int64_t value, void *ud)
{
    (void)u; (void)type; (void)value; (void)ud;
    const ModuleEntry *m = module_at((uint32_t)addr);
    int ok = (m && addr + size <= (uint64_t)m->base + m->size) ||
             (addr >= ARENA_START && addr + size <= ARENA_END);
    if (!ok)
        return;                          /* unmapped: the emulator faults right after */
    for (int i = 0; i < size; i++) {
        if (njournal == capjournal) {
            capjournal = capjournal ? capjournal * 2 : 65536;
            journal = realloc(journal, capjournal * sizeof *journal);
        }
        journal[njournal].addr = (uint32_t)addr + i;
        journal[njournal].old = R8((uint32_t)addr + i);
        njournal++;
    }
}

static bool hook_fetch_unmapped(uc_engine *u, uc_mem_type type, uint64_t addr, int size, int64_t value, void *ud)
{
    (void)u; (void)type; (void)addr; (void)size; (void)value; (void)ud;
    return false;
}

/* ------------------------------------------------------------------ x87 transcendentals
   Unicorn's fsin/fcos/fpatan/... differ from real hardware in the last bits (and so do the
   branches that follow them): run those instructions on the host CPU, like the port does. */
extern const uint32_t x87_special_addr[];
extern const uint8_t x87_special_op[];
extern const int x87_special_count;
extern const uint32_t sse_special_addr[];
extern const uint8_t sse_special_len[];
extern const int sse_special_count;

/* rcpss/rcpps/rsqrtss/rsqrtps (register forms only in this program): on the host CPU */
static void hook_sse(uc_engine *u, uint64_t addr, uint32_t size, void *ud)
{
    int len = (int)(uintptr_t)ud;
    const uint8_t *b = (const uint8_t *)(uintptr_t)addr;
    int ss = b[0] == 0xf3, k = ss ? 1 : 0;
    if (b[k] != 0x0f || (b[k + 1] != 0x52 && b[k + 1] != 0x53) || (b[k + 2] >> 6) != 3)
        return;
    int rsqrt = b[k + 1] == 0x52, dst = (b[k + 2] >> 3) & 7, src = b[k + 2] & 7;
    uint8_t d[16], s_[16];
    uc_reg_read(u, UC_X86_REG_XMM0 + dst, d);
    uc_reg_read(u, UC_X86_REG_XMM0 + src, s_);
    __m128 vs = _mm_loadu_ps((const float *)s_), vd = _mm_loadu_ps((const float *)d);
    __m128 r = ss ? (rsqrt ? _mm_rsqrt_ss(vs) : _mm_rcp_ss(vs)) : (rsqrt ? _mm_rsqrt_ps(vs) : _mm_rcp_ps(vs));
    if (ss) r = _mm_move_ss(vd, r);
    _mm_storeu_ps((float *)d, r);
    uc_reg_write(u, UC_X86_REG_XMM0 + dst, d);
    uint32_t next = (uint32_t)addr + (uint32_t)len;
    uc_reg_write(u, UC_X86_REG_EIP, &next);
    (void)size;
}

static void hook_x87(uc_engine *u, uint64_t addr, uint32_t size, void *ud)
{
    uint8_t op = (uint8_t)(uintptr_t)ud;
    uint16_t sw, cw, tag;
    uc_reg_read(u, UC_X86_REG_FPSW, &sw);
    uc_reg_read(u, UC_X86_REG_FPCW, &cw);
    uc_reg_read(u, UC_X86_REG_FPTAG, &tag);
    CPU t;
    memset(&t, 0, sizeof t);
    t.ftop = (sw >> 11) & 7;
    t.fsw = sw & 0x4700;
    for (int i = 0; i < 8; i++) {
        uint8_t v[16] = {0};
        uc_reg_read(u, UC_X86_REG_FP0 + i, v);
        memcpy(&t.st[i], v, 10);
    }
    fpu_setcw(&t, cw);
    uint32_t top0 = t.ftop;
    switch (op) {
    case 0xfe: fpu_fsin(&t); break;
    case 0xff: fpu_fcos(&t); break;
    case 0xfb: fpu_fsincos(&t); break;
    case 0xf2: fpu_fptan(&t); break;
    case 0xf3: fpu_fpatan(&t); break;
    case 0xf1: fpu_fyl2x(&t); break;
    case 0xf9: fpu_fyl2xp1(&t); break;
    case 0xf0: fpu_f2xm1(&t); break;
    }
    for (int i = 0; i < 8; i++) {
        uint8_t v[16] = {0};
        memcpy(v, &t.st[i], 10);
        uc_reg_write(u, UC_X86_REG_FP0 + i, v);
    }
    if (t.ftop != top0) {
        if (((top0 - 1) & 7) == t.ftop) tag &= ~(3u << (2 * t.ftop));   /* pushed: valid */
        else tag |= 3u << (2 * top0);                                   /* popped: empty */
    }
    sw = (uint16_t)((sw & ~0x7f00u) | t.fsw | ((t.ftop & 7) << 11));
    uc_reg_write(u, UC_X86_REG_FPSW, &sw);
    uc_reg_write(u, UC_X86_REG_FPTAG, &tag);
    uint32_t next = (uint32_t)addr + size;
    uc_reg_write(u, UC_X86_REG_EIP, &next);
}

/* CPUID: the CPU the port reports (runtime cpu_cpuid), so code that picks paths by CPU features
   takes the same ones under emulation */
static int hook_cpuid(uc_engine *u, void *ud)
{
    (void)ud;
    CPU t;
    memset(&t, 0, sizeof t);
    uc_reg_read(u, UC_X86_REG_EAX, &t.eax);
    uc_reg_read(u, UC_X86_REG_ECX, &t.ecx);
    cpu_cpuid(&t);
    uc_reg_write(u, UC_X86_REG_EAX, &t.eax);
    uc_reg_write(u, UC_X86_REG_EBX, &t.ebx);
    uc_reg_write(u, UC_X86_REG_ECX, &t.ecx);
    uc_reg_write(u, UC_X86_REG_EDX, &t.edx);
    return 1;                           /* the instruction is done */
}

/* ------------------------------------------------------------------ GDT so fs: works */
static void set_gdt_entry(uint32_t at, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    uint64_t d = (limit & 0xffff) | ((uint64_t)(base & 0xffffff) << 16) | ((uint64_t)access << 40) |
                 ((uint64_t)((limit >> 16) & 0xf) << 48) | ((uint64_t)flags << 52) | ((uint64_t)(base >> 24) << 56);
    W64(at, d);
}

static void uc_setup(void)
{
    if (uc_open(UC_ARCH_X86, UC_MODE_32, &uc)) fatal("uc_open");
    /* the modules outside the arena (those inside, like Bink, are part of it) */
    for (int i = 0; i < module_count; i++) {
        const ModuleEntry *m = &module_table[i];
        if (m->base >= ARENA_START && m->base + m->size <= ARENA_END) continue;
        uint32_t sz = (m->size + 0xffff) & ~0xffffu;
        if (uc_mem_map_ptr(uc, m->base, sz, UC_PROT_ALL, (void *)(uintptr_t)m->base)) fatal("map %s", m->name);
    }
    if (uc_mem_map_ptr(uc, ARENA_START, ARENA_END - ARENA_START, UC_PROT_ALL, (void *)(uintptr_t)ARENA_START))
        fatal("map arena");
    if (uc_mem_map(uc, SENTINEL, 0x1000, UC_PROT_ALL)) fatal("map sentinel");
    uc_hook h1, h2;
    uc_hook_add(uc, &h1, UC_HOOK_MEM_WRITE, hook_write, NULL, 1, 0);
    uc_hook_add(uc, &h2, UC_HOOK_MEM_FETCH_UNMAPPED, hook_fetch_unmapped, NULL, 1, 0);
    uc_hook h3;
    if (uc_hook_add(uc, &h3, UC_HOOK_INSN, hook_cpuid, NULL, 1, 0, UC_X86_INS_CPUID))
        fatal("cpuid hook");
    for (int i = 0; i < sse_special_count; i++) {
        uc_hook h;
        uc_hook_add(uc, &h, UC_HOOK_CODE, hook_sse, (void *)(uintptr_t)sse_special_len[i],
                    sse_special_addr[i], sse_special_addr[i]);
    }
    for (int i = 0; i < x87_special_count && !getenv("FNCHECK_NO_X87_HOOK"); i++) {
        uc_hook h;
        uc_hook_add(uc, &h, UC_HOOK_CODE, hook_x87, (void *)(uintptr_t)x87_special_op[i],
                    x87_special_addr[i], x87_special_addr[i]);
    }
    /* GDT: 14 code (ring 3), 15 data (ring 3), 16 fs -> TEB, 17 stack (ring 0) */
    gdt_base = vm_alloc(0, 0x10000, 0);
    set_gdt_entry(gdt_base + 14 * 8, 0, 0xfffff, 0xfb, 0xc);
    set_gdt_entry(gdt_base + 15 * 8, 0, 0xfffff, 0xf3, 0xc);
    set_gdt_entry(gdt_base + 16 * 8, teb, 0xfff, 0xf3, 0x4);
    set_gdt_entry(gdt_base + 17 * 8, 0, 0xfffff, 0x93, 0xc);
    uc_x86_mmr gdtr = {0, gdt_base, 31 * 8 - 1, 0};
    uc_reg_write(uc, UC_X86_REG_GDTR, &gdtr);
    int sel;
    sel = 0x73; uc_reg_write(uc, UC_X86_REG_CS, &sel);
    sel = 0x7b; uc_reg_write(uc, UC_X86_REG_DS, &sel);
    sel = 0x7b; uc_reg_write(uc, UC_X86_REG_ES, &sel);
    sel = 0x83; uc_reg_write(uc, UC_X86_REG_FS, &sel);
    sel = 0x88; uc_reg_write(uc, UC_X86_REG_SS, &sel);
}

/* ------------------------------------------------------------------ random inputs */
static uint64_t rng;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

static int tame;          /* odd seeds: only small numbers and pointers into the scratch area */

static uint32_t rnd_value(void)
{
    if (tame)
        return rnd() % 3 ? scratch + (rnd() % 0xf000 & ~3u) : rnd() % 32;
    switch (rnd() % 6) {
    case 0: return 0;
    case 1: return rnd() % 16;
    case 2: return rnd() % 256;
    case 3: return scratch + (rnd() % 0x8000) * 4 % 0xf000;
    case 4: return 0x3f800000u;          /* 1.0f */
    default: return rnd();
    }
}

static void fill_scratch(void)
{
    for (uint32_t i = 0; i < 0x10000; i += 4)
        W32(scratch + i, (rnd() % 4 == 0) ? scratch + (rnd() % 0xf000 & ~3u) : rnd() % 3 == 0 ? rnd() % 64 : rnd());
}

/* ------------------------------------------------------------------ one test */
enum { R_PASS, R_SKIP_IMPORT, R_SKIP_FAULT, R_SKIP_LONG, R_FAIL };
static const char *rname[] = {"pass", "import", "fault", "long", "FAIL"};

typedef struct { uint32_t r[8]; uint32_t top; uint8_t st[8][10]; uint16_t tag; uint8_t xmm[8][16]; } State;
extern int fpu_sw_exceptions;

static int regid[8] = {UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                       UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI};

static int verbose;

static const CPU *fixed;        /* a captured state to replay instead of random inputs */

static int run_one(uint32_t fn, uint64_t seed, char *why, size_t whysz)
{
    State in = {0};
    long double fin[8] = {0};
    uint32_t ftop_in = 6, fcw_in = 0x37f, mxcsr_in = 0x1f80;
    if (fixed) {
        for (int i = 0; i < 8; i++) in.r[i] = (&fixed->eax)[i];
        for (int r = 0; r < 8; r++) memcpy(in.xmm[r], &fixed->xmm[r], 16);
        ftop_in = fixed->ftop & 7;
        for (int i = 0; i < 8; i++) fin[i] = fixed->st[i];
        fcw_in = fixed->fcw;
        mxcsr_in = fixed->mxcsr;
        W32(in.r[4], SENTINEL);         /* (its return address) */
    } else {
    rng = seed * 0x9e3779b97f4a7c15ull + fn;
    if (!rng) rng = 1;
    tame = seed & 1;
    fill_scratch();
    for (int i = 0; i < 8; i++) in.r[i] = rnd_value();
    in.r[4] = stack_top - 0x800;
    for (int i = 0; i < 16; i++)
        W32(in.r[4] + 4 + 4 * i, rnd_value());
    W32(in.r[4], SENTINEL);
    /* two values on the x87 stack (functions like _ftol take their argument there) */
    for (int i = 6; i < 8; i++)
        fin[i] = (long double)((int32_t)(rnd() % 2000000) - 1000000) / 1000.0L;
    /* SSE registers: four floats each, small and signed (as vectors and matrices hold) */
    for (int r = 0; r < 8; r++)
        for (int k = 0; k < 4; k++) {
            float f = (float)((int32_t)(rnd() % 20000) - 10000) / 100.0f;
            memcpy(in.xmm[r] + 4 * k, &f, 4);
        }
    }
    uint32_t teb_seh = R32(teb);

    /* ---- original code under emulation */
    njournal = 0;
    for (int i = 0; i < 8; i++) uc_reg_write(uc, regid[i], &in.r[i]);
    uint32_t fl = 0x202; uc_reg_write(uc, UC_X86_REG_EFLAGS, &fl);
    uint16_t cw = (uint16_t)fcw_in, sw = (uint16_t)(ftop_in << 11), tag = 0;
    for (int i = 0; i < 8; i++)                         /* phys top..7 in use */
        if (i < (int)ftop_in) tag |= (uint16_t)(3u << (2 * i));
    if (fixed)
        tag = 0xffff;                   /* a call starts with the x87 stack empty */
    for (int i = 0; i < 8; i++) {
        uint8_t v[16] = {0};
        if (i >= (int)ftop_in) memcpy(v, &fin[i], 10);
        uc_reg_write(uc, UC_X86_REG_FP0 + i, v);
    }
    uc_reg_write(uc, UC_X86_REG_FPCW, &cw);
    uc_reg_write(uc, UC_X86_REG_FPSW, &sw);
    uc_reg_write(uc, UC_X86_REG_FPTAG, &tag);
    for (int r = 0; r < 8; r++) uc_reg_write(uc, UC_X86_REG_XMM0 + r, in.xmm[r]);
    uint32_t mx = mxcsr_in; uc_reg_write(uc, UC_X86_REG_MXCSR, &mx);
    uc_err e = uc_emu_start(uc, fn, SENTINEL, 0, MAX_INSNS);
    uint32_t pc = 0;
    uc_reg_read(uc, UC_X86_REG_EIP, &pc);
    int result = R_PASS;
    if (e == UC_ERR_FETCH_UNMAPPED && thunk_name(pc)) result = R_SKIP_IMPORT;
    else if (e != UC_ERR_OK) result = R_SKIP_FAULT;
    else if (pc != SENTINEL) result = R_SKIP_LONG;
    if (fixed && result != R_PASS)
        snprintf(why, whysz, "%s at %08x%s%s", uc_strerror(e), pc, thunk_name(pc) ? " " : "",
                 thunk_name(pc) ? thunk_name(pc) : "");
    State out = {0};
    for (int i = 0; i < 8; i++) uc_reg_read(uc, regid[i], &out.r[i]);
    uc_reg_read(uc, UC_X86_REG_FPSW, &sw);
    uc_reg_read(uc, UC_X86_REG_FPTAG, &out.tag);
    out.top = (sw >> 11) & 7;
    for (int i = 0; i < 8; i++) {
        uint8_t v[16] = {0};
        uc_reg_read(uc, UC_X86_REG_FP0 + i, v);
        memcpy(out.st[i], v, 10);
    }
    for (int r = 0; r < 8; r++) uc_reg_read(uc, UC_X86_REG_XMM0 + r, out.xmm[r]);
    /* MMX use leaves the x87 top at 0 with every register tagged (or, after EMMS, empty):
       the port keeps MMX apart, so the x87 stack is not compared then */
    int used_mmx = out.top == 0 && (out.tag == 0 || out.tag == 0xffff);
    /* final values of written bytes, then undo the writes */
    size_t nw = njournal;
    Undo *writes = malloc((nw ? nw : 1) * sizeof *writes);
    for (size_t i = 0; i < nw; i++) {
        writes[i].addr = journal[i].addr;
        writes[i].old = R8(journal[i].addr);
    }
    for (size_t i = nw; i-- > 0;)
        W8(journal[i].addr, journal[i].old);
    W32(teb, teb_seh);
    if (result != R_PASS) {
        free(writes);
        return result;
    }

    /* ---- translated code in a child process */
    int pfd[2];
    if (pipe(pfd)) fatal("pipe");
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        alarm(10);
        for (int i = 0; i < 8; i++) (&cpu->eax)[i] = in.r[i];
        for (int r = 0; r < 8; r++) memcpy(&cpu->xmm[r], in.xmm[r], 16);
        cpu->mxcsr = mxcsr_in;
        _mm_setcsr(0x1f80);
        cpu->ftop = ftop_in;
        cpu->fsw = 0;
        for (int i = 0; i < 8; i++) cpu->st[i] = i >= (int)ftop_in ? fin[i] : 0;
        fpu_sw_exceptions = 0;
        fpu_setcw(cpu, (uint16_t)fcw_in);
        cpu->df = 0;
        icall(cpu, fn);
        char msg[4096];
        int n = 0;
        if (cpu->ret != SENTINEL)
            n += snprintf(msg + n, sizeof msg - n, "returned to %08x; ", cpu->ret);
        static const char *rn[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
        for (int i = 0; i < 8; i++)
            if ((&cpu->eax)[i] != out.r[i])
                n += snprintf(msg + n, sizeof msg - n, "%s %08x vs %08x; ", rn[i], (&cpu->eax)[i], out.r[i]);
        for (int r = 0; r < 8; r++)
            if (memcmp(&cpu->xmm[r], out.xmm[r], 16)) {
                float a[4], b[4];
                memcpy(a, &cpu->xmm[r], 16);
                memcpy(b, out.xmm[r], 16);
                n += snprintf(msg + n, sizeof msg - n, "xmm%d %g %g %g %g vs %g %g %g %g; ", r,
                              a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
            }
        if (used_mmx)
            ;
        else if ((cpu->ftop & 7) != out.top)
            n += snprintf(msg + n, sizeof msg - n, "fpu top %u vs %u; ", cpu->ftop & 7, out.top);
        else
            for (int p = out.top; p < 8; p++) {
                if (memcmp(&cpu->st[p], out.st[p], 10)) {
                    long double uv;
                    memset(&uv, 0, sizeof uv);
                    memcpy(&uv, out.st[p], 10);
                    n += snprintf(msg + n, sizeof msg - n, "st phys%d %.20Lg vs %.20Lg; ", p, cpu->st[p], uv);
                }
            }
        size_t bad = 0;
        for (size_t i = 0; i < nw && n < 3500; i++) {
            uint8_t want = writes[i].old;      /* the emulator's final value at this address */
            if (R8(writes[i].addr) != want) {
                if (bad++ < 8)
                    n += snprintf(msg + n, sizeof msg - n, "mem %08x %02x vs %02x; ", writes[i].addr, R8(writes[i].addr), want);
            }
        }
        if (bad > 8)
            n += snprintf(msg + n, sizeof msg - n, "(%zu bytes differ) ", bad);
        if (write(pfd[1], msg, n) < 0) {}
        _exit(n ? 1 : 0);
    }
    close(pfd[1]);
    int len = (int)read(pfd[0], why, whysz - 1);
    close(pfd[0]);
    why[len > 0 ? len : 0] = 0;
    int status;
    waitpid(pid, &status, 0);
    free(writes);
    if (WIFSIGNALED(status)) {
        snprintf(why, whysz, "translated code died with signal %d", WTERMSIG(status));
        return R_FAIL;
    }
    if (WEXITSTATUS(status) != 0)
        return R_FAIL;
    return R_PASS;
}

/* ------------------------------------------------------------------ the recorded device */
/* A capture's .dev file (runtime/devrec.c): the calls the function made into the runtime and
   their results, played back to both A/B runs in place of the runtime (main.c thunk_replay), each
   device call also folded into the draw-stream hash (runtime/drawhash.c) as in the game. A run
   that calls something else, or stops short, differs from the original's calls: reported. */
typedef struct {
    uint32_t thunk, eax, ecx, edx, dsp, npush, nw, nu, args[6];
    uint64_t hash;                      /* the draw hash after it, in the game */
    const char *name;
    uint8_t st[4][10];
    uint32_t *waddr, *wlen, *uaddr, *ulen;
    uint8_t **wdata;
} DevCall;
static DevCall *dev;
static uint32_t ndev, devpos;
static uint64_t dev_hash0;              /* the draw hash where the recording starts */
static int dev_decompiled;              /* (this run has the rewrites) */

static void dev_need(gzFile f, void *d, unsigned n)
{
    if (gzread(f, d, n) != (int)n) fatal("device recording: short file");
}

static int dev_load(const char *cap_path)
{
    char path[1300];
    snprintf(path, sizeof path, "%s.dev", cap_path);
    gzFile f = gzopen(path, "rb");
    if (!f) return 0;
    char magic[8];
    dev_need(f, magic, 8);
    if (memcmp(magic, "LRDEV001", 8)) fatal("%s: not a device recording", path);
    dev_need(f, &dev_hash0, 8);
    uint32_t nopt;
    dev_need(f, &nopt, 4);
    for (uint32_t k = 0; k < nopt; k++) {   /* (set before the runtime started: dev_options) */
        uint8_t n;
        char kv[256];
        dev_need(f, &n, 1);
        dev_need(f, kv, n);
    }
    static const char *names[512];
    int nnames = 0;
    for (;;) {
        char tag;
        dev_need(f, &tag, 1);
        if (tag == 'Z') break;
        if (tag != 'E') fatal("%s: bad record", path);
        if (!(ndev & 1023)) dev = realloc(dev, (ndev + 1024) * sizeof *dev);
        DevCall *d = &dev[ndev++];
        memset(d, 0, sizeof *d);
        uint8_t n;
        char name[256];
        dev_need(f, &d->thunk, 4);
        dev_need(f, &n, 1);
        dev_need(f, name, n);
        name[n] = 0;
        int k = 0;                      /* (one copy of each name: the draw hash looks names up by pointer first) */
        while (k < nnames && strcmp(names[k], name)) k++;
        if (k == nnames && nnames < 512) names[nnames++] = strdup(name);
        d->name = k < 512 ? names[k] : strdup(name);
        dev_need(f, &d->eax, 4); dev_need(f, &d->ecx, 4); dev_need(f, &d->edx, 4);
        dev_need(f, &d->dsp, 4); dev_need(f, &d->npush, 4);
        for (uint32_t j = 0; j < d->npush; j++) dev_need(f, d->st[j], 10);
        dev_need(f, &d->nw, 4);
        d->waddr = malloc(d->nw * 4 + 4); d->wlen = malloc(d->nw * 4 + 4); d->wdata = malloc(d->nw * sizeof *d->wdata + 8);
        for (uint32_t j = 0; j < d->nw; j++) {
            dev_need(f, &d->waddr[j], 4); dev_need(f, &d->wlen[j], 4);
            d->wdata[j] = malloc(d->wlen[j]);
            dev_need(f, d->wdata[j], d->wlen[j]);
        }
        dev_need(f, &d->nu, 4);
        d->uaddr = malloc(d->nu * 4 + 4); d->ulen = malloc(d->nu * 4 + 4);
        for (uint32_t j = 0; j < d->nu; j++) { dev_need(f, &d->uaddr[j], 4); dev_need(f, &d->ulen[j], 4); }
        dev_need(f, d->args, sizeof d->args);
        dev_need(f, &d->hash, 8);
    }
    gzclose(f);
    printf("device recording %s: %u calls\n", path, ndev);
    return 1;
}

/* the recording's options, set before the runtime starts (its hooks read them at start-up) */
static void dev_options(const char *cap_path)
{
    char path[1300];
    snprintf(path, sizeof path, "%s.dev", cap_path);
    gzFile f = gzopen(path, "rb");
    if (!f) return;
    char magic[8];
    uint64_t h;
    uint32_t nopt;
    if (gzread(f, magic, 8) == 8 && !memcmp(magic, "LRDEV001", 8) && gzread(f, &h, 8) == 8 &&
        gzread(f, &nopt, 4) == 4)
        for (uint32_t k = 0; k < nopt; k++) {
            uint8_t n;
            char kv[256];
            if (gzread(f, &n, 1) != 1 || gzread(f, kv, n) != n) break;
            kv[n] = 0;
            if (strncasecmp(kv, "decomp=", 7))     /* (not the switch for the rewrites: --ab sets them) */
                option_parse(kv);
        }
    gzclose(f);
}

static const char *dev_name(uint32_t t)
{
    return devpos < ndev && dev[devpos].thunk == t ? dev[devpos].name : NULL;
}

/* Where a recorded write goes in this run. The recording holds the addresses the original wrote
   (a surface's GetDesc into its caller's local...); one in the stack goes where this run's
   pointer argument points instead: a caller below a rewrite runs at other depths (the tidied
   rewrites keep no stack layout of the original's), so its locals are elsewhere. The argument is
   the nearest pointer at or below the address. */
static uint32_t dev_write_addr(CPU *c, const DevCall *d, uint32_t a)
{
    if (a < c->stack_lo || a >= c->stack_hi)
        return a;
    int best = -1;
    for (int k = 0; k < 6 && 4 + 4u * (uint32_t)k < d->dsp; k++) {
        uint32_t p = d->args[k];
        if (p >= c->stack_lo && p < c->stack_hi && p <= a && a - p < 0x1000
            && (best < 0 || p > d->args[best]))
            best = k;
    }
    return best < 0 ? a : R32(c->esp + 4 + 4u * (uint32_t)best) + (a - d->args[best]);
}

static int dev_replay(CPU *c, uint32_t t)
{
    if (devpos >= ndev || dev[devpos].thunk != t) {
        fprintf(stderr, "  calls differ at call %u of %u: this run calls %08x, the original called %s\n",
                devpos + 1, ndev, t, devpos < ndev ? dev[devpos].name : "nothing more");
        _exit(4);
    }
    static long dump_at = -2;           /* FNCHECK_DUMP=N: memory before call N, as SHOLD_DEVREC_DUMP */
    if (dump_at == -2) dump_at = getenv("FNCHECK_DUMP") ? atol(getenv("FNCHECK_DUMP")) : -1;
    if (devpos + 1 == (uint32_t)dump_at) {
        extern void mem_dump(const char *path);
        char p[64];
        snprintf(p, sizeof p, "fncheck_dump_%s.bin", dev_decompiled ? "decompiled" : "translated");
        mem_dump(p);
    }
    DevCall *d = &dev[devpos++];
    /* the translated run is the original on the original's data: its arguments must be the
       recorded ones exactly; the first that aren't is where the playback drifted */
    static int drift_told;
    if (!dev_decompiled && !drift_told)
        for (int j = 0; j < 6 && 4 + 4u * (uint32_t)j < d->dsp; j++)     /* (its real arguments) */
            if (R32(c->esp + 4 + 4u * (uint32_t)j) != d->args[j]) {
                fprintf(stderr, "  (playback drift: call %u, %s, argument %d is %08x, recorded %08x)\n",
                        devpos, d->name, j, R32(c->esp + 4 + 4u * (uint32_t)j), d->args[j]);
                drift_told = 1;
                break;
            }
    for (uint32_t j = 0; j < d->nw; j++)
        memcpy(gptr(dev_write_addr(c, d, d->waddr[j])), d->wdata[j], d->wlen[j]);
    for (uint32_t j = 0; j < d->nu; j++)
        draw_hash_bytes(gptr(d->uaddr[j]), d->ulen[j]);
    /* the draw hash after the call, against the game's: the first call where the translated run's
       differs is where its data drifted from the game's (the draws themselves, which the arguments'
       pointers only show by address) */
    if (!drift_told && draw_hash != d->hash) {
        if (dev_decompiled)
            fprintf(stderr, "  draws differ from the original's from call %u of %u, %s\n", devpos, ndev, d->name);
        else
            fprintf(stderr, "  (playback drift: call %u, %s: what the original draws differs from the game's)\n", devpos, d->name);
        drift_told = 1;
    }
    c->eax = d->eax; c->ecx = d->ecx; c->edx = d->edx;
    c->ret = R32(c->esp);
    c->esp += d->dsp;
    for (uint32_t j = d->npush; j-- > 0; ) {
        f80 v;
        memcpy(&v, d->st[j], 10);
        FPUSH(v);
    }
    return 1;
}

/* ------------------------------------------------------------------ native A/B (--ab) */
/* one run in a child: the pages (image and regions) it changed, sent as (address, 4096 bytes),
   then the registers; the parent still holds the starting state */
static CPU cap_cpu;                 /* the captured registers (capture_load) */
#define PG 4096u
static void ab_child(uint32_t fn, int with_decomp, int fd)
{
    extern void decomp_install(void);
    typedef struct { uint32_t addr; int (**hook)(CPU *); int (*fn)(CPU *); } DecompEntry;
    extern const DecompEntry __start_decomp_table[] __attribute__((weak)), __stop_decomp_table[] __attribute__((weak));
    /* (the setup's game_init already switched the DECOMP rewrites in: the translated run goes without) */
    for (const DecompEntry *d = __start_decomp_table; d < __stop_decomp_table; d++)
        *d->hook = NULL;
    extern void decomp_stackfix_install(void);
    decomp_stackfix_install();          /* (the stack-leftover fixes, in both runs) */
    if (with_decomp) decomp_install();
    uint32_t nreg = (uint32_t)mem_region_count() + 1, *rb = malloc(nreg * 4), *rs = malloc(nreg * 4);
    rb[0] = IMAGE_BASE; rs[0] = IMAGE_SIZE;
    for (uint32_t i = 1; i < nreg; i++) mem_region((int)i - 1, &rb[i], &rs[i]);
    size_t npages = 0;
    for (uint32_t i = 0; i < nreg; i++) npages += rs[i] / PG;
    uint32_t *h0 = malloc(npages * 4);
    size_t k = 0;
    for (uint32_t i = 0; i < nreg; i++)
        for (uint32_t p = 0; p < rs[i] / PG; p++)
            h0[k++] = (uint32_t)crc32(0, (const Bytef *)(uintptr_t)(rb[i] + p * PG), PG);
    *cpu = cap_cpu;
    cpu->fs_base = teb;
    W32(cpu->esp, SENTINEL);
    fpu_setcw(cpu, cap_cpu.fcw);
    cpu_ldmxcsr(cpu, cap_cpu.mxcsr);    /* (the game's SSE modes too: rounding, flush to zero) */
    if (ndev) {
        extern int (*thunk_replay)(CPU *c, uint32_t t);
        extern const char *(*thunk_replay_name)(uint32_t t);
        thunk_replay = dev_replay;
        thunk_replay_name = dev_name;
        api_via_thunk = 1;              /* (imports called directly come through their thunks too) */
        draw_hashing = 1;
        draw_hash = dev_hash0;
        devpos = 0;
        dev_decompiled = with_decomp;
    }
    alarm(60);
    icall(cpu, fn);
    if (ndev && devpos != ndev) {
        fprintf(stderr, "  calls differ: this run made %u of the original's %u calls (the next: %s)\n",
                devpos, ndev, dev[devpos].name);
        _exit(4);
    }
    k = 0;
    for (uint32_t i = 0; i < nreg; i++)
        for (uint32_t p = 0; p < rs[i] / PG; p++, k++) {
            uint32_t a = rb[i] + p * PG;
            if ((uint32_t)crc32(0, (const Bytef *)(uintptr_t)a, PG) != h0[k]) {
                if (write(fd, &a, 4) != 4 || write(fd, (void *)(uintptr_t)a, PG) != (ssize_t)PG) _exit(3);
            }
        }
    uint32_t end = 0;
    if (write(fd, &end, 4) != 4 || write(fd, cpu, sizeof *cpu) != (ssize_t)sizeof *cpu ||
        write(fd, &draw_hash, 8) != 8) _exit(3);
    _exit(0);
}

typedef struct { uint32_t n; uint32_t *addr; uint8_t *data; CPU regs; uint64_t draws; int ok; } AbRun;

static void ab_read(int fd, AbRun *r)
{
    FILE *f = fdopen(fd, "rb");
    r->n = 0; r->addr = NULL; r->data = NULL; r->ok = 0;
    for (;;) {
        uint32_t a;
        if (fread(&a, 4, 1, f) != 1) return;
        if (!a) break;
        r->addr = realloc(r->addr, (r->n + 1) * 4);
        r->data = realloc(r->data, (size_t)(r->n + 1) * PG);
        if (fread(r->data + (size_t)r->n * PG, PG, 1, f) != 1) return;
        r->addr[r->n++] = a;
    }
    r->ok = fread(&r->regs, sizeof r->regs, 1, f) == 1 && fread(&r->draws, 8, 1, f) == 1;
    fclose(f);
}

static const uint8_t *ab_page(const AbRun *r, uint32_t a)
{
    for (uint32_t i = 0; i < r->n; i++)
        if (r->addr[i] == a) return r->data + (size_t)i * PG;
    return (const uint8_t *)(uintptr_t)a;           /* unchanged: the starting state */
}

static int ab_check(uint32_t fn)
{
    AbRun run[2];
    for (int w = 0; w < 2; w++) {
        int pfd[2];
        if (pipe(pfd)) fatal("pipe");
        pid_t pid = fork();
        if (pid == 0) { close(pfd[0]); ab_child(fn, w, pfd[1]); }
        close(pfd[1]);
        ab_read(pfd[0], &run[w]);
        int st;
        waitpid(pid, &st, 0);
        if (!run[w].ok && ndev && WIFEXITED(st) && WEXITSTATUS(st) == 4) {
            printf("%08x: %s (its calls differ from the %s: above)\n", fn,
                   w ? "DIFFER" : "PLAYBACK FAILED", w ? "original's" : "recording; a tool problem");
            return 1;
        }
        if (!run[w].ok) {
            printf("%08x: the %s run did not finish (%s %d)\n", fn, w ? "decompiled" : "translated",
                   WIFSIGNALED(st) ? "signal" : "exit", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
            return 1;
        }
    }
    int bad = 0;
    static const char *rn[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    for (int i = 0; i < 8; i++)
        if ((&run[0].regs.eax)[i] != (&run[1].regs.eax)[i]) {
            /* ecx and edx are the caller's to lose (a rewrite needn't leave the original's), unless
               the function returns a 64-bit value in edx: noted, not counted */
            int scratch = i == 1 || i == 2;
            printf("  %s %08x (translated) vs %08x (decompiled)%s\n", rn[i], (&run[0].regs.eax)[i],
                   (&run[1].regs.eax)[i], scratch ? " (scratch: not counted)" : "");
            bad += !scratch;
        }
    if (run[0].regs.ret != run[1].regs.ret) { printf("  returned to %08x vs %08x\n", run[0].regs.ret, run[1].regs.ret); bad++; }
    if (ndev && run[0].draws != run[1].draws) {
        printf("  draws differ (draw hash %016llx translated vs %016llx decompiled; SHOLD_DRAW_TRACE in a game run finds the call)\n",
               (unsigned long long)run[0].draws, (unsigned long long)run[1].draws);
        bad++;
    }
    /* the x87 stack: what the function leaves pushed (a float result in ST0), bit for bit */
    if (run[0].regs.ftop != run[1].regs.ftop) {
        printf("  x87 depth %u vs %u\n", run[0].regs.ftop, run[1].regs.ftop);
        bad++;
    } else {
        uint32_t pushed = (cap_cpu.ftop - run[0].regs.ftop) & 7;
        if (pushed > 4)                 /* (it popped: nothing of its own left to compare) */
            pushed = 0;
        for (uint32_t k = 0, n = pushed; k < n; k++) {
            uint32_t at = (run[0].regs.ftop + k) & 7;
            if (memcmp(&run[0].regs.st[at], &run[1].regs.st[at], 10)) {
                printf("  ST%u %Lg (translated) vs %Lg (decompiled)\n", k, run[0].regs.st[at], run[1].regs.st[at]);
                bad++;
            }
        }
    }
    /* a callee-cleanup function's arguments, popped on return, which it may have used as scratch */
    uint32_t args_lo = cap_cpu.esp + 4, args_hi = run[0].regs.esp;
    size_t bytes = 0, changed = 0;
    for (int w = 0; w < 2; w++)
        for (uint32_t i = 0; i < run[w].n; i++) {
            uint32_t a = run[w].addr[i];
            if (w == 1 && ab_page(&run[0], a) != (const uint8_t *)(uintptr_t)a) continue;  /* done */
            const uint8_t *p0 = ab_page(&run[0], a), *p1 = ab_page(&run[1], a);
            for (uint32_t b = 0; b < PG; b++) {
                /* the stack below the entry: the function's own frame, which a rewrite keeping its
                   locals in C doesn't write the same (runtime/decomp/HOWTO.md) */
                if (a + b >= cap_cpu.stack_lo && a + b < cap_cpu.esp)
                    continue;
                if (a + b >= args_lo && a + b < args_hi)
                    continue;
                if (p0[b] != ((const uint8_t *)(uintptr_t)a)[b] || p1[b] != ((const uint8_t *)(uintptr_t)a)[b]) changed++;
                if (p0[b] != p1[b] && bytes++ < 16)
                    printf("  mem %08x %02x (translated) vs %02x (decompiled)\n", a + b, p0[b], p1[b]);
            }
        }
    printf("%08x: %s (%zu bytes changed; %zu differ%s)\n", fn, bad || bytes ? "DIFFER" : "same", changed, bytes,
           ndev ? (run[0].draws == run[1].draws ? "; draws the same" : "; draws differ") : "");
    return bad || bytes;
}

/* ------------------------------------------------------------------ captured states */

static void need(gzFile f, void *d, unsigned n)
{
    if (gzread(f, d, n) != (int)n) fatal("capture: short file");
}

static uint32_t capture_load(const char *path)
{
    gzFile f = gzopen(path, "rb");
    if (!f) fatal("cannot open %s", path);
    char magic[8];
    uint32_t h[4];
    need(f, magic, 8);
    need(f, h, sizeof h);
    if (memcmp(magic, "LRCAP001", 8) || h[2] != sizeof(CPU))
        fatal("%s: not a capture of this build", path);
    need(f, &cap_cpu, sizeof cap_cpu);
    for (;;) {
        uint32_t r[2];
        need(f, r, 8);
        if (!r[1]) break;
        if (r[0] != IMAGE_BASE)
            vm_alloc(r[0], r[1], 0);    /* (for the region table; the arena is all writable) */
        need(f, (void *)(uintptr_t)r[0], r[1]);
    }
    gzclose(f);
    printf("capture %s: %08x at frame %u (fcw %04x, mxcsr %04x)\n", path, h[0], h[1], cap_cpu.fcw, cap_cpu.mxcsr);
    return h[0];
}


int main(int argc, char **argv)
{
    int seeds = 4;
    uint32_t only = 0;
    const char *capture_path = NULL;
    int ab = 0;
    const char *home = getenv("HOME");
    snprintf(user_dir, 1024, "%s/.local/share/stranglehold-native", home);
    snprintf(game_dir, 1024, "%s/.local/share/stranglehold-native/game", home);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) snprintf(game_dir, 1024, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) snprintf(user_dir, 1024, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--seeds") && i + 1 < argc) seeds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capture_path = argv[++i];
        else if (!strcmp(argv[i], "--ab")) ab = 1;
    }
    log_mask = 0;
    deterministic = 1;                  /* the game as shipped: no Enhanced hooks, no mods */
    extern int frame_step_pinned;
    frame_step_pinned = 1;              /* the rewrites' drawn-frame step at 1, as the translation
                                           has it (game.c; the recordings run at frame_rate 60) */
    if (capture_path)
        dev_options(capture_path);      /* (the recorded run's options, before the runtime starts) */
    /* options that change what the rewrites do where the translation can only do the original,
       at original after the recording's (as tests/scene.sh's ab_pins, record.sh) */
    static const char *const pins[] = {
        "hud_layout=original",          /* the HUD's margins (game.c hud_native) */
        "sun=original",                 /* the light's direction (decomp/shaders.c, shadowmap.c) */
        "shadow_distance=original",     /* the shadow map's ranges (decomp/shadowmap.c) */
    };
    for (size_t k = 0; k < sizeof pins / sizeof *pins; k++)
        option_parse(pins[k]);
    runtime_setup();
    log_mask = LOG_ALWAYS;
    cpu = cpu_new(0x100000);
    set_current_cpu(cpu);
    fpu_activate(cpu);
    extern void dlls_attach(CPU *c);
    dlls_attach(cpu);                   /* the C runtime, MFC and Bink set up, as in the game */
    teb = cpu->fs_base;
    stack_top = cpu->stack_hi - 0x10000;
    uint32_t cap_fn = capture_path ? capture_load(capture_path) : 0;
    if (cap_fn)
        dev_load(capture_path);         /* (the calls it makes into the runtime, if recorded) */
    scratch = vm_alloc(0, 0x10000, 0);
    uc_setup();
    if (cap_fn && ab)
        return ab_check(cap_fn);
    if (cap_fn) {
        fixed = &cap_cpu;
        char why[4096] = "";
        int r = run_one(cap_fn, 0, why, sizeof why);
        printf("%08x: %s %s\n", cap_fn, rname[r], why);
        return r == R_FAIL;
    }
    int count[5] = {0};
    uint32_t lo = 0, hi = 0xffffffffu;
    if (getenv("FNCHECK_RANGE"))            /* "lo-hi": only functions in that address range */
        sscanf(getenv("FNCHECK_RANGE"), "%x-%x", &lo, &hi);
    for (int k = 0; k < func_count; k++) {
        uint32_t fn = func_table[k].addr;
        if (only && fn != only) continue;
        if (fn < lo || fn > hi) continue;
        int worst = -1;
        char why[4096] = "";
        for (int s = 0; s < seeds; s++) {
            char w[4096];
            int r = run_one(fn, s + 1, w, sizeof w);
            count[r]++;
            if (r == R_FAIL && worst != R_FAIL) {
                worst = R_FAIL;
                snprintf(why, sizeof why, "seed %d: %s", s + 1, w);
            } else if (r == R_PASS && worst < 0) worst = R_PASS;
            if (verbose)
                printf("%08x seed %d: %s %s\n", fn, s + 1, rname[r], r == R_FAIL ? w : "");
        }
        if (worst == R_FAIL)
            printf("FAIL %08x: %s\n", fn, why);
        fflush(stdout);
    }
    printf("runs: %d pass, %d skipped (import %d, fault %d, long %d), %d FAIL\n", count[R_PASS],
           count[1] + count[2] + count[3], count[1], count[2], count[3], count[R_FAIL]);
    return count[R_FAIL] ? 1 : 0;
}
