/* RECORDED DEVICE for the offline checker (tests/fncheck.c --capture FILE --ab): while a captured
   function runs (capture.c, from its entry to its return), every call it makes into the runtime
   through a thunk (Direct3D, Windows, DirectInput...) is logged to FILE.dev: which thunk, its
   name, what it returned (eax, ecx, edx, the stack it popped, anything it left on the x87 stack),
   the bytes it changed behind the pointers among its first eight arguments, and the buffer
   uploads it made (their guest bytes, which the draw-stream hash counts: d3d9.c). fncheck plays
   them back to both its runs in place of the runtime, so a function that draws can be checked
   without the game or a device. Only the capturing thread's calls are logged, and only the
   outermost (a callback's own calls run inside the logged one). When a call waits and the other
   threads take their turns (--test's scheduler, sync.c: the streaming loader finishing a texture,
   say), the memory pages they changed meanwhile are logged as that call's writes too.

   File (gzip): "LRDEV001", then per call 'E', thunk, name length (byte) and name, eax, ecx, edx,
   esp change, x87 values pushed (count, then each as 10 bytes), writes (count; address, length,
   bytes each), uploads (count; address, length each), then its first six stack arguments as
   passed and the draw-stream hash after it (to find where a playback drifts); 'Z' ends it. The
   header has the draw hash at the start and the options in force (count, then "key=value" each
   with a length byte), which fncheck sets before its runtime starts: the runtime's own hooks
   (the projection's aspect, the HUD's scale, split screen) depend on them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "runtime.h"
#include "image.h"

int dev_recording;
int api_via_thunk;                      /* (cpu.h API_CALL) */
static gzFile rf;
static uint32_t rec_fn, rec_esp;
static CPU *rec_cpu;
static int depth;
static uint32_t ncalls;                 /* (the calls recorded so far) */

#define ARGS 8
#define SPAN 512
static struct { uint32_t addr, len; uint8_t before[SPAN]; } snap[ARGS];
static int nsnap;
static uint32_t esp0, ftop0, args0[10];
struct D3DOutSpec { const char *name; unsigned short out[10]; };
extern const struct D3DOutSpec d3d_out_specs[];     /* (tools/d3d9gen.py) */

/* a Direct3D method's output parameters' sizes, by its thunk's name */
static const unsigned short *out_spec(const char *name)
{
    static const char *last;
    static const unsigned short *last_spec;
    if (!name) return NULL;
    if (name == last) return last_spec;
    last = name;
    last_spec = NULL;
    for (int i = 0; d3d_out_specs[i].name; i++)
        if (!strcmp(d3d_out_specs[i].name, name)) { last_spec = d3d_out_specs[i].out; break; }
    return last_spec;
}
static uint32_t up_addr[64], up_len[64];
static int nup;
static uint32_t al_addr[256], al_len[256];      /* zero-filled blocks made during the call */
static int nal;

int mem_span(uint32_t addr, uint32_t *end);     /* mem.c */

/* the other threads' turns during a call: page checksums before, the changed pages after */
#define PAGE 4096u
static uint32_t *pg_addr;              /* (address, checksum) pairs */
static size_t npg, cap_pg;
static uint32_t *ow_addr;               /* pages they changed, logged with the call */
static size_t now_, cap_ow;

static void pages_each(void (*fn)(uint32_t a))
{
    uint32_t b, s;
    for (uint32_t a = IMAGE_BASE; a < IMAGE_BASE + IMAGE_SIZE; a += PAGE) fn(a);
    for (int k = 0, n = mem_region_count(); k < n; k++) {
        mem_region(k, &b, &s);
        for (uint32_t a = b; a + PAGE <= b + s; a += PAGE) fn(a);
    }
}
static void page_snap(uint32_t a)
{
    if (npg == cap_pg) {
        cap_pg = cap_pg ? cap_pg * 2 : 65536;
        pg_addr = realloc(pg_addr, cap_pg * 8);
    }
    pg_addr[2 * npg] = a;
    pg_addr[2 * npg++ + 1] = (uint32_t)crc32(0, (const Bytef *)gptr(a), PAGE);
}
static int pg_cmp_addr(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static void page_cmp(uint32_t a)
{
    size_t lo = 0, hi = npg;                /* (the snapshot sorted by address: a binary search) */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (pg_addr[2 * mid] < a) lo = mid + 1; else hi = mid;
    }
    if (lo < npg && pg_addr[2 * lo] == a && pg_addr[2 * lo + 1] == (uint32_t)crc32(0, (const Bytef *)gptr(a), PAGE))
        return;
    if (now_ == cap_ow) { cap_ow = cap_ow ? cap_ow * 2 : 64; ow_addr = realloc(ow_addr, cap_ow * 4); }
    ow_addr[now_++] = a;
}
void devrec_others_begin(void)
{
    if (current_cpu() != rec_cpu) return;
    npg = 0;
    pages_each(page_snap);
    qsort(pg_addr, npg, 8, pg_cmp_addr);
}
void devrec_others_end(void)
{
    if (current_cpu() != rec_cpu) return;
    if (!depth) {
        LOG(LOG_ALWAYS, "devrec: other threads ran outside a recorded call: the playback may drift");
        return;
    }
    pages_each(page_cmp);
}

static void put(const void *d, unsigned n) { if (gzwrite(rf, d, n) != (int)n) fatal("devrec: write failed"); }
static void put32(uint32_t v) { put(&v, 4); }

void devrec_start(const char *cap_path, uint32_t fn, CPU *c)
{
    char path[1300];
    snprintf(path, sizeof path, "%s.dev", cap_path);
    rf = gzopen(path, "wb1");
    if (!rf) { LOG(LOG_ALWAYS, "devrec: cannot write %s", path); return; }
    put("LRDEV001", 8);
    draw_hashing = 1;                   /* (the running draw hash goes with each call) */
    put(&draw_hash, 8);
    extern int option_count(void);
    extern const char *option_key(int i), *option_value(int i);
    put32((uint32_t)option_count());
    for (int i = 0; i < option_count(); i++) {
        char kv[300];
        snprintf(kv, sizeof kv, "%s=%s", option_key(i), option_value(i));
        uint8_t n = (uint8_t)(strlen(kv) > 255 ? 255 : strlen(kv));
        put(&n, 1);
        put(kv, n);
    }
    rec_fn = fn;
    rec_esp = c->esp;
    rec_cpu = c;
    depth = 0;
    ncalls = 0;
    dev_recording = 1;
    api_via_thunk = 1;
}

/* the captured function returned (its generated entry, after the call) */
void capture_return(CPU *c, uint32_t fn)
{
    if (!dev_recording || fn != rec_fn || c != rec_cpu || c->esp <= rec_esp)
        return;
    put("Z", 1);
    gzclose(rf);
    dev_recording = 0;
    api_via_thunk = 0;
    LOG(LOG_ALWAYS, "devrec: %08x's calls recorded", fn);
}

void devrec_call_begin(CPU *c, uint32_t t)
{
    (void)t;
    if (current_cpu() != rec_cpu || depth++)
        return;
    /* SHOLD_DEVREC_DUMP=N: all of memory just before the Nth recorded call (to compare with
       fncheck's FNCHECK_DUMP=N where a playback drifts) */
    static long dump_at = -2;
    if (dump_at == -2) dump_at = getenv("SHOLD_DEVREC_DUMP") ? atol(getenv("SHOLD_DEVREC_DUMP")) : -1;
    if (++ncalls == (uint32_t)dump_at) {
        extern void mem_dump(const char *path);
        mem_dump("devrec_dump.bin");
        LOG(LOG_ALWAYS, "devrec: memory before call %ld dumped to devrec_dump.bin", dump_at);
    }
    esp0 = c->esp;
    ftop0 = c->ftop;
    now_ = 0;
    for (int i = 0; i < 10; i++) args0[i] = R32(c->esp + 4 + 4u * (uint32_t)i);
    nsnap = nup = nal = 0;
    for (int i = 0; i < ARGS; i++) {
        uint32_t v = R32(c->esp + 4 + 4u * (uint32_t)i), end;
        if (v < 0x10000 || !mem_span(v, &end))
            continue;
        uint32_t len = end - v < SPAN ? end - v : SPAN;
        snap[nsnap].addr = v;
        snap[nsnap].len = len;
        memcpy(snap[nsnap].before, gptr(v), len);
        nsnap++;
    }
}

void devrec_alloc(uint32_t addr, uint32_t len)
{
    if (dev_recording && depth >= 1 && current_cpu() == rec_cpu && nal < 256 && len < (16u << 20)) {
        al_addr[nal] = addr;
        al_len[nal++] = len;
    }
}

void devrec_upload(uint32_t addr, uint32_t len)
{
    if (dev_recording && depth == 1 && nup < 64) {
        up_addr[nup] = addr;
        up_len[nup++] = len;
    }
}

void devrec_call_end(CPU *c, uint32_t t, const char *name)
{
    if (current_cpu() != rec_cpu || --depth)
        return;
    uint8_t n = name ? (uint8_t)(strlen(name) > 255 ? 255 : strlen(name)) : 0;
    put("E", 1);
    put32(t);
    put(&n, 1);
    put(name ? name : "", n);
    put32(c->eax); put32(c->ecx); put32(c->edx);
    put32(c->esp - esp0);
    uint32_t pushed = (ftop0 - c->ftop) & 7;
    if (pushed > 4) pushed = 0;
    put32(pushed);
    for (uint32_t k = 0; k < pushed; k++)
        put(&c->st[(c->ftop + k) & 7], 10);
    /* the changed runs behind the argument pointers */
    uint32_t nw = 0;
    for (int i = 0; i < nsnap; i++)
        for (uint32_t b = 0; b < snap[i].len; ) {
            if (snap[i].before[b] == R8(snap[i].addr + b)) { b++; continue; }
            uint32_t e = b;
            while (e < snap[i].len && snap[i].before[e] != R8(snap[i].addr + e)) e++;
            nw++;
            b = e;
        }
    /* a Direct3D method's outputs, whole: what it wrote may equal what was there */
    const unsigned short *os = out_spec(name);
    uint32_t nout = 0;
    if (os)
        for (int k = 1; k < 10; k++) {          /* (parameter k-1 is argument k: 0 is the object) */
            uint32_t end;
            if (os[k - 1] && args0[k] >= 0x10000 && mem_span(args0[k], &end) && end - args0[k] >= os[k - 1])
                nout++;
        }
    put32(nw + (uint32_t)now_ + nout + (uint32_t)nal);
    for (int i = 0; i < nal; i++) {         /* (blocks it made: whole) */
        put32(al_addr[i]);
        put32(al_len[i]);
        put(gptr(al_addr[i]), al_len[i]);
    }
    for (size_t k = 0; k < now_; k++) {     /* (the other threads' pages first: the call's own writes win) */
        put32(ow_addr[k]);
        put32(PAGE);
        put(gptr(ow_addr[k]), PAGE);
    }
    for (int i = 0; i < nsnap; i++)
        for (uint32_t b = 0; b < snap[i].len; ) {
            if (snap[i].before[b] == R8(snap[i].addr + b)) { b++; continue; }
            uint32_t e = b;
            while (e < snap[i].len && snap[i].before[e] != R8(snap[i].addr + e)) e++;
            put32(snap[i].addr + b);
            put32(e - b);
            put(gptr(snap[i].addr + b), e - b);
            b = e;
        }
    if (os)
        for (int k = 1; k < 10; k++) {
            uint32_t end;
            if (os[k - 1] && args0[k] >= 0x10000 && mem_span(args0[k], &end) && end - args0[k] >= os[k - 1]) {
                put32(args0[k]);
                put32(os[k - 1]);
                put(gptr(args0[k]), os[k - 1]);
            }
        }
    put32((uint32_t)nup);
    for (int i = 0; i < nup; i++) { put32(up_addr[i]); put32(up_len[i]); }
    put(args0, 24);
    put(&draw_hash, 8);
}
