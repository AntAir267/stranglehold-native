/* ole32: COM start-up and object creation. DirectSound and the intro videos' DirectShow
   streams are created this way; DirectPlay is
   parked until multiplayer (design point 6): its creation fails as if not installed.
   Also the task allocator, GUID strings and OLEAUT32's BSTRs and VARIANTs (by ordinal, as
   DFEngine.dll imports them); type libraries and OLE fonts are not available. */
#include <ctype.h>
#include "runtime.h"
#include "sound.h"

uint32_t video_cocreate(uint32_t clsid, uint32_t iid, uint32_t ppv);

API(OLE32, CoInitialize, 1) { return 0; }
API(OLE32, CoUninitialize, 0) { return 0; }

API(OLE32, CoCreateGuid, 1)
{
    static uint32_t n;
    uint32_t g = ARG(0);
    W32(g, 0x1d5a0000u + ++n);
    W16(g + 4, 0x4e53);
    W16(g + 6, 0x4111);
    for (int i = 0; i < 8; i++)
        W8(g + 8 + i, (uint8_t)(0x80 + i + n));
    return 0;
}

API(OLE32, CoCreateInstance, 5)
{
    uint32_t clsid = ARG(0), iid = ARG(3);
    uint32_t hr = dsound_cocreate(clsid, iid, ARG(4));
    if (hr == 0x80040154u)
        hr = video_cocreate(clsid, iid, ARG(4));
    if (hr != 0x80040154u)
        return hr;
    LOG(LOG_ALWAYS, "CoCreateInstance({" GUID_FMT "}, iid {" GUID_FMT "}): not available",
        GUID_ARGS(clsid), GUID_ARGS(iid));
    if (ARG(4)) W32(ARG(4), 0);
    return 0x80040154u;                  /* REGDB_E_CLASSNOTREG */
}

/* the class factory route: no classes are registered */
API(OLE32, CoGetClassObject, 5)
{
    LOG(LOG_ALWAYS, "CoGetClassObject({" GUID_FMT "}): not available", GUID_ARGS(ARG(0)));
    if (ARG(4)) W32(ARG(4), 0);
    return 0x80040154u;                  /* REGDB_E_CLASSNOTREG */
}

/* no ProgIDs are registered: Windows answers an unknown one with CO_E_CLASSSTRING */
API(OLE32, CLSIDFromProgID, 2)
{
    char b[256];
    LOG(LOG_ALWAYS, "CLSIDFromProgID(\"%s\"): not registered", gwstr(ARG(0), b, sizeof b));
    return 0x800401f3u;                  /* CO_E_CLASSSTRING */
}

API(OLE32, OleInitialize, 1) { return 0; }
API(OLE32, OleUninitialize, 0) { return 0; }
API(OLE32, OleLockRunning, 3) { return 0; }

/* the task allocator: the process heap */
API(OLE32, CoTaskMemAlloc, 1) { return heap_alloc(process_heap, ARG(0) ? ARG(0) : 1, 0); }
API(OLE32, CoTaskMemFree, 1) { if (ARG(0)) heap_free(process_heap, ARG(0)); return 0; }
API(OLE32, CoTaskMemRealloc, 2)
{
    if (!ARG(0)) return heap_alloc(process_heap, ARG(1) ? ARG(1) : 1, 0);
    if (!ARG(1)) { heap_free(process_heap, ARG(0)); return 0; }
    return heap_realloc(process_heap, ARG(0), ARG(1), 0, 0);
}

/* streams on memory are not offered (would need an IStream) */
API(OLE32, CreateStreamOnHGlobal, 3)
{
    LOG(LOG_ALWAYS, "CreateStreamOnHGlobal(%x): not supported", ARG(0));
    if (ARG(2)) W32(ARG(2), 0);
    return 0x8007000eu;                  /* E_OUTOFMEMORY */
}

/* "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}": 38 characters and the terminator */
API(OLE32, StringFromGUID2, 3)
{
    char b[48];
    snprintf(b, sizeof b, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", GUID_ARGS(ARG(0)));
    if (ARG(2) < 39) return 0;
    gwput(ARG(1), b, 39);
    return 39;
}

static int hexval(int ch) { return isdigit(ch) ? ch - '0' : isxdigit(ch) ? (tolower(ch) - 'a' + 10) : -1; }

API(OLE32, CLSIDFromString, 2)
{
    char b[64];
    uint32_t out = ARG(1);
    if (!ARG(0)) { memset(gptr(out), 0, 16); return 0; }  /* CLSID_NULL */
    gwstr(ARG(0), b, sizeof b);
    static const int groups[] = {8, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2};
    uint8_t g[16];
    const char *p = b;
    int ok = strlen(b) == 38 && *p++ == '{';
    for (int i = 0, o = 0; ok && i < 11; i++) {
        uint32_t v = 0;
        for (int k = 0; k < groups[i]; k++) {
            int h = hexval((unsigned char)*p++);
            if (h < 0) { ok = 0; break; }
            v = v << 4 | (uint32_t)h;
        }
        if (!ok) break;
        if (i == 0) { memcpy(g, &v, 4); o = 4; }
        else if (i < 3) { uint16_t w = (uint16_t)v; memcpy(g + o, &w, 2); o += 2; }
        else g[o++] = (uint8_t)v;
        if ((i <= 2 || i == 4) && *p++ != '-') ok = 0;
    }
    if (ok && (p[0] != '}' || p[1])) ok = 0;
    if (!ok) {
        LOG(LOG_ALWAYS, "CLSIDFromString(\"%s\"): not a CLSID", b);
        return 0x800401f3u;              /* CO_E_CLASSSTRING (ProgIDs are not registered) */
    }
    memcpy(gptr(out), g, 16);
    return 0;
}

/* ------------------------------------------------------------------ OLEAUT32 (by ordinal)
   [verified: ordinals from the export table of Wine's i386 oleaut32.dll] */
/* a BSTR: its length in bytes, then the UTF-16 text and a terminator; the program's pointer
   is to the text */
static uint32_t bstr_alloc(uint32_t src, uint32_t bytes)
{
    uint32_t p = galloc(4 + bytes + 2);
    W32(p, bytes);
    if (src) memcpy(gptr(p + 4), gptr(src), bytes);
    W16(p + 4 + bytes, 0);
    return p + 4;
}

static uint32_t bstr_free(uint32_t b) { if (b) gfree(b - 4); return 0; }

/* #2 SysAllocString */
API(OLEAUT32, 2, 1)
{
    if (!ARG(0)) return 0;
    uint32_t n = 0;
    while (R16(ARG(0) + 2 * n)) n++;
    return bstr_alloc(ARG(0), 2 * n);
}
/* #4 SysAllocStringLen */
API(OLEAUT32, 4, 2) { return bstr_alloc(ARG(0), 2 * ARG(1)); }
/* #6 SysFreeString */
API(OLEAUT32, 6, 1) { return bstr_free(ARG(0)); }
/* #7 SysStringLen */
API(OLEAUT32, 7, 1) { return ARG(0) ? R32(ARG(0) - 4) / 2 : 0; }
/* #149 SysStringByteLen */
API(OLEAUT32, 149, 1) { return ARG(0) ? R32(ARG(0) - 4) : 0; }

/* #8 VariantInit: VT_EMPTY */
API(OLEAUT32, 8, 1) { W16(ARG(0), 0); return 0; }

/* #9 VariantClear: frees what the VARIANT owns (a BSTR, or releases an interface); arrays and
   records are not handled [guess: DFEngine keeps to simple types] */
API(OLEAUT32, 9, 1)
{
    uint32_t v = ARG(0), vt = R16(v);
    if (!(vt & 0x4000)) {                /* not VT_BYREF */
        if (vt == 8) bstr_free(R32(v + 8));                         /* VT_BSTR */
        else if ((vt == 9 || vt == 13) && R32(v + 8)) {             /* VT_DISPATCH, VT_UNKNOWN */
            uint32_t obj = R32(v + 8);
            guest_call(c, R32(R32(obj) + 8), 1, obj);               /* Release */
        } else if (vt & 0x2000)
            LOG(LOG_ALWAYS, "VariantClear: array variant (type %x) not freed", vt);
    }
    W16(v, 0);
    return 0;
}

/* #161 LoadTypeLib, #162 LoadRegTypeLib: no type libraries */
API(OLEAUT32, 161, 2)
{
    char b[512];
    LOG(LOG_ALWAYS, "LoadTypeLib(\"%s\"): not available", gwstr(ARG(0), b, sizeof b));
    if (ARG(1)) W32(ARG(1), 0);
    return 0x80029c4au;                  /* TYPE_E_CANTLOADLIBRARY */
}
API(OLEAUT32, 162, 5)
{
    LOG(LOG_ALWAYS, "LoadRegTypeLib({" GUID_FMT "}): not registered", GUID_ARGS(ARG(0)));
    if (ARG(4)) W32(ARG(4), 0);
    return 0x8002801du;                  /* TYPE_E_LIBNOTREGISTERED */
}

/* #277 VarUI4FromStr: a decimal number (spaces around it allowed) */
API(OLEAUT32, 277, 4)
{
    char b[64];
    const char *p = gwstr(ARG(0), b, sizeof b);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '+') p++;
    uint64_t v = 0;
    int digits = 0;
    for (; isdigit((unsigned char)*p); p++, digits++) {
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > 0xffffffffull) return 0x8002000au;                  /* DISP_E_OVERFLOW */
    }
    while (*p == ' ' || *p == '\t') p++;
    if (!digits || *p) return 0x80020005u;                          /* DISP_E_TYPEMISMATCH */
    W32(ARG(3), (uint32_t)v);
    return 0;
}

/* #420 OleCreateFontIndirect: no OLE fonts */
API(OLEAUT32, 420, 3)
{
    LOG(LOG_ALWAYS, "OleCreateFontIndirect: not available");
    if (ARG(2)) W32(ARG(2), 0);
    return 0x80004001u;                  /* E_NOTIMPL */
}
