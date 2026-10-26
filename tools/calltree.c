/* tools/calltree.sh: the call tree of one call of a function (SHOLD_TRACE_FN, default 0x4d2db0, the
   game view) on frame SHOLD_TRACE_FRAME, from a copy of the program built with
   -finstrument-functions:
   a calling-context tree, function offsets in the program (tools/calltree.py names them) */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#define NI __attribute__((no_instrument_function))
extern char __executable_start;
extern unsigned long frame_count;
typedef struct { uint32_t fn, parent, first, next, n; } Node;
#define MAXN 400000
static Node *nodes; static uint32_t nn;
static __thread int active, depth; static __thread uint32_t cur;
static long target = -2;
static void *root_fn;
static NI uint32_t child(uint32_t p, uint32_t fn)
{
    for (uint32_t k = nodes[p].first; k; k = nodes[k].next)
        if (nodes[k].fn == fn) return k;
    if (nn == MAXN) return p;
    Node *x = &nodes[nn];
    x->fn = fn; x->parent = p; x->first = 0; x->n = 0; x->next = nodes[p].first; nodes[p].first = nn;
    return nn++;
}
NI void __cyg_profile_func_enter(void *fn, void *site)
{
    if (!active) {
        if (target == -2) {
            const char *e = getenv("SHOLD_TRACE_FRAME"), *f = getenv("SHOLD_TRACE_FN");
            char name[32];
            target = e ? atol(e) : -1;
            snprintf(name, sizeof name, "F_%08lx", f ? strtoul(f, NULL, 16) : 0x4d2db0ul);
            root_fn = dlsym(RTLD_DEFAULT, name);
        }
        if (fn != root_fn) return;
        if ((long)frame_count != target) return;
        nodes = calloc(MAXN, sizeof *nodes); nn = 1; cur = 0; active = 1; depth = 0;
    }
    cur = child(cur, (uint32_t)((char *)fn - &__executable_start));
    nodes[cur].n++;
    depth++;
}
static NI void dump(FILE *f, uint32_t k, int d)
{
    fprintf(f, "%*s%x %u\n", d, "", nodes[k].fn, nodes[k].n);
    for (uint32_t j = nodes[k].first; j; j = nodes[j].next) dump(f, j, d + 1);
}
NI void __cyg_profile_func_exit(void *fn, void *site)
{
    if (!active) return;
    cur = nodes[cur].parent;
    if (--depth == 0) {
        FILE *f = fopen(getenv("SHOLD_TRACE_OUT") ? getenv("SHOLD_TRACE_OUT") : "trace.txt", "w");
        dump(f, nodes[0].first, 0);
        fclose(f);
        active = 0;
        target = -1;
    }
}
