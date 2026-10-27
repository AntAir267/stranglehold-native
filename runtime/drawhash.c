/* DRAW-STREAM HASH (--hash): every Direct3D device call the program makes folded into a running
   hash, printed with each memory hash, so an A/B run (tests/scene.sh --ab) compares what is drawn
   as well as the memory. A call counts its method, its values and interfaces, and the data its
   input pointers point at (matrices, viewports, materials, lights, rects, shader constants,
   drawing-from-memory vertices: d3d_device_argkinds, tools/d3d9gen.py), never the pointers
   themselves (they may be stack addresses a rewritten function doesn't share); buffer uploads
   count their bytes at Unlock (d3d9.c). SHOLD_DRAW_TRACE=N logs, in frame N, each call's method
   with the hash after it: diff two runs' logs for the first call that differs. */
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

extern const char *const d3d_device_methods[], *const d3d_device_argkinds[];
int draw_hashing;
uint64_t draw_hash = 0xcbf29ce484222325ull;
static int tracing(void);

void draw_hash_bytes(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint64_t h = draw_hash;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    draw_hash = h;
    if (n > 64 && tracing())            /* (a buffer upload) */
        LOG(LOG_ALWAYS, "draw upload %zu bytes -> %016llx", n, (unsigned long long)h);
}

static void hash_u32(uint32_t v) { draw_hash_bytes(&v, 4); }

static int tracing(void)
{
    static long frame = -2;
    if (frame == -2) {
        const char *e = getenv("SHOLD_DRAW_TRACE");
        frame = e ? atol(e) : -1;
    }
    return frame >= 0 && (long)frame_count == frame;
}

static void hash_guest(uint32_t p, uint32_t n)
{
    if (p && n && n < (64u << 20))
        draw_hash_bytes(gptr(p), n);
}

static uint32_t verts(uint32_t prim, uint32_t count)
{
    switch (prim) {
    case 1: return count;               /* points */
    case 2: return count * 2;           /* line list */
    case 3: return count + 1;           /* line strip */
    case 4: return count * 3;           /* triangle list */
    default: return count + 2;          /* strip, fan */
    }
}

/* a device method's call (method: its name after "IDirect3DDevice9::") */
void draw_hash_call(CPU *c, const char *method)
{
    static const char *last_name, *last_kinds;
    static int last_index;
    if (method != last_name) {          /* (names are the table's own strings: one lookup each) */
        last_name = method;
        last_kinds = NULL;
        for (int i = 0; d3d_device_methods[i]; i++)
            if (!strcmp(d3d_device_methods[i], method)) { last_kinds = d3d_device_argkinds[i]; last_index = i; break; }
    }
    if (!last_kinds)
        return;
    hash_u32((uint32_t)last_index);
    const char *k = last_kinds;
    for (int i = 0; k[i]; i++) {
        uint32_t a = ARG(i + 1);
        switch (k[i]) {
        case 's': case 'i': hash_u32(a); break;
        case 'm': hash_guest(a, 64); break;
        case 'v': hash_guest(a, 24); break;
        case 'M': hash_guest(a, 68); break;
        case 'l': hash_guest(a, 104); break;
        case 'r': case 'c': hash_guest(a, 16); break;
        case 'R': hash_guest(a, ARG(i) * 16u); break;
        case 'f': case 'n': hash_guest(a, ARG(i + 2) * 16u); break;
        case 'b': hash_guest(a, ARG(i + 2) * 4u); break;
        case 'u': hash_guest(a, verts(ARG(1), ARG(2)) * ARG(4)); break;
        case 'U':
            if (i == 4) hash_guest(a, verts(ARG(1), ARG(4)) * (ARG(6) == 101 ? 2u : 4u));
            else hash_guest(a, (ARG(2) + ARG(3)) * ARG(8));
            break;
        }
    }
    if (tracing())
        LOG(LOG_ALWAYS, "draw %s -> %016llx", method, (unsigned long long)draw_hash);
}
