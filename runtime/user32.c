/* USER32: window classes, windows, their procedures and extra data, the creation hook MFC
   subclasses with, posted and sent messages, timers, painting, properties. The first top-level
   window shown becomes the host window (SDL, win.c), whose input arrives as its messages.
   Menus, dialogs, scroll bars and the clipboard are in user32x.c (mostly placeholders: LA Rush
   draws everything with Direct3D). */
#define _GNU_SOURCE
#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include "runtime.h"
#include "win.h"

#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_SETTEXT 0x000c
#define WM_GETTEXT 0x000d
#define WM_GETTEXTLENGTH 0x000e
#define WM_PAINT 0x000f
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001c
#define WM_SETCURSOR 0x0020
#define WM_MOUSEACTIVATE 0x0021
#define WM_GETMINMAXINFO 0x0024
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCACTIVATE 0x0086
#define WM_KEYDOWN 0x0100
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSCHAR 0x0106
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113

#define WS_CHILD 0x40000000u
#define WS_VISIBLE 0x10000000u
#define WS_DISABLED 0x08000000u
#define WS_CAPTION 0x00c00000u
#define WS_THICKFRAME 0x00040000u
#define WS_POPUP 0x80000000u
#define CW_USEDEFAULT 0x80000000u

uint32_t main_hwnd;                     /* the window shown in the host window */
int IsChild_check(uint32_t parent, uint32_t child);
static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ classes */
typedef struct {
    char name[96];
    uint32_t atom, style, proc, clsextra, wndextra, hinst, icon, cursor, brush, menuname, iconsm;
    uint8_t extra[64];
} Cls;
#define MAXCLS 128
static Cls classes[MAXCLS];
static int nclasses;

static Cls *class_find(uint32_t name)
{
    for (int i = 0; i < nclasses; i++) {
        if (name < 0x10000 ? classes[i].atom == name : !strcasecmp(classes[i].name, gptr(name)))
            return &classes[i];
    }
    return NULL;
}

static uint32_t class_add(uint32_t style, uint32_t proc, uint32_t clsextra, uint32_t wndextra, uint32_t hinst,
                          uint32_t icon, uint32_t cursor, uint32_t brush, uint32_t menuname, uint32_t name, uint32_t iconsm)
{
    if (class_find(name)) {
        set_last_error(1410);           /* ERROR_CLASS_ALREADY_EXISTS */
        return 0;
    }
    if (nclasses >= MAXCLS)
        fatal("too many window classes");
    Cls *k = &classes[nclasses++];
    memset(k, 0, sizeof *k);
    if (name < 0x10000)
        snprintf(k->name, sizeof k->name, "#%u", name);
    else
        snprintf(k->name, sizeof k->name, "%s", gptr(name));
    extern uint32_t atom_add(const char *s);
    k->atom = atom_add(k->name);
    k->style = style; k->proc = proc; k->clsextra = clsextra; k->wndextra = wndextra; k->hinst = hinst;
    k->icon = icon; k->cursor = cursor; k->brush = brush; k->menuname = menuname; k->iconsm = iconsm;
    LOG(LOG_WIN, "RegisterClass(\"%s\", proc=%08x, style=%x, extra %u) -> %x", k->name, proc, style, wndextra, k->atom);
    return k->atom;
}

API(USER32, RegisterClassA, 1)
{
    uint32_t wc = ARG(0);
    return class_add(R32(wc), R32(wc + 4), R32(wc + 8), R32(wc + 12), R32(wc + 16), R32(wc + 20),
                     R32(wc + 24), R32(wc + 28), R32(wc + 32), R32(wc + 36), 0);
}

API(USER32, RegisterClassExA, 1)
{
    uint32_t wc = ARG(0);
    return class_add(R32(wc + 4), R32(wc + 8), R32(wc + 12), R32(wc + 16), R32(wc + 20), R32(wc + 24),
                     R32(wc + 28), R32(wc + 32), R32(wc + 36), R32(wc + 40), R32(wc + 44));
}

API(USER32, UnregisterClassA, 2)
{
    Cls *k = class_find(ARG(0));
    if (!k) { set_last_error(1411); return 0; }
    k->name[0] = 0;
    k->atom = 0xffffffffu;
    return 1;
}

static void fill_wndclass(uint32_t wc, const Cls *k, int ex)
{
    uint32_t o = ex ? 4 : 0;
    if (ex) W32(wc, 48);
    W32(wc + o, k->style); W32(wc + o + 4, k->proc); W32(wc + o + 8, k->clsextra); W32(wc + o + 12, k->wndextra);
    W32(wc + o + 16, k->hinst); W32(wc + o + 20, k->icon); W32(wc + o + 24, k->cursor);
    W32(wc + o + 28, k->brush); W32(wc + o + 32, k->menuname);
    /* lpszClassName is the caller's string */
    if (ex) W32(wc + 44, k->iconsm);
}

API(USER32, GetClassInfoA, 3)
{
    Cls *k = class_find(ARG(1));
    LOG(LOG_WIN, "GetClassInfoA(%s) -> %s", ARG(1) < 0x10000 ? "#atom" : gstr(ARG(1)), k ? "found" : "none");
    if (!k) { set_last_error(1411); return 0; }
    fill_wndclass(ARG(2), k, 0);
    W32(ARG(2) + 36, ARG(1));
    return k->atom;
}

API(USER32, GetClassInfoExA, 3)
{
    Cls *k = class_find(ARG(1));
    if (!k) { set_last_error(1411); return 0; }
    fill_wndclass(ARG(2), k, 1);
    W32(ARG(2) + 40, ARG(1));
    return k->atom;
}

/* ------------------------------------------------------------------ windows */
typedef struct { uint32_t atom; char name[64]; uint32_t data; } Prop;
typedef struct {
    int used, destroying;
    uint32_t hwnd;
    Cls *cls;
    uint32_t proc, style, exstyle, id, userdata, parent, owner, hinst, menu, ctxhelp;
    uint32_t tid;                       /* the thread that created it: its messages go to that queue */
    int32_t x, y, w, h;                 /* window rectangle (screen; children: parent client) */
    char text[256];
    int need_paint, shown_once;
    uint8_t extra[256];
    Prop props[16];
    int nprops;
} Win;
#define MAXWIN 512
#define HWND_BASE 0x00010020u
#define HWND_DESKTOP_ 0x00010010u
static Win wins[MAXWIN];
static uint32_t focus_hwnd, active_hwnd, capture_hwnd;

static Win *win_of(uint32_t h)
{
    if (h < HWND_BASE || (h - HWND_BASE) % 4) return NULL;
    uint32_t i = (h - HWND_BASE) / 4;
    if (i >= MAXWIN || !wins[i].used) return NULL;
    return &wins[i];
}

static Win *win_new(void)
{
    pthread_mutex_lock(&wlock);
    for (int i = 0; i < MAXWIN; i++)
        if (!wins[i].used) {
            memset(&wins[i], 0, sizeof wins[i]);
            wins[i].used = 1;
            wins[i].hwnd = HWND_BASE + 4u * i;
            pthread_mutex_unlock(&wlock);
            return &wins[i];
        }
    pthread_mutex_unlock(&wlock);
    fatal("too many windows");
}

static void frame_size(uint32_t style, uint32_t exstyle, int32_t *l, int32_t *t, int32_t *r, int32_t *b)
{
    *l = *t = *r = *b = 0;
    if (style & WS_CHILD) {
        if (style & 0x00800000u) *l = *t = *r = *b = 1;    /* WS_BORDER */
        return;
    }
    if ((style & WS_CAPTION) == WS_CAPTION) *t += 19;
    if (style & WS_THICKFRAME) { *l += 4; *t += 4; *r += 4; *b += 4; }
    else if (style & (WS_CAPTION | 0x00400000u)) { *l += 3; *t += 3; *r += 3; *b += 3; }
    (void)exstyle;
}

static void client_size(const Win *w, int32_t *cw, int32_t *ch)
{
    int32_t l, t, r, b;
    frame_size(w->style, w->exstyle, &l, &t, &r, &b);
    *cw = w->w - l - r;
    *ch = w->h - t - b;
    if (*cw < 0) *cw = 0;
    if (*ch < 0) *ch = 0;
}

/* screen position of the client area's origin */
static void client_origin(const Win *w, int32_t *x, int32_t *y)
{
    int32_t l, t, r, b;
    frame_size(w->style, w->exstyle, &l, &t, &r, &b);
    *x = w->x + l;
    *y = w->y + t;
    Win *p = (w->style & WS_CHILD) ? win_of(w->parent) : NULL;
    if (p) {
        int32_t px, py;
        client_origin(p, &px, &py);
        *x += px;
        *y += py;
    }
}

uint32_t call_wndproc(CPU *c, uint32_t proc, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    if (!proc)
        return 0;
    return guest_call(c, proc, 4, hwnd, msg, wp, lp);
}

static uint32_t sendmsg(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    Win *w = win_of(hwnd);
    if (!w)
        return 0;
    return call_wndproc(c, w->proc, hwnd, msg, wp, lp);
}

/* the window input from the host goes to */
static uint32_t input_hwnd(void)
{
    if (focus_hwnd && win_of(focus_hwnd)) return focus_hwnd;
    return main_hwnd;
}

/* ------------------------------------------------------------------ hooks */
typedef struct { int used; int32_t id; uint32_t proc; } Hook;
#define MAXHOOK 32
static Hook hooks[MAXHOOK];
#define HHOOK_BASE 0x000f0010u

API(USER32, SetWindowsHookExA, 4)
{
    for (int i = 0; i < MAXHOOK; i++)
        if (!hooks[i].used) {
            hooks[i] = (Hook){1, (int32_t)ARG(0), ARG(1)};
            LOG(LOG_WIN, "SetWindowsHookExA(%d, %08x) -> %x", (int32_t)ARG(0), ARG(1), HHOOK_BASE + 4u * i);
            return HHOOK_BASE + 4u * i;
        }
    return 0;
}

API(USER32, UnhookWindowsHookEx, 1)
{
    uint32_t i = (ARG(0) - HHOOK_BASE) / 4;
    if (i < MAXHOOK) hooks[i].used = 0;
    return 1;
}

/* the next installed hook of the same kind after index `after` (newest first, as Windows) */
static int hook_next(int32_t id, int after)
{
    for (int i = after - 1; i >= 0; i--)
        if (hooks[i].used && hooks[i].id == id)
            return i;
    return -1;
}

static uint32_t call_hooks(CPU *c, int32_t id, uint32_t code, uint32_t wp, uint32_t lp)
{
    int i = hook_next(id, MAXHOOK);
    if (i < 0)
        return 0;
    return guest_call(c, hooks[i].proc, 3, code, wp, lp);
}

API(USER32, CallNextHookEx, 4)
{
    uint32_t idx = (ARG(0) - HHOOK_BASE) / 4;
    if (idx >= MAXHOOK) return 0;
    int i = hook_next(hooks[idx].id, (int)idx);
    if (i < 0) return 0;
    return guest_call(c, hooks[i].proc, 3, ARG(1), ARG(2), ARG(3));
}

/* ------------------------------------------------------------------ creation */
static void host_follow(Win *w)
{
    if (w->hwnd != main_hwnd) return;
    int32_t cw, ch;
    client_size(w, &cw, &ch);
    host_window_resize(cw, ch);
}

static uint32_t create_window(CPU *c, uint32_t exstyle, uint32_t cname, uint32_t title, uint32_t style,
                              int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t parent, uint32_t menu,
                              uint32_t hinst, uint32_t param)
{
    Cls *k = class_find(cname);
    if (!k) {
        LOG(LOG_ALWAYS, "CreateWindowEx: unknown class %s", cname < 0x10000 ? "#atom" : gstr(cname));
        set_last_error(1407);
        return 0;
    }
    if (!(style & WS_CHILD) && (uint32_t)x == CW_USEDEFAULT) { x = 0; y = 0; }
    if ((uint32_t)cx == CW_USEDEFAULT) { cx = 800; cy = 600; }
    Win *w = win_new();
    uint32_t h = w->hwnd;
    w->tid = current_thread_id();
    w->cls = k;
    w->proc = k->proc;
    w->style = style;
    w->exstyle = exstyle;
    w->x = x; w->y = y; w->w = cx; w->h = cy;
    w->hinst = hinst;
    if (style & WS_CHILD) { w->parent = parent; w->id = menu; }
    else { w->owner = parent; w->menu = menu; }
    if (title) snprintf(w->text, sizeof w->text, "%s", gptr(title));
    LOG(LOG_WIN, "CreateWindowEx(\"%s\", \"%s\", style %x ex %x, %d,%d %dx%d, parent %x) -> %x",
        k->name, title ? gptr(title) : "", style, exstyle, x, y, cx, cy, parent, h);

    uint32_t cs = galloc(48 + 8);
    W32(cs, param); W32(cs + 4, hinst); W32(cs + 8, menu); W32(cs + 12, parent);
    W32(cs + 16, cy); W32(cs + 20, cx); W32(cs + 24, y); W32(cs + 28, x);
    W32(cs + 32, style); W32(cs + 36, title); W32(cs + 40, cname); W32(cs + 44, exstyle);
    /* the CBT hook first (MFC subclasses the window here) */
    uint32_t cbt = cs + 48;
    W32(cbt, cs); W32(cbt + 4, 0);
    if (call_hooks(c, 5, 3, h, cbt)) {                     /* WH_CBT, HCBT_CREATEWND */
        LOG(LOG_WIN, "  the CBT hook refused the window");
        w->used = 0;
        gfree(cs);
        return 0;
    }
    w->x = (int32_t)R32(cs + 28); w->y = (int32_t)R32(cs + 24);
    w->w = (int32_t)R32(cs + 20); w->h = (int32_t)R32(cs + 16);
    if (!sendmsg(c, h, WM_NCCREATE, 0, cs)) {
        LOG(LOG_WIN, "  WM_NCCREATE refused the window");
        sendmsg(c, h, WM_NCDESTROY, 0, 0);
        w->used = 0;
        gfree(cs);
        return 0;
    }
    uint32_t rc = galloc(16);
    W32(rc, w->x); W32(rc + 4, w->y); W32(rc + 8, w->x + w->w); W32(rc + 12, w->y + w->h);
    sendmsg(c, h, WM_NCCALCSIZE, 0, rc);
    gfree(rc);
    if ((int32_t)sendmsg(c, h, WM_CREATE, 0, cs) == -1) {
        LOG(LOG_WIN, "  WM_CREATE refused the window");
        sendmsg(c, h, WM_DESTROY, 0, 0);
        sendmsg(c, h, WM_NCDESTROY, 0, 0);
        w->used = 0;
        gfree(cs);
        return 0;
    }
    gfree(cs);
    if (!win_of(h))
        return 0;                       /* destroyed while being created */
    int32_t cw, ch;
    client_size(w, &cw, &ch);
    sendmsg(c, h, WM_SIZE, 0, ((uint32_t)ch << 16) | ((uint32_t)cw & 0xffff));
    sendmsg(c, h, WM_MOVE, 0, ((uint32_t)w->y << 16) | ((uint32_t)w->x & 0xffff));
    if (style & WS_VISIBLE) {
        w->style &= ~WS_VISIBLE;
        extern uint32_t show_window(CPU *c, uint32_t hwnd, uint32_t cmd);
        show_window(c, h, 5);
    }
    return h;
}

API(USER32, CreateWindowExA, 12)
{
    return create_window(c, ARG(0), ARG(1), ARG(2), ARG(3), (int32_t)ARG(4), (int32_t)ARG(5),
                         (int32_t)ARG(6), (int32_t)ARG(7), ARG(8), ARG(9), ARG(10), ARG(11));
}

uint32_t show_window(CPU *c, uint32_t hwnd, uint32_t cmd)
{
    Win *w = win_of(hwnd);
    if (!w) return 0;
    int was = (w->style & WS_VISIBLE) != 0;
    int show = cmd != 0;                /* SW_HIDE */
    LOG(LOG_WIN, "ShowWindow(%x, %u)", hwnd, cmd);
    if (show == was)
        return was;
    if (show) w->style |= WS_VISIBLE; else w->style &= ~WS_VISIBLE;
    sendmsg(c, hwnd, WM_SHOWWINDOW, show, 0);
    if (!(w->style & WS_CHILD) && show && !main_hwnd) {
        /* the first top-level window shown is the one the host window shows */
        main_hwnd = hwnd;
        int32_t cw, ch;
        client_size(w, &cw, &ch);
        host_window_create(w->text, cw > 0 ? cw : 640, ch > 0 ? ch : 480);
    }
    if (hwnd == main_hwnd)
        host_window_show(show);
    if (show && !(w->style & WS_CHILD) && cmd != 4 && cmd != 8) {     /* SW_SHOWNOACTIVATE, SW_SHOWNA */
        uint32_t old = active_hwnd;
        active_hwnd = hwnd;
        if (old != hwnd) {
            sendmsg(c, hwnd, WM_ACTIVATEAPP, 1, 0);
            sendmsg(c, hwnd, WM_NCACTIVATE, 1, 0);
            sendmsg(c, hwnd, WM_ACTIVATE, 1, old);
            if (!focus_hwnd || !win_of(focus_hwnd)) {
                focus_hwnd = hwnd;
                sendmsg(c, hwnd, WM_SETFOCUS, 0, 0);
            }
        }
    }
    if (show) w->need_paint = 1;
    return was;
}

API(USER32, ShowWindow, 2) { return show_window(c, ARG(0), ARG(1)); }

static void destroy_window(CPU *c, uint32_t hwnd)
{
    Win *w = win_of(hwnd);
    if (!w || w->destroying) return;
    w->destroying = 1;
    LOG(LOG_WIN, "DestroyWindow(%x)", hwnd);
    call_hooks(c, 5, 4, hwnd, 0);                        /* HCBT_DESTROYWND */
    if (w->style & WS_VISIBLE) show_window(c, hwnd, 0);
    sendmsg(c, hwnd, WM_DESTROY, 0, 0);
    for (int i = 0; i < MAXWIN; i++)                      /* children, and windows it owns */
        if (wins[i].used && (wins[i].parent == hwnd || wins[i].owner == hwnd))
            destroy_window(c, wins[i].hwnd);
    sendmsg(c, hwnd, WM_NCDESTROY, 0, 0);
    if (focus_hwnd == hwnd) focus_hwnd = 0;
    if (active_hwnd == hwnd) active_hwnd = 0;
    if (capture_hwnd == hwnd) capture_hwnd = 0;
    if (main_hwnd == hwnd) {
        main_hwnd = 0;
        host_window_show(0);
    }
    w->used = 0;
}

API(USER32, DestroyWindow, 1) { if (!win_of(ARG(0))) { set_last_error(1400); return 0; } destroy_window(c, ARG(0)); return 1; }

static void set_pos(CPU *c, Win *w, int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t flags)
{
    int moved = !(flags & 2) && (x != w->x || y != w->y);
    int sized = !(flags & 1) && (cx != w->w || cy != w->h);
    if (!(flags & 2)) { w->x = x; w->y = y; }
    if (!(flags & 1)) { w->w = cx; w->h = cy; }
    if (flags & 0x40) show_window(c, w->hwnd, 5);          /* SWP_SHOWWINDOW */
    if (flags & 0x80) show_window(c, w->hwnd, 0);          /* SWP_HIDEWINDOW */
    if (moved || sized) {
        uint32_t wp = galloc(28);
        W32(wp, w->hwnd); W32(wp + 4, 0); W32(wp + 8, w->x); W32(wp + 12, w->y);
        W32(wp + 16, w->w); W32(wp + 20, w->h);
        W32(wp + 24, (flags & ~3u) | (moved ? 0 : 2) | (sized ? 0 : 1));
        sendmsg(c, w->hwnd, WM_WINDOWPOSCHANGED, 0, wp);
        gfree(wp);
        if (sized) {
            host_follow(w);
            w->need_paint = 1;
        }
    }
}

API(USER32, SetWindowPos, 7)
{
    Win *w = win_of(ARG(0));
    LOG(LOG_WIN, "SetWindowPos(%x, after %x, %d,%d %dx%d, flags %x)", ARG(0), ARG(1),
        (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5), ARG(6));
    if (!w) return 0;
    set_pos(c, w, (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4), (int32_t)ARG(5), ARG(6));
    return 1;
}

API(USER32, MoveWindow, 6)
{
    Win *w = win_of(ARG(0));
    if (!w) return 0;
    set_pos(c, w, (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4), ARG(5) ? 0 : 8);
    return 1;
}

/* DeferWindowPos: applied at once */
API(USER32, BeginDeferWindowPos, 1) { return 0x000e0001u; }
API(USER32, DeferWindowPos, 8)
{
    Win *w = win_of(ARG(1));
    if (w) set_pos(c, w, (int32_t)ARG(3), (int32_t)ARG(4), (int32_t)ARG(5), (int32_t)ARG(6), ARG(7));
    return ARG(0);
}
API(USER32, EndDeferWindowPos, 1) { return 1; }

/* ------------------------------------------------------------------ window data */
API(USER32, GetWindowLongA, 2)
{
    Win *w = win_of(ARG(0));
    int32_t i = (int32_t)ARG(1);
    if (!w) { set_last_error(1400); return 0; }
    switch (i) {
    case -4: return w->proc;
    case -6: return w->hinst;
    case -8: return w->owner ? w->owner : w->parent;
    case -12: return w->id;
    case -16: return w->style;
    case -20: return w->exstyle;
    case -21: return w->userdata;
    }
    if (i >= 0 && (uint32_t)i + 4 <= w->cls->wndextra && i + 4 <= (int)sizeof w->extra) {
        uint32_t v;
        memcpy(&v, w->extra + i, 4);
        return v;
    }
    set_last_error(1413);
    return 0;
}

API(USER32, SetWindowLongA, 3)
{
    Win *w = win_of(ARG(0));
    int32_t i = (int32_t)ARG(1);
    uint32_t v = ARG(2), old = 0;
    if (!w) { set_last_error(1400); return 0; }
    switch (i) {
    case -4: old = w->proc; w->proc = v; break;
    case -6: old = w->hinst; w->hinst = v; break;
    case -8: old = w->owner; w->owner = v; break;
    case -12: old = w->id; w->id = v; break;
    case -16: old = w->style; w->style = v; break;
    case -20: old = w->exstyle; w->exstyle = v; break;
    case -21: old = w->userdata; w->userdata = v; break;
    default:
        if (i >= 0 && (uint32_t)i + 4 <= w->cls->wndextra && i + 4 <= (int)sizeof w->extra) {
            memcpy(&old, w->extra + i, 4);
            memcpy(w->extra + i, &v, 4);
        } else {
            set_last_error(1413);
            return 0;
        }
    }
    LOG(LOG_WIN, "SetWindowLongA(%x, %d, %08x) (was %08x)", ARG(0), i, v, old);
    return old;
}

API(USER32, GetClassLongA, 2)
{
    Win *w = win_of(ARG(0));
    if (!w) return 0;
    Cls *k = w->cls;
    switch ((int32_t)ARG(1)) {
    case -8: return k->menuname;
    case -10: return k->brush;
    case -12: return k->cursor;
    case -14: return k->icon;
    case -16: return k->hinst;
    case -18: return k->wndextra;
    case -20: return k->clsextra;
    case -24: return k->proc;
    case -26: return k->style;
    case -32: return k->atom;
    case -34: return k->iconsm;
    }
    return 0;
}

API(USER32, GetClassNameA, 3)
{
    Win *w = win_of(ARG(0));
    if (!w) return 0;
    gstrncpy(ARG(1), w->cls->name, ARG(2));
    return strlen(gptr(ARG(1)));
}

API(USER32, CallWindowProcA, 5) { return call_wndproc(c, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)); }

static Prop *prop_find(Win *w, uint32_t name)
{
    for (int i = 0; i < w->nprops; i++)
        if (name < 0x10000 ? w->props[i].atom == name : (!w->props[i].atom && !strcasecmp(w->props[i].name, gptr(name))))
            return &w->props[i];
    return NULL;
}

API(USER32, SetPropA, 3)
{
    Win *w = win_of(ARG(0));
    if (!w) return 0;
    Prop *p = prop_find(w, ARG(1));
    if (!p) {
        if (w->nprops >= 16) return 0;
        p = &w->props[w->nprops++];
        memset(p, 0, sizeof *p);
        if (ARG(1) < 0x10000) p->atom = ARG(1);
        else snprintf(p->name, sizeof p->name, "%s", gptr(ARG(1)));
    }
    p->data = ARG(2);
    return 1;
}

API(USER32, GetPropA, 2)
{
    Win *w = win_of(ARG(0));
    Prop *p = w ? prop_find(w, ARG(1)) : NULL;
    return p ? p->data : 0;
}

API(USER32, RemovePropA, 2)
{
    Win *w = win_of(ARG(0));
    Prop *p = w ? prop_find(w, ARG(1)) : NULL;
    if (!p) return 0;
    uint32_t d = p->data;
    *p = w->props[--w->nprops];
    return d;
}

API(USER32, SetWindowTextA, 2)
{
    return sendmsg(c, ARG(0), WM_SETTEXT, 0, ARG(1));
}

API(USER32, GetWindowTextA, 3)
{
    if (!ARG(2)) return 0;
    return sendmsg(c, ARG(0), WM_GETTEXT, ARG(2), ARG(1));
}

API(USER32, GetWindowTextLengthA, 1) { return sendmsg(c, ARG(0), WM_GETTEXTLENGTH, 0, 0); }
API(USER32, SetWindowContextHelpId, 2) { Win *w = win_of(ARG(0)); if (w) w->ctxhelp = ARG(1); return w != NULL; }

/* ------------------------------------------------------------------ relations, state */
API(USER32, IsWindow, 1) { return win_of(ARG(0)) != NULL; }
API(USER32, IsWindowVisible, 1)
{
    for (Win *w = win_of(ARG(0)); w; w = (w->style & WS_CHILD) ? win_of(w->parent) : NULL)
        if (!(w->style & WS_VISIBLE)) return 0;
    return win_of(ARG(0)) != NULL;
}
API(USER32, IsWindowEnabled, 1) { Win *w = win_of(ARG(0)); return w && !(w->style & WS_DISABLED); }
API(USER32, EnableWindow, 2)
{
    Win *w = win_of(ARG(0));
    if (!w) return 0;
    int was_disabled = (w->style & WS_DISABLED) != 0;
    if (ARG(1)) w->style &= ~WS_DISABLED; else w->style |= WS_DISABLED;
    if (was_disabled == !ARG(1)) return was_disabled;
    sendmsg(c, ARG(0), 0x000a, ARG(1) != 0, 0);           /* WM_ENABLE */
    return was_disabled;
}
API(USER32, IsIconic, 1) { return 0; }
API(USER32, IsZoomed, 1) { return 0; }
API(USER32, IsChild, 2)
{
    for (Win *w = win_of(ARG(1)); w && (w->style & WS_CHILD); w = win_of(w->parent))
        if (w->parent == ARG(0)) return 1;
    return 0;
}
API(USER32, GetParent, 1)
{
    Win *w = win_of(ARG(0));
    if (!w) return 0;
    return (w->style & WS_CHILD) ? w->parent : w->owner;
}
API(USER32, SetParent, 2)
{
    Win *w = win_of(ARG(0));
    if (!w) return 0;
    uint32_t old = w->parent;
    w->parent = ARG(1);
    return old;
}
API(USER32, GetDesktopWindow, 0) { return HWND_DESKTOP_; }

static uint32_t first_child(uint32_t parent, int after_index)
{
    for (int i = after_index + 1; i < MAXWIN; i++)
        if (wins[i].used && (parent ? ((wins[i].style & WS_CHILD) && wins[i].parent == parent)
                                    : !(wins[i].style & WS_CHILD)))
            return wins[i].hwnd;
    return 0;
}

API(USER32, GetWindow, 2)
{
    Win *w = win_of(ARG(0));
    uint32_t cmd = ARG(1);
    if (!w && ARG(0) != HWND_DESKTOP_) return 0;
    switch (cmd) {
    case 5: return first_child(ARG(0) == HWND_DESKTOP_ ? 0 : ARG(0), -1);          /* GW_CHILD */
    case 4: return w ? w->owner : 0;                                                /* GW_OWNER */
    case 2: {                                                                       /* GW_HWNDNEXT */
        uint32_t par = (w->style & WS_CHILD) ? w->parent : 0;
        return first_child(par, (int)((w->hwnd - HWND_BASE) / 4));
    }
    case 0: return w ? first_child((w->style & WS_CHILD) ? w->parent : 0, -1) : 0;   /* GW_HWNDFIRST */
    default: return 0;
    }
}

API(USER32, GetTopWindow, 1) { return first_child(ARG(0) == HWND_DESKTOP_ ? 0 : ARG(0), -1); }
API(USER32, GetLastActivePopup, 1) { return ARG(0); }
API(USER32, GetActiveWindow, 0) { return active_hwnd; }
API(USER32, GetForegroundWindow, 0) { return active_hwnd; }
API(USER32, SetActiveWindow, 1) { uint32_t o = active_hwnd; if (win_of(ARG(0))) active_hwnd = ARG(0); return o; }
API(USER32, SetForegroundWindow, 1) { if (win_of(ARG(0))) active_hwnd = ARG(0); return 1; }
API(USER32, BringWindowToTop, 1) { return 1; }
API(USER32, GetFocus, 0) { return focus_hwnd; }
API(USER32, SetFocus, 1)
{
    uint32_t old = focus_hwnd;
    if (ARG(0) == old) return old;
    if (old && win_of(old)) sendmsg(c, old, WM_KILLFOCUS, ARG(0), 0);
    focus_hwnd = ARG(0);
    if (focus_hwnd) sendmsg(c, focus_hwnd, WM_SETFOCUS, old, 0);
    return old;
}
API(USER32, GetCapture, 0) { return capture_hwnd; }
API(USER32, SetCapture, 1) { uint32_t o = capture_hwnd; capture_hwnd = ARG(0); return o; }
API(USER32, ReleaseCapture, 0) { capture_hwnd = 0; return 1; }
API(USER32, GetWindowThreadProcessId, 2) { if (ARG(1)) W32(ARG(1), 0x7d0u); return current_thread_id(); }
API(USER32, GetDlgCtrlID, 1) { Win *w = win_of(ARG(0)); return w ? w->id : 0; }

API(USER32, GetDlgItem, 2)
{
    for (int i = 0; i < MAXWIN; i++)
        if (wins[i].used && (wins[i].style & WS_CHILD) && wins[i].parent == ARG(0) && wins[i].id == ARG(1))
            return wins[i].hwnd;
    return 0;
}

API(USER32, FindWindowA, 2)
{
    for (int i = 0; i < MAXWIN; i++) {
        Win *w = &wins[i];
        if (!w->used || (w->style & WS_CHILD)) continue;
        if (ARG(0) && (ARG(0) < 0x10000 ? w->cls->atom != ARG(0) : strcasecmp(w->cls->name, gstr(ARG(0)))))
            continue;
        if (ARG(1) && strcmp(w->text, gstr(ARG(1))))
            continue;
        return w->hwnd;
    }
    return 0;
}

API(USER32, EnumChildWindows, 3)
{
    uint32_t list[MAXWIN];
    int n = 0;
    for (int i = 0; i < MAXWIN; i++)
        if (wins[i].used && (wins[i].style & WS_CHILD) && IsChild_check(ARG(0), wins[i].hwnd))
            list[n++] = wins[i].hwnd;
    for (int i = 0; i < n; i++)
        if (win_of(list[i]) && !guest_call(c, ARG(1), 2, list[i], ARG(2)))
            break;
    return 1;
}

int IsChild_check(uint32_t parent, uint32_t child)
{
    for (Win *w = win_of(child); w && (w->style & WS_CHILD); w = win_of(w->parent))
        if (w->parent == parent) return 1;
    return 0;
}

API(USER32, WindowFromPoint, 2) { return main_hwnd; }

/* ------------------------------------------------------------------ geometry */
API(USER32, GetWindowRect, 2)
{
    Win *w = win_of(ARG(0));
    uint32_t r = ARG(1);
    if (ARG(0) == HWND_DESKTOP_) {
        int dw, dh;
        host_desktop_size(&dw, &dh);
        W32(r, 0); W32(r + 4, 0); W32(r + 8, dw); W32(r + 12, dh);
        return 1;
    }
    if (!w) return 0;
    int32_t x = w->x, y = w->y;
    Win *p = (w->style & WS_CHILD) ? win_of(w->parent) : NULL;
    if (p) { int32_t px, py; client_origin(p, &px, &py); x += px; y += py; }
    W32(r, x); W32(r + 4, y); W32(r + 8, x + w->w); W32(r + 12, y + w->h);
    return 1;
}

API(USER32, GetClientRect, 2)
{
    Win *w = win_of(ARG(0));
    uint32_t r = ARG(1);
    if (ARG(0) == HWND_DESKTOP_) {
        int dw, dh;
        host_desktop_size(&dw, &dh);
        W32(r, 0); W32(r + 4, 0); W32(r + 8, dw); W32(r + 12, dh);
        return 1;
    }
    if (!w) return 0;
    int32_t cw, ch;
    client_size(w, &cw, &ch);
    W32(r, 0); W32(r + 4, 0); W32(r + 8, cw); W32(r + 12, ch);
    return 1;
}

static void origin_of(uint32_t h, int32_t *x, int32_t *y)
{
    Win *w = win_of(h);
    *x = *y = 0;
    if (w) client_origin(w, x, y);
}

API(USER32, ClientToScreen, 2)
{
    int32_t x, y;
    origin_of(ARG(0), &x, &y);
    W32(ARG(1), R32(ARG(1)) + x);
    W32(ARG(1) + 4, R32(ARG(1) + 4) + y);
    return 1;
}

API(USER32, ScreenToClient, 2)
{
    int32_t x, y;
    origin_of(ARG(0), &x, &y);
    W32(ARG(1), R32(ARG(1)) - x);
    W32(ARG(1) + 4, R32(ARG(1) + 4) - y);
    return 1;
}

API(USER32, MapWindowPoints, 4)
{
    int32_t fx, fy, tx, ty;
    origin_of(ARG(0), &fx, &fy);
    origin_of(ARG(1), &tx, &ty);
    int32_t dx = fx - tx, dy = fy - ty;
    for (uint32_t i = 0; i < ARG(3); i++) {
        W32(ARG(2) + 8 * i, R32(ARG(2) + 8 * i) + dx);
        W32(ARG(2) + 8 * i + 4, R32(ARG(2) + 8 * i + 4) + dy);
    }
    return ((uint32_t)(uint16_t)dy << 16) | (uint16_t)dx;
}

static void adjust(uint32_t rect, uint32_t style, uint32_t exstyle)
{
    int32_t l, t, r, b;
    frame_size(style, exstyle, &l, &t, &r, &b);
    W32(rect, R32(rect) - l);
    W32(rect + 4, R32(rect + 4) - t);
    W32(rect + 8, R32(rect + 8) + r);
    W32(rect + 12, R32(rect + 12) + b);
}

API(USER32, AdjustWindowRect, 3) { adjust(ARG(0), ARG(1), 0); return 1; }
API(USER32, AdjustWindowRectEx, 4) { adjust(ARG(0), ARG(1), ARG(3)); return 1; }

API(USER32, GetWindowPlacement, 2)
{
    Win *w = win_of(ARG(0));
    uint32_t p = ARG(1);
    if (!w) return 0;
    W32(p, 44); W32(p + 4, 0); W32(p + 8, (w->style & WS_VISIBLE) ? 1 : 0);
    W32(p + 12, -1); W32(p + 16, -1); W32(p + 20, -1); W32(p + 24, -1);
    W32(p + 28, w->x); W32(p + 32, w->y); W32(p + 36, w->x + w->w); W32(p + 40, w->y + w->h);
    return 1;
}

API(USER32, SetWindowPlacement, 2)
{
    Win *w = win_of(ARG(0));
    uint32_t p = ARG(1);
    if (!w) return 0;
    int32_t l = (int32_t)R32(p + 28), t = (int32_t)R32(p + 32);
    set_pos(c, w, l, t, (int32_t)R32(p + 36) - l, (int32_t)R32(p + 40) - t, 4);
    show_window(c, ARG(0), R32(p + 8));
    return 1;
}

/* ------------------------------------------------------------------ messages */
typedef struct { uint32_t hwnd, msg, wparam, lparam, time, tid; } Msg;
#define QSIZE 2048
static Msg queue[QSIZE];
static int qhead, qtail;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
/* one queue for all threads, each message marked with the thread whose queue it is in (the
   window's creator; PostThreadMessage's target): every thread sees only its own */
static int quit_posted;
static uint32_t quit_code, quit_tid;
static uint32_t last_time, last_pos;

static void queue_put_tid(uint32_t tid, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    pthread_mutex_lock(&qlock);
    int next = (qtail + 1) % QSIZE;
    if (next != qhead) {
        queue[qtail] = (Msg){hwnd, msg, wp, lp, (uint32_t)now_ms(), tid};
        qtail = next;
    } else
        LOG(LOG_ALWAYS, "message queue full: message %x dropped", msg);
    pthread_mutex_unlock(&qlock);
}

static void queue_put(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    Win *w = win_of(hwnd);
    queue_put_tid(w ? w->tid : current_thread_id(), hwnd, msg, wp, lp);
}

/* input from the host window (win.c): to the window with the focus */
void post_message(uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    uint32_t h = input_hwnd();
    if (h) queue_put(h, msg, wparam, lparam);
}

API(USER32, PostMessageA, 4)
{
    LOG(LOG_WIN, "PostMessageA(%x, %x, %x, %x)", ARG(0), ARG(1), ARG(2), ARG(3));
    if (ARG(0) && !win_of(ARG(0)) && ARG(0) != 0xffffu) { set_last_error(1400); return 0; }
    queue_put(ARG(0) == 0xffffu ? main_hwnd : ARG(0), ARG(1), ARG(2), ARG(3));
    return 1;
}

API(USER32, PostThreadMessageA, 4) { queue_put_tid(ARG(0), 0, ARG(1), ARG(2), ARG(3)); return 1; }

API(USER32, SendMessageA, 4)
{
    LOG(LOG_WIN, "SendMessageA(%x, %x, %x, %x)", ARG(0), ARG(1), ARG(2), ARG(3));
    if (ARG(0) == 0xffffu) {                                /* HWND_BROADCAST: top-level windows */
        for (int i = 0; i < MAXWIN; i++)
            if (wins[i].used && !(wins[i].style & WS_CHILD))
                sendmsg(c, wins[i].hwnd, ARG(1), ARG(2), ARG(3));
        return 1;
    }
    return sendmsg(c, ARG(0), ARG(1), ARG(2), ARG(3));
}

API(USER32, SendNotifyMessageA, 4) { sendmsg(c, ARG(0), ARG(1), ARG(2), ARG(3)); return 1; }
API(USER32, InSendMessage, 0) { return 0; }
API(USER32, GetMessagePos, 0) { return last_pos; }
API(USER32, GetMessageTime, 0) { return last_time; }

API(USER32, PostQuitMessage, 1)
{
    LOG(LOG_WIN, "PostQuitMessage(%u)", ARG(0));
    pthread_mutex_lock(&qlock);
    quit_posted = 1;
    quit_code = ARG(0);
    quit_tid = current_thread_id();
    pthread_mutex_unlock(&qlock);
    return 0;
}

/* ------------------------------------------------------------------ timers */
typedef struct { int used; uint32_t hwnd, id, proc, tid; double period, next; } Timer;
#define MAXTIMER 64
static Timer timers[MAXTIMER];

API(USER32, SetTimer, 4)
{
    uint32_t id = ARG(1);
    double ms = ARG(2) < 10 ? 10 : ARG(2);
    for (int i = 0; i < MAXTIMER; i++)
        if (timers[i].used && timers[i].hwnd == ARG(0) && ARG(0) && timers[i].id == id) {
            timers[i].period = ms; timers[i].next = now_ms() + ms; timers[i].proc = ARG(3);
            return id;
        }
    for (int i = 0; i < MAXTIMER; i++)
        if (!timers[i].used) {
            if (!ARG(0)) id = 0x7000 + i;
            timers[i] = (Timer){1, ARG(0), id, ARG(3), current_thread_id(), ms, now_ms() + ms};
            LOG(LOG_WIN, "SetTimer(%x, %u, %u ms) -> %u", ARG(0), ARG(1), ARG(2), id);
            return id;
        }
    return 0;
}

API(USER32, KillTimer, 2)
{
    for (int i = 0; i < MAXTIMER; i++)
        if (timers[i].used && timers[i].hwnd == ARG(0) && timers[i].id == ARG(1)) {
            timers[i].used = 0;
            return 1;
        }
    return 0;
}

/* a due timer as a WM_TIMER message (in place of a posted one: lowest priority, as Windows) */
static int timer_due(Msg *m, uint32_t hwnd_filter, int remove)
{
    double now = now_ms();
    for (int i = 0; i < MAXTIMER; i++) {
        Timer *t = &timers[i];
        if (!t->used || now < t->next || t->tid != current_thread_id()) continue;
        if (hwnd_filter && t->hwnd != hwnd_filter) continue;
        *m = (Msg){t->hwnd, WM_TIMER, t->id, t->proc, (uint32_t)now, t->tid};
        if (remove) t->next = now + t->period;
        return 1;
    }
    return 0;
}

static int paint_due(Msg *m, uint32_t hwnd_filter)
{
    for (int i = 0; i < MAXWIN; i++)
        if (wins[i].used && wins[i].need_paint && (wins[i].style & WS_VISIBLE) &&
            wins[i].tid == current_thread_id() && (!hwnd_filter || wins[i].hwnd == hwnd_filter)) {
            *m = (Msg){wins[i].hwnd, WM_PAINT, 0, 0, (uint32_t)now_ms(), wins[i].tid};
            return 1;
        }
    return 0;
}

static int in_filter(uint32_t msg, uint32_t lo, uint32_t hi)
{
    if (!lo && !hi) return 1;
    return msg >= lo && msg <= hi;
}

/* the next message for the filters (posted, then quit, paint, timers); 1 if one is there */
static int next_message(uint32_t m, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove)
{
    Msg q;
    int found = 0;
    uint32_t me = current_thread_id();
    pthread_mutex_lock(&qlock);
    for (int i = qhead; i != qtail; i = (i + 1) % QSIZE) {
        Msg *e = &queue[i];
        if (e->tid != me)
            continue;
        if ((hwnd && hwnd != 0xffffffffu && e->hwnd != hwnd && !IsChild_check(hwnd, e->hwnd)) ||
            (hwnd == 0xffffffffu && e->hwnd) || !in_filter(e->msg, lo, hi))
            continue;
        q = *e;
        found = 1;
        if (remove) {                   /* close the gap */
            for (int j = i; j != qhead; ) {
                int p = (j - 1 + QSIZE) % QSIZE;
                queue[j] = queue[p];
                j = p;
            }
            qhead = (qhead + 1) % QSIZE;
        }
        break;
    }
    if (!found && quit_posted && quit_tid == me && in_filter(WM_QUIT, lo, hi)) {
        q = (Msg){0, WM_QUIT, quit_code, 0, (uint32_t)now_ms(), me};
        if (remove) quit_posted = 0;
        found = 1;
    }
    pthread_mutex_unlock(&qlock);
    if (!found && in_filter(WM_PAINT, lo, hi))
        found = paint_due(&q, hwnd);
    if (!found && in_filter(WM_TIMER, lo, hi))
        found = timer_due(&q, hwnd, remove);
    if (!found)
        return 0;
    W32(m, q.hwnd); W32(m + 4, q.msg); W32(m + 8, q.wparam); W32(m + 12, q.lparam);
    W32(m + 16, q.time); W32(m + 20, 0); W32(m + 24, 0);
    last_time = q.time;
    if (remove && q.msg != WM_PAINT && q.msg != WM_TIMER && q.msg != 0x0200)
        LOG(LOG_WIN, "message %x to %x (w=%x l=%x)", q.msg, q.hwnd, q.wparam, q.lparam);
    return 1;
}

API(USER32, PeekMessageA, 5)
{
    host_pump();
    return next_message(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4) & 1);
}

static void wait_a_little(void)
{
    extern void det_yield(void);
    if (deterministic) {                /* --test: the others' turn instead (sync.c) */
        det_yield();
        return;
    }
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
}

API(USER32, GetMessageA, 4)
{
    for (;;) {
        host_pump();
        if (next_message(ARG(0), ARG(1), ARG(2), ARG(3), 1))
            return R32(ARG(0) + 4) != WM_QUIT;
        frame_presented_idle();
        wait_a_little();
    }
}

API(USER32, WaitMessage, 0)
{
    uint32_t m = galloc(28);
    for (;;) {
        host_pump();
        if (next_message(m, 0, 0, 0, 0)) break;
        frame_presented_idle();
        wait_a_little();
    }
    gfree(m);
    return 1;
}

API(USER32, MsgWaitForMultipleObjects, 5)
{
    uint32_t n = ARG(0), ms = ARG(3);
    uint32_t hs[64];
    for (uint32_t i = 0; i < n && i < 64; i++) hs[i] = R32(ARG(1) + 4 * i);
    double until = now_ms() + (ms == 0xffffffffu ? 1e18 : ms);
    uint32_t m = galloc(28);
    uint32_t r = 0x102;
    for (;;) {
        host_pump();
        if (n) {
            int w = wait_handles((int)n, hs, ARG(2) != 0, 0);
            if (w != 0x102) { r = (uint32_t)w; break; }
        }
        if (next_message(m, 0, 0, 0, 0)) { r = n; break; }
        if (now_ms() >= until) break;
        wait_a_little();
    }
    gfree(m);
    return r;
}

API(USER32, TranslateMessage, 1)
{
    uint32_t m = ARG(0), msg = R32(m + 4), vk = R32(m + 8);
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        int ch = vk_to_char(vk, host_shift_down());
        if (ch) {
            queue_put(R32(m), msg == WM_KEYDOWN ? 0x0102 : WM_SYSCHAR, ch, R32(m + 12));
            return 1;
        }
    }
    return 0;
}

API(USER32, DispatchMessageA, 1)
{
    uint32_t m = ARG(0), hwnd = R32(m), msg = R32(m + 4);
    if (msg == WM_TIMER && R32(m + 12))
        return guest_call(c, R32(m + 12), 4, hwnd, msg, R32(m + 8), (uint32_t)now_ms());
    if (!win_of(hwnd))
        return 0;
    uint32_t r = sendmsg(c, hwnd, msg, R32(m + 8), R32(m + 12));
    if (msg == WM_PAINT) {
        Win *w = win_of(hwnd);
        if (w && w->need_paint) {
            LOG(LOG_WIN, "window %x did not validate after WM_PAINT", hwnd);
            w->need_paint = 0;          /* (Windows would repaint forever; stop here) */
        }
    }
    return r;
}

/* ------------------------------------------------------------------ painting */
API(USER32, InvalidateRect, 3)
{
    if (!ARG(0)) { for (int i = 0; i < MAXWIN; i++) if (wins[i].used) wins[i].need_paint = 1; return 1; }
    Win *w = win_of(ARG(0));
    if (w) w->need_paint = 1;
    return w != NULL;
}
API(USER32, InvalidateRgn, 3) { Win *w = win_of(ARG(0)); if (w) w->need_paint = 1; return 1; }
API(USER32, ValidateRect, 2) { Win *w = win_of(ARG(0)); if (w) w->need_paint = 0; return 1; }
API(USER32, RedrawWindow, 4)
{
    Win *w = win_of(ARG(0));
    if (w && (ARG(3) & 1)) w->need_paint = 1;              /* RDW_INVALIDATE */
    if (w && (ARG(3) & 0x100) && w->need_paint) {          /* RDW_UPDATENOW */
        w->need_paint = 0;
        sendmsg(c, ARG(0), WM_PAINT, 0, 0);
    }
    return 1;
}
API(USER32, UpdateWindow, 1)
{
    Win *w = win_of(ARG(0));
    if (w && w->need_paint && (w->style & WS_VISIBLE))
        sendmsg(c, ARG(0), WM_PAINT, 0, 0);
    return 1;
}
API(USER32, LockWindowUpdate, 1) { return 1; }

extern uint32_t gdi_window_dc(uint32_t hwnd);
API(USER32, BeginPaint, 2)
{
    Win *w = win_of(ARG(0));
    uint32_t ps = ARG(1), dc = gdi_window_dc(ARG(0));
    memset(gptr(ps), 0, 64);
    W32(ps, dc);
    int erase = w && w->need_paint;
    if (w) {
        int32_t cw, ch;
        client_size(w, &cw, &ch);
        W32(ps + 16, cw);
        W32(ps + 20, ch);
        w->need_paint = 0;
    }
    if (erase)
        W32(ps + 4, !sendmsg(c, ARG(0), WM_ERASEBKGND, dc, 0));
    return dc;
}
API(USER32, EndPaint, 2) { return 1; }
API(USER32, GetDC, 1) { return gdi_window_dc(ARG(0)); }
API(USER32, GetDCEx, 3) { return gdi_window_dc(ARG(0)); }
API(USER32, GetWindowDC, 1) { return gdi_window_dc(ARG(0)); }
API(USER32, ReleaseDC, 2) { return 1; }
API(USER32, WindowFromDC, 1) { return main_hwnd; }

/* ------------------------------------------------------------------ the default procedure */
uint32_t def_window_proc(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    Win *w = win_of(hwnd);
    switch (msg) {
    case WM_NCCREATE:
        if (w && lp && R32(lp + 36)) {
            uint32_t t = R32(lp + 36);
            if (t >= 0x10000) snprintf(w->text, sizeof w->text, "%s", gptr(t));
        }
        return 1;
    case WM_NCCALCSIZE: {
        if (w && lp) {
            int32_t l, t, r, b;
            frame_size(w->style, w->exstyle, &l, &t, &r, &b);
            W32(lp, R32(lp) + l); W32(lp + 4, R32(lp + 4) + t);
            W32(lp + 8, R32(lp + 8) - r); W32(lp + 12, R32(lp + 12) - b);
        }
        return 0;
    }
    case WM_SETTEXT:
        if (w) {
            snprintf(w->text, sizeof w->text, "%s", lp ? gptr(lp) : "");
            if (hwnd == main_hwnd) host_window_title(w->text);
        }
        return 1;
    case WM_GETTEXT:
        if (!w || !wp) return 0;
        gstrncpy(lp, w->text, wp);
        return strlen(gptr(lp));
    case WM_GETTEXTLENGTH:
        return w ? strlen(w->text) : 0;
    case WM_CLOSE:
        destroy_window(c, hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        if (w) w->need_paint = 0;
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == 0xf060)                         /* SC_CLOSE */
            sendmsg(c, hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == 0x73)                                     /* Alt+F4 */
            sendmsg(c, hwnd, WM_SYSCOMMAND, 0xf060, 0);
        return 0;
    case WM_WINDOWPOSCHANGED:
        if (w && lp) {
            uint32_t fl = R32(lp + 24);
            if (!(fl & 1)) {
                int32_t cw, ch;
                client_size(w, &cw, &ch);
                sendmsg(c, hwnd, WM_SIZE, 0, ((uint32_t)ch << 16) | ((uint32_t)cw & 0xffff));
            }
            if (!(fl & 2))
                sendmsg(c, hwnd, WM_MOVE, 0, ((uint32_t)w->y << 16) | ((uint32_t)w->x & 0xffff));
        }
        return 0;
    case WM_NCHITTEST: return 1;                            /* HTCLIENT */
    case WM_MOUSEACTIVATE: return 1;                        /* MA_ACTIVATE */
    case WM_NCACTIVATE: return 1;
    case WM_SETCURSOR: return 0;
    case 0x0011: return 1;                                  /* WM_QUERYENDSESSION */
    case 0x0013: return 1;                                  /* WM_QUERYOPEN */
    case 0x007f: return 0;                                  /* WM_GETICON */
    default: return 0;
    }
}

API(USER32, DefWindowProcA, 4) { return def_window_proc(c, ARG(0), ARG(1), ARG(2), ARG(3)); }
API(USER32, DefFrameProcA, 5) { return def_window_proc(c, ARG(0), ARG(2), ARG(3), ARG(4)); }
API(USER32, DefMDIChildProcA, 4) { return def_window_proc(c, ARG(0), ARG(1), ARG(2), ARG(3)); }

/* ------------------------------------------------------------------ registered messages */
API(USER32, RegisterWindowMessageA, 1)
{
    extern uint32_t atom_add(const char *s);
    uint32_t a = atom_add(gstr(ARG(0)));
    LOG(LOG_WIN, "RegisterWindowMessageA(\"%s\") -> %x", gstr(ARG(0)), a);
    return a;
}

API(USER32, RegisterClipboardFormatA, 1)
{
    extern uint32_t atom_add(const char *s);
    return atom_add(gstr(ARG(0)));
}
