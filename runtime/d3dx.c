#define _GNU_SOURCE
/* d3dx9_28.dll: the three functions the program imports (builds/gog.py says why the DLL itself is
   not translated). */
#include <stdio.h>
#include "runtime.h"

#define D3DERR_INVALIDCALL 0x8876086cu
#define E_NOTIMPL          0x80004001u

/* D3DXMatrixInverse(out, &determinant or NULL, m): NULL when m has no inverse */
API(D3DX9_28, D3DXMatrixInverse, 3)
{
    uint32_t out = ARG(0), det_out = ARG(1), in = ARG(2);
    float m[16], inv[16];
    for (int i = 0; i < 16; i++)
        m[i] = RF32(in + 4 * i);
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det_out)
        WF32(det_out, det);
    if (det == 0.0f)
        return 0;
    for (int i = 0; i < 16; i++)
        WF32(out + 4 * i, inv[i] / det);
    return out;
}

/* ID3DXBuffer: a block of program memory (QueryInterface, AddRef, Release, GetBufferPointer,
   GetBufferSize) */
#include <stdlib.h>
#include <string.h>
#include "bink_shaders.h"
typedef struct { uint32_t data, size; int refs; } XBuf;
static ComClass cls_xbuf;
static XBuf *xbuf_of(uint32_t o) { return com_host(o); }
METHOD(xbuf_QueryInterface, 3) { if (ARG(2)) W32(ARG(2), 0); return 0x80004002u; }
METHOD(xbuf_AddRef, 1) { return ++xbuf_of(ARG(0))->refs; }
METHOD(xbuf_Release, 1)
{
    XBuf *b = xbuf_of(ARG(0));
    int r = --b->refs;
    if (r == 0) gfree(b->data);          /* (the host record and object stay: tiny, and rare) */
    return r < 0 ? 0 : r;
}
METHOD(xbuf_GetBufferPointer, 1) { return xbuf_of(ARG(0))->data; }
METHOD(xbuf_GetBufferSize, 1) { return xbuf_of(ARG(0))->size; }

static uint32_t xbuf_new(const void *bytes, uint32_t n)
{
    if (!cls_xbuf.vtbl) {
        static void (*const m[])(CPU *) = {xbuf_QueryInterface, xbuf_AddRef, xbuf_Release,
                                           xbuf_GetBufferPointer, xbuf_GetBufferSize};
        static const char *const names[] = {"QueryInterface", "AddRef", "Release", "GetBufferPointer",
                                            "GetBufferSize"};
        com_class_init(&cls_xbuf, "ID3DXBuffer", 5, m, names);
    }
    XBuf *b = calloc(1, sizeof *b);
    b->data = galloc(n);
    b->size = n;
    b->refs = 1;
    memcpy(gptr(b->data), bytes, n);
    return com_new(&cls_xbuf, b);
}

/* Shaders come precompiled in the game's shader cache; the only ones compiled at run time are
   Bink's YUV -> RGB pixel shaders, whose HLSL RAD's code hands over (s0-s2 the Y, Cr, Cb planes,
   s3 an alpha plane in the second; c0-c4 set by the caller) [verified: the two sources logged at
   start-up]. They get our assembled equivalents (data/bink); anything else is refused. */
API(D3DX9_28, D3DXCompileShader, 10)
{
    const char *src = gptr(ARG(0));
    uint32_t len = ARG(1);
    int has_tex2 = memmem(src, len, "tex2D( tex2", 11) != NULL, has_tex3 = memmem(src, len, "tex3", 4) != NULL;
    int bink = memmem(src, len, "tor", 3) && memmem(src, len, "gamma", 5) && has_tex2;
    if (ARG(8)) W32(ARG(8), 0);
    if (ARG(9)) W32(ARG(9), 0);
    if (bink && ARG(5) && !strcmp(gstr(ARG(5)), "ps_2_0")) {
        uint32_t buf = has_tex3 ? xbuf_new(bink_yuva4, sizeof bink_yuva4) : xbuf_new(bink_yuv3, sizeof bink_yuv3);
        if (ARG(7)) W32(ARG(7), buf);
        LOG(LOG_ALWAYS, "D3DXCompileShader: Bink YUV shader (%s planes)", has_tex3 ? "4" : "3");
        return 0;
    }
    LOG(LOG_ALWAYS, "D3DXCompileShader(%s, %s, %u bytes): not available",
        ARG(4) ? gstr(ARG(4)) : "-", ARG(5) ? gstr(ARG(5)) : "-", len);
    if (ARG(7)) W32(ARG(7), 0);
    return E_NOTIMPL;
}

API(D3DX9_28, D3DXDisassembleShader, 4)
{
    if (ARG(3)) W32(ARG(3), 0);
    return E_NOTIMPL;
}
