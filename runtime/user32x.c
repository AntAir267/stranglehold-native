/* USER32 beyond windows and messages (user32.c): rectangles, character helpers, system colours
   and metrics, the keyboard, cursor and icons, message boxes, wsprintfA, and placeholders for
   menus, accelerators, dialogs, scroll bars and the clipboard, which MFC links against but LA
   Rush (a Direct3D game in one frame window) does not use. */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include "runtime.h"
#include "win.h"

/* ------------------------------------------------------------------ rectangles */
API(USER32, SetRect, 5) { W32(ARG(0), ARG(1)); W32(ARG(0) + 4, ARG(2)); W32(ARG(0) + 8, ARG(3)); W32(ARG(0) + 12, ARG(4)); return 1; }
API(USER32, SetRectEmpty, 1) { memset(gptr(ARG(0)), 0, 16); return 1; }
API(USER32, CopyRect, 2) { if (!ARG(0) || !ARG(1)) return 0; memmove(gptr(ARG(0)), gptr(ARG(1)), 16); return 1; }
API(USER32, OffsetRect, 3)
{
    uint32_t r = ARG(0);
    int32_t dx = (int32_t)ARG(1), dy = (int32_t)ARG(2);
    W32(r, R32(r) + dx); W32(r + 4, R32(r + 4) + dy);
    W32(r + 8, R32(r + 8) + dx); W32(r + 12, R32(r + 12) + dy);
    return 1;
}
API(USER32, InflateRect, 3)
{
    uint32_t r = ARG(0);
    int32_t dx = (int32_t)ARG(1), dy = (int32_t)ARG(2);
    W32(r, R32(r) - dx); W32(r + 4, R32(r + 4) - dy);
    W32(r + 8, R32(r + 8) + dx); W32(r + 12, R32(r + 12) + dy);
    return 1;
}
static int empty(int32_t l, int32_t t, int32_t r, int32_t b) { return r <= l || b <= t; }
API(USER32, IsRectEmpty, 1)
{
    uint32_t r = ARG(0);
    return !r || empty((int32_t)R32(r), (int32_t)R32(r + 4), (int32_t)R32(r + 8), (int32_t)R32(r + 12));
}
API(USER32, EqualRect, 2) { return !memcmp(gptr(ARG(0)), gptr(ARG(1)), 16); }
API(USER32, PtInRect, 3)
{
    uint32_t r = ARG(0);
    int32_t x = (int32_t)ARG(1), y = (int32_t)ARG(2);
    return x >= (int32_t)R32(r) && x < (int32_t)R32(r + 8) && y >= (int32_t)R32(r + 4) && y < (int32_t)R32(r + 12);
}
API(USER32, IntersectRect, 3)
{
    int32_t l = (int32_t)R32(ARG(1)) > (int32_t)R32(ARG(2)) ? (int32_t)R32(ARG(1)) : (int32_t)R32(ARG(2));
    int32_t t = (int32_t)R32(ARG(1) + 4) > (int32_t)R32(ARG(2) + 4) ? (int32_t)R32(ARG(1) + 4) : (int32_t)R32(ARG(2) + 4);
    int32_t r = (int32_t)R32(ARG(1) + 8) < (int32_t)R32(ARG(2) + 8) ? (int32_t)R32(ARG(1) + 8) : (int32_t)R32(ARG(2) + 8);
    int32_t b = (int32_t)R32(ARG(1) + 12) < (int32_t)R32(ARG(2) + 12) ? (int32_t)R32(ARG(1) + 12) : (int32_t)R32(ARG(2) + 12);
    if (empty(l, t, r, b)) { memset(gptr(ARG(0)), 0, 16); return 0; }
    W32(ARG(0), l); W32(ARG(0) + 4, t); W32(ARG(0) + 8, r); W32(ARG(0) + 12, b);
    return 1;
}
API(USER32, UnionRect, 3)
{
    uint32_t a = ARG(1), b = ARG(2);
    int ea = empty((int32_t)R32(a), (int32_t)R32(a + 4), (int32_t)R32(a + 8), (int32_t)R32(a + 12));
    int eb = empty((int32_t)R32(b), (int32_t)R32(b + 4), (int32_t)R32(b + 8), (int32_t)R32(b + 12));
    if (ea && eb) { memset(gptr(ARG(0)), 0, 16); return 0; }
    if (ea) { memmove(gptr(ARG(0)), gptr(b), 16); return 1; }
    if (eb) { memmove(gptr(ARG(0)), gptr(a), 16); return 1; }
    int32_t v[4];
    for (int i = 0; i < 4; i++) {
        int32_t x = (int32_t)R32(a + 4 * i), y = (int32_t)R32(b + 4 * i);
        v[i] = i < 2 ? (x < y ? x : y) : (x > y ? x : y);
    }
    for (int i = 0; i < 4; i++) W32(ARG(0) + 4 * i, v[i]);
    return 1;
}

/* ------------------------------------------------------------------ characters */
API(USER32, CharLowerA, 1)
{
    if (ARG(0) < 0x10000) return (uint32_t)tolower((int)ARG(0));
    for (char *p = gptr(ARG(0)); *p; p++) *p = (char)tolower((unsigned char)*p);
    return ARG(0);
}
API(USER32, CharUpperA, 1)
{
    if (ARG(0) < 0x10000) return (uint32_t)toupper((int)ARG(0));
    for (char *p = gptr(ARG(0)); *p; p++) *p = (char)toupper((unsigned char)*p);
    return ARG(0);
}
API(USER32, CharLowerW, 1)
{
    if (ARG(0) < 0x10000) return ARG(0) < 0x80 ? (uint32_t)tolower((int)ARG(0)) : ARG(0);
    for (uint32_t p = ARG(0); R16(p); p += 2) if (R16(p) < 0x80) W16(p, tolower((int)R16(p)));
    return ARG(0);
}
API(USER32, CharUpperW, 1)
{
    if (ARG(0) < 0x10000) return ARG(0) < 0x80 ? (uint32_t)toupper((int)ARG(0)) : ARG(0);
    for (uint32_t p = ARG(0); R16(p); p += 2) if (R16(p) < 0x80) W16(p, toupper((int)R16(p)));
    return ARG(0);
}
API(USER32, CharLowerBuffA, 2)
{
    char *p = gptr(ARG(0));
    for (uint32_t i = 0; i < ARG(1); i++) p[i] = (char)tolower((unsigned char)p[i]);
    return ARG(1);
}
API(USER32, CharNextA, 1) { return R8(ARG(0)) ? ARG(0) + 1 : ARG(0); }
API(USER32, CharToOemA, 2) { if (ARG(0) != ARG(1)) strcpy(gptr(ARG(1)), gstr(ARG(0))); return 1; }
API(USER32, OemToCharA, 2) { if (ARG(0) != ARG(1)) strcpy(gptr(ARG(1)), gstr(ARG(0))); return 1; }

/* ------------------------------------------------------------------ system colours, metrics */
static const uint32_t syscolors[31] = {
    0xc8d0d4, 0x6b2408, 0x6a240a, 0x808080, 0xc8d0d4, 0xffffff, 0x000000, 0x000000,
    0x000000, 0xffffff, 0xc8d0d4, 0xc8d0d4, 0x808080, 0x6a240a, 0xffffff, 0xc8d0d4,
    0x808080, 0x808080, 0x000000, 0xc8d0d4, 0xffffff, 0x404040, 0xc8d0d4, 0x000000,
    0xe1ffff, 0xb5b5b5, 0x800000, 0xf0caa6, 0xc0c0c0, 0xd77800, 0xf0f0f0};

API(USER32, GetSysColor, 1) { return ARG(0) < 31 ? syscolors[ARG(0)] : 0; }

API(USER32, GetSysColorBrush, 1)
{
    extern uint32_t gdi_solid_brush(uint32_t color);
    return gdi_solid_brush(ARG(0) < 31 ? syscolors[ARG(0)] : 0);
}

API(USER32, GetSystemMetrics, 1)
{
    int w, h;
    host_desktop_size(&w, &h);
    switch (ARG(0)) {
    case 0: case 16: case 61: case 78: return w;           /* SM_CXSCREEN, full screen, maximized, virtual */
    case 1: case 17: case 62: case 79: return h - (ARG(0) == 17 ? 38 : 0);
    case 2: case 3: return 16;                              /* scroll bar arrows */
    case 4: return 19;                                      /* SM_CYCAPTION */
    case 5: case 6: return 1;                               /* border */
    case 7: case 8: return 3;                               /* dialog frame */
    case 11: case 12: return 32;                            /* icon */
    case 13: case 14: return 32;                            /* cursor */
    case 15: return 19;                                     /* menu */
    case 19: return 1;                                      /* SM_MOUSEPRESENT */
    case 20: case 21: return 16;
    case 28: case 29: return 112;                           /* min track */
    case 30: case 31: return 18;                            /* caption buttons */
    case 32: case 33: return 4;                             /* sizing frame */
    case 36: case 37: return 4;                             /* double-click area */
    case 43: return 3;                                      /* SM_CMOUSEBUTTONS */
    case 49: case 50: return 16;                            /* small icon */
    case 75: return 1;                                      /* SM_MOUSEWHEELPRESENT */
    case 76: case 77: return 0;                             /* virtual screen origin */
    case 80: return 1;                                      /* SM_CMONITORS */
    default:
        LOG(LOG_WIN, "GetSystemMetrics(%u) -> 0", ARG(0));
        return 0;
    }
}

API(USER32, SystemParametersInfoA, 4)
{
    uint32_t act = ARG(0), p = ARG(2);
    switch (act) {
    case 0x30: {                                            /* SPI_GETWORKAREA */
        int w, h;
        host_desktop_size(&w, &h);
        W32(p, 0); W32(p + 4, 0); W32(p + 8, w); W32(p + 12, h - 38);
        return 1;
    }
    case 0x29: {                                            /* SPI_GETNONCLIENTMETRICS */
        uint32_t n = R32(p);
        memset(gptr(p + 4), 0, n > 4 ? n - 4 : 0);
        W32(p, n);
        W32(p + 4, 1); W32(p + 8, 16); W32(p + 12, 16); W32(p + 16, 18); W32(p + 20, 18);
        static const uint32_t fonts[] = {24, 84, 144, 204, 264};   /* the LOGFONTs in it */
        for (int i = 0; i < 5; i++) {
            uint32_t lf = p + fonts[i];
            if (fonts[i] + 60 > n) break;
            W32(lf, (uint32_t)-11); W32(lf + 16, i == 0 ? 700 : 400);
            gstrncpy(lf + 28, "Tahoma", 32);
        }
        return 1;
    }
    case 0x1f: {                                            /* SPI_GETICONTITLELOGFONT */
        memset(gptr(p), 0, 60);
        W32(p, (uint32_t)-11); W32(p + 16, 400);
        gstrncpy(p + 28, "Tahoma", 32);
        return 1;
    }
    case 0x10: case 0x5a: case 0x2c: case 0x48: case 0x1009: case 0x1013:
        if (p) W32(p, 0);                                   /* screen saver off, no drag, ... */
        return 1;
    case 0x68: if (p) W32(p, 3); return 1;                  /* SPI_GETWHEELSCROLLLINES */
    default:
        LOG(LOG_WIN, "SystemParametersInfoA(%x) ignored", act);
        if (p && ARG(1) == 0) W32(p, 0);
        return 1;
    }
}

/* the W form: the same, but the LOGFONTs are LOGFONTW (92 bytes: NONCLIENTMETRICSW is 500) */
static void put_logfont_w(uint32_t lf, int bold)
{
    memset(gptr(lf), 0, 92);
    W32(lf, (uint32_t)-11); W32(lf + 16, bold ? 700 : 400);
    gwput(lf + 28, "Tahoma", 32);
}

API(USER32, SystemParametersInfoW, 4)
{
    uint32_t act = ARG(0), p = ARG(2);
    switch (act) {
    case 0x29: {                                            /* SPI_GETNONCLIENTMETRICS */
        uint32_t n = R32(p);
        memset(gptr(p + 4), 0, n > 4 ? n - 4 : 0);
        W32(p, n);
        W32(p + 4, 1); W32(p + 8, 16); W32(p + 12, 16); W32(p + 16, 18); W32(p + 20, 18);
        W32(p + 116, 12); W32(p + 120, 15);                 /* small caption */
        W32(p + 216, 18); W32(p + 220, 18);                 /* menu */
        static const uint32_t fonts[] = {24, 124, 224, 316, 408};
        for (int i = 0; i < 5; i++)
            if (fonts[i] + 92 <= n) put_logfont_w(p + fonts[i], i == 0);
        return 1;
    }
    case 0x1f:                                              /* SPI_GETICONTITLELOGFONT */
        put_logfont_w(p, 0);
        return 1;
    default:
        return w_USER32_SystemParametersInfoA_body(c);
    }
}

/* one monitor: the desktop */
#define HMONITOR_ 0x00070001u
API(USER32, MonitorFromWindow, 2) { return HMONITOR_; }

API(USER32, GetMonitorInfoW, 2)
{
    uint32_t mi = ARG(1), size = R32(mi);
    if (ARG(0) != HMONITOR_ || (size != 40 && size != 104)) { set_last_error(87); return 0; }
    int w, h;
    host_desktop_size(&w, &h);
    W32(mi + 4, 0); W32(mi + 8, 0); W32(mi + 12, w); W32(mi + 16, h);           /* rcMonitor */
    W32(mi + 20, 0); W32(mi + 24, 0); W32(mi + 28, w); W32(mi + 32, h - 38);    /* rcWork */
    W32(mi + 36, 1);                                                            /* MONITORINFOF_PRIMARY */
    if (size == 104) gwput(mi + 40, "\\\\.\\DISPLAY1", 32);                       /* MONITORINFOEXW */
    return 1;
}

API(USER32, ChangeDisplaySettingsA, 2)
{
    uint32_t dm = ARG(0);
    if (dm) LOG(LOG_ALWAYS, "ChangeDisplaySettingsA(%ux%u, %u bits) not done: the desktop mode is never changed",
                R32(dm + 108), R32(dm + 112), R32(dm + 104));
    return 0;                                               /* DISP_CHANGE_SUCCESSFUL */
}

/* ------------------------------------------------------------------ keyboard */
API(USER32, GetKeyState, 1)
{
    uint32_t vk = ARG(0) & 0xff;
    return host_vk_down((int)vk) ? 0xff80u : 0;
}

API(USER32, GetKeyboardState, 1)
{
    for (int vk = 0; vk < 256; vk++)
        W8(ARG(0) + vk, host_vk_down(vk) ? 0x80 : 0);
    return 1;
}

API(USER32, GetAsyncKeyState, 1) { return host_vk_down(ARG(0) & 0xff) ? 0x8000 : 0; }
API(USER32, GetKeyboardLayout, 1) { return 0x04090409u; }

static uint32_t map_vk(uint32_t code, uint32_t type)
{
    switch (type) {
    case 0: return vk_to_scan(code);
    case 1: case 3: return scan_to_vk(code);
    case 2: return vk_to_char(code, 0);
    }
    return 0;
}
API(USER32, MapVirtualKeyA, 2) { return map_vk(ARG(0), ARG(1)); }
API(USER32, MapVirtualKeyExA, 3) { return map_vk(ARG(0), ARG(1)); }

static int shift_in(uint32_t state)
{
    return state && ((R8(state + 0x10) & 0x80) || (R8(state + 0xa0) & 0x80) || (R8(state + 0xa1) & 0x80));
}

API(USER32, ToAsciiEx, 6)
{
    int ch = vk_to_char(ARG(0), shift_in(ARG(2)));
    if (!ch) return 0;
    W16(ARG(3), (uint16_t)ch);
    return 1;
}

API(USER32, ToUnicode, 6)
{
    int ch = vk_to_char(ARG(0), shift_in(ARG(2)));
    if (!ch || !ARG(4)) return 0;
    W16(ARG(3), (uint16_t)ch);
    return 1;
}

API(USER32, GetKeyNameTextA, 3)
{
    uint32_t scan = (ARG(0) >> 16) & 0x1ff;
    gstrncpy(ARG(1), scan_name(scan), ARG(2));
    return strlen(gptr(ARG(1)));
}

/* ------------------------------------------------------------------ cursor, icons, bitmaps */
static int cursor_count;
API(USER32, LoadIconA, 2) { return 0x00011000u + (ARG(1) & 0xffff); }
API(USER32, LoadCursorA, 2) { return 0x00012000u + (ARG(1) & 0xffff); }
API(USER32, LoadIconW, 2) { return 0x00011000u + (ARG(1) & 0xffff); }
API(USER32, LoadCursorW, 2) { return 0x00012000u + (ARG(1) & 0xffff); }
API(USER32, DestroyIcon, 1) { return 1; }
API(USER32, DestroyCursor, 1) { return 1; }
API(USER32, DrawIcon, 4) { return 1; }
API(USER32, SetCursor, 1) { return 0; }
API(USER32, ShowCursor, 1)
{
    cursor_count += ARG(0) ? 1 : -1;
    host_cursor_visible(cursor_count >= 0);
    return (uint32_t)cursor_count;
}
API(USER32, GetCursorPos, 1) { W32(ARG(0), 0); W32(ARG(0) + 4, 0); return 1; }
API(USER32, SetCursorPos, 2) { return 1; }
API(USER32, ClipCursor, 1) { return 1; }
API(USER32, GetClipCursor, 1)                               /* (never clipped: the desktop) */
{
    int w, h;
    host_desktop_size(&w, &h);
    W32(ARG(0), 0); W32(ARG(0) + 4, 0); W32(ARG(0) + 8, w); W32(ARG(0) + 12, h);
    return 1;
}

API(USER32, LoadBitmapA, 2)
{
    uint32_t data, size;
    if (!resource_find(2, ARG(1), &data, &size)) {
        LOG(LOG_WIN, "LoadBitmapA(%u) -> not found", ARG(1));
        return 0;
    }
    return data;
}

API(USER32, MessageBeep, 1) { return 1; }

static uint32_t message_box(const char *text, const char *cap, uint32_t style)
{
    LOG(LOG_ALWAYS, "MessageBox(\"%s\", \"%s\", %x)", cap ? cap : "", text ? text : "", style);
    host_message_box(cap ? cap : "Stranglehold", text ? text : "");
    uint32_t type = style & 0xf;
    return type == 4 || type == 3 ? 6 : 1;                   /* IDYES for yes/no, else IDOK */
}

API(USER32, MessageBoxA, 4) { return message_box(gstr(ARG(1)), gstr(ARG(2)), ARG(3)); }
API(USER32, MessageBoxW, 4)
{
    char t[2048], k[256];
    return message_box(ARG(1) ? gwstr(ARG(1), t, sizeof t) : NULL, ARG(2) ? gwstr(ARG(2), k, sizeof k) : NULL, ARG(3));
}

API(USER32, WinHelpA, 4) { return 0; }
API(USER32, WinHelpW, 4) { return 0; }

/* ------------------------------------------------------------------ text drawing (none) */
API(USER32, DrawTextA, 5) { return 16; }
API(USER32, DrawTextW, 5) { return 16; }
API(USER32, DrawTextExA, 6) { return 16; }
API(USER32, DrawEdge, 4) { return 1; }
API(USER32, DrawFocusRect, 2) { return 1; }
API(USER32, FillRect, 3) { return 1; }
API(USER32, GrayStringA, 9) { return 1; }
API(USER32, TabbedTextOutA, 8) { return ((uint32_t)16 << 16) | (8u * (uint32_t)(int32_t)ARG(4)); }
API(USER32, GetTabbedTextExtentA, 5) { return ((uint32_t)16 << 16) | (8u * (uint32_t)(int32_t)ARG(2)); }
API(USER32, ScrollWindow, 5) { return 1; }
API(USER32, ScrollWindowEx, 8) { return 1; }

/* ------------------------------------------------------------------ menus (empty) */
#define FAKE_MENU 0x000d0000u
static uint32_t menus;
API(USER32, CreateMenu, 0) { return FAKE_MENU + 4 * ++menus; }
API(USER32, CreatePopupMenu, 0) { return FAKE_MENU + 4 * ++menus; }
API(USER32, LoadMenuA, 2) { LOG(LOG_WIN, "LoadMenuA(%x) (menus are empty)", ARG(1)); return FAKE_MENU + 4 * ++menus; }
API(USER32, DestroyMenu, 1) { return 1; }
API(USER32, GetMenu, 1) { return 0; }
API(USER32, SetMenu, 2) { return 1; }
API(USER32, GetSystemMenu, 2) { return FAKE_MENU; }
API(USER32, GetSubMenu, 2) { return 0; }
API(USER32, GetMenuItemCount, 1) { return 0; }
API(USER32, GetMenuItemID, 2) { return 0xffffffffu; }
API(USER32, GetMenuState, 3) { return 0xffffffffu; }
API(USER32, GetMenuStringA, 5) { if (ARG(2) && ARG(3)) W8(ARG(2), 0); return 0; }
API(USER32, GetMenuItemInfoA, 4) { return 0; }
API(USER32, GetMenuCheckMarkDimensions, 0) { return 0x000d000du; }
API(USER32, EnableMenuItem, 3) { return 0xffffffffu; }
API(USER32, CheckMenuItem, 3) { return 0xffffffffu; }
API(USER32, DeleteMenu, 3) { return 1; }
API(USER32, RemoveMenu, 3) { return 1; }
API(USER32, AppendMenuA, 4) { return 1; }
API(USER32, InsertMenuA, 5) { return 1; }
API(USER32, InsertMenuItemA, 4) { return 1; }
API(USER32, ModifyMenuA, 5) { return 1; }
API(USER32, SetMenuItemBitmaps, 5) { return 1; }
API(USER32, DrawMenuBar, 1) { return 1; }
API(USER32, TrackPopupMenu, 7) { return 0; }
API(USER32, TrackPopupMenuEx, 6) { return 0; }

/* ------------------------------------------------------------------ accelerators (none) */
API(USER32, LoadAcceleratorsA, 2) { return 0x000a0010u; }
API(USER32, CreateAcceleratorTableA, 2) { return 0x000a0010u; }
API(USER32, DestroyAcceleratorTable, 1) { return 1; }
API(USER32, CopyAcceleratorTableA, 3) { return 0; }
API(USER32, TranslateAcceleratorA, 3) { return 0; }
API(USER32, TranslateMDISysAccel, 2) { return 0; }

/* ------------------------------------------------------------------ dialogs (none) */
API(USER32, DialogBoxParamA, 5) { LOG(LOG_ALWAYS, "DialogBoxParamA(template %x): dialogs are not supported", ARG(1)); return 0xffffffffu; }
/* a UTF-16 string in a dialog template (0xffff + ordinal, or text) as UTF-8 into out; returns the
   address after it */
static uint32_t dlg_str(uint32_t p, char *out, size_t n)
{
    size_t k = 0;
    out[0] = 0;
    if (R16(p) == 0xffff) {
        snprintf(out, n, "#%u", R16(p + 2));
        return p + 4;
    }
    for (uint16_t ch; (ch = R16(p)); p += 2)
        if (k + 4 < n) {
            if (ch < 0x80) out[k++] = (char)ch;
            else if (ch < 0x800) { out[k++] = (char)(0xc0 | ch >> 6); out[k++] = (char)(0x80 | (ch & 0x3f)); }
            else { out[k++] = (char)(0xe0 | ch >> 12); out[k++] = (char)(0x80 | ((ch >> 6) & 0x3f)); out[k++] = (char)(0x80 | (ch & 0x3f)); }
        }
    out[k] = 0;
    return p + 2;
}

/* Dialogs are not shown; the program's in-code templates (the engine's message box) are logged so
   what it wanted to say is seen: the title and every control's text. */
API(USER32, DialogBoxIndirectParamW, 5)
{
    uint32_t t = ARG(1);
    char buf[1024];
    if (t && R16(t + 2) != 0xffff) {         /* (a DLGTEMPLATE, not the extended form) */
        uint32_t style = R32(t);
        uint16_t n = R16(t + 8);
        uint32_t p = t + 18;
        p = dlg_str(p, buf, sizeof buf);    /* menu */
        p = dlg_str(p, buf, sizeof buf);    /* class */
        p = dlg_str(p, buf, sizeof buf);
        LOG(LOG_ALWAYS, "DialogBoxIndirectParamW: \"%s\", %u controls (dialogs are not shown)", buf, n);
        if (style & 0x40) p += 2, p = dlg_str(p, buf, sizeof buf);      /* DS_SETFONT: size, name */
        for (int i = 0; i < n && i < 32; i++) {
            p = (p + 3) & ~3u;
            uint16_t id = R16(p + 16);
            p += 18;
            char cls[64];
            p = dlg_str(p, cls, sizeof cls);
            p = dlg_str(p, buf, sizeof buf);
            LOG(LOG_ALWAYS, "  control %u (%s): \"%s\"", id, cls, buf);
            uint16_t extra = R16(p);
            p += 2 + extra;
        }
    } else
        LOG(LOG_ALWAYS, "DialogBoxIndirectParamW: dialogs are not supported");
    return 0xffffffffu;
}
API(USER32, CreateDialogIndirectParamA, 5) { LOG(LOG_ALWAYS, "CreateDialogIndirectParamA: dialogs are not supported"); return 0; }
API(USER32, EndDialog, 2) { return 1; }
API(USER32, IsDialogMessageA, 2) { return 0; }
API(USER32, GetDialogBaseUnits, 0) { return 0x00100008u; }
API(USER32, MapDialogRect, 2)
{
    uint32_t r = ARG(1);
    W32(r, (int32_t)R32(r) * 8 / 4); W32(r + 8, (int32_t)R32(r + 8) * 8 / 4);
    W32(r + 4, (int32_t)R32(r + 4) * 16 / 8); W32(r + 12, (int32_t)R32(r + 12) * 16 / 8);
    return 1;
}
API(USER32, SetDlgItemTextA, 3) { return 1; }
API(USER32, GetDlgItemTextA, 4) { if (ARG(3)) W8(ARG(2), 0); return 0; }
API(USER32, SetDlgItemInt, 4) { return 1; }
API(USER32, GetDlgItemInt, 4) { if (ARG(2)) W32(ARG(2), 0); return 0; }
API(USER32, CheckDlgButton, 3) { return 1; }
API(USER32, CheckRadioButton, 4) { return 1; }
API(USER32, IsDlgButtonChecked, 2) { return 0; }
API(USER32, SendDlgItemMessageA, 5) { return 0; }
API(USER32, GetNextDlgTabItem, 3) { return 0; }
API(USER32, GetNextDlgGroupItem, 3) { return 0; }

/* ------------------------------------------------------------------ scroll bars (none) */
API(USER32, GetScrollInfo, 3) { return 0; }
API(USER32, SetScrollInfo, 4) { return 0; }
API(USER32, GetScrollPos, 2) { return 0; }
API(USER32, SetScrollPos, 4) { return 0; }
API(USER32, GetScrollRange, 4) { if (ARG(2)) W32(ARG(2), 0); if (ARG(3)) W32(ARG(3), 0); return 1; }
API(USER32, SetScrollRange, 5) { return 1; }
API(USER32, ShowScrollBar, 3) { return 1; }

/* ------------------------------------------------------------------ clipboard, DDE, misc (none) */
API(USER32, CountClipboardFormats, 0) { return 0; }
API(USER32, IsClipboardFormatAvailable, 1) { return 0; }
API(USER32, ReuseDDElParam, 5) { return ((ARG(4) & 0xffff) << 16) | (ARG(3) & 0xffff); }
API(USER32, UnpackDDElParam, 4) { if (ARG(2)) W32(ARG(2), ARG(1) & 0xffff); if (ARG(3)) W32(ARG(3), ARG(1) >> 16); return 1; }
API(USER32, SetWindowRgn, 3) { return 1; }
API(USER32, ShowOwnedPopups, 2) { return 1; }

/* ------------------------------------------------------------------ wsprintfA */
/* Windows' own printf subset: %[-][#][0][width][.prec][l|h]{d,i,u,x,X,c,C,s,S}, and for the
   game's debug messages (game.c) the C library's f, e, g (a double: 8 bytes). wide: wsprintfW,
   where %s and %c take UTF-16 and %S, %C 8-bit (wsprintfA here reads all as 8-bit). */
static int wsprintf_fmt(char *out, size_t cap, const char *fmt, uint32_t args, int wide)
{
    size_t k = 0;
    for (const char *p = fmt; *p && k + 1 < cap; p++) {
        if (*p != '%') { out[k++] = *p; continue; }
        p++;
        if (*p == '%') { out[k++] = '%'; continue; }
        char spec[32] = "%";
        int s = 1;
        while (*p == '-' || *p == '#' || *p == '0' || *p == '+' || *p == ' ')
            spec[s++] = *p++;
        while (isdigit((unsigned char)*p) && s < 20) spec[s++] = *p++;
        if (*p == '.') { spec[s++] = *p++; while (isdigit((unsigned char)*p) && s < 28) spec[s++] = *p++; }
        while (*p == 'l' || *p == 'h') p++;
        char conv = *p;
        if (!conv) break;
        char tmp[512];
        uint32_t v = R32(args);
        args += 4;
        switch (conv) {
        case 'd': case 'i': spec[s++] = 'd'; spec[s] = 0; snprintf(tmp, sizeof tmp, spec, (int32_t)v); break;
        case 'u': spec[s++] = 'u'; spec[s] = 0; snprintf(tmp, sizeof tmp, spec, v); break;
        case 'x': case 'X': spec[s++] = conv; spec[s] = 0; snprintf(tmp, sizeof tmp, spec, v); break;
        case 'c': case 'C': {
            uint32_t ch = (wide && conv == 'c') ? (v & 0xffff) : (v & 0xff);
            spec[s++] = 'c'; spec[s] = 0; snprintf(tmp, sizeof tmp, spec, ch < 0x100 ? (int)ch : '?');
            break;
        }
        case 's': case 'S': {
            char wb[512];
            const char *str = !v ? "(null)" : (wide && conv == 's') ? gwstr(v, wb, sizeof wb) : gstr(v);
            spec[s++] = 's'; spec[s] = 0; snprintf(tmp, sizeof tmp, spec, str);
            break;
        }
        case 'f': case 'e': case 'g': {
            double d;
            uint32_t w[2] = {v, R32(args)};
            memcpy(&d, w, 8);
            args += 4;
            spec[s++] = conv; spec[s] = 0; snprintf(tmp, sizeof tmp, spec, d);
            break;
        }
        default: tmp[0] = 0; args -= 4; break;
        }
        for (char *t = tmp; *t && k + 1 < cap; t++)
            out[k++] = *t;
    }
    out[k] = 0;
    return (int)k;
}

int wsprintf_impl(char *out, size_t cap, const char *fmt, uint32_t args)
{
    return wsprintf_fmt(out, cap, fmt, args, 0);
}

API_CDECL(USER32, wsprintfA)
{
    char buf[1025];
    int n = wsprintf_impl(buf, sizeof buf, gstr(ARG(1)), c->esp + 12);
    memcpy(gptr(ARG(0)), buf, n + 1);
    return n;
}

API_CDECL(USER32, wsprintfW)
{
    char fmt[1025], buf[1025];
    wsprintf_fmt(buf, sizeof buf, gwstr(ARG(1), fmt, sizeof fmt), c->esp + 12, 1);
    return gwput(ARG(0), buf, sizeof buf);
}
