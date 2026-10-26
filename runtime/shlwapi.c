/* SHLWAPI: the path helpers MFC uses. */
#include <strings.h>
#include "runtime.h"

static uint32_t file_name_of(uint32_t p)
{
    uint32_t r = p;
    for (uint32_t q = p; R8(q); q++)
        if ((R8(q) == '\\' || R8(q) == '/' || R8(q) == ':') && R8(q + 1))
            r = q + 1;
    return r;
}

API(SHLWAPI, PathFindFileNameA, 1) { return ARG(0) ? file_name_of(ARG(0)) : 0; }

API(SHLWAPI, PathFindExtensionA, 1)
{
    if (!ARG(0)) return 0;
    uint32_t dot = 0, q = file_name_of(ARG(0));
    for (; R8(q); q++) {
        if (R8(q) == '.') dot = q;
        else if (R8(q) == ' ') { /* spaces keep the extension */ }
    }
    return dot ? dot : q;                       /* the terminating zero when there is none */
}

API(SHLWAPI, PathRemoveExtensionA, 1)
{
    if (!ARG(0)) return 0;
    uint32_t q = file_name_of(ARG(0)), dot = 0;
    for (; R8(q); q++) if (R8(q) == '.') dot = q;
    if (dot) W8(dot, 0);
    return 0;
}

API(SHLWAPI, PathIsUNCA, 1) { return ARG(0) && R8(ARG(0)) == '\\' && R8(ARG(0) + 1) == '\\'; }

API(SHLWAPI, PathStripToRootA, 1)
{
    char *p = gptr(ARG(0));
    if (p[0] && p[1] == ':') { p[2] = p[2] == '\\' ? '\\' : 0; if (p[2]) p[3] = 0; return 1; }
    if (p[0] == '\\') { p[1] = p[1] == '\\' ? p[1] : 0; return 1; }
    p[0] = 0;
    return 0;
}

API(SHLWAPI, UrlUnescapeA, 4) { return 0; }
