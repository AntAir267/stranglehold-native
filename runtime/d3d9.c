/* The Direct3D 9 bridge to DXVK (design: the program's D3D9 runs on DXVK's native Linux build).
   The program's COM objects are ours (com.c): their vtables point at runtime functions, mostly
   generated (gen/d3d9_gen.c), which call DXVK's object behind them. Written here: the objects'
   identity and reference counts, the methods whose arguments differ between the program's
   32-bit layout and DXVK's 64-bit one (present parameters, locked rectangles, window handles),
   and locks: the program cannot address DXVK's memory (above 4 GB), so a lock hands it a copy in
   its own memory (persistent for vertex and index buffers, per lock for textures and surfaces),
   copied into DXVK's at unlock. */
#define _GNU_SOURCE
#define INITGUID
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include "d3d9_bridge.h"
#include "win.h"

/* ------------------------------------------------------------------ objects */
typedef struct {
    void *host;
    uint32_t guest;
    int kind;
    /* vertex/index buffers: the program's copy */
    uint32_t shadow, size;
    int nlocks;
    struct { uint32_t off, len; void *hp; } locks[4];
} Ent;

#define HCAP 65536
static Ent *ents[HCAP];                  /* open addressing on the host pointer */
static pthread_mutex_t dlock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t hslot(void *h) { return (uint32_t)(((uintptr_t)h >> 4) * 2654435761u) & (HCAP - 1); }

static Ent *ent_find(void *h)
{
    for (uint32_t i = hslot(h), n = 0; n < HCAP; i = (i + 1) & (HCAP - 1), n++) {
        if (!ents[i]) return NULL;
        if (ents[i] != (Ent *)1 && ents[i]->host == h) return ents[i];
    }
    return NULL;
}

static void ent_put(Ent *e)
{
    for (uint32_t i = hslot(e->host), n = 0; n < HCAP; i = (i + 1) & (HCAP - 1), n++)
        if (!ents[i] || ents[i] == (Ent *)1) { ents[i] = e; return; }
    fatal("too many Direct3D objects");
}

static void ent_del(Ent *e)
{
    for (uint32_t i = hslot(e->host), n = 0; n < HCAP; i = (i + 1) & (HCAP - 1), n++) {
        if (!ents[i]) return;
        if (ents[i] == e) { ents[i] = (Ent *)1; return; }
    }
}

void *d3d_host(uint32_t guest) { return com_host(guest); }
void *d3d_host_or_null(uint32_t guest) { return guest ? com_host(guest) : NULL; }

static Ent *ent_of_guest(uint32_t guest)
{
    void *h = com_host(guest);
    pthread_mutex_lock(&dlock);
    Ent *e = ent_find(h);
    pthread_mutex_unlock(&dlock);
    return e;
}

/* a resource's own class from its type (the program may hold it as a base texture) */
static int kind_of_resource(void *h)
{
    uint32_t t = ((uint32_t (*)(void *))VT(h)[10])(h);       /* IDirect3DResource9::GetType */
    switch (t) {
    case 1: return DK_IDirect3DSurface9;
    case 2: return DK_IDirect3DVolume9;
    case 3: return DK_IDirect3DTexture9;
    case 4: return DK_IDirect3DVolumeTexture9;
    case 5: return DK_IDirect3DCubeTexture9;
    case 6: return DK_IDirect3DVertexBuffer9;
    case 7: return DK_IDirect3DIndexBuffer9;
    }
    fatal("Direct3D resource of unknown type %u", t);
}

/* An entry left for a host object that died without the program releasing it (a texture's
   surfaces go with the texture) must not be handed out for a new object DXVK puts at the same
   address: objects from Create* calls are always new (d3d_wrap_new), and an entry of another
   kind is stale. (larush-native 30bb00e) */
static uint32_t wrap(int kind, void *host, int is_new)
{
    if (!host) return 0;
    pthread_mutex_lock(&dlock);
    Ent *e = ent_find(host);
    if (e && (is_new || (kind != DK_IDirect3DResource9 && kind != DK_IDirect3DBaseTexture9 &&
                         kind != DK_IUnknown && e->kind != kind))) {
        LOG(LOG_D3D, "%s %p: the old %s %08x there is gone", d3d_classes[kind].iface, host,
            d3d_classes[e->kind].iface, e->guest);
        ent_del(e);
        W32(e->guest, 0);                /* as after a release: a use finds no vtable */
        if (e->shadow) gfree(e->shadow);
        free(e);
        e = NULL;
    }
    if (e) {
        pthread_mutex_unlock(&dlock);
        return e->guest;
    }
    pthread_mutex_unlock(&dlock);
    if (kind == DK_IDirect3DResource9 || kind == DK_IDirect3DBaseTexture9)
        kind = kind_of_resource(host);
    e = calloc(1, sizeof *e);
    e->host = host;
    e->kind = kind;
    e->guest = com_new(&d3d_classes[kind], host);
    pthread_mutex_lock(&dlock);
    Ent *again = ent_find(host);         /* another thread wrapped it meanwhile */
    if (again) {
        pthread_mutex_unlock(&dlock);
        free(e);
        return again->guest;
    }
    ent_put(e);
    pthread_mutex_unlock(&dlock);
    LOG(LOG_D3D, "%s %p -> %08x", d3d_classes[kind].iface, host, e->guest);
    return e->guest;
}

uint32_t d3d_wrap(int kind, void *host) { return wrap(kind, host, 0); }
uint32_t d3d_wrap_new(int kind, void *host) { return wrap(kind, host, 1); }

void *d3d_hwnd(uint32_t hwnd)
{
    (void)hwnd;                          /* every window is the host window */
    return host_sdl_window();
}

float d3d_argf(CPU *c, int n)
{
    uint32_t u = ARG(n);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

void d3d_unsupported(CPU *c, const char *name, int nargs)
{
    LOG(LOG_ALWAYS, "%s is not supported (called from %08x)", name, R32(c->esp));
    api_return(c, 0x8876086cu, nargs);   /* D3DERR_INVALIDCALL */
}

/* ------------------------------------------------------------------ IUnknown */
static const struct { const GUID *iid; int kind; } iids[] = {
    {&IID_IDirect3D9, DK_IDirect3D9}, {&IID_IDirect3DDevice9, DK_IDirect3DDevice9},
    {&IID_IDirect3DResource9, DK_IDirect3DResource9}, {&IID_IDirect3DVertexBuffer9, DK_IDirect3DVertexBuffer9},
    {&IID_IDirect3DVolume9, DK_IDirect3DVolume9}, {&IID_IDirect3DSwapChain9, DK_IDirect3DSwapChain9},
    {&IID_IDirect3DSurface9, DK_IDirect3DSurface9}, {&IID_IDirect3DIndexBuffer9, DK_IDirect3DIndexBuffer9},
    {&IID_IDirect3DBaseTexture9, DK_IDirect3DBaseTexture9}, {&IID_IDirect3DTexture9, DK_IDirect3DTexture9},
    {&IID_IDirect3DCubeTexture9, DK_IDirect3DCubeTexture9}, {&IID_IDirect3DVolumeTexture9, DK_IDirect3DVolumeTexture9},
    {&IID_IDirect3DVertexDeclaration9, DK_IDirect3DVertexDeclaration9},
    {&IID_IDirect3DVertexShader9, DK_IDirect3DVertexShader9}, {&IID_IDirect3DPixelShader9, DK_IDirect3DPixelShader9},
    {&IID_IDirect3DStateBlock9, DK_IDirect3DStateBlock9}, {&IID_IDirect3DQuery9, DK_IDirect3DQuery9},
};

HOSTFN(sp_any_QueryInterface, 3)
{
    void *self = d3d_host(ARG(0));
    Ent *me = ent_of_guest(ARG(0));
    GUID g;
    memcpy(&g, gptr(ARG(1)), sizeof g);
    void *o = NULL;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const GUID *, void **))VT(self)[0])(self, &g, &o);
    if ((int32_t)r < 0 || !o) {
        if (ARG(2)) W32(ARG(2), 0);
        LOG(LOG_D3D, "QueryInterface(" GUID_FMT ") on %s -> %08x", GUID_ARGS(ARG(1)), me ? d3d_classes[me->kind].iface : "?", r);
        return r;
    }
    int kind = me ? me->kind : DK_IDirect3DResource9;
    for (size_t i = 0; i < sizeof iids / sizeof *iids; i++)
        if (!memcmp(&g, iids[i].iid, sizeof g)) { kind = iids[i].kind; break; }
    if (o == self && me)
        kind = me->kind;                 /* the same object: keep its class */
    W32(ARG(2), d3d_wrap(kind, o));
    return r;
}

HOSTFN(sp_any_AddRef, 1)
{
    void *self = d3d_host(ARG(0));
    return ((uint32_t (*)(void *))VT(self)[1])(self);
}

HOSTFN(sp_any_Release, 1)
{
    void *self = d3d_host(ARG(0));
    Ent *e = ent_of_guest(ARG(0));
    uint32_t r = ((uint32_t (*)(void *))VT(self)[2])(self);
    if (r == 0) {
        extern void d3d_lockcache_drop(void *host);
        d3d_lockcache_drop(self);           /* (a dynamic texture's kept lock copies) */
    }
    if (r == 0 && e) {
        pthread_mutex_lock(&dlock);
        ent_del(e);
        pthread_mutex_unlock(&dlock);
        LOG(LOG_D3D, "%s %08x released", d3d_classes[e->kind].iface, e->guest);
        if (e->shadow) gfree(e->shadow);
        W32(e->guest, 0);                /* a use after release finds no vtable */
        free(e);
    }
    return r;
}

/* SetPrivateData with D3DSPD_IUNKNOWN would hand DXVK a program object to reference count: kept
   out of DXVK (the data is stored as plain bytes instead, the pointer included) */
HOSTFN(sp_any_SetPrivateData, 5)
{
    void *self = d3d_host(ARG(0));
    GUID g;
    memcpy(&g, gptr(ARG(1)), sizeof g);
    uint32_t flags = ARG(4) & ~1u;
    return (uint32_t)((HRESULT (*)(void *, const GUID *, const void *, DWORD, DWORD))VT(self)[4])(
        self, &g, gptr(ARG(2)), ARG(3), flags);
}

HOSTFN(sp_any_GetPrivateData, 4)
{
    void *self = d3d_host(ARG(0));
    GUID g;
    memcpy(&g, gptr(ARG(1)), sizeof g);
    DWORD n = ARG(3) ? R32(ARG(3)) : 0;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const GUID *, void *, DWORD *))VT(self)[5])(
        self, &g, ARG(2) ? gptr(ARG(2)) : NULL, &n);
    if (ARG(3)) W32(ARG(3), n);
    return r;
}

/* ------------------------------------------------------------------ present parameters */
/* the program's D3DPRESENT_PARAMETERS: 14 dwords, hDeviceWindow at +28 */
static void pp_in(uint32_t g, D3DPRESENT_PARAMETERS *pp, uint32_t focus)
{
    memset(pp, 0, sizeof *pp);
    pp->BackBufferWidth = R32(g);
    pp->BackBufferHeight = R32(g + 4);
    pp->BackBufferFormat = (D3DFORMAT)R32(g + 8);
    pp->BackBufferCount = R32(g + 12);
    pp->MultiSampleType = (D3DMULTISAMPLE_TYPE)R32(g + 16);
    pp->MultiSampleQuality = R32(g + 20);
    pp->SwapEffect = (D3DSWAPEFFECT)R32(g + 24);
    pp->hDeviceWindow = d3d_hwnd(R32(g + 28) ? R32(g + 28) : focus);
    pp->Windowed = R32(g + 32);
    pp->EnableAutoDepthStencil = R32(g + 36);
    pp->AutoDepthStencilFormat = (D3DFORMAT)R32(g + 40);
    pp->Flags = R32(g + 44);
    pp->FullScreen_RefreshRateInHz = R32(g + 48);
    pp->PresentationInterval = R32(g + 52);
    LOG(LOG_ALWAYS, "present parameters: %ux%u format %u x%u, ms %u, swap %u, %s, depth %u (format %u), "
        "flags %x, %u Hz, interval %x", pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
        pp->BackBufferCount, pp->MultiSampleType, pp->SwapEffect, pp->Windowed ? "windowed" : "full screen",
        pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat, pp->Flags, pp->FullScreen_RefreshRateInHz,
        pp->PresentationInterval);
    /* the desktop's mode is never changed: full screen becomes a window of the back buffer's size
       (the host window goes full screen on its own, borderless, with Alt+Enter) */
    if (!pp->Windowed) {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
    }
    if (pp->BackBufferWidth && pp->BackBufferHeight)
        host_window_resize((int)pp->BackBufferWidth, (int)pp->BackBufferHeight);
}

static void pp_out(uint32_t g, const D3DPRESENT_PARAMETERS *pp)
{
    W32(g, pp->BackBufferWidth);
    W32(g + 4, pp->BackBufferHeight);
    W32(g + 8, pp->BackBufferFormat);
    W32(g + 12, pp->BackBufferCount);
}

/* ------------------------------------------------------------------ IDirect3D9 */
static void *dxvk;
static void *(*dxvk_create9)(UINT);

API(D3D9, Direct3DCreate9, 1)
{
    if (!dxvk) {
        char dir[1024], path[1200];
        ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
        dir[n > 0 ? n : 0] = 0;
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        snprintf(path, sizeof path, "%s/lib/libdxvk_d3d9.so.0", dir);
        setenv("DXVK_WSI_DRIVER", "SDL3", 0);
        static char logdir[1100];
        snprintf(logdir, sizeof logdir, "%s", user_dir);
        setenv("DXVK_LOG_PATH", logdir, 0);
        setenv("DXVK_STATE_CACHE_PATH", logdir, 0);
        dxvk = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!dxvk)
            fatal("cannot load DXVK (%s): %s", path, dlerror());
        dxvk_create9 = (void *(*)(UINT))dlsym(dxvk, "Direct3DCreate9");
        if (!dxvk_create9)
            fatal("DXVK has no Direct3DCreate9");
        d3d_classes_init();
    }
    if (!host_sdl_window())
        LOG(LOG_ALWAYS, "Direct3DCreate9 without a host window (headless run): DXVK cannot present");
    void *d = dxvk_create9(ARG(0));
    LOG(LOG_ALWAYS, "Direct3DCreate9(%u) -> %p", ARG(0), d);
    return d3d_wrap_new(DK_IDirect3D9, d);
}

HOSTFN(sp_IDirect3D9_CreateDevice, 7)
{
    void *self = d3d_host(ARG(0));
    uint32_t flags = ARG(4);
    D3DPRESENT_PARAMETERS pp;
    pp_in(ARG(5), &pp, ARG(3));
    /* Direct3D 9 sets the FPU to single precision unless told to preserve it: so does this, on the
       program's control word; DXVK always preserves (the host FPU is not its to change) */
    if (!(flags & D3DCREATE_FPU_PRESERVE))
        fpu_setcw(c, c->fcw & ~0x300u);
    void *dev = NULL;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, void **))
                            VT(self)[16])(self, ARG(1), (D3DDEVTYPE)ARG(2), d3d_hwnd(ARG(3)),
                                          flags | D3DCREATE_FPU_PRESERVE, &pp, &dev);
    LOG(LOG_ALWAYS, "CreateDevice(adapter %u, type %u, flags %x) -> %08x", ARG(1), ARG(2), flags, r);
    pp_out(ARG(5), &pp);
    if (ARG(6)) W32(ARG(6), (int32_t)r >= 0 ? d3d_wrap_new(DK_IDirect3DDevice9, dev) : 0);
    return r;
}

HOSTFN(sp_IDirect3D9_GetAdapterMonitor, 2) { return 0x00120001u; }
HOSTFN(sp_IDirect3D9_RegisterSoftwareDevice, 2) { return 0x8876086cu; }

/* ------------------------------------------------------------------ IDirect3DDevice9 */
HOSTFN(sp_IDirect3DDevice9_Reset, 2)
{
    void *self = d3d_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    pp_in(ARG(1), &pp, 0);
    uint32_t r = (uint32_t)((HRESULT (*)(void *, D3DPRESENT_PARAMETERS *))VT(self)[16])(self, &pp);
    LOG(LOG_ALWAYS, "Reset -> %08x", r);
    pp_out(ARG(1), &pp);
    return r;
}

HOSTFN(sp_IDirect3DDevice9_CreateAdditionalSwapChain, 3)
{
    void *self = d3d_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    pp_in(ARG(1), &pp, 0);
    void *sc = NULL;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, D3DPRESENT_PARAMETERS *, void **))VT(self)[13])(self, &pp, &sc);
    pp_out(ARG(1), &pp);
    if (ARG(2)) W32(ARG(2), (int32_t)r >= 0 ? d3d_wrap_new(DK_IDirect3DSwapChain9, sc) : 0);
    return r;
}

HOSTFN(sp_IDirect3DDevice9_GetCreationParameters, 2)
{
    void *self = d3d_host(ARG(0));
    D3DDEVICE_CREATION_PARAMETERS p;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, D3DDEVICE_CREATION_PARAMETERS *))VT(self)[9])(self, &p);
    W32(ARG(1), p.AdapterOrdinal);
    W32(ARG(1) + 4, p.DeviceType);
    W32(ARG(1) + 8, main_hwnd);
    W32(ARG(1) + 12, p.BehaviorFlags & ~(uint32_t)D3DCREATE_FPU_PRESERVE);
    return r;
}

/* SHOLD_SHOT="frame,frame,...": the back buffer of those presented frames saved as
   shot_<frame>.png in the user folder (checking the picture without capturing the desktop) */
#include <zlib.h>
static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8] = {n >> 24, n >> 16, n >> 8, n, type[0], type[1], type[2], type[3]};
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t crc = crc32(crc32(0, (const uint8_t *)type, 4), d, n);
    uint8_t t[4] = {crc >> 24, crc >> 16, crc >> 8, crc};
    fwrite(t, 1, 4, f);
}

static void shot_maybe(void *dev)
{
    static int parsed;
    static unsigned long want[1024];
    static int nwant;
    if (!parsed) {
        parsed = 1;
        const char *e = getenv("SHOLD_SHOT");
        for (const char *p = e; p && *p && nwant < 1024; ) {
            want[nwant++] = strtoul(p, (char **)&p, 10);
            while (*p == ',' || *p == ' ') p++;
        }
    }
    int hit = 0;
    for (int i = 0; i < nwant; i++) hit |= want[i] == frame_count + 1;
    if (!hit) return;
    void *bb = NULL, *sys = NULL;
    if (((HRESULT (*)(void *, UINT, UINT, D3DBACKBUFFER_TYPE, void **))VT(dev)[18])(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb) < 0)
        return;
    D3DSURFACE_DESC d;
    ((HRESULT (*)(void *, D3DSURFACE_DESC *))VT(bb)[12])(bb, &d);
    if (((HRESULT (*)(void *, UINT, UINT, D3DFORMAT, D3DPOOL, void **, HANDLE *))VT(dev)[36])(
            dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL) >= 0 &&
        ((HRESULT (*)(void *, void *, void *))VT(dev)[32])(dev, bb, sys) >= 0) {
        D3DLOCKED_RECT lr;
        if (((HRESULT (*)(void *, D3DLOCKED_RECT *, const RECT *, DWORD))VT(sys)[13])(sys, &lr, NULL, D3DLOCK_READONLY) >= 0) {
            uint32_t w = d.Width, h = d.Height;
            size_t rowb = 1 + 3 * (size_t)w;
            uint8_t *raw = malloc(rowb * h);
            for (uint32_t y = 0; y < h; y++) {
                const uint8_t *src = (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch;
                uint8_t *dst = raw + y * rowb;
                dst[0] = 0;
                for (uint32_t x = 0; x < w; x++) {          /* X8R8G8B8 / A8R8G8B8: B G R A */
                    dst[1 + 3 * x] = src[4 * x + 2];
                    dst[2 + 3 * x] = src[4 * x + 1];
                    dst[3 + 3 * x] = src[4 * x];
                }
            }
            ((HRESULT (*)(void *))VT(sys)[14])(sys);
            uLongf zn = compressBound(rowb * h);
            uint8_t *z = malloc(zn);
            compress2(z, &zn, raw, rowb * h, 6);
            char path[1200];
            snprintf(path, sizeof path, "%s/shot_%lu.png", user_dir, frame_count + 1);
            FILE *f = fopen(path, "wb");
            if (f) {
                static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
                fwrite(sig, 1, 8, f);
                uint8_t ih[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0};
                png_chunk(f, "IHDR", ih, 13);
                png_chunk(f, "IDAT", z, (uint32_t)zn);
                png_chunk(f, "IEND", NULL, 0);
                fclose(f);
                LOG(LOG_ALWAYS, "frame %lu saved to %s (%ux%u, format %u)", frame_count + 1, path, w, h, d.Format);
            }
            free(z);
            free(raw);
        }
    }
    if (sys) ((ULONG (*)(void *))VT(sys)[2])(sys);
    ((ULONG (*)(void *))VT(bb)[2])(bb);
}

HOSTFN(sp_IDirect3DDevice9_Present, 5)
{
    void *self = d3d_host(ARG(0));
    shot_maybe(self);
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const RECT *, const RECT *, HWND, const RGNDATA *))VT(self)[17])(
        self, (const RECT *)gptr(ARG(1)), (const RECT *)gptr(ARG(2)), NULL, (const RGNDATA *)gptr(ARG(4)));
    frame_presented();
    return r;
}

HOSTFN(sp_IDirect3DDevice9_GetNPatchMode, 1)
{
    LOG(LOG_ALWAYS, "GetNPatchMode: returns 0 (N-patches are never on)");
    return 0;
}

/* ------------------------------------------------------------------ IDirect3DSwapChain9 */
HOSTFN(sp_IDirect3DSwapChain9_Present, 6)
{
    void *self = d3d_host(ARG(0));
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD))VT(self)[3])(
        self, (const RECT *)gptr(ARG(1)), (const RECT *)gptr(ARG(2)), NULL, (const RGNDATA *)gptr(ARG(4)), ARG(5));
    frame_presented();
    return r;
}

HOSTFN(sp_IDirect3DSwapChain9_GetPresentParameters, 2)
{
    void *self = d3d_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, D3DPRESENT_PARAMETERS *))VT(self)[9])(self, &pp);
    uint32_t g = ARG(1);
    W32(g, pp.BackBufferWidth); W32(g + 4, pp.BackBufferHeight); W32(g + 8, pp.BackBufferFormat);
    W32(g + 12, pp.BackBufferCount); W32(g + 16, pp.MultiSampleType); W32(g + 20, pp.MultiSampleQuality);
    W32(g + 24, pp.SwapEffect); W32(g + 28, main_hwnd); W32(g + 32, pp.Windowed);
    W32(g + 36, pp.EnableAutoDepthStencil); W32(g + 40, pp.AutoDepthStencilFormat); W32(g + 44, pp.Flags);
    W32(g + 48, pp.FullScreen_RefreshRateInHz); W32(g + 52, pp.PresentationInterval);
    return r;
}

/* ------------------------------------------------------------------ buffer locks */
static uint32_t buffer_lock(CPU *c, int desc_index_size)
{
    void *self = d3d_host(ARG(0));
    Ent *e = ent_of_guest(ARG(0));
    uint32_t off = ARG(1), len = ARG(2), flags = ARG(4);
    if (!e) fatal("buffer lock on an unknown object");
    if (!e->shadow) {
        /* D3DVERTEXBUFFER_DESC / D3DINDEXBUFFER_DESC: Size at 16 */
        uint8_t desc[32];
        ((HRESULT (*)(void *, void *))VT(self)[13])(self, desc);
        memcpy(&e->size, desc + desc_index_size, 4);
        e->shadow = galloc(e->size + 16);
        memset(gptr(e->shadow), 0, e->size);
    }
    if (off > e->size) off = e->size;
    if (!len || off + len > e->size) len = e->size - off;
    void *hp = NULL;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, UINT, UINT, void **, DWORD))VT(self)[11])(self, off, len, &hp, flags);
    if ((int32_t)r < 0 || !hp) {
        W32(ARG(3), 0);
        return r;
    }
    if (flags & D3DLOCK_READONLY)
        memcpy(gptr(e->shadow + off), hp, len);
    if (e->nlocks < 4)
        e->locks[e->nlocks] = (typeof(e->locks[0])){off, (flags & D3DLOCK_READONLY) ? 0 : len, hp};
    e->nlocks++;
    W32(ARG(3), e->shadow + off);
    return r;
}

static uint32_t buffer_unlock(CPU *c)
{
    void *self = d3d_host(ARG(0));
    Ent *e = ent_of_guest(ARG(0));
    if (e && e->nlocks > 0) {
        e->nlocks--;
        if (e->nlocks < 4 && e->locks[e->nlocks].len)
            memcpy(e->locks[e->nlocks].hp, gptr(e->shadow + e->locks[e->nlocks].off), e->locks[e->nlocks].len);
    }
    return (uint32_t)((HRESULT (*)(void *))VT(self)[12])(self);
}

HOSTFN(sp_IDirect3DVertexBuffer9_Lock, 5) { return buffer_lock(c, 16); }
HOSTFN(sp_IDirect3DVertexBuffer9_Unlock, 1) { return buffer_unlock(c); }
HOSTFN(sp_IDirect3DIndexBuffer9_Lock, 5) { return buffer_lock(c, 16); }
HOSTFN(sp_IDirect3DIndexBuffer9_Unlock, 1) { return buffer_unlock(c); }

/* ------------------------------------------------------------------ texture and surface locks */
/* bytes per block and block size of a format (4 for the DXT formats, else 1) */
static int fmt_info(uint32_t f, int *block)
{
    *block = 1;
    switch (f) {
    case D3DFMT_DXT1: *block = 4; return 8;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: *block = 4; return 16;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
    case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: case D3DFMT_G16R16: case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16: case D3DFMT_X8L8V8U8: case D3DFMT_R32F: case D3DFMT_G16R16F:
    case D3DFMT_D32: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24X4S4: case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_A2W10V10U10: case D3DFMT_INDEX32:
        return 4;
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4: case D3DFMT_A8R3G3B2: case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5:
    case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1: case D3DFMT_L16: case D3DFMT_R16F:
    case D3DFMT_A8P8: case D3DFMT_INDEX16:
        return 2;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_R3G3B2: case D3DFMT_A4L4:
        return 1;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_Q16W16V16U16: case D3DFMT_G32R32F:
        return 8;
    case D3DFMT_A32B32G32R32F: return 16;
    }
    return 4;
}

typedef struct Lk { struct Lk *next; void *host; uint32_t sub; uint32_t shadow, bytes; void *hp; int readonly, keep; } Lk;
static Lk *lks;

/* A DYNAMIC or SYSTEMMEM texture's lock gives the program the same copy every time, kept between
   locks: Bink's frame buffers are SYSTEMMEM L8 textures (Y 1280x720, Cr and Cb 640x360, two sets
   [verified: SHOLD_LOG_LOCKS]); it decodes into the buffers it was given whenever it likes and
   redraws only the blocks that changed, so the other blocks must still hold the last frame (with a
   fresh copy per lock the movies showed blocks of stale memory). The copy is the texture's truth
   (the GPU never writes these); it goes when the texture does (d3d_lockcache_drop). */
typedef struct Kept { struct Kept *next; void *host; uint32_t sub, shadow, bytes; } Kept;
static Kept *kept;

void d3d_lockcache_drop(void *host)
{
    pthread_mutex_lock(&dlock);
    for (Kept **pk = &kept; *pk; ) {
        Kept *k = *pk;
        if (k->host == host) { *pk = k->next; gfree(k->shadow); free(k); }
        else pk = &k->next;
    }
    pthread_mutex_unlock(&dlock);
}

static uint32_t rect_lock(CPU *c, void *self, uint32_t sub, int lock_i, D3DSURFACE_DESC *d, uint32_t lr,
                          uint32_t rect, uint32_t flags, HRESULT (*lock)(void *, uint32_t, D3DLOCKED_RECT *, const RECT *, DWORD))
{
    D3DLOCKED_RECT hl;
    /* the whole surface on the host: the program's copy covers it, and a rectangle is an offset */
    HRESULT r = lock(self, sub, &hl, NULL, flags & ~(uint32_t)D3DLOCK_NO_DIRTY_UPDATE);
    (void)lock_i;
    if (r < 0) { W32(lr, 0); W32(lr + 4, 0); return (uint32_t)r; }
    int block, bpb = fmt_info(d->Format, &block);
    uint32_t rows = (d->Height + block - 1) / block;
    uint32_t bytes = (uint32_t)hl.Pitch * rows;
    uint32_t sh = 0;
    int keep = (d->Usage & D3DUSAGE_DYNAMIC) || d->Pool == D3DPOOL_SYSTEMMEM;
    if (getenv("SHOLD_LOG_LOCKS")) {
        static int n;
        if (d->Format < 0x100 && n++ < 80)
            LOG(LOG_ALWAYS, "lock %p sub %u: %ux%u format %u usage %x pool %u flags %x", self, sub,
                d->Width, d->Height, d->Format, d->Usage, d->Pool, flags);
    }
    if (keep) {
        pthread_mutex_lock(&dlock);
        for (Kept *k = kept; k; k = k->next)
            if (k->host == self && k->sub == sub && k->bytes == bytes) { sh = k->shadow; break; }
        pthread_mutex_unlock(&dlock);
    }
    if (!sh) {
        sh = galloc(bytes + 16);
        if (!(flags & D3DLOCK_DISCARD))
            memcpy(gptr(sh), hl.pBits, bytes);
        if (keep) {
            Kept *k = malloc(sizeof *k);
            *k = (Kept){NULL, self, sub, sh, bytes};
            pthread_mutex_lock(&dlock);
            k->next = kept;
            kept = k;
            pthread_mutex_unlock(&dlock);
        }
    }
    uint32_t off = 0;
    if (rect) {
        int32_t x = (int32_t)R32(rect), y = (int32_t)R32(rect + 4);
        off = (uint32_t)(y / block) * (uint32_t)hl.Pitch + (uint32_t)(x / block) * (uint32_t)bpb;
    }
    Lk *k = malloc(sizeof *k);
    *k = (Lk){NULL, self, sub, sh, bytes, hl.pBits, (flags & D3DLOCK_READONLY) != 0, keep};
    pthread_mutex_lock(&dlock);
    k->next = lks;
    lks = k;
    pthread_mutex_unlock(&dlock);
    W32(lr, (uint32_t)hl.Pitch);
    W32(lr + 4, sh + off);
    return (uint32_t)r;
}

static void rect_unlock_copy(void *self, uint32_t sub)
{
    pthread_mutex_lock(&dlock);
    Lk **pk = &lks, *k = NULL;
    for (; *pk; pk = &(*pk)->next)
        if ((*pk)->host == self && (*pk)->sub == sub) { k = *pk; *pk = k->next; break; }
    pthread_mutex_unlock(&dlock);
    if (!k) {
        LOG(LOG_ALWAYS, "unlock of %p (%u) that was not locked", self, sub);
        return;
    }
    if (getenv("SHOLD_LOG_LOCKS")) {
        static int n;
        if (k->bytes >= 262144 && n++ < 40) {
            uint32_t nz = 0;
            for (uint32_t i = 0; i < k->bytes; i += 4) nz += (R32(k->shadow + i) & 0xffffffu) != 0;
            LOG(LOG_ALWAYS, "unlock %p sub %u: %u bytes, %u nonzero dwords", self, sub, k->bytes, nz);
        }
    }
    if (!k->readonly)
        memcpy(k->hp, gptr(k->shadow), k->bytes);
    if (!k->keep)
        gfree(k->shadow);
    free(k);
}

/* the per-interface lock functions, with the subresource in a common form */
static HRESULT tex_lock(void *s, uint32_t sub, D3DLOCKED_RECT *l, const RECT *r, DWORD f)
{ return ((HRESULT (*)(void *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD))VT(s)[19])(s, sub, l, r, f); }
static HRESULT cube_lock(void *s, uint32_t sub, D3DLOCKED_RECT *l, const RECT *r, DWORD f)
{ return ((HRESULT (*)(void *, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT *, const RECT *, DWORD))VT(s)[19])(s, (D3DCUBEMAP_FACES)(sub >> 16), sub & 0xffff, l, r, f); }
static HRESULT surf_lock(void *s, uint32_t sub, D3DLOCKED_RECT *l, const RECT *r, DWORD f)
{ (void)sub; return ((HRESULT (*)(void *, D3DLOCKED_RECT *, const RECT *, DWORD))VT(s)[13])(s, l, r, f); }

HOSTFN(sp_IDirect3DTexture9_LockRect, 5)
{
    void *self = d3d_host(ARG(0));
    D3DSURFACE_DESC d;
    ((HRESULT (*)(void *, UINT, D3DSURFACE_DESC *))VT(self)[17])(self, ARG(1), &d);
    return rect_lock(c, self, ARG(1), 19, &d, ARG(2), ARG(3), ARG(4), tex_lock);
}

HOSTFN(sp_IDirect3DTexture9_UnlockRect, 2)
{
    void *self = d3d_host(ARG(0));
    rect_unlock_copy(self, ARG(1));
    return (uint32_t)((HRESULT (*)(void *, UINT))VT(self)[20])(self, ARG(1));
}

HOSTFN(sp_IDirect3DCubeTexture9_LockRect, 6)
{
    void *self = d3d_host(ARG(0));
    D3DSURFACE_DESC d;
    ((HRESULT (*)(void *, UINT, D3DSURFACE_DESC *))VT(self)[17])(self, ARG(2), &d);
    return rect_lock(c, self, (ARG(1) << 16) | ARG(2), 19, &d, ARG(3), ARG(4), ARG(5), cube_lock);
}

HOSTFN(sp_IDirect3DCubeTexture9_UnlockRect, 3)
{
    void *self = d3d_host(ARG(0));
    rect_unlock_copy(self, (ARG(1) << 16) | ARG(2));
    return (uint32_t)((HRESULT (*)(void *, D3DCUBEMAP_FACES, UINT))VT(self)[20])(self, (D3DCUBEMAP_FACES)ARG(1), ARG(2));
}

HOSTFN(sp_IDirect3DSurface9_LockRect, 4)
{
    void *self = d3d_host(ARG(0));
    D3DSURFACE_DESC d;
    ((HRESULT (*)(void *, D3DSURFACE_DESC *))VT(self)[12])(self, &d);
    return rect_lock(c, self, 0, 13, &d, ARG(1), ARG(2), ARG(3), surf_lock);
}

HOSTFN(sp_IDirect3DSurface9_UnlockRect, 1)
{
    void *self = d3d_host(ARG(0));
    rect_unlock_copy(self, 0);
    return (uint32_t)((HRESULT (*)(void *))VT(self)[14])(self);
}

/* volumes: boxes of slices */
static uint32_t box_lock(CPU *c, void *self, uint32_t sub, const D3DVOLUME_DESC *d, uint32_t lb, uint32_t box,
                         uint32_t flags, HRESULT (*lock)(void *, uint32_t, D3DLOCKED_BOX *, const D3DBOX *, DWORD))
{
    D3DLOCKED_BOX hb;
    HRESULT r = lock(self, sub, &hb, NULL, flags);
    if (r < 0) { W32(lb, 0); W32(lb + 4, 0); W32(lb + 8, 0); return (uint32_t)r; }
    int block, bpb = fmt_info(d->Format, &block);
    uint32_t bytes = (uint32_t)hb.SlicePitch * d->Depth;
    uint32_t sh = galloc(bytes + 16);
    if (!(flags & D3DLOCK_DISCARD))
        memcpy(gptr(sh), hb.pBits, bytes);
    uint32_t off = 0;
    if (box)
        off = R32(box + 16) * (uint32_t)hb.SlicePitch + (R32(box + 4) / block) * (uint32_t)hb.RowPitch +
              (R32(box) / block) * (uint32_t)bpb;
    Lk *k = malloc(sizeof *k);
    *k = (Lk){NULL, self, sub, sh, bytes, hb.pBits, (flags & D3DLOCK_READONLY) != 0};
    pthread_mutex_lock(&dlock);
    k->next = lks;
    lks = k;
    pthread_mutex_unlock(&dlock);
    W32(lb, (uint32_t)hb.RowPitch);
    W32(lb + 4, (uint32_t)hb.SlicePitch);
    W32(lb + 8, sh + off);
    return (uint32_t)r;
}

static HRESULT voltex_lock(void *s, uint32_t sub, D3DLOCKED_BOX *l, const D3DBOX *b, DWORD f)
{ return ((HRESULT (*)(void *, UINT, D3DLOCKED_BOX *, const D3DBOX *, DWORD))VT(s)[19])(s, sub, l, b, f); }
static HRESULT vol_lock(void *s, uint32_t sub, D3DLOCKED_BOX *l, const D3DBOX *b, DWORD f)
{ (void)sub; return ((HRESULT (*)(void *, D3DLOCKED_BOX *, const D3DBOX *, DWORD))VT(s)[9])(s, l, b, f); }

HOSTFN(sp_IDirect3DVolumeTexture9_LockBox, 5)
{
    void *self = d3d_host(ARG(0));
    D3DVOLUME_DESC d;
    ((HRESULT (*)(void *, UINT, D3DVOLUME_DESC *))VT(self)[17])(self, ARG(1), &d);
    return box_lock(c, self, ARG(1), &d, ARG(2), ARG(3), ARG(4), voltex_lock);
}

HOSTFN(sp_IDirect3DVolumeTexture9_UnlockBox, 2)
{
    void *self = d3d_host(ARG(0));
    rect_unlock_copy(self, ARG(1));
    return (uint32_t)((HRESULT (*)(void *, UINT))VT(self)[20])(self, ARG(1));
}

HOSTFN(sp_IDirect3DVolume9_LockBox, 4)
{
    void *self = d3d_host(ARG(0));
    D3DVOLUME_DESC d;
    ((HRESULT (*)(void *, D3DVOLUME_DESC *))VT(self)[8])(self, &d);
    return box_lock(c, self, 0, &d, ARG(1), ARG(2), ARG(3), vol_lock);
}

HOSTFN(sp_IDirect3DVolume9_UnlockBox, 1)
{
    void *self = d3d_host(ARG(0));
    rect_unlock_copy(self, 0);
    return (uint32_t)((HRESULT (*)(void *))VT(self)[10])(self);
}

/* ------------------------------------------------------------------ the rest */
static uint32_t get_container(CPU *c, int index)
{
    void *self = d3d_host(ARG(0));
    GUID g;
    memcpy(&g, gptr(ARG(1)), sizeof g);
    void *o = NULL;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const GUID *, void **))VT(self)[index])(self, &g, &o);
    int kind = DK_IDirect3DResource9;
    for (size_t i = 0; i < sizeof iids / sizeof *iids; i++)
        if (!memcmp(&g, iids[i].iid, sizeof g)) { kind = iids[i].kind; break; }
    W32(ARG(2), (int32_t)r >= 0 && o ? d3d_wrap(kind, o) : 0);
    return r;
}

HOSTFN(sp_IDirect3DSurface9_GetContainer, 3) { return get_container(c, 11); }
HOSTFN(sp_IDirect3DVolume9_GetContainer, 3) { return get_container(c, 7); }

HOSTFN(sp_IDirect3DSurface9_GetDC, 2)
{
    LOG(LOG_ALWAYS, "IDirect3DSurface9::GetDC is not supported");
    if (ARG(1)) W32(ARG(1), 0);
    return 0x8876086cu;
}

HOSTFN(sp_IDirect3DSurface9_ReleaseDC, 2) { return 0x8876086cu; }
