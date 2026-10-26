/* CAPTURED STATES: a hooked function's entry in a real game, saved with all of its memory, so the
   function checker can replay it (tests/fncheck.c --capture): the original machine code under
   emulation against the translated or decompiled C, on the game's own data, without the game.

     SHOLD_CAPTURE=addr[@frame][xcount],...    (hex address; from that frame; that many calls)

   Files go to <user folder>/captures/<addr>_<frame>_<n>.cap (gzip): "LRCAP001", the function,
   the frame, sizeof(CPU) and the CPU record, then (base, size, bytes) for the image and every
   region, ending with (0, 0). Beside it, <file>.dev: the calls it makes into the runtime until it
   returns, with their results (devrec.c), for checking it offline. Only hooked functions (HOOKS in the build config) can be captured:
   the check sits in their generated entry. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <zlib.h>
#include "runtime.h"
#include "image.h"

int capture_armed;
static struct { uint32_t fn; unsigned long from; int left, n; } caps[16];
static int ncaps = -1;

static void capture_init(void)
{
    ncaps = 0;
    const char *e = getenv("SHOLD_CAPTURE");
    for (const char *p = e; p && *p && ncaps < 16; ) {
        char *q;
        caps[ncaps].fn = (uint32_t)strtoul(p, &q, 16);
        caps[ncaps].from = *q == '@' ? strtoul(q + 1, &q, 10) : 0;
        caps[ncaps].left = *q == 'x' ? (int)strtol(q + 1, &q, 10) : 1;
        caps[ncaps].n = 0;
        ncaps++;
        while (*q == ',' || *q == ' ') q++;
        p = q;
    }
}

static void put(gzFile f, const void *d, unsigned n)
{
    while (n) {
        unsigned k = n > (1u << 30) ? 1u << 30 : n;
        if (gzwrite(f, d, k) != (int)k) fatal("capture: write failed");
        d = (const char *)d + k;
        n -= k;
    }
}

void capture_point(CPU *c, uint32_t fn)
{
    if (ncaps < 0) capture_init();
    int open = 0;
    for (int i = 0; i < ncaps; i++) {
        if (caps[i].left > 0) open = 1;
        if (caps[i].fn != fn || caps[i].left <= 0 || frame_count < caps[i].from)
            continue;
        char path[1200];
        snprintf(path, sizeof path, "%s/captures", user_dir);
        mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/captures/%08x_%lu_%d.cap", user_dir, fn, frame_count, caps[i].n);
        extern volatile int det_watch_quiet;
        det_watch_quiet = 1;            /* (writing takes seconds: not a stuck turn, sync.c) */
        gzFile f = gzopen(path, "wb1");
        if (!f) { LOG(LOG_ALWAYS, "capture: cannot write %s", path); caps[i].left = 0; det_watch_quiet = 0; continue; }
        uint32_t h[4] = {fn, (uint32_t)frame_count, (uint32_t)sizeof(CPU), 0};
        put(f, "LRCAP001", 8);
        put(f, h, sizeof h);
        put(f, c, sizeof *c);
        uint32_t r[2] = {IMAGE_BASE, IMAGE_SIZE};
        put(f, r, 8);
        put(f, (const void *)(uintptr_t)IMAGE_BASE, IMAGE_SIZE);
        for (int k = 0, n = mem_region_count(); k < n; k++) {
            mem_region(k, &r[0], &r[1]);
            if (!r[1]) continue;
            put(f, r, 8);
            put(f, (const void *)(uintptr_t)r[0], r[1]);
        }
        r[0] = r[1] = 0;
        put(f, r, 8);
        gzclose(f);
        det_watch_quiet = 0;
        LOG(LOG_ALWAYS, "capture: %08x at frame %lu written to %s", fn, frame_count, path);
        extern void devrec_start(const char *cap_path, uint32_t fn, CPU *c);
        devrec_start(path, fn, c);      /* (its calls into the runtime until it returns: devrec.c) */
        caps[i].n++;
        caps[i].left--;
    }
    if (!open) capture_armed = 0;
}

/* at start-up: armed if SHOLD_CAPTURE names anything */
__attribute__((constructor)) static void capture_arm(void)
{
    const char *e = getenv("SHOLD_CAPTURE");
    capture_armed = e && *e;
}
