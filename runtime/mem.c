/* Memory the program sees: everything below 2 GB in one reserved arena.
   VirtualAlloc-style regions (64 KB granular, zero-filled) and Windows-style heaps on top.
   Allocation is deterministic: the same calls give the same addresses on every run. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "runtime.h"

static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;
#define GRAN 0x10000u

/* ------------------------------------------------------------------ regions */
typedef struct { uint32_t base, size; } Region;
static Region *regions;
static int nregions, cap_regions;

void mem_init(void)
{
    void *m = mmap((void *)(uintptr_t)ARENA_START, ARENA_END - ARENA_START, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != (void *)(uintptr_t)ARENA_START)
        fatal("cannot reserve program memory at %08x: %s", ARENA_START, strerror(errno));
    process_heap = heap_create(0, 0);
}

static int region_find(uint32_t a)
{
    int lo = 0, hi = nregions - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        Region *r = &regions[mid];
        if (a < r->base) hi = mid - 1;
        else if (a >= r->base + r->size) lo = mid + 1;
        else return mid;
    }
    return -1;
}

static void region_insert(uint32_t base, uint32_t size)
{
    if (nregions == cap_regions) {
        cap_regions = cap_regions ? cap_regions * 2 : 256;
        regions = realloc(regions, cap_regions * sizeof *regions);
    }
    int i = nregions;
    while (i > 0 && regions[i - 1].base > base) {
        regions[i] = regions[i - 1];
        i--;
    }
    regions[i].base = base;
    regions[i].size = size;
    nregions++;
}

static uint32_t region_place(uint32_t size)
{
    uint32_t cand = ARENA_START;
    for (int i = 0; i < nregions; i++) {
        if (regions[i].base >= cand + size)
            break;
        uint32_t end = (regions[i].base + regions[i].size + GRAN - 1) & ~(GRAN - 1);
        if (end > cand)
            cand = end;
    }
    if ((uint64_t)cand + size > ARENA_END)
        return 0;
    return cand;
}

uint32_t vm_alloc(uint32_t addr, uint32_t size, int reserve_only)
{
    (void)reserve_only;
    if (!size)
        return 0;
    pthread_mutex_lock(&mem_lock);
    uint32_t r = 0;
    if (addr) {
        int k = region_find(addr);
        if (k >= 0) {
            /* commit inside a reserved region: memory is already usable */
            if (addr + size <= regions[k].base + regions[k].size)
                r = addr & ~0xfffu;
        } else {
            uint32_t base = addr & ~(GRAN - 1);
            uint32_t sz = ((addr + size + 0xfff) & ~0xfffu) - base;
            int clash = 0;
            for (int i = 0; i < nregions; i++)
                if (regions[i].base < base + sz && base < regions[i].base + regions[i].size)
                    clash = 1;
            if (!clash && base >= ARENA_START && (uint64_t)base + sz <= ARENA_END) {
                region_insert(base, sz);
                r = base;
            }
        }
    } else {
        uint32_t sz = (size + 0xfff) & ~0xfffu;
        uint32_t base = region_place(sz);
        if (base) {
            region_insert(base, sz);
            r = base;
        }
    }
    pthread_mutex_unlock(&mem_lock);
    LOG(LOG_MEM, "vm_alloc(%08x, %x) -> %08x", addr, size, r);
    return r;
}

/* A moved table (TABLES in the build config) ends right before a page the program cannot
   touch: one entry too many stops the game with a memory fault instead of overwriting whatever
   follows. The page lies in the gap regions leave before the next 64 KB boundary, so no region
   gets it and memory hashes and dumps never read it. */
uint32_t vm_alloc_table(uint32_t bytes)
{
    uint32_t size = (bytes + 0xfff) & ~0xfffu;
    if (!(size & 0xffff))
        size += 0x1000;
    uint32_t mem = vm_alloc(0, size, 0);
    if (!mem)
        return 0;
    mprotect((void *)(uintptr_t)(mem + size), 0x1000, PROT_NONE);
    return mem + size - bytes;
}

int vm_free(uint32_t addr, uint32_t size, int release)
{
    pthread_mutex_lock(&mem_lock);
    int k = region_find(addr);
    int ok = 0;
    if (k >= 0) {
        if (release) {
            if (regions[k].base == addr) {
                madvise((void *)(uintptr_t)addr, regions[k].size, MADV_DONTNEED);
                memmove(&regions[k], &regions[k + 1], (nregions - k - 1) * sizeof *regions);
                nregions--;
                ok = 1;
            }
        } else {
            uint32_t a = addr & ~0xfffu;
            uint32_t e = size ? (addr + size + 0xfff) & ~0xfffu : regions[k].base + regions[k].size;
            madvise((void *)(uintptr_t)a, e - a, MADV_DONTNEED);
            ok = 1;
        }
    }
    pthread_mutex_unlock(&mem_lock);
    LOG(LOG_MEM, "vm_free(%08x, %x, %d) -> %d", addr, size, release, ok);
    return ok;
}

uint32_t vm_query_base(uint32_t addr)
{
    pthread_mutex_lock(&mem_lock);
    int k = region_find(addr);
    uint32_t r = k >= 0 ? regions[k].base : 0;
    pthread_mutex_unlock(&mem_lock);
    return r;
}

/* ------------------------------------------------------------------ heaps
   Blocks carry an 8-byte header in program memory: [size of previous block][size | flags].
   Free blocks hold next/prev links of their bin list in their first 8 bytes. */
#define F_USED 1u
#define F_LAST 2u
#define NBINS 96
#define MAXHEAPS 256

typedef struct {
    int used;
    uint32_t bins[NBINS];
    uint32_t segs[1024];
    int nsegs;
} Heap;
static Heap heaps[MAXHEAPS];
uint32_t process_heap;

#define HEAP_ID(h) ((h) - 0x7e000000u)
static Heap *heap_of(uint32_t h)
{
    uint32_t id = HEAP_ID(h);
    if (id >= MAXHEAPS || !heaps[id].used)
        return NULL;
    return &heaps[id];
}

static inline uint32_t bsize(uint32_t b) { return R32(b + 4) & ~7u; }
static inline uint32_t bflags(uint32_t b) { return R32(b + 4) & 7u; }

static int bin_of(uint32_t size)
{
    if (size < 512)
        return size >> 3;                  /* 0..63, exact 8-byte classes */
    int lg = 31 - __builtin_clz(size);     /* 9.. */
    int b = 64 + (lg - 9);
    return b < NBINS ? b : NBINS - 1;
}

static void bin_push(Heap *h, uint32_t b)
{
    int k = bin_of(bsize(b));
    uint32_t head = h->bins[k];
    W32(b + 8, head);
    W32(b + 12, 0);
    if (head)
        W32(head + 12, b);
    h->bins[k] = b;
}

static void bin_remove(Heap *h, uint32_t b)
{
    uint32_t nx = R32(b + 8), pv = R32(b + 12);
    if (pv)
        W32(pv + 8, nx);
    else
        h->bins[bin_of(bsize(b))] = nx;
    if (nx)
        W32(nx + 12, pv);
}

uint32_t heap_create(uint32_t initial, uint32_t max)
{
    (void)initial; (void)max;
    pthread_mutex_lock(&mem_lock);
    int id = -1;
    for (int i = 0; i < MAXHEAPS; i++)
        if (!heaps[i].used) { id = i; break; }
    if (id >= 0) {
        memset(&heaps[id], 0, sizeof heaps[id]);
        heaps[id].used = 1;
    }
    pthread_mutex_unlock(&mem_lock);
    return id < 0 ? 0 : 0x7e000000u + id;
}

int heap_destroy(uint32_t hh)
{
    Heap *h = heap_of(hh);
    if (!h)
        return 0;
    uint32_t segs[1024];
    int n = h->nsegs;
    memcpy(segs, h->segs, n * sizeof *segs);
    pthread_mutex_lock(&mem_lock);
    h->used = 0;
    pthread_mutex_unlock(&mem_lock);
    for (int i = 0; i < n; i++)
        vm_free(segs[i], 0, 1);
    return 1;
}

static uint32_t heap_grow(Heap *h, uint32_t need)
{
    uint32_t sz = need + 16 > 0x100000 ? (need + 16 + 0xffff) & ~0xffffu : 0x100000;
    pthread_mutex_unlock(&mem_lock);
    uint32_t seg = vm_alloc(0, sz, 0);
    pthread_mutex_lock(&mem_lock);
    if (!seg || h->nsegs >= 1024)
        return 0;
    h->segs[h->nsegs++] = seg;
    /* one free block covering the segment; block address = header address */
    W32(seg, 0);
    W32(seg + 4, sz | F_LAST);
    bin_push(h, seg);
    return seg;
}

static uint32_t heap_alloc_block(uint32_t hh, uint32_t size, int zero);

/* (a zero-filled block made during a recorded call, the runtime's own included: devrec.c keeps its
   contents, which a playback has no allocator to give) */
uint32_t heap_alloc(uint32_t hh, uint32_t size, int zero)
{
    uint32_t p = heap_alloc_block(hh, size, zero);
    if (zero && p && dev_recording) {
        extern void devrec_alloc(uint32_t addr, uint32_t size);
        devrec_alloc(p, size);
    }
    return p;
}

static uint32_t heap_alloc_block(uint32_t hh, uint32_t size, int zero)
{
    Heap *h = heap_of(hh);
    if (!h)
        return 0;
    if (size > 0x7ff00000u)
        return 0;
    uint32_t need = ((size + 7) & ~7u) + 8;
    if (need < 24)
        need = 24;
    pthread_mutex_lock(&mem_lock);
    uint32_t b = 0;
    for (int k = bin_of(need); k < NBINS && !b; k++) {
        for (uint32_t x = h->bins[k]; x; x = R32(x + 8))
            if (bsize(x) >= need) { b = x; break; }
    }
    if (!b) {
        b = heap_grow(h, need);
        if (!b) {
            pthread_mutex_unlock(&mem_lock);
            return 0;
        }
    }
    bin_remove(h, b);
    uint32_t bs = bsize(b), fl = bflags(b);
    if (bs - need >= 24) {
        uint32_t rest = b + need;
        W32(rest, need);
        W32(rest + 4, (bs - need) | (fl & F_LAST));
        if (!(fl & F_LAST))
            W32(rest + (bs - need), bs - need);
        bin_push(h, rest);
        W32(b + 4, need | F_USED);
    } else {
        W32(b + 4, bs | F_USED | (fl & F_LAST));
    }
    pthread_mutex_unlock(&mem_lock);
    uint32_t p = b + 8;
    if (zero)
        memset(gptr(p), 0, bsize(b) - 8);
    LOG(LOG_MEM, "heap_alloc(%x, %u) -> %08x", hh, size, p);
    return p;
}

static int valid_block(uint32_t p)
{
    return p >= ARENA_START + 8 && p < ARENA_END && (R32(p - 4) & F_USED);
}

int heap_free(uint32_t hh, uint32_t p)
{
    Heap *h = heap_of(hh);
    if (!h || !p)
        return p == 0;
    if (!valid_block(p)) {
        LOG(LOG_ALWAYS, "heap_free(%x, %08x): not an allocated block", hh, p);
        return 0;
    }
    pthread_mutex_lock(&mem_lock);
    uint32_t b = p - 8;
    uint32_t bs = bsize(b), fl = bflags(b) & ~F_USED;
    /* merge with the next block */
    if (!(fl & F_LAST)) {
        uint32_t n = b + bs;
        if (!(bflags(n) & F_USED)) {
            bin_remove(h, n);
            fl |= bflags(n) & F_LAST;
            bs += bsize(n);
        }
    }
    /* merge with the previous block */
    uint32_t ps = R32(b);
    if (ps) {
        uint32_t pb = b - ps;
        if (!(bflags(pb) & F_USED)) {
            bin_remove(h, pb);
            bs += ps;
            b = pb;
        }
    }
    W32(b + 4, bs | fl);
    if (!(fl & F_LAST))
        W32(b + bs, bs);
    bin_push(h, b);
    pthread_mutex_unlock(&mem_lock);
    LOG(LOG_MEM, "heap_free(%x, %08x)", hh, p);
    return 1;
}

uint32_t heap_size(uint32_t hh, uint32_t p)
{
    (void)hh;
    if (!valid_block(p))
        return 0xffffffffu;
    return bsize(p - 8) - 8;
}

uint32_t heap_realloc(uint32_t hh, uint32_t p, uint32_t size, int zero, int in_place_only)
{
    if (!p)
        return in_place_only ? 0 : heap_alloc(hh, size, zero);
    if (!valid_block(p))
        return 0;
    uint32_t old = bsize(p - 8) - 8;
    if (size <= old)
        return p;                       /* keep the block (Windows may shrink; harmless) */
    if (in_place_only)
        return 0;
    uint32_t q = heap_alloc(hh, size, 0);
    if (!q)
        return 0;
    memcpy(gptr(q), gptr(p), old);
    if (zero)
        memset(gptr(q) + old, 0, bsize(q - 8) - 8 - old);
    heap_free(hh, p);
    return q;
}

/* ------------------------------------------------------------------ runtime-owned blocks */
static uint32_t rt_heap;

uint32_t galloc(uint32_t size)
{
    if (!rt_heap)
        rt_heap = heap_create(0, 0);
    uint32_t p = heap_alloc(rt_heap, size, 1);
    if (!p)
        fatal("out of program memory (%u bytes)", size);
    return p;
}

void gfree(uint32_t p)
{
    if (p)
        heap_free(rt_heap, p);
}

uint32_t gstrdup(const char *s)
{
    uint32_t n = strlen(s) + 1;
    uint32_t p = galloc(n);
    memcpy(gptr(p), s, n);
    return p;
}

void gstrncpy(uint32_t dst, const char *src, uint32_t n)
{
    if (!n)
        return;
    uint32_t k = strlen(src);
    if (k >= n)
        k = n - 1;
    memcpy(gptr(dst), src, k);
    gptr(dst)[k] = 0;
}

/* ------------------------------------------------------------------ rep movs / stos */
void rep_movs(CPU *c, int n, uint32_t src, uint32_t dst, uint32_t count)
{
    uint32_t total = count * n;
    if (!c->df) {
        if (dst <= src || dst >= src + total) {
            memmove(gptr(dst), gptr(src), total);
            return;
        }
        for (uint32_t i = 0; i < count; i++)        /* overlapping forward copy repeats */
            memmove(gptr(dst + i * n), gptr(src + i * n), n);
    } else {
        uint32_t s0 = src - (count - 1) * n, d0 = dst - (count - 1) * n;
        if (dst >= src || d0 + total <= s0) {
            memmove(gptr(d0), gptr(s0), total);
            return;
        }
        for (uint32_t i = 0; i < count; i++)
            memmove(gptr(dst - i * n), gptr(src - i * n), n);
    }
}

void rep_stos(CPU *c, int n, uint32_t dst, uint32_t value, uint32_t count)
{
    uint32_t start = c->df ? dst - (count - 1) * n : dst;
    if (n == 1) {
        memset(gptr(start), value & 0xff, count);
        return;
    }
    uint8_t *p = (uint8_t *)gptr(start);
    if (n == 2) {
        uint16_t v = (uint16_t)value;
        for (uint32_t i = 0; i < count; i++)
            memcpy(p + 2 * i, &v, 2);
    } else {
        for (uint32_t i = 0; i < count; i++)
            memcpy(p + 4 * i, &value, 4);
    }
}

/* ------------------------------------------------------------------ test support */
#include "image.h"

static uint64_t hash_range(uint64_t h, uint32_t a, uint32_t n)
{
    const uint64_t *p = (const uint64_t *)(uintptr_t)a;
    for (uint32_t i = 0; i < n / 8; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
        h ^= h >> 29;
    }
    return h;
}

/* The image's data and every region handed out (stacks, heaps, surfaces, sound buffers). */
/* thread stacks (cpu_new): left out of the hash, since what lies below a stack pointer is
   leftovers, which decompiled code (runtime/decomp/) leaves differently from the translation */
static uint32_t stacks[64];
static int nstacks;
void mem_mark_stack(uint32_t base)
{
    pthread_mutex_lock(&mem_lock);
    if (nstacks < 64) stacks[nstacks++] = base;
    pthread_mutex_unlock(&mem_lock);
}

uint64_t mem_hash(void)
{
    uint32_t data = IMAGE_BASE + 0xc3000u;
    uint64_t h = hash_range(0xcbf29ce484222325ull, data, IMAGE_SIZE - 0xc3000u);
    pthread_mutex_lock(&mem_lock);
    for (int i = 0; i < nregions; i++) {
        int stack = 0;
        for (int k = 0; k < nstacks; k++)
            stack |= stacks[k] == regions[i].base;
        if (!stack)
            h = hash_range(h ^ regions[i].base, regions[i].base, regions[i].size);
    }
    pthread_mutex_unlock(&mem_lock);
    return h;
}

/* Dump format: repeated [u32 base][u32 size][bytes]. */
void mem_dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) fatal("cannot write %s", path);
    uint32_t hdr[2] = {IMAGE_BASE, IMAGE_SIZE};
    fwrite(hdr, 4, 2, f);
    fwrite((void *)(uintptr_t)IMAGE_BASE, 1, IMAGE_SIZE, f);
    pthread_mutex_lock(&mem_lock);
    for (int i = 0; i < nregions; i++) {
        hdr[0] = regions[i].base;
        hdr[1] = regions[i].size;
        fwrite(hdr, 4, 2, f);
        fwrite((void *)(uintptr_t)regions[i].base, 1, regions[i].size, f);
    }
    pthread_mutex_unlock(&mem_lock);
    fclose(f);
}

/* an address in the image or a region (devrec.c): 1, and where that block ends */
int mem_span(uint32_t addr, uint32_t *end)
{
    if (addr >= IMAGE_BASE && addr < IMAGE_BASE + IMAGE_SIZE) { *end = IMAGE_BASE + IMAGE_SIZE; return 1; }
    pthread_mutex_lock(&mem_lock);
    int k = region_find(addr);
    if (k >= 0) *end = regions[k].base + regions[k].size;
    pthread_mutex_unlock(&mem_lock);
    return k >= 0;
}

/* the regions, for captured states (capture.c): their number, and one's base and size */
int mem_region_count(void) { return nregions; }
void mem_region(int i, uint32_t *base, uint32_t *size)
{
    pthread_mutex_lock(&mem_lock);
    *base = i < nregions ? regions[i].base : 0;
    *size = i < nregions ? regions[i].size : 0;
    pthread_mutex_unlock(&mem_lock);
}

/* Every place a byte string occurs in the program's memory (its heaps and regions), up to max:
   for the runtime's own small edits of loaded data (game.c). */
int mem_find_all(const void *needle, uint32_t n, uint32_t *found, int max)
{
    int k = 0;
    pthread_mutex_lock(&mem_lock);
    for (int i = 0; i < nregions && k < max; i++) {
        const uint8_t *base = (const uint8_t *)(uintptr_t)regions[i].base, *p = base;
        size_t left = regions[i].size;
        while (k < max && left >= n) {
            const uint8_t *q = memmem(p, left, needle, n);
            if (!q) break;
            found[k++] = regions[i].base + (uint32_t)(q - base);
            left -= (size_t)(q - p) + 1;
            p = q + 1;
        }
    }
    pthread_mutex_unlock(&mem_lock);
    return k;
}
