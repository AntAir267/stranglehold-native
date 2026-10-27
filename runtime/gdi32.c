/* GDI32: device contexts and objects with plausible answers (a 32-bit 96-dpi display), for MFC's
   housekeeping. Nothing is drawn: LA Rush renders with Direct3D. A bitmap resource drawn to a
   window (a splash screen) is shown through the host window. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <strings.h>
#include <pthread.h>
#include "runtime.h"
#include "win.h"

enum { O_NONE, O_PEN = 1, O_BRUSH = 2, O_DC = 3, O_PAL = 5, O_FONT = 6, O_BITMAP = 7, O_REGION = 8,
       O_MEMDC = 10, O_EXTPEN = 11 };            /* GetObjectType's OBJ_ values */

typedef struct {
    int type;
    uint32_t color, style, width;              /* pens, brushes */
    uint8_t logfont[60];                        /* fonts */
    int32_t bw, bh, bpp;                        /* bitmaps */
    uint32_t bits;                              /* DIB section bits (program memory) or a resource */
    int32_t rl, rt, rr, rb;                     /* regions: a rectangle */
    /* DCs */
    uint32_t hwnd, sel_pen, sel_brush, sel_font, sel_bitmap, sel_pal;
    uint32_t bkcolor, textcolor, bkmode, mapmode, rop2, align, polyfill, stretch;
    int32_t curx, cury, vox, voy, wox, woy, vex, vey, wex, wey;
    int saved;
} Obj;

#define MAXOBJ 4096
static Obj objs[MAXOBJ];
static pthread_mutex_t olock = PTHREAD_MUTEX_INITIALIZER;
#define HBASE 0x01000000u
static uint32_t stock[20];

static uint32_t obj_new(int type)
{
    pthread_mutex_lock(&olock);
    for (int i = 1; i < MAXOBJ; i++)
        if (!objs[i].type) {
            memset(&objs[i], 0, sizeof objs[i]);
            objs[i].type = type;
            pthread_mutex_unlock(&olock);
            return HBASE + 4u * i;
        }
    pthread_mutex_unlock(&olock);
    LOG(LOG_ALWAYS, "GDI: out of objects");
    return 0;
}

static Obj *obj(uint32_t h)
{
    if (h < HBASE || (h - HBASE) % 4) return NULL;
    uint32_t i = (h - HBASE) / 4;
    return i < MAXOBJ && objs[i].type ? &objs[i] : NULL;
}

static void dc_defaults(Obj *d)
{
    d->sel_pen = stock[7]; d->sel_brush = stock[0]; d->sel_font = stock[13]; d->sel_pal = stock[15];
    d->bkcolor = 0xffffff; d->textcolor = 0; d->bkmode = 2; d->mapmode = 1; d->rop2 = 13;
    d->polyfill = 1; d->stretch = 1; d->vex = d->vey = d->wex = d->wey = 1;
}

static void stock_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    static const uint32_t colors[6] = {0xffffff, 0xc0c0c0, 0x808080, 0x404040, 0x000000, 0};
    for (int i = 0; i <= 5; i++) {                  /* WHITE_BRUSH .. NULL_BRUSH */
        stock[i] = obj_new(O_BRUSH);
        obj(stock[i])->color = colors[i];
        obj(stock[i])->style = i == 5 ? 1 : 0;
    }
    for (int i = 6; i <= 8; i++) {                  /* WHITE_PEN, BLACK_PEN, NULL_PEN */
        stock[i] = obj_new(O_PEN);
        obj(stock[i])->color = i == 6 ? 0xffffff : 0;
        obj(stock[i])->style = i == 8 ? 5 : 0;
    }
    for (int i = 10; i <= 17; i++) {                /* fonts, DEFAULT_PALETTE (15) */
        if (i == 15) { stock[i] = obj_new(O_PAL); continue; }
        stock[i] = obj_new(O_FONT);
        Obj *f = obj(stock[i]);
        int32_t h = -11;
        memcpy(f->logfont, &h, 4);
        uint32_t weight = 400;
        memcpy(f->logfont + 16, &weight, 4);
        snprintf((char *)f->logfont + 28, 32, "%s", i == 10 || i == 11 ? "Courier" : "MS Sans Serif");
    }
    stock[18] = obj_new(O_BRUSH);                    /* DC_BRUSH */
    stock[19] = obj_new(O_PEN);                      /* DC_PEN */
}

/* each window's DC (GetDC/BeginPaint): one per window, kept */
uint32_t gdi_window_dc(uint32_t hwnd)
{
    stock_init();
    for (int i = 1; i < MAXOBJ; i++)
        if (objs[i].type == O_DC && objs[i].hwnd == hwnd)
            return HBASE + 4u * i;
    uint32_t h = obj_new(O_DC);
    Obj *d = obj(h);
    d->hwnd = hwnd ? hwnd : 0xffffffffu;
    dc_defaults(d);
    return h;
}

uint32_t gdi_solid_brush(uint32_t color)
{
    stock_init();
    uint32_t h = obj_new(O_BRUSH);
    if (h) obj(h)->color = color;
    return h;
}

API(GDI32, GetStockObject, 1) { stock_init(); return ARG(0) < 20 ? stock[ARG(0)] : 0; }

API(GDI32, CreateCompatibleDC, 1)
{
    stock_init();
    uint32_t h = obj_new(O_MEMDC);
    if (h) dc_defaults(obj(h));
    return h;
}

static uint32_t create_dc(const char *driver, const char *device)
{
    stock_init();
    uint32_t h = obj_new(O_DC);
    if (h) { dc_defaults(obj(h)); obj(h)->hwnd = 0xfffffffeu; }
    LOG(LOG_WIN, "CreateDC(\"%s\", \"%s\") -> %x", driver ? driver : "", device ? device : "", h);
    return h;
}

API(GDI32, CreateDCA, 4) { return create_dc(gstr(ARG(0)), gstr(ARG(1))); }
API(GDI32, CreateDCW, 4)
{
    char a[128], b[128];
    return create_dc(gwstr(ARG(0), a, sizeof a), gwstr(ARG(1), b, sizeof b));
}

API(GDI32, DeleteDC, 1) { Obj *o = obj(ARG(0)); if (o && (o->type == O_DC || o->type == O_MEMDC)) o->type = 0; return 1; }

API(GDI32, DeleteObject, 1)
{
    Obj *o = obj(ARG(0));
    for (int i = 0; i < 20; i++) if (stock[i] == ARG(0)) return 1;
    if (!o) return 0;
    if (o->type == O_BITMAP && o->bits && o->bits >= ARENA_START && !module_at(o->bits))
        gfree(o->bits);
    o->type = 0;
    return 1;
}

API(GDI32, UnrealizeObject, 1) { return 1; }

API(GDI32, GetObjectType, 1) { Obj *o = obj(ARG(0)); return o ? (uint32_t)o->type : 0; }

API(GDI32, SelectObject, 2)
{
    Obj *d = obj(ARG(0)), *o = obj(ARG(1));
    if (!d || !o) return 0;
    uint32_t old = 0;
    switch (o->type) {
    case O_PEN: case O_EXTPEN: old = d->sel_pen; d->sel_pen = ARG(1); break;
    case O_BRUSH: old = d->sel_brush; d->sel_brush = ARG(1); break;
    case O_FONT: old = d->sel_font; d->sel_font = ARG(1); break;
    case O_BITMAP: old = d->sel_bitmap ? d->sel_bitmap : obj_new(O_BITMAP); d->sel_bitmap = ARG(1); break;
    case O_REGION: return 2;                       /* SIMPLEREGION */
    }
    return old;
}

API(GDI32, SelectPalette, 3) { Obj *d = obj(ARG(0)); uint32_t o = d ? d->sel_pal : 0; if (d) d->sel_pal = ARG(1); return o; }

/* fonts are kept as LOGFONTA (60 bytes); LOGFONTW is the same with the face name in UTF-16 (92) */
static void logfont_to_w(const uint8_t *a, uint8_t *w)
{
    memcpy(w, a, 28);
    for (int i = 0; i < 32; i++) { w[28 + 2 * i] = a[28 + i]; w[29 + 2 * i] = 0; }
    w[90] = w[91] = 0;
}

static uint32_t get_object(uint32_t h, uint32_t n, uint32_t out, int wide)
{
    Obj *o = obj(h);
    if (!o) return 0;
    switch (o->type) {
    case O_FONT:
        if (wide) {
            uint8_t lw[92];
            if (!out) return 92;
            logfont_to_w(o->logfont, lw);
            memcpy(gptr(out), lw, n < 92 ? n : 92);
            return n < 92 ? n : 92;
        }
        if (!out) return 60;
        memcpy(gptr(out), o->logfont, n < 60 ? n : 60);
        return n < 60 ? n : 60;
    case O_BITMAP: {
        if (!out) return 24;
        uint8_t b[24] = {0};
        int32_t w = o->bw, h = o->bh, stride = ((o->bw * o->bpp + 15) / 16) * 2;
        memcpy(b + 4, &w, 4); memcpy(b + 8, &h, 4); memcpy(b + 12, &stride, 4);
        b[16] = 1; b[18] = (uint8_t)o->bpp;
        memcpy(b + 20, &o->bits, 4);
        memcpy(gptr(out), b, n < 24 ? n : 24);
        return n < 24 ? n : 24;
    }
    case O_BRUSH:
        if (!out) return 12;
        if (n >= 12) { W32(out, o->style); W32(out + 4, o->color); W32(out + 8, 0); }
        return 12;
    case O_PEN:
        if (!out) return 16;
        if (n >= 16) { W32(out, o->style); W32(out + 4, o->width); W32(out + 8, 0); W32(out + 12, o->color); }
        return 16;
    case O_PAL:
        if (out && n >= 2) W16(out, 20);
        return 2;
    }
    return 0;
}

API(GDI32, GetObjectA, 3) { return get_object(ARG(0), ARG(1), ARG(2), 0); }
API(GDI32, GetObjectW, 3) { return get_object(ARG(0), ARG(1), ARG(2), 1); }

/* ------------------------------------------------------------------ object creation */
API(GDI32, CreateSolidBrush, 1) { return gdi_solid_brush(ARG(0)); }
API(GDI32, CreateHatchBrush, 2) { uint32_t h = gdi_solid_brush(ARG(1)); if (h) obj(h)->style = 2; return h; }
API(GDI32, CreatePatternBrush, 1) { uint32_t h = gdi_solid_brush(0); if (h) obj(h)->style = 3; return h; }
API(GDI32, CreateDIBPatternBrushPt, 2) { uint32_t h = gdi_solid_brush(0); if (h) obj(h)->style = 6; return h; }

API(GDI32, CreatePen, 3)
{
    stock_init();
    uint32_t h = obj_new(O_PEN);
    if (h) { obj(h)->style = ARG(0); obj(h)->width = ARG(1); obj(h)->color = ARG(2); }
    return h;
}

API(GDI32, ExtCreatePen, 5)
{
    stock_init();
    uint32_t h = obj_new(O_EXTPEN);
    if (h) { obj(h)->style = ARG(0); obj(h)->width = ARG(1); obj(h)->color = ARG(2) ? R32(ARG(2) + 4) : 0; }
    return h;
}

API(GDI32, CreateFontIndirectA, 1)
{
    stock_init();
    uint32_t h = obj_new(O_FONT);
    if (h) memcpy(obj(h)->logfont, gptr(ARG(0)), 60);
    return h;
}

API(GDI32, CreateFontIndirectW, 1)
{
    stock_init();
    uint32_t h = obj_new(O_FONT);
    if (!h) return 0;
    uint8_t *lf = obj(h)->logfont;
    memcpy(lf, gptr(ARG(0)), 28);
    gwstr(ARG(0) + 28, (char *)lf + 28, 32);
    return h;
}

API(GDI32, CreateFontA, 14)
{
    stock_init();
    uint32_t h = obj_new(O_FONT);
    if (!h) return 0;
    uint8_t *lf = obj(h)->logfont;
    for (int i = 0; i < 5; i++) { uint32_t v = ARG(i); memcpy(lf + 4 * i, &v, 4); }
    for (int i = 0; i < 8; i++) lf[20 + i] = (uint8_t)ARG(5 + i);
    if (ARG(13)) snprintf((char *)lf + 28, 32, "%s", gstr(ARG(13)));
    return h;
}

static uint32_t bitmap_new(int32_t w, int32_t h, int bpp)
{
    stock_init();
    uint32_t b = obj_new(O_BITMAP);
    if (b) { obj(b)->bw = w; obj(b)->bh = h < 0 ? -h : h; obj(b)->bpp = bpp; }
    return b;
}

API(GDI32, CreateBitmap, 5) { return bitmap_new((int32_t)ARG(0), (int32_t)ARG(1), (int)(ARG(2) * ARG(3))); }
API(GDI32, CreateCompatibleBitmap, 3) { return bitmap_new((int32_t)ARG(1), (int32_t)ARG(2), 32); }

API(GDI32, CreateDIBSection, 6)
{
    uint32_t bmi = ARG(1);
    int32_t w = (int32_t)R32(bmi + 4), h = (int32_t)R32(bmi + 8);
    int bpp = R16(bmi + 14);
    uint32_t b = bitmap_new(w, h, bpp);
    if (!b) return 0;
    uint32_t stride = (uint32_t)((w * bpp + 31) / 32) * 4;
    uint32_t bits = galloc(stride * (uint32_t)(h < 0 ? -h : h) + 16);
    memset(gptr(bits), 0, stride * (uint32_t)(h < 0 ? -h : h));
    obj(b)->bits = bits;
    if (ARG(3)) W32(ARG(3), bits);
    LOG(LOG_WIN, "CreateDIBSection(%dx%d, %d bits) -> %x (bits %08x)", w, h, bpp, b, bits);
    return b;
}

static uint32_t region(int32_t l, int32_t t, int32_t r, int32_t b)
{
    uint32_t h = obj_new(O_REGION);
    if (h) { Obj *o = obj(h); o->rl = l; o->rt = t; o->rr = r; o->rb = b; }
    return h;
}

API(GDI32, CreateRectRgn, 4) { return region((int32_t)ARG(0), (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3)); }
API(GDI32, CreateRectRgnIndirect, 1) { uint32_t r = ARG(0); return region((int32_t)R32(r), (int32_t)R32(r + 4), (int32_t)R32(r + 8), (int32_t)R32(r + 12)); }
API(GDI32, CreateEllipticRgn, 4) { return region((int32_t)ARG(0), (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3)); }
API(GDI32, SetRectRgn, 5) { Obj *o = obj(ARG(0)); if (o) { o->rl = (int32_t)ARG(1); o->rt = (int32_t)ARG(2); o->rr = (int32_t)ARG(3); o->rb = (int32_t)ARG(4); } return 1; }
API(GDI32, CombineRgn, 4) { return 2; }
API(GDI32, OffsetRgn, 3) { Obj *o = obj(ARG(0)); if (o) { o->rl += (int32_t)ARG(1); o->rr += (int32_t)ARG(1); o->rt += (int32_t)ARG(2); o->rb += (int32_t)ARG(2); } return 2; }
API(GDI32, GetRgnBox, 2)
{
    Obj *o = obj(ARG(0));
    if (!o) return 0;
    W32(ARG(1), o->rl); W32(ARG(1) + 4, o->rt); W32(ARG(1) + 8, o->rr); W32(ARG(1) + 12, o->rb);
    return 2;
}

/* ------------------------------------------------------------------ device capabilities */
API(GDI32, GetDeviceCaps, 2)
{
    int w, h;
    host_desktop_size(&w, &h);
    switch (ARG(1)) {
    case 0: return 0x300;                       /* DRIVERVERSION */
    case 2: return 1;                           /* TECHNOLOGY: DT_RASDISPLAY */
    case 4: return w * 254 / 960;               /* HORZSIZE mm */
    case 6: return h * 254 / 960;
    case 8: return w;                           /* HORZRES */
    case 10: return h;
    case 12: return 32;                         /* BITSPIXEL */
    case 14: return 1;                          /* PLANES */
    case 24: return 0xffffffffu;                /* NUMCOLORS: true colour */
    case 38: return 0x7e99;                     /* RASTERCAPS */
    case 40: return 36; case 42: return 36; case 44: return 51;   /* aspect */
    case 88: case 90: return 96;                /* LOGPIXELSX/Y */
    case 104: return 0;                         /* SIZEPALETTE */
    case 116: return 60;                        /* VREFRESH */
    case 117: return w; case 118: return h;     /* DESKTOPHORZRES/VERTRES */
    default: return 0;
    }
}

/* ------------------------------------------------------------------ DC attributes */
#define DCATTR(name, field) \
    API(GDI32, Set##name, 2) { Obj *d = obj(ARG(0)); uint32_t o = d ? d->field : 0; if (d) d->field = ARG(1); return o; } \
    API(GDI32, Get##name, 1) { Obj *d = obj(ARG(0)); return d ? d->field : 0; }
DCATTR(BkColor, bkcolor)
DCATTR(TextColor, textcolor)
DCATTR(BkMode, bkmode)
DCATTR(MapMode, mapmode)
DCATTR(ROP2, rop2)
DCATTR(TextAlign, align)
DCATTR(PolyFillMode, polyfill)
DCATTR(StretchBltMode, stretch)

API(GDI32, SaveDC, 1) { Obj *d = obj(ARG(0)); return d ? (uint32_t)++d->saved : 0; }
API(GDI32, RestoreDC, 2) { Obj *d = obj(ARG(0)); if (d && d->saved) d->saved--; return 1; }
API(GDI32, GetNearestColor, 2) { return ARG(1) & 0xffffff; }
API(GDI32, SetTextCharacterExtra, 2) { return 0; }
API(GDI32, SetTextJustification, 3) { return 1; }
API(GDI32, SetMapperFlags, 2) { return 0; }
API(GDI32, SetArcDirection, 2) { return 1; }
API(GDI32, SetBrushOrgEx, 4) { if (ARG(3)) { W32(ARG(3), 0); W32(ARG(3) + 4, 0); } return 1; }
API(GDI32, SetColorAdjustment, 2) { return 1; }

static int pt_out(uint32_t p, int32_t x, int32_t y) { if (p) { W32(p, x); W32(p + 4, y); } return 1; }
API(GDI32, SetViewportOrgEx, 4) { Obj *d = obj(ARG(0)); if (!d) return 0; pt_out(ARG(3), d->vox, d->voy); d->vox = (int32_t)ARG(1); d->voy = (int32_t)ARG(2); return 1; }
API(GDI32, SetWindowOrgEx, 4) { Obj *d = obj(ARG(0)); if (!d) return 0; pt_out(ARG(3), d->wox, d->woy); d->wox = (int32_t)ARG(1); d->woy = (int32_t)ARG(2); return 1; }
API(GDI32, OffsetViewportOrgEx, 4) { Obj *d = obj(ARG(0)); if (!d) return 0; pt_out(ARG(3), d->vox, d->voy); d->vox += (int32_t)ARG(1); d->voy += (int32_t)ARG(2); return 1; }
API(GDI32, OffsetWindowOrgEx, 4) { Obj *d = obj(ARG(0)); if (!d) return 0; pt_out(ARG(3), d->wox, d->woy); d->wox += (int32_t)ARG(1); d->woy += (int32_t)ARG(2); return 1; }
API(GDI32, GetViewportOrgEx, 2) { Obj *d = obj(ARG(0)); return d ? pt_out(ARG(1), d->vox, d->voy) : 0; }
API(GDI32, GetWindowOrgEx, 2) { Obj *d = obj(ARG(0)); return d ? pt_out(ARG(1), d->wox, d->woy) : 0; }
API(GDI32, SetViewportExtEx, 4) { Obj *d = obj(ARG(0)); if (!d) return 0; pt_out(ARG(3), d->vex, d->vey); d->vex = (int32_t)ARG(1); d->vey = (int32_t)ARG(2); return 1; }
API(GDI32, SetWindowExtEx, 4) { Obj *d = obj(ARG(0)); if (!d) return 0; pt_out(ARG(3), d->wex, d->wey); d->wex = (int32_t)ARG(1); d->wey = (int32_t)ARG(2); return 1; }
API(GDI32, GetViewportExtEx, 2) { Obj *d = obj(ARG(0)); return d ? pt_out(ARG(1), d->vex, d->vey) : 0; }
API(GDI32, GetWindowExtEx, 2) { Obj *d = obj(ARG(0)); return d ? pt_out(ARG(1), d->wex, d->wey) : 0; }
API(GDI32, ScaleViewportExtEx, 6) { return pt_out(ARG(5), 1, 1); }
API(GDI32, ScaleWindowExtEx, 6) { return pt_out(ARG(5), 1, 1); }
API(GDI32, DPtoLP, 3) { return 1; }
API(GDI32, LPtoDP, 3) { return 1; }
API(GDI32, MoveToEx, 4) { Obj *d = obj(ARG(0)); if (!d) return 0; pt_out(ARG(3), d->curx, d->cury); d->curx = (int32_t)ARG(1); d->cury = (int32_t)ARG(2); return 1; }
API(GDI32, GetCurrentPositionEx, 2) { Obj *d = obj(ARG(0)); return d ? pt_out(ARG(1), d->curx, d->cury) : 0; }

/* ------------------------------------------------------------------ clipping */
API(GDI32, GetClipBox, 2)
{
    int w, h;
    host_desktop_size(&w, &h);
    W32(ARG(1), 0); W32(ARG(1) + 4, 0); W32(ARG(1) + 8, w); W32(ARG(1) + 12, h);
    return 2;
}
API(GDI32, GetClipRgn, 2) { return 0; }
API(GDI32, SelectClipRgn, 2) { return 2; }
API(GDI32, ExtSelectClipRgn, 3) { return 2; }
API(GDI32, SelectClipPath, 2) { return 1; }
API(GDI32, IntersectClipRect, 5) { return 2; }
API(GDI32, ExcludeClipRect, 5) { return 2; }
API(GDI32, OffsetClipRgn, 3) { return 2; }
API(GDI32, PtVisible, 3) { return 1; }
API(GDI32, RectVisible, 2) { return 1; }

/* ------------------------------------------------------------------ text */
static int font_height(Obj *d)
{
    Obj *f = d ? obj(d->sel_font) : NULL;
    int32_t h = 0;
    if (f) memcpy(&h, f->logfont, 4);
    if (h < 0) h = -h;
    return h ? h * 4 / 3 : 16;
}

API(GDI32, GetTextMetricsA, 2)
{
    Obj *d = obj(ARG(0));
    int h = font_height(d);
    uint32_t t = ARG(1);
    memset(gptr(t), 0, 56);
    W32(t, h); W32(t + 4, h * 4 / 5); W32(t + 8, h - h * 4 / 5);
    W32(t + 20, h / 2); W32(t + 24, h);                     /* average and maximum widths */
    W32(t + 28, 400);
    W32(t + 44, 0x20); W8(t + 45, 0x7e); W8(t + 46, 0x1f); W8(t + 47, 0x20);
    W8(t + 52, 0x26);                                       /* TMPF_VECTOR | TRUETYPE, FF_SWISS */
    return 1;
}

API(GDI32, GetTextExtentPoint32A, 4)
{
    Obj *d = obj(ARG(0));
    int h = font_height(d);
    W32(ARG(3), (uint32_t)((int32_t)ARG(2) * (h / 2)));
    W32(ARG(3) + 4, h);
    return 1;
}

API(GDI32, GetCharWidthA, 4)
{
    Obj *d = obj(ARG(0));
    int h = font_height(d);
    for (uint32_t ch = ARG(1); ch <= ARG(2); ch++)
        W32(ARG(3) + 4 * (ch - ARG(1)), h / 2);
    return 1;
}

API(GDI32, GetTextFaceA, 3)
{
    Obj *d = obj(ARG(0));
    Obj *f = d ? obj(d->sel_font) : NULL;
    const char *n = f ? (const char *)f->logfont + 28 : "MS Sans Serif";
    if (ARG(2)) gstrncpy(ARG(2), n, ARG(1));
    return strlen(n) + 1;
}

API(GDI32, EnumFontFamiliesExA, 5) { return 1; }
API(GDI32, TextOutA, 5) { return 1; }
API(GDI32, ExtTextOutA, 8) { return 1; }

/* ------------------------------------------------------------------ drawing (nothing drawn) */
API(GDI32, PatBlt, 6) { return 1; }
API(GDI32, BitBlt, 9)
{
    Obj *s = obj(ARG(5));
    Obj *b = s ? obj(s->sel_bitmap) : NULL;
    if (b && b->bits && module_at(b->bits))
        host_show_bitmap(b->bits, (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4));
    return 1;
}
API(GDI32, StretchBlt, 11) { return 1; }
API(GDI32, StretchDIBits, 13) { return (uint32_t)(int32_t)ARG(8); }
API(GDI32, GetDIBits, 7) { return ARG(3); }
API(GDI32, GetPixel, 3) { return 0; }
API(GDI32, SetPixel, 4) { return ARG(3); }
API(GDI32, LineTo, 3) { Obj *d = obj(ARG(0)); if (d) { d->curx = (int32_t)ARG(1); d->cury = (int32_t)ARG(2); } return 1; }
API(GDI32, PolylineTo, 3) { return 1; }
API(GDI32, PolyBezierTo, 3) { return 1; }
API(GDI32, PolyDraw, 4) { return 1; }
API(GDI32, ArcTo, 9) { return 1; }
API(GDI32, Rectangle, 5) { return 1; }
API(GDI32, Ellipse, 5) { return 1; }
API(GDI32, Escape, 5) { return 0; }

/* ------------------------------------------------------------------ printing, metafiles (none) */
API(GDI32, StartDocA, 2) { return 0; }
API(GDI32, EndDoc, 1) { return 0; }
API(GDI32, AbortDoc, 1) { return 0; }
API(GDI32, StartPage, 1) { return 0; }
API(GDI32, EndPage, 1) { return 0; }
API(GDI32, SetAbortProc, 2) { return 1; }
API(GDI32, CreateMetaFileA, 1) { return 0; }
API(GDI32, CloseMetaFile, 1) { return 0; }
API(GDI32, DeleteMetaFile, 1) { return 1; }
API(GDI32, CopyMetaFileA, 2) { return 0; }
API(GDI32, PlayMetaFile, 2) { return 1; }
API(GDI32, PlayMetaFileRecord, 4) { return 1; }
API(GDI32, EnumMetaFile, 4) { return 1; }
