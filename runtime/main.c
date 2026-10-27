/* Start-up: map the user's LARush.exe and the DLLs translated with it at their own addresses,
   connect their imports (to the runtime or to each other), run each DLL's entry and then the
   program's on the main thread. Also: calls through pointers, callbacks into the program, and
   the error paths of the generated code. */
#define _GNU_SOURCE
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "runtime.h"
#include "image.h"
#include "input.h"
#include "options.h"
#include "pad.h"

unsigned log_mask = LOG_ALWAYS;
char game_dir[1024];
char user_dir[1024];
uint32_t image_base = IMAGE_BASE;
CPU *main_cpu;
int headless = 1;
int hidden_window;                      /* --hidden: the window DXVK needs, never shown (tests) */
static FILE *logfp;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec t_start;

/* ------------------------------------------------------------------ logging */
/* Test mode (--test): a virtual clock that moves 1/60 s per presented frame and a microsecond
   per read (so busy-waits still end), timers run on the main thread at frame ends, nothing
   depends on the host's timing, and two runs with the same scripted input match exactly. */
int deterministic;
static double vclock;

static double real_ms(void);
double host_ms(void) { return real_ms(); }      /* the host's clock, whatever --test does */

static double real_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t_start.tv_sec) * 1000.0 + (t.tv_nsec - t_start.tv_nsec) / 1e6;
}

double now_ms(void)
{
    if (deterministic) {
        vclock += 0.001;
        return vclock;
    }
    return real_ms();
}

/* With a log file everything goes there; LOG_ALWAYS messages are also echoed to stderr. */
static void vlog(unsigned cat, const char *fmt, va_list ap)
{
    char buf[4096];
    vsnprintf(buf, sizeof buf, fmt, ap);
    CPU *c = current_cpu();
    pthread_mutex_lock(&log_lock);
    double t = deterministic ? vclock : real_ms();
    int th = c ? c->thread_index : -1;
    if (logfp) {
        fprintf(logfp, "%9.3f [%d] %s\n", t / 1000.0, th, buf);
        fflush(logfp);
    }
    if (!logfp || cat == LOG_ALWAYS)
        fprintf(stderr, "%9.3f [%d] %s\n", t / 1000.0, th, buf);
    pthread_mutex_unlock(&log_lock);
}

void logmsg(unsigned cat, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(cat, fmt, ap);
    va_end(ap);
}

static void dump_state(CPU *c)
{
    if (!c)
        return;
    logmsg(LOG_ALWAYS, "  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x",
           c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    if (c->esp >= ARENA_START && c->esp < ARENA_END) {
        char line[256];
        int n = 0;
        for (int i = 0; i < 12; i++)
            n += snprintf(line + n, sizeof line - n, " %08x", R32(c->esp + 4 * i));
        logmsg(LOG_ALWAYS, "  stack:%s", line);
    }
    void *bt[64];
    int nb = backtrace(bt, 64);
    char **sym = backtrace_symbols(bt, nb);
    for (int i = 0; i < nb && sym; i++) {
        const char *p = strstr(sym[i], "(F_");
        const char *q = strstr(sym[i], "(w_");
        if (p || q || i < 6)
            logmsg(LOG_ALWAYS, "  at %s", sym[i]);
    }
    free(sym);
}

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logmsg(LOG_ALWAYS, "FATAL: %s", buf);
    dump_state(current_cpu());
    if (logfp)
        fflush(logfp);
    _exit(2);
}

/* ------------------------------------------------------------------ calls through pointers */
/* each module's code ranges and the translated function at each address */
typedef struct { uint32_t lo, hi; void (**fns)(CPU *); } TextRange;
static TextRange text_ranges[8 * MODULE_COUNT];
static int ntext_ranges;
static void (*thunk_fns[THUNK_MAX])(CPU *);
static const char *thunk_names[THUNK_MAX];
static int nthunks;
static pthread_mutex_t thunk_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t thunk_addr(void (*fn)(CPU *c), const char *name)
{
    pthread_mutex_lock(&thunk_lock);
    for (int i = 0; i < nthunks; i++)
        if (thunk_fns[i] == fn) {
            pthread_mutex_unlock(&thunk_lock);
            return THUNK_BASE + 16u * i;
        }
    if (nthunks >= (int)THUNK_MAX)
        fatal("out of thunks");
    thunk_fns[nthunks] = fn;
    thunk_names[nthunks] = name;
    uint32_t a = THUNK_BASE + 16u * nthunks++;
    pthread_mutex_unlock(&thunk_lock);
    return a;
}

const char *thunk_name(uint32_t a)
{
    if (a - THUNK_BASE < 16u * nthunks && !(a & 15))
        return thunk_names[(a - THUNK_BASE) >> 4];
    return NULL;
}

/* The reference build (ref_cpu.c) runs the original code in an emulator instead. */
void (*icall_hook)(CPU *c, uint32_t target);

/* LOG_DRAW: every Direct3D device call with its first arguments */
static void thunk_trace(CPU *c, uint32_t t)
{
    const char *n = thunk_names[(t - THUNK_BASE) >> 4];
    if (n && !strncmp(n, "IDirect3DDevice9::", 18))
        LOG(LOG_DRAW, "%s(%x, %x, %x, %x, %x)", n + 18, R32(c->esp + 8), R32(c->esp + 12),
            R32(c->esp + 16), R32(c->esp + 20), R32(c->esp + 24));
    if (n && !strcmp(n, "IDirect3DDevice9::SetVertexShaderConstantF"))
        for (uint32_t i = 0, p = R32(c->esp + 12); i < R32(c->esp + 16) && i < 8; i++, p += 16) {
            float v[4];
            memcpy(v, gptr(p), 16);
            LOG(LOG_DRAW, "    c%u = %g %g %g %g", R32(c->esp + 8) + i, v[0], v[1], v[2], v[3]);
        }
}

/* The offline checker (tests/fncheck.c) plays recorded calls back in place of the runtime
   (devrec.c): thunk_replay does the call, thunk_replay_name names the thunk (its own tables are
   empty there). */
int (*thunk_replay)(CPU *c, uint32_t t);
const char *(*thunk_replay_name)(uint32_t t);
void devrec_call_begin(CPU *c, uint32_t t), devrec_call_end(CPU *c, uint32_t t, const char *name);

static int is_thunk(uint32_t t)
{
    return !(t & 15) && t - THUNK_BASE < 16u * (thunk_replay ? THUNK_MAX : (uint32_t)nthunks);
}

/* a thunk's call: traced (LOG_DRAW), hashed (--hash), recorded (devrec.c) or played back */
static void run_thunk(CPU *c, uint32_t t)
{
    const char *n = thunk_replay ? thunk_replay_name(t) : thunk_names[(t - THUNK_BASE) >> 4];
    if ((log_mask & LOG_DRAW) && !thunk_replay)
        thunk_trace(c, t);
    if (draw_hashing && n && !strncmp(n, "IDirect3DDevice9::", 18))
        draw_hash_call(c, n + 18);
    if (thunk_replay) {
        thunk_replay(c, t);
        return;
    }
    if (dev_recording) devrec_call_begin(c, t);
    thunk_fns[(t - THUNK_BASE) >> 4](c);
    if (dev_recording) devrec_call_end(c, t, n);
}

int call_thunk(CPU *c, uint32_t t)
{
    if (!is_thunk(t))
        return 0;
    run_thunk(c, t);
    return 1;
}

void icall(CPU *c, uint32_t t)
{
    if (icall_hook) {
        icall_hook(c, t);
        return;
    }
    for (int i = 0; i < ntext_ranges; i++) {
        const TextRange *r = &text_ranges[i];
        if (t - r->lo < r->hi - r->lo) {
            void (*fn)(CPU *) = r->fns[t - r->lo];
            if (fn) {
                fn(c);
                return;
            }
            fatal("call to %08x: code the translator did not find as a function (add it to "
                  "EXTRA_ENTRIES), return address %08x", t, R32(c->esp));
        }
    }
    if (is_thunk(t)) {
        run_thunk(c, t);
        return;
    }
    fatal("call to %08x: not code (return address %08x)", t, R32(c->esp));
}

#define SENTINEL 0x7efe0000u

uint32_t guest_callv(CPU *c, uint32_t fn, int nargs, const uint32_t *args)
{
    uint32_t keep[8] = {c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi};
    uint32_t keep_ret = c->ret, keep_df = c->df, keep_fl = c->eflags;
    uint32_t esp = c->esp - 64;      /* leave the caller's outgoing area alone */
    for (int i = nargs - 1; i >= 0; i--) {
        esp -= 4;
        W32(esp, args[i]);
    }
    esp -= 4;
    W32(esp, SENTINEL);
    c->esp = esp;
    c->df = 0;
    icall(c, fn);
    if (c->ret != SENTINEL)
        fatal("callback %08x returned to %08x", fn, c->ret);
    uint32_t r = c->eax;
    c->eax = keep[0]; c->ecx = keep[1]; c->edx = keep[2]; c->ebx = keep[3];
    c->esp = keep[4]; c->ebp = keep[5]; c->esi = keep[6]; c->edi = keep[7];
    c->ret = keep_ret;
    c->df = keep_df;
    c->eflags = keep_fl;
    return r;
}

uint32_t guest_call(CPU *c, uint32_t fn, int nargs, ...)
{
    uint32_t args[16];
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++)
        args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return guest_callv(c, fn, nargs, args);
}

/* ------------------------------------------------------------------ error paths */
void bad_ret(CPU *c, uint32_t site, uint32_t expected)
{
    fatal("call at %08x returned to %08x instead of %08x", site, c->ret, expected);
}
void bad_jump(CPU *c, uint32_t site, uint32_t target)
{
    (void)c;
    fatal("jump table at %08x went to %08x, not a known case", site, target);
}
void nonlocal_jump(CPU *c, uint32_t site, uint32_t target)
{
    (void)c;
    fatal("exception/longjmp continuation at %08x to %08x is not supported yet", site, target);
}
void unsupported(CPU *c, uint32_t site, const char *what)
{
    (void)c;
    fatal("instruction at %08x not translated: %s", site, what);
}
void trap_int3(CPU *c, uint32_t site)
{
    (void)c;
    fatal("int3 at %08x", site);
}
void div_fault(CPU *c, uint32_t site)
{
    (void)c;
    fatal("integer divide fault at %08x", site);
}
void fell_off(CPU *c, uint32_t site)
{
    (void)c;
    fatal("ran past the end of translated code after %08x", site);
}
void returned_from_noreturn(CPU *c, uint32_t site)
{
    (void)c;
    fatal("call at %08x to a function that never returns came back", site);
}
void missing_import(CPU *c, const char *dll, const char *name)
{
    fatal("%s!%s is not implemented (called from %08x)", dll, name, R32(c->esp));
}

/* ------------------------------------------------------------------ threads' CPU records */
static __thread CPU *tls_cpu;
CPU *current_cpu(void) { return tls_cpu; }
void set_current_cpu(CPU *c) { tls_cpu = c; }
static int thread_counter;

CPU *cpu_new(uint32_t stack_size)
{
    CPU *c = aligned_alloc(64, (sizeof(CPU) + 63) & ~63u);
    memset(c, 0, sizeof *c);
    if (stack_size < 0x10000)
        stack_size = 0x10000;
    uint32_t stack = vm_alloc(0, stack_size, 0);
    if (!stack)
        fatal("no memory for a thread stack");
    c->stack_lo = stack;
    c->stack_hi = stack + stack_size;
    extern void mem_mark_stack(uint32_t base);
    mem_mark_stack(stack);              /* (left out of memory hashes) */
    uint32_t teb = galloc(0x1000);
    memset(gptr(teb), 0, 0x1000);
    W32(teb + 0x00, 0xffffffffu);        /* end of the SEH chain */
    W32(teb + 0x04, c->stack_hi);
    W32(teb + 0x08, c->stack_lo);
    W32(teb + 0x18, teb);
    c->fs_base = teb;
    c->esp = c->stack_hi - 16;
    c->thread_index = __atomic_fetch_add(&thread_counter, 1, __ATOMIC_SEQ_CST);
    W32(teb + 0x24, 0x100 + 4 * c->thread_index);
    fpu_new(c);                          /* the host FPU is the caller's: not touched here */
    c->fcw = 0x27f;                      /* what Windows gives a new thread */
    return c;
}

/* ------------------------------------------------------------------ loading the modules */
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

const ModuleEntry *module_by_name(const char *name)
{
    const char *b = strrchr(name, '\\');
    b = b ? b + 1 : name;
    size_t n = strlen(b);
    for (int i = 0; i < module_count; i++) {
        const char *m = module_table[i].name;
        size_t k = strlen(m);
        /* "MFC71.dll", "mfc71.DLL" or "mfc71" */
        if ((n == k && !strcasecmp(b, m)) || (n + 4 == k && !strncasecmp(b, m, n) && !strcasecmp(m + n, ".dll")))
            return &module_table[i];
    }
    return NULL;
}

const ModuleEntry *module_at(uint32_t a)
{
    for (int i = 0; i < module_count; i++)
        if (a - module_table[i].base < module_table[i].size)
            return &module_table[i];
    return NULL;
}

/* One module at its own base, laid out as Windows maps it (the translation reads nothing from
   it: the code is the generated C; data, resources and tables come from here). */
static void load_module(const ModuleEntry *me)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", game_dir, me->file);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        fatal("cannot open %s: %s", path, strerror(errno));
    struct stat st;
    fstat(fd, &st);
    uint8_t *f = malloc(st.st_size);
    if (read(fd, f, st.st_size) != st.st_size)
        fatal("cannot read %s", path);
    close(fd);
    uint32_t pe = rd32(f + 0x3c);
    if (memcmp(f + pe, "PE\0\0", 4))
        fatal("%s is not a Windows program", path);
    uint16_t nsec = rd16(f + pe + 6), optsz = rd16(f + pe + 20);
    const uint8_t *opt = f + pe + 24;
    if (rd32(opt + 28) != me->base || rd32(opt + 56) != me->size)
        fatal("%s is not the %s this build was translated from (%s)", path, me->name, BUILD_NAME);
    uint32_t size = (me->size + 0xffff) & ~0xffffu;
    void *m;
    if (me->base >= ARENA_START && me->base + size <= ARENA_END) {
        /* inside the program's memory: taken from it like any allocation at a chosen address */
        if (vm_alloc(me->base, size, 0) != me->base)
            fatal("cannot place %s at %08x", me->name, me->base);
        m = (void *)(uintptr_t)me->base;
        memset(m, 0, size);
    } else {
        m = mmap((void *)(uintptr_t)me->base, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (m != (void *)(uintptr_t)me->base)
            fatal("cannot map %s at %08x: %s", me->name, me->base, strerror(errno));
    }
    uint32_t hdrsize = rd32(opt + 60);
    memcpy(m, f, hdrsize < (uint32_t)st.st_size ? hdrsize : (uint32_t)st.st_size);
    const uint8_t *sh = opt + optsz;
    for (int i = 0; i < nsec; i++, sh += 40) {
        uint32_t vs = rd32(sh + 8), va = rd32(sh + 12), rs = rd32(sh + 16), ra = rd32(sh + 20);
        uint32_t n = rs < vs || !vs ? rs : vs;
        if (ra + n > (uint32_t)st.st_size)
            fatal("damaged section in %s", path);
        memcpy((uint8_t *)m + va, f + ra, n);
    }
    free(f);
}

/* An imported variable (MSVC name "?name@@3..." or a static member "?name@class@@0-2...") from a DLL
   that is not translated (wxWidgets: wxDefaultSize, wxAppConsole::ms_appInitFn, which a start-up
   initialiser writes): the slot points at a block of program memory instead of a thunk. wx's
   default size and position are (-1, -1). */
static int is_data_import(const char *n)
{
    const char *at = n[0] == '?' ? strstr(n, "@@") : NULL;
    return at && at[2] >= '0' && at[2] <= '4';
}

static void connect_imports(void)
{
    for (int i = 0; i < import_count; i++) {
        const ImportEntry *e = &import_table[i];
        if (is_data_import(e->name)) {
            uint32_t b = galloc(64);
            memset(gptr(b), !strncmp(e->name, "?wxDefault", 10) ? 0xff : 0, 64);
            W32(e->iat, b);
            LOG(LOG_API, "%s!%s: a variable at %08x", e->dll, e->name, b);
            continue;
        }
        char *name = malloc(strlen(e->dll) + strlen(e->name) + 2);
        sprintf(name, "%s!%s", e->dll, e->name);
        W32(e->iat, thunk_addr(e->fn, name));
    }
    for (int i = 0; i < link_count; i++)
        W32(link_table[i].iat, link_table[i].target);
}

static void build_function_index(void)
{
    for (int k = 0; k < module_count; k++)
        for (int j = 0; j < module_table[k].ntext; j++) {
            TextRange *r = &text_ranges[ntext_ranges++];
            r->lo = module_table[k].text[j][0];
            r->hi = module_table[k].text[j][1];
            r->fns = calloc(r->hi - r->lo, sizeof *r->fns);
        }
    for (int i = 0; i < func_count; i++) {
        uint32_t a = func_table[i].addr;
        for (int j = 0; j < ntext_ranges; j++)
            if (a - text_ranges[j].lo < text_ranges[j].hi - text_ranges[j].lo)
                text_ranges[j].fns[a - text_ranges[j].lo] = func_table[i].fn;
    }
}

/* ------------------------------------------------------------------ crashes */
static __attribute__((unused)) void on_signal(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    static volatile int in;
    if (in++)
        _exit(3);
    fatal("signal %d (%s) at address %p", sig, strsignal(sig), si->si_addr);
}

void process_exit(int code)
{
    LOG(LOG_ALWAYS, "program exited with code %d", code);
    settings_save();
    if (logfp)
        fflush(logfp);
    exit(code);
}

extern void sync_init(void);
extern void files_init(void);

/* Everything before the program's entry point runs: memory, the image, imports, devices. */
void runtime_setup(void)
{
    mem_init();
    for (int i = 0; i < module_count; i++)
        load_module(&module_table[i]);
    build_function_index();
    connect_imports();
    extern void sysdlls_map(void);
    sysdlls_map();
    sync_init();
    files_init();
    settings_load();
    win_init();
    pad_init();
    dsound_init();
    dinput_init();
    extern void game_init(void);
    game_init();
}

/* DLL_PROCESS_ATTACH for each DLL, in the order the build lists them (the C runtime first) */
void dlls_attach(CPU *c)
{
    for (int i = 1; i < module_count; i++) {
        const ModuleEntry *m = &module_table[i];
        if (!m->entry)
            continue;
        uint32_t r = guest_call(c, m->entry, 3, m->base, 1u, 0u);
        LOG(LOG_ALWAYS, "%s started (%u)", m->name, r);
        if (!r)
            fatal("%s failed to start", m->name);
    }
}

/* ------------------------------------------------------------------ main */
int run_frames_limit;
static unsigned long dump_frame, dump_call;
static char dump_path[1024];
double run_seconds_limit;
unsigned long frame_count;
static unsigned long wait_frames;       /* input_wait's frames */

extern void timers_run_due(double now);
static FILE *hashfp;
static unsigned long hash_every = 1;
unsigned long api_calls;
int api_trace_on;
static unsigned long trace_lo, trace_hi;
static FILE *tracefp;

/* --hash-calls FROM TO FILE: a memory hash after each runtime call in that range, with the
   call site, to find the first call after which two runs differ. */
void api_trace(CPU *c)
{
    if (dump_call && api_calls == dump_call) {
        mem_dump(dump_path);
        LOG(LOG_ALWAYS, "memory after call %lu (to %08x) written to %s", api_calls, c->ret, dump_path);
    }
    if (!tracefp || api_calls < trace_lo || api_calls > trace_hi)
        return;
    fprintf(tracefp, "%lu %08x %016llx\n", api_calls, c->ret, (unsigned long long)mem_hash());
    if (api_calls == trace_hi)
        fflush(tracefp);
}

/* FRAME LIMIT (Enhanced; options.txt frame_limit = off | screen | <fps>; not in test runs): a frame
   waits for its time before the game goes on (its input read after the wait, so what the next frame
   shows is as fresh as it can be). "screen" is 3 under the screen's refresh: with variable refresh
   (G-Sync, FreeSync) frames then stay inside the range the screen follows, never waiting for vsync.
   The wait sleeps to half a millisecond before and spins the rest (Windows' timers are not that
   exact). */
static double limit_ms;
static int limit_read;

void frame_limit_options(void) { limit_read = 0; }

int frame_limit_screen(void)
{
    int hz = host_refresh_hz();
    return hz > 33 ? hz - 3 : hz;
}

static void frame_limit(void)
{
    static double next;
    if (deterministic)
        return;
    if (!limit_read) {
        const char *v = option_str("frame_limit", "off");
        int fps = !strcasecmp(v, "screen") ? frame_limit_screen() : atoi(v);
        limit_ms = fps >= 10 ? 1000.0 / fps : 0;
        limit_read = 1;
        if (fps >= 10)
            LOG(LOG_ALWAYS, "frame limit: %d fps", fps);
    }
    /* SHOLD_FRAME_TIMES: each 300 frames, the average and slowest frame time without the
       limiter's wait (what the frame rate would be uncapped) */
    static int ft_on = -1;
    static double ft_after, ft_sum, ft_max;
    static int ft_n;
    if (ft_on < 0) ft_on = getenv("SHOLD_FRAME_TIMES") != NULL;
    if (ft_on) {
        double t = real_ms();
        if (ft_after > 0) {
            double d = t - ft_after;
            ft_sum += d;
            if (d > ft_max) ft_max = d;
            if (++ft_n == 300) {
                LOG(LOG_ALWAYS, "frames %lu: %.2f ms a frame (%.0f fps uncapped), slowest %.1f ms",
                    frame_count, ft_sum / ft_n, 1000.0 * ft_n / ft_sum, ft_max);
                ft_n = 0; ft_sum = ft_max = 0;
            }
        }
    }
    if (limit_ms <= 0) {
        if (ft_on) ft_after = real_ms();
        return;
    }
    double now = real_ms();
    if (next > now) {
        if (next - now > 1.0) {
            struct timespec ts = {0, (long)((next - now - 0.5) * 1e6)};
            nanosleep(&ts, NULL);
        }
        while ((now = real_ms()) < next)
            __builtin_ia32_pause();
    }
    next = (next > now - limit_ms ? next : now) + limit_ms;    /* (a frame late: from now) */
    if (ft_on) ft_after = real_ms();
}

/* SHOLD_POKE="addr=value[@frame],..." (hex, the frame decimal): dwords written into the
   program's memory every frame, from that frame on (experiments: flags found with REA) */
static void pokes(void)
{
    static int n = -1;
    static uint32_t at[16], val[16];
    static unsigned long from[16];
    if (n < 0) {
        n = 0;
        const char *e = getenv("SHOLD_POKE");
        for (const char *p = e; p && *p && n < 16; ) {
            char *q;
            at[n] = strtoul(p, &q, 16);
            if (*q != '=') break;
            val[n] = strtoul(q + 1, &q, 16);
            from[n] = *q == '@' ? strtoul(q + 1, &q, 10) : 0;
            LOG(LOG_ALWAYS, "poke %08x = %08x every frame from frame %lu", at[n], val[n], from[n]);
            n++;
            p = *q == ',' ? q + 1 : q;
        }
    }
    for (int i = 0; i < n; i++)
        if (frame_count >= from[i])
            W32(at[i], val[i]);
}

void game_frame(unsigned long frame);   /* game.c */

void frame_presented(void)
{
    frame_count++;
    extern void det_yield(void);
    det_yield();                        /* --test: the other threads' turn (sync.c) */
    pokes();
    {   /* SHOLD_CPU_FROM=<frame>: the game thread's CPU time a frame from then, logged every 400 frames
           (timing changes on the game thread without its waits; work/cpu_far.sh) */
        static long from = -2;
        static double t0;
        if (from == -2)
            from = getenv("SHOLD_CPU_FROM") ? atol(getenv("SHOLD_CPU_FROM")) : -1;
        if (from >= 0 && frame_count >= (unsigned long)from) {
            struct timespec ts;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
            double t = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
            if (frame_count == (unsigned long)from)
                t0 = t;
            else if ((frame_count - (unsigned long)from) % 400 == 0)
                LOG(LOG_ALWAYS, "game thread CPU %.3f ms a frame over %lu frames", (t - t0) / (frame_count - (unsigned long)from),
                    frame_count - (unsigned long)from);
        }
    }
    {   /* SHOLD_LOG_FRAME=<n>: the d3d and draw logs for that one frame */
        static long log_frame = -2;
        static unsigned saved_mask;
        if (log_frame == -2) {
            const char *e = getenv("SHOLD_LOG_FRAME");
            log_frame = e && *e ? atol(e) : -1;
        }
        if (log_frame >= 0 && frame_count == (unsigned long)log_frame) {
            saved_mask = log_mask;
            log_mask |= LOG_D3D | LOG_DRAW;
        } else if (log_frame >= 0 && frame_count == (unsigned long)log_frame + 1)
            log_mask = saved_mask;
    }
    if (deterministic) {
        /* SHOLD_TEST_FPS=<fps>[@<frame>]: the virtual clock's frame time from that frame on (60 fps
           otherwise: a race as a faster or slower machine plays it, still the same every run) */
        static double vstep = -1;
        static unsigned long vstep_from;
        if (vstep < 0) {
            const char *e = getenv("SHOLD_TEST_FPS");
            const char *at = e ? strchr(e, '@') : NULL;
            vstep = e && atof(e) > 0 ? 1000.0 / atof(e) : 1000.0 / 60.0;
            vstep_from = at ? strtoul(at + 1, NULL, 10) : 0;
        }
        vclock += frame_count >= vstep_from ? vstep : 1000.0 / 60.0;
        timers_run_due(vclock);
    }
    {
        /* SHOLD_PACE=<fps>: frames no faster than that in real time, as with vsync (timing a race
           as it is played: the CPU and GPU share the APU's power, which a flat-out run does not; and
           real-time runs off-screen, which have no vsync, at a monitor's rate) */
        static double pace_ms = -1, pace_next;
        if (pace_ms < 0) {
            const char *e = getenv("SHOLD_PACE");
            pace_ms = e && atof(e) > 0 ? 1000.0 / atof(e) : 0;
        }
        if (pace_ms > 0) {
            double now = real_ms();
            if (pace_next > now) {
                struct timespec ts = {0, (long)((pace_next - now) * 1e6)};
                nanosleep(&ts, NULL);
            }
            pace_next = (pace_next > now - pace_ms ? pace_next : now) + pace_ms;
        }
    }
    frame_limit();
    if (hashfp && frame_count % hash_every == 0) {
        fprintf(hashfp, "%lu %lu %016llx %016llx\n", frame_count, api_calls, (unsigned long long)mem_hash(),
                (unsigned long long)draw_hash);
        fflush(hashfp);
    }
    if (dump_frame && frame_count >= dump_frame) {
        /* SHOLD_DUMP_COUNT=N: N frames running from FRAME, each to FILE.<frame> */
        static long count = -1;
        if (count < 0) count = getenv("SHOLD_DUMP_COUNT") ? atol(getenv("SHOLD_DUMP_COUNT")) : 0;
        if (frame_count == dump_frame || frame_count < dump_frame + (unsigned long)count) {
            char p[1200];
            if (count > 0) snprintf(p, sizeof p, "%s.%lu", dump_path, frame_count);
            else snprintf(p, sizeof p, "%s", dump_path);
            mem_dump(p);
            LOG(LOG_ALWAYS, "memory of frame %lu written to %s", frame_count, p);
        }
    }
    {
        /* SHOLD_PEEK=addr[:n],... (hex; tests): those n dwords (default 1) logged every frame */
        static int np = -1;
        static uint32_t pa[16], pn[16];
        if (np < 0) {
            np = 0;
            for (const char *e = getenv("SHOLD_PEEK"); e && *e && np < 16; ) {
                char *q;
                pa[np] = (uint32_t)strtoul(e, &q, 16);
                pn[np] = *q == ':' ? (uint32_t)strtoul(q + 1, &q, 10) : 1;
                np++;
                e = *q == ',' ? q + 1 : q;
            }
        }
        if (np) {
            char line[2048];
            int o = snprintf(line, sizeof line, "peek %lu", frame_count);
            for (int k = 0; k < np; k++)
                for (uint32_t j = 0; j < pn[k] && o < (int)sizeof line - 40; j++) {
                    uint32_t w = R32(pa[k] + 4 * j);
                    float f;
                    memcpy(&f, &w, 4);
                    o += snprintf(line + o, sizeof line - o, " %08x=%08x(%g)", pa[k] + 4 * j, w, f);
                }
            LOG(LOG_ALWAYS, "%s", line);
        }
    }
    if (run_frames_limit && frame_count >= (unsigned long)run_frames_limit) {
        LOG(LOG_ALWAYS, "frame limit %d reached", run_frames_limit);
        process_exit(0);
    }
    if (run_seconds_limit > 0 && (deterministic ? vclock : real_ms()) > run_seconds_limit * 1000.0) {
        LOG(LOG_ALWAYS, "time limit reached after %lu frames", frame_count);
        process_exit(0);
    }
    game_frame(frame_count + wait_frames);
    input_frame(frame_count + wait_frames);
    win_pump();
}

void frame_presented_idle(void)
{
    if (run_seconds_limit > 0 && (deterministic ? vclock : real_ms()) > run_seconds_limit * 1000.0) {
        LOG(LOG_ALWAYS, "time limit reached after %lu frames (waiting for messages)", frame_count);
        process_exit(0);
    }
}

/* The program reads its devices without presenting frames while it waits for a key (the
   controls menus' "press the control for ..." busy-waits on DirectInput): host events and
   scripted input keep coming as if frames passed - every 1/60 s, or with --test every 60 reads
   (no clock moves there). Scripted frame numbers count these too. */
void input_wait(void)
{
    static unsigned long reads, frame_seen;
    static double last;
    if (current_cpu() != main_cpu)
        return;
    if (frame_seen != frame_count) {
        frame_seen = frame_count;
        reads = 0;
        last = deterministic ? 0 : real_ms();
        return;
    }
    if (deterministic ? ++reads < 60 : real_ms() - last < 1000.0 / 60.0)
        return;
    reads = 0;
    last = deterministic ? 0 : real_ms();
    wait_frames++;
    LOG(LOG_INPUT, "input: the program waits on its devices (frame %lu + %lu)", frame_count, wait_frames);
    game_frame(frame_count + wait_frames);
    input_frame(frame_count + wait_frames);
    win_pump();
}

#ifndef RUNTIME_NO_MAIN
static void usage(void)
{
    fprintf(stderr,
            "usage: stranglehold [--game DIR] [--user DIR] [--log LIST] [--window|--fullscreen] [--frames N] [--seconds S]\n"
            "              [--test] [--hash FILE] [--hash-every N] [--dump-memory FRAME FILE]\n"
            "              [--option KEY=VALUE]   (see options.txt in the user folder)\n"
            "  --test  deterministic run: virtual clock, timers at frame ends, no real-time sound\n"
            "  --log  comma list: api,file,d3d,ddraw,sound,input,win,mem,thread,reg,draw,all\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *cli_opts[16];
    int ncli = 0;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
#ifdef _WIN32
    const char *home = getenv("APPDATA");           /* %APPDATA%\stranglehold-native, '/' separated */
    snprintf(user_dir, sizeof user_dir, "%s/stranglehold-native", home ? home : ".");
    for (char *p = user_dir; *p; p++)
        if (*p == '\\') *p = '/';
#else
    const char *home = getenv("HOME");
    snprintf(user_dir, sizeof user_dir, "%s/.local/share/stranglehold-native", home ? home : ".");
#endif
    snprintf(game_dir, sizeof game_dir, "%s/game", user_dir);
    const char *logpath = NULL;
    /* Started without arguments (the applications-menu entry, a double click) it plays: in a window
       (options.txt's display decides full screen), the log in the user folder. With arguments it is
       a test run, headless unless --window. */
    static char default_log[1100];
    if (argc == 1) {
        headless = 0;
        snprintf(default_log, sizeof default_log, "%s/last-run.log", user_dir);
        logpath = default_log;
    }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--game") && v) { snprintf(game_dir, sizeof game_dir, "%s", v); i++; }
        else if (!strcmp(a, "--user") && v) { snprintf(user_dir, sizeof user_dir, "%s", v); i++; }
        else if (!strcmp(a, "--logfile") && v) { logpath = v; i++; }
        else if (!strcmp(a, "--window")) headless = 0;
        else if (!strcmp(a, "--hidden")) { headless = 0; hidden_window = 1; }    /* (a window never shown) */
        else if (!strcmp(a, "--fullscreen")) { extern int start_fullscreen; headless = 0; start_fullscreen = 1; }
        else if (!strcmp(a, "--test")) deterministic = 1;
        else if (!strcmp(a, "--option") && v) { cli_opts[ncli++ & 15] = v; i++; }
        else if (!strcmp(a, "--hash") && v) {
            hashfp = fopen(v, "w");
            draw_hashing = 1;
            if (!hashfp) fatal("cannot write %s", v);
            i++;
        }
        else if (!strcmp(a, "--hash-calls") && i + 3 < argc) {
            trace_lo = strtoul(argv[i + 1], NULL, 10);
            trace_hi = strtoul(argv[i + 2], NULL, 10);
            tracefp = fopen(argv[i + 3], "w");
            if (!tracefp) fatal("cannot write %s", argv[i + 3]);
            api_trace_on = 1;
            i += 3;
        }
        else if (!strcmp(a, "--dump-memory-call") && i + 2 < argc) {
            dump_call = strtoul(argv[i + 1], NULL, 10);
            snprintf(dump_path, sizeof dump_path, "%s", argv[i + 2]);
            api_trace_on = 1;
            i += 2;
        }
        else if (!strcmp(a, "--hash-every") && v) { hash_every = strtoul(v, NULL, 10); if (!hash_every) hash_every = 1; i++; }
        else if (!strcmp(a, "--dump-memory") && i + 2 < argc) {
            dump_frame = strtoul(argv[i + 1], NULL, 10);
            snprintf(dump_path, sizeof dump_path, "%s", argv[i + 2]);
            i += 2;
        }
        else if (!strcmp(a, "--frames") && v) { run_frames_limit = atoi(v); i++; }
        else if (!strcmp(a, "--seconds") && v) { run_seconds_limit = atof(v); i++; }
        else if (!strcmp(a, "--log") && v) {
            static const struct { const char *n; unsigned m; } cats[] = {
                {"api", LOG_API}, {"file", LOG_FILE}, {"d3d", LOG_D3D}, {"ddraw", LOG_DDRAW},
                {"sound", LOG_SOUND}, {"input", LOG_INPUT}, {"win", LOG_WIN}, {"mem", LOG_MEM},
                {"thread", LOG_THREAD}, {"reg", LOG_REG}, {"draw", LOG_DRAW}, {"all", 0xffffu}};
            char tmp[256];
            snprintf(tmp, sizeof tmp, "%s", v);
            for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ","))
                for (size_t k = 0; k < sizeof cats / sizeof *cats; k++)
                    if (!strcmp(t, cats[k].n))
                        log_mask |= cats[k].m;
            i++;
        } else
            usage();
    }
    mkdir(user_dir, 0755);
    if (logpath) {
        logfp = fopen(logpath, "w");
        if (!logfp)
            fatal("cannot write %s", logpath);
    }
    struct sigaction sa = {0};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    options_load();
    for (int k = 0; k < ncli && k < 16; k++)
        option_parse(cli_opts[k]);
    if (deterministic) {
        extern int fpu_sw_exceptions;
        fpu_sw_exceptions = 0;          /* the reference emulator does not model them */
    }
    extern void prof_init(void);
    prof_init();
    runtime_setup();
    logmsg(LOG_ALWAYS, "Stranglehold native (%s): %d functions, game folder %s", BUILD_NAME, func_count, game_dir);

    CPU *c = cpu_new(STACK_RESERVE);
    main_cpu = c;
    set_current_cpu(c);
    fpu_activate(c);
    dlls_attach(c);
    c->esp -= 4;
    W32(c->esp, SENTINEL);
    icall(c, IMAGE_ENTRY);
    process_exit((int)c->eax);
}
#endif
