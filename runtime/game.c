/* Stranglehold's own hooks and per-frame work. Empty to start: filled as the port needs it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "runtime.h"
#include "options.h"
#include "input.h"
#include "funcs.h"
#include "pad.h"

/* ------------------------------------------------------------------ PC or console behaviour
   UStrangleholdEngineInfo's vtable slot +0xf0 (vtable 0xdb85c4, the object at [0x1066700]) answers
   "PC controls?" with the shared return-1 stub 0x567a30 [verified: runtime read of the slot;
   0x567a30 is mov eax,1/ret, pointed at 5001 times, so only this slot is changed]. Six places ask
   it [verified: disassembly]: the camera picks the stick's rate orbit (vtable +0x14c) or the
   mouse's absolute orbit (+0x150) by it (0x5d7a3a, 0x5e359e), the cinema camera picks the PC or
   the Xbox CinemaCameraConfig (0x5dab63), and 0x5e0d6c, 0x5e0efc, 0x6d52cc. It now answers from
   the device used last: console (0) while a controller is in use, PC (1) with keyboard and mouse.
   With the PC answer the right stick did not turn the camera [verified: a run]. */
#define ENGINEINFO_PC_SLOT (0xdb85c4u + 0xf0u)
static void engine_is_pc(CPU *c)
{
    c->eax = !input_controller_last();
    api_return(c, c->eax, 0);
}


/* ------------------------------------------------------------------ rumble
   On the 360 the PlayerController makes XeDrv.XnaForceFeedbackManager, whose native
   ApplyForceFeedback(ControllerId, DeltaTime) plays the waveform scripts start
   (ClientPlayForceFeedbackWaveform). The PC config has that line commented out, and its
   UXnaForceFeedbackManager (vtable 0xe5e4d4) keeps UForceFeedbackManager's empty
   ApplyForceFeedback, slot 55 (+0xdc: ret 8) [verified: the vtables; UObject has 54 slots]. The
   user's config turns the manager on (tools/upkpatch.py) and this slot gets ffm_apply, which does
   what the console's did, UE3's waveform rules: samples of (left amplitude, right amplitude, left
   function, right function: bytes; duration: float), played in turn, each shaped over its time
   (constant, linear up or down, quarter or half sine, noise), times ScaleAllWaveformsBy, looping if
   asked; the manager's CurrentSample, ElapsedTime and FFWaveform are kept up to date for the
   scripts. Left is the heavy (low) motor. Field places are found from the first waveform played:
   the manager's FFWaveform is the pointer to a UForceFeedbackWaveform (vtable 0xe1db6c), after the
   bools (bAllowsForceFeedback bit 0, bIsPaused bit 1 [guess: UE3's order]) and followed by
   CurrentSample, ElapsedTime, ScaleAllWaveformsBy (1.0 by default, checked); the waveform's own
   fields start at the same object size: bIsLooping, then Samples (data, count, max) [guess: UE3's
   ForceFeedbackManager.uc / ForceFeedbackWaveform.uc layout, checked against the data when found]. */
#define XNA_FFM_VTABLE    0xe5e4d4u
#define FFM_APPLY_SLOT    (XNA_FFM_VTABLE + 55u * 4u)
#define FFWAVE_VTABLE     0xe1db6cu
static int ffm_wave_off = -1;           /* the manager's FFWaveform field, once found */

static int guest_ok(uint32_t a) { return a >= ARENA_START && a < ARENA_END; }

static double wave_shape(int fn, double pct)
{
    switch (fn) {
    case 0: return 1;                                   /* WF_Constant */
    case 1: return pct;                                 /* WF_LinearIncreasing */
    case 2: return 1 - pct;                             /* WF_LinearDecreasing */
    case 3: return sin(pct * M_PI / 2);                 /* WF_Sin0to90 */
    case 4: return sin(M_PI / 2 + pct * M_PI / 2);      /* WF_Sin90to180 */
    case 5: return sin(pct * M_PI);                     /* WF_Sin0to180 */
    case 6: return (double)rand() / RAND_MAX;           /* WF_Noise */
    }
    return 1;
}

static void motors(uint16_t low, uint16_t high)
{
    static uint16_t was_low, was_high;
    if (!low && !high && !was_low && !was_high)
        return;
    if ((low != 0) != (was_low != 0) || (high != 0) != (was_high != 0))
        LOG(LOG_INPUT, "rumble: motors %u %u", low, high);
    for (int s = 0; s < PAD_SLOTS; s++)
        input_joy_rumble(s, low, high, low || high ? 200 : 0);
    was_low = low;
    was_high = high;
}

/* one manager's waveform advanced by dt; its motor levels (0-255) into *l, *r */
static void ffm_step(uint32_t self, float dt, double *l, double *r)
{
    *l = *r = 0;
    if (ffm_wave_off < 0) {
        for (int off = 0x20; off <= 0x80; off += 4) {
            uint32_t w = R32(self + off);
            if (guest_ok(w) && R32(w) == FFWAVE_VTABLE && R32(self + off + 12) == 0x3f800000u) {
                ffm_wave_off = off;
                LOG(LOG_ALWAYS, "rumble: the manager's waveform field at +%#x", off);
                break;
            }
        }
        if (ffm_wave_off < 0) return;
    }
    uint32_t bools = R32(self + ffm_wave_off - 4), wave = R32(self + ffm_wave_off);
    if (!wave || !guest_ok(wave) || R32(wave) != FFWAVE_VTABLE || !(bools & 1) || (bools & 2))
        return;
    uint32_t fields = (uint32_t)ffm_wave_off - 4;        /* (the object size: both classes' first field) */
    int loop = R32(wave + fields) & 1;
    uint32_t data = R32(wave + fields + 4);
    int32_t n = (int32_t)R32(wave + fields + 8);
    int32_t cur = (int32_t)R32(self + ffm_wave_off + 4);
    float elapsed = RF32(self + ffm_wave_off + 8) + dt, scale = RF32(self + ffm_wave_off + 12);
    if (n <= 0 || n > 256 || !guest_ok(data)) return;
    while (cur < n && elapsed > RF32(data + 8 * cur + 4)) {
        elapsed -= RF32(data + 8 * cur + 4);
        cur++;
    }
    if (cur >= n) {
        if (loop) cur = 0;
        else {
            W32(self + ffm_wave_off, 0);                /* (done: FFWaveform = None) */
            W32(self + ffm_wave_off + 4, 0);
            WF32(self + ffm_wave_off + 8, 0);
            return;
        }
    }
    W32(self + ffm_wave_off + 4, (uint32_t)cur);
    WF32(self + ffm_wave_off + 8, elapsed);
    uint32_t smp = data + 8 * (uint32_t)cur;
    float dur = RF32(smp + 4);
    double pct = dur > 0 ? elapsed / dur : 0;
    if (pct > 1) pct = 1;
    double lv = R8(smp) * wave_shape(R8(smp + 2), pct) * scale, rv = R8(smp + 1) * wave_shape(R8(smp + 3), pct) * scale;
    *l = lv < 0 ? 0 : lv > 255 ? 255 : lv;
    *r = rv < 0 ? 0 : rv > 255 ? 255 : rv;
}

/* if the engine ever calls ApplyForceFeedback (the console's path), it plays the same way */
static void ffm_apply(CPU *c)
{
    uint32_t self = c->ecx;
    float dt = RF32(c->esp + 8);
    api_return(c, 0, 2);
    double l, r;
    ffm_step(self, dt, &l, &r);
    motors((uint16_t)(l * 257), (uint16_t)(r * 257));
}

/* On PC nothing calls ApplyForceFeedback [verified: the slot never ran in a level, while the
   manager exists: SHOLD_FIND found the class default and one live instance]. So the managers are
   found by a scan of program memory for their vtable, a slice each tick (a full pass about every
   1.5 s until one is found, then every 6 s), and every one found is stepped each tick from the engine loop; the strongest drives the
   motors (the class default never has a waveform). */
#define FFM_MAX 8
static uint32_t ffm_found[FFM_MAX], ffm_next[FFM_MAX];
static int ffm_nfound, ffm_nnext, scan_region;
static uint32_t scan_at;

static void ffm_scan_slice(void)
{
    /* bytes a tick: 16 MB while no manager is known, 4 MB (a pass about every 6 s, catching a
       new one after a level change) once one is */
    uint32_t budget = ffm_nfound ? 4u << 20 : 16u << 20;
    while (budget) {
        if (scan_region >= mem_region_count()) {
            memcpy(ffm_found, ffm_next, sizeof ffm_found);
            ffm_nfound = ffm_nnext;
            ffm_nnext = 0;
            scan_region = 0;
            scan_at = 0;
            return;
        }
        uint32_t b, n;
        mem_region(scan_region, &b, &n);
        if (scan_at < b) scan_at = b;
        uint32_t end = b + n, stop = scan_at + budget < end ? scan_at + budget : end;
        for (uint32_t a = scan_at; a + 4 <= stop; a += 4)
            if (R32(a) == XNA_FFM_VTABLE && ffm_nnext < FFM_MAX)
                ffm_next[ffm_nnext++] = a;
        budget -= stop - scan_at;
        scan_at = stop;
        if (stop >= end) { scan_region++; scan_at = 0; }
    }
}

static void rumble_tick(float dt)
{
    ffm_scan_slice();
    double l = 0, r = 0;
    for (int i = 0; i < ffm_nfound; i++) {
        if (R32(ffm_found[i]) != XNA_FFM_VTABLE) continue;
        double a, b;
        ffm_step(ffm_found[i], dt, &a, &b);
        if (a > l) l = a;
        if (b > r) r = b;
    }
    motors((uint16_t)(l * 257), (uint16_t)(r * 257));
}

static uint32_t guest_wstr(const char *s);

/* ------------------------------------------------------------------ controller layout
   The 360 lets the player pick one of four controller configurations (profile setting 108); they
   differ only in the action set (XBoxActions, XBoxActionsAlt1-3) [verified: work/ctrl_pc]. The PC
   always pushes PCConfig when the profile loads (0x6a7240, cdecl, the MwyInputHandler as its one
   argument: FString "PCConfig" to the handler's load-by-name 0x4a3130) [verified: disassembly].
   tools/upkpatch.py makes XBoxConfigAlt1-3_99 complete PC configs with that layout's actions
   (PCConfig itself has the standard one); hook_load_profile_config pushes the chosen one instead.
   The choice is option controller_layout for now: the PC's Control Config opens its keyboard
   screen, not the 360's layout screen (the menus are one WooScreenSet blob) [verified: shot]. */
static int hook_load_profile_config_fn(CPU *c)
{
    int layout = atoi(option_str("controller_layout", "1"));
    if (layout < 2 || layout > 4)
        return 0;                           /* (PCConfig: the standard layout) */
    char name[32];
    snprintf(name, sizeof name, "XBoxConfigAlt%d_99", layout - 1);
    uint32_t n = (uint32_t)strlen(name) + 1, fs = galloc(12);
    W32(fs, guest_wstr(name));
    W32(fs + 4, n);
    W32(fs + 8, n);
    uint32_t handler = R32(c->esp + 4), ret = R32(c->esp);
    LOG(LOG_ALWAYS, "controller layout %d: %s", layout, name);
    c->ecx = handler;
    guest_call(c, 0x4a3130u, 1, fs);
    c->ret = ret;
    c->esp += 4;                            /* (cdecl: the caller pops the argument) */
    return 1;
}

/* ------------------------------------------------------------------ PC-only key texts
   A few PC strings have the keys written in, keys the 360 never had because its menus drew
   button images there [verified: strings.eng on both]: Press_Start "Press ENTER" (the title),
   Press_Start_Button, Press_A_button_To_Begin_Match and tips_PressAContinue ("Press SPACEBAR to
   Continue"). While a controller is in use the localized text has the PC Start key's name
   (display table entry 0) replaced by the Xbox Start glyph and the PC L2 name (entry 9,
   "SPACEBAR" in English, which these strings use for A) by the A glyph (entry 16), from the
   same display tables as the prompts, so every language works; the title's two strings take the
   word START instead, its font having no glyphs [verified: shot, "Press a"]. The texts are shorter, so the
   engine's own string is edited in place (it frees it with its own allocator). */
static uint32_t disp_pc, disp_xbox;     /* (the display tables: defined with the prompts below) */
static uint32_t guest_wstr(const char *s);

static int wstr_eq(uint32_t w, const char *s)
{
    if (!w) return 0;
    for (;; w += 2, s++) {
        uint32_t ch = R16(w);
        if (ch != (uint8_t)*s) return 0;
        if (!ch) return 1;
    }
}

/* in the FString at fs, every occurrence of the text of from (an FString) replaced by to's */
static void fstr_replace(uint32_t fs, uint32_t from, uint32_t to)
{
    uint32_t d = R32(fs), fd = R32(from), td = R32(to);
    int32_t n = (int32_t)R32(fs + 4) - 1, fn = (int32_t)R32(from + 4) - 1, tn = (int32_t)R32(to + 4) - 1;
    if (!d || !fd || !td || n <= 0 || fn <= 0 || tn < 0 || tn > fn) return;
    int32_t o = 0;
    for (int32_t i = 0; i < n; ) {
        int match = i + fn <= n;
        for (int32_t k = 0; match && k < fn; k++)
            if (R16(d + 2 * (uint32_t)(i + k)) != R16(fd + 2 * (uint32_t)k)) match = 0;
        if (match) {
            for (int32_t k = 0; k < tn; k++) W16(d + 2 * (uint32_t)o++, R16(td + 2 * (uint32_t)k));
            i += fn;
        } else
            W16(d + 2 * (uint32_t)o++, R16(d + 2 * (uint32_t)i++));
    }
    W16(d + 2 * (uint32_t)o, 0);
    W32(fs + 4, (uint32_t)o + 1);
}

static int hook_localize_fn(CPU *c)
{
    if (!disp_xbox || !input_controller_last())
        return 0;
    uint32_t out = R32(c->esp + 4), key = R32(c->esp + 12);
    if (!wstr_eq(key, "Press_Start") && !wstr_eq(key, "Press_Start_Button") &&
        !wstr_eq(key, "Press_A_button_To_Begin_Match") && !wstr_eq(key, "tips_PressAContinue"))
        return 0;
    int title = wstr_eq(key, "Press_Start") || wstr_eq(key, "Press_Start_Button");
    F_006f85c0_orig(c);                     /* (returns to our caller's state: ret popped) */
    if (title) {
        /* the title's font has no button glyphs (it drew "a"): the word START, as consoles label it */
        static uint32_t start_fs;
        if (!start_fs) {
            start_fs = galloc(12);
            W32(start_fs, guest_wstr("START"));
            W32(start_fs + 4, 6);
            W32(start_fs + 8, 6);
        }
        fstr_replace(out, disp_pc + 12 * 0, start_fs);
    } else {
        fstr_replace(out, disp_pc + 12 * 0, disp_xbox + 12 * 0);
        fstr_replace(out, disp_pc + 12 * 9, disp_xbox + 12 * 16);
    }
    return 1;
}

/* 0x659620's crash guard (builds/gog.py): the chain the original reads, and if it is empty only the
   flag is set (its first part) */
static int hook_widescreen_flag_fn(CPU *c)
{
    uint32_t eng = R32(0x10ae0e8u), obj = 0;
    uint32_t x = eng ? R32(eng + 0x2d8) : 0;
    if (x) {
        uint32_t r = guest_call(c, 0x6419d0u, 1, x);       /* (cdecl getter) */
        obj = r ? R32(r + 0xe4) : 0;
    }
    if (obj)
        return 0;                           /* (the original: it can notify) */
    uint32_t self = c->ecx, wide = R32(c->esp + 4) & 0xff;
    if (R8(self + 0x1b8) != wide) {
        W8(self + 0x1b8, wide);
        LOG(LOG_ALWAYS, "widescreen flag %u set without notifying (no game object yet)", wide);
    }
    c->ret = R32(c->esp);
    c->esp += 8;                            /* (ret 4) */
    return 1;
}

/* ------------------------------------------------------------------ menus
   The menus' screen engine (DESIGN.md section 8). The patched Settings menus open the 360's
   controller-layout screen or the PC's keyboard screen on screen variable 9999, which this hook
   answers with "a pad is in use" [guess until run: the IfVar path reaches GetVar 0x6a5250]. */
static int hook_screen_var_fn(CPU *c)
{
    if (R32(c->esp + 4) != 9999)
        return 0;
    c->eax = input_controller_last() ? 1 : 0;
    c->ret = R32(c->esp);
    c->esp += 12;                           /* (ret 8) */
    return 1;
}

/* The image drawer 0x6dc770 refuses 13 button images by name (_stricmp against the strings at
   0xdff75c-0xdff8a0: Generic_Elements-Nav_A/X/B/Y, _HudElements-SB_IconBackingFill,
   Generic_Elements-Xbox_Back/LA/LB/LT/RA/RB/RT/Start; nothing else uses them) [verified:
   disassembly]. While a pad is in use their first letters are changed so nothing matches and the
   menus draw the Xbox buttons; with keyboard and mouse the names are put back. */
static const uint32_t blocked_images[] = {0xdff75c, 0xdff774, 0xdff78c, 0xdff7a4, 0xdff7c4, 0xdff7dc,
                                          0xdff7f8, 0xdff814, 0xdff830, 0xdff84c, 0xdff868, 0xdff884,
                                          0xdff8a0};
static void images_tick(void)
{
    static int shown = -1;
    int want = input_controller_last() != 0;
    if (want == shown)
        return;
    static uint8_t first[sizeof blocked_images / sizeof *blocked_images];
    for (size_t i = 0; i < sizeof blocked_images / sizeof *blocked_images; i++) {
        if (shown < 0)
            first[i] = (uint8_t)R8(blocked_images[i]);
        W8(blocked_images[i], want ? 1 : first[i]);
    }
    shown = want;
    LOG(LOG_INPUT, "menus: Xbox button images %s", want ? "shown" : "hidden (PC)");
}

static int hook_button_name_fn(CPU *c);

void game_init(void)
{
    hook_screen_var = hook_screen_var_fn;
    hook_widescreen_flag = hook_widescreen_flag_fn;
    hook_localize = hook_localize_fn;
    hook_load_profile_config = hook_load_profile_config_fn;
    if (R32(FFM_APPLY_SLOT) == 0x4024a0u)        /* (the empty ret 8) */
        W32(FFM_APPLY_SLOT, thunk_addr(ffm_apply, "UXnaForceFeedbackManager::ApplyForceFeedback (runtime)"));
    else
        LOG(LOG_ALWAYS, "rumble: the force feedback slot is not the expected stub; left alone");
    hook_button_name = hook_button_name_fn;
    if (R32(ENGINEINFO_PC_SLOT) == 0x567a30u)
        W32(ENGINEINFO_PC_SLOT, thunk_addr(engine_is_pc, "UStrangleholdEngineInfo::IsPC (runtime)"));
    else
        LOG(LOG_ALWAYS, "engine info: slot +0xf0 is not the expected stub; left alone");
}

/* SHOLD_POSTMSG="frame:msg:wparam,..." (tests, hex msg/wparam): window messages posted as if from
   the host window at those frames (focus changes, close) */
void game_frame(unsigned long frame)
{
    static char *spec;
    static int parsed;
    if (!parsed) { parsed = 1; const char *e = getenv("SHOLD_POSTMSG"); spec = e ? strdup(e) : NULL; }
    if (!spec) return;
    for (char *p = spec; p && *p; ) {
        unsigned long f = strtoul(p, &p, 10);
        unsigned msg = 0, wp = 0;
        if (*p == ':') msg = (unsigned)strtoul(p + 1, &p, 16);
        if (*p == ':') wp = (unsigned)strtoul(p + 1, &p, 16);
        if (f == frame) {
            extern void post_message(uint32_t msg, uint32_t wparam, uint32_t lparam);
            LOG(LOG_ALWAYS, "SHOLD_POSTMSG: frame %lu: message %x (%x)", frame, msg, wp);
            post_message(msg, wp, 0);
        }
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
}

int game_driving(void)
{
    return 0;
}

/* hooks the shared runtime calls (from 1nsane-native and larush-native): nothing to do here yet */
void video_skip_press(void) {}
uint32_t video_cocreate(uint32_t clsid, uint32_t iid, uint32_t ppv) { (void)clsid; (void)iid; (void)ppv; return 0x80040111u; }
void padgame_frame(unsigned long frame) { (void)frame; }    /* (pads go in on the game thread: pad_tick) */
int ifc_rumble_wanted(void) { return 0; }
void ifc_frame(unsigned long frame) { (void)frame; }
void frame_rate_reapply(void) {}

/* ------------------------------------------------------------------ the engine's main loop
   WinMain's engine part (0x401940) runs the engine under wxWidgets unless its command line has
   -nowxwindows, and the shipped game never passes command-line switches through (ParseParam
   0x6f3000 sees an empty appCmdLine there [verified: gdb on the translated ParseParam]). So
   wxEntry stands in for the -nowxwindows branch (0x401b4f) [verified: disassembly]: the flag
   0x10619a0 cleared, then GEngineLoop (0x10619a4) Init (0x407e70) and Tick (0x4054e0) until
   GIsRequestingExit (0x107147c); WinMain calls Exit (0x404e90) itself after. Between ticks the
   window messages are pumped, as wx's loop did: the engine itself never calls PeekMessage
   [verified: gdb, no PeekMessage calls in a 30 s run; keys never reached the window procedure]. */
#define ENGINE_LOOP        0x10619a4u
#define USING_WX           0x10619a0u
#define REQUESTING_EXIT    0x107147cu

/* ------------------------------------------------------------------ controllers
   The engine has no gamepad input on PC (no XInput; DirectInput only for the mouse), but it keeps
   the console's: the XboxTypeS key names (FName globals, 8 bytes each, from 0x10ae4e8 in this
   order [verified: the start-up code 0x7a11c0 stores each name's FName there]) and the control
   configs that bind them. The pads' state goes in as the Xbox 360's viewport sent it: through the
   ViewportClient of the window client's first viewport (UWindowsClient's viewports at 0x1084968,
   data and count [verified: the window procedure 0x8f5100 looks its viewport up there]; the
   client at viewport +0x38, the FViewport at viewport +4) - InputKey (vtable +0x24: viewport,
   controller id, key, event 0 pressed / 1 released, amount) for buttons, InputAxis (+0x28:
   viewport, controller id, key, delta, delta time) for the sticks every tick (down positive) [verified: the
   keyboard's release sweep 0x8f50a8 and the mouse 0x8f4d99 call them so]. The keyboard is
   controller -3 and the mouse -1 there; a pad is controller 0 [guess: the 360's player 1]. */
#include <math.h>
#include <time.h>
#include "pad.h"
#define VIEWPORTS 0x1084968u
#define XKEY(n)   (0x10ae4e8u + 8u * (n))
enum { K_LX, K_LY, K_RX, K_RY, K_LTHUMB, K_RTHUMB, K_BACK, K_START, K_A, K_B, K_X, K_Y, K_LB, K_RB,
       K_LT, K_RT, K_DUP, K_DDOWN, K_DRIGHT, K_DLEFT, K_COUNT };
#define TRIGGER_DOWN 3855               /* XInput's trigger threshold, 30 of 255 */

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static uint32_t first_viewport(void)
{
    if ((int32_t)R32(VIEWPORTS + 4) <= 0 || !R32(VIEWPORTS))
        return 0;
    return R32(R32(VIEWPORTS));
}

static void send_key(CPU *c, uint32_t vp, int key, int pressed)
{
    uint32_t client = R32(vp + 0x38);
    if (!client) return;
    c->ecx = client;
    guest_call(c, R32(R32(client) + 0x24), 6, vp + 4, 0u, R32(XKEY(key)), R32(XKEY(key) + 4),
               pressed ? 0u : 1u, f2u(1.0f));
}

static void send_axis(CPU *c, uint32_t vp, int key, float v, float dt)
{
    uint32_t client = R32(vp + 0x38);
    if (!client) return;
    c->ecx = client;
    guest_call(c, R32(R32(client) + 0x28), 6, vp + 4, 0u, R32(XKEY(key)), R32(XKEY(key) + 4),
               f2u(v), f2u(dt));
}

/* every connected controller together, as player 1's: buttons held on any, each axis the one
   furthest from rest */
static int pads_merged(uint8_t b[PAD_BUTTONS], int a[PAD_AXES])
{
    int any = 0;
    memset(b, 0, PAD_BUTTONS);
    memset(a, 0, PAD_AXES * sizeof *a);
    for (int s = 0; s < PAD_SLOTS; s++) {
        uint8_t pb[PAD_BUTTONS];
        int pa[PAD_AXES];
        if (!pad_read(s, pb, pa)) continue;
        any = 1;
        for (int i = 0; i < PAD_BUTTONS; i++) b[i] |= pb[i];
        for (int i = 0; i < PAD_AXES; i++) if (abs(pa[i]) > abs(a[i])) a[i] = pa[i];
    }
    return any;
}

/* SHOLD_FIND=<vtable hex>@<tick> (tests): objects with that vtable, listed once at that tick */
static void find_objects(unsigned long tick)
{
    static long at = -2;
    static uint32_t vt;
    if (at == -2) {
        const char *e = getenv("SHOLD_FIND");
        at = -1;
        if (e && strchr(e, '@')) { vt = (uint32_t)strtoul(e, NULL, 16); at = atol(strchr(e, '@') + 1); }
    }
    if (at < 0 || tick != (unsigned long)at) return;
    int found = 0;
    for (int i = 0; i < mem_region_count(); i++) {
        uint32_t b, n;
        mem_region(i, &b, &n);
        for (uint32_t a = b; a + 4 <= b + n; a += 4)
            if (R32(a) == vt && found++ < 20)
                LOG(LOG_ALWAYS, "find: %08x at %08x", vt, a);
    }
    LOG(LOG_ALWAYS, "find: %d objects with vtable %08x at tick %lu", found, vt, tick);
}

static void pad_tick(CPU *c)
{
    static uint8_t was[K_COUNT];
    static int had_pad;
    static double last;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9, dt = last ? now - last : 0;
    last = now;
    uint32_t vp = first_viewport();
    uint8_t b[PAD_BUTTONS];
    int a[PAD_AXES];
    int have = pads_merged(b, a);
    if (!vp || (!have && !had_pad))
        return;
    uint8_t is[K_COUNT] = {0};
    is[K_LTHUMB] = b[PAD_LSTICK]; is[K_RTHUMB] = b[PAD_RSTICK]; is[K_BACK] = b[PAD_BACK];
    is[K_START] = b[PAD_START]; is[K_A] = b[PAD_A]; is[K_B] = b[PAD_B]; is[K_X] = b[PAD_X];
    is[K_Y] = b[PAD_Y]; is[K_LB] = b[PAD_LB]; is[K_RB] = b[PAD_RB];
    is[K_LT] = a[PAD_LT] > TRIGGER_DOWN; is[K_RT] = a[PAD_RT] > TRIGGER_DOWN;
    is[K_DUP] = b[PAD_DUP]; is[K_DDOWN] = b[PAD_DDOWN]; is[K_DRIGHT] = b[PAD_DRIGHT];
    is[K_DLEFT] = b[PAD_DLEFT];
    for (int k = K_LTHUMB; k < K_COUNT; k++)
        if (is[k] != was[k]) {
            LOG(LOG_INPUT, "pad: key %d %s", k, is[k] ? "pressed" : "released");
            send_key(c, vp, k, is[k]);
            was[k] = is[k];
        }
    /* the sticks every tick, as the 360's viewport did: -1 .. 1, right and DOWN positive (SDL's
       own sense) [verified: the 360 menu config moves down on LeftY > 0.5, and the PC's keyboard
       movement is Desire_MoveY = S - W, forward negative; up positive had the menus upside down] */
    send_axis(c, vp, K_LX, a[PAD_LX] / 32768.0f, (float)dt);
    send_axis(c, vp, K_LY, a[PAD_LY] / 32768.0f, (float)dt);
    send_axis(c, vp, K_RX, a[PAD_RX] / 32768.0f, (float)dt);
    send_axis(c, vp, K_RY, a[PAD_RY] / 32768.0f, (float)dt);
    had_pad = have;
}

/* ------------------------------------------------------------------ button prompts
   The prompts' button names come from the engine info object itself ([0x1066700],
   UStrangleholdEngineInfo, vtable 0xdb85c4, which carries the controller-token fields; 0x6d1000 is
   called on it [verified: gdb, ecx = the engine info, 18 names]; a separate
   UWooControllerTokenManager at [engine info +0x2b8] has an empty list): its ButtonMappings
   (TArray<FString> at +0x288: data, count, max; 12-byte FStrings) name the
   ControllerSettings localization key of each virtual pad button, which 0x6d1000 copies and the
   prompt builder localizes [verified: disassembly]. The PC config names PC*ButtonName (keyboard
   keys); the 360's XBox*ButtonName, which the PC's localization still has as the font's button
   glyphs [verified: work/loceng_pc/ControllerSettings.eng]. A copy with the XBox names is made once
   the list is there (prompts_tick); hook_button_name answers from it while a controller is in use. */
#define ENGINE_INFO        0x1066700u
#define ENGINEINFO_VTABLE  0xdb85c4u
static uint32_t tok_mgr, tok_pc_data, tok_xbox_data;

/* the XBox copy of the engine info's ButtonMappings (made once, when the list is first there) */
static void prompts_copy(uint32_t tm)
{
    int32_t n = (int32_t)R32(tm + 0x28c);
    if (tok_xbox_data || n <= 0 || n > 64)
        return;
    tok_mgr = tm;
    tok_pc_data = R32(tm + 0x288);
    uint32_t x_data = galloc(12 * (uint32_t)n);
    for (int i = 0; i < n; i++) {
        uint32_t f = tok_pc_data + 12 * (uint32_t)i, src = R32(f);
        int32_t len = (int32_t)R32(f + 4);
        char name[96] = "";
        for (int k = 0; k < len - 1 && k < 94; k++) name[k] = (char)R16(src + 2 * k);
        char x[100];
        snprintf(x, sizeof x, "%s%s", !strncmp(name, "PC", 2) ? "XBox" : "", !strncmp(name, "PC", 2) ? name + 2 : name);
        uint32_t m = (uint32_t)strlen(x) + 1, w = galloc(2 * m);
        for (uint32_t k = 0; k < m; k++) W16(w + 2 * k, (uint8_t)x[k]);
        W32(x_data + 12 * (uint32_t)i, w);
        W32(x_data + 12 * (uint32_t)i + 4, m);
        W32(x_data + 12 * (uint32_t)i + 8, m);
    }
    tok_xbox_data = x_data;
    LOG(LOG_ALWAYS, "prompts: %d button names, XBox copy ready", n);
}

/* The prompts' display text: at start-up the controller token manager (UWooControllerTokenManager,
   vtable 0xdbbcdc, at [engine info +0x2b8]) localizes its copy of the names (+0x5c: data, count)
   into display strings (+0x68: data, count, max) with Localize("Controls", name,
   "ControllerSettings") (0x6bdc70, from engine init 0x65ba50) [verified: gdb backtrace of the
   Localize calls for PCStartButtonName...; the tutorial text reads those]. An XBox table is made
   once the same way (Localize 0x6f85c0, cdecl: &out, section, key, package, 0, 0), and the
   +0x68 data pointer follows the device in use; the engine's own table is put back before exit. */
#define TOKENMGR_VTABLE  0xdbbcdcu
#define LOC_SECTION      0xdfde9cu      /* L"Controls" (the strings 0x6bdc70 passes) */
#define LOC_PACKAGE      0xdfde74u      /* L"ControllerSettings" */
static uint32_t disp_mgr, disp_pc, disp_xbox;

static uint32_t guest_wstr(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1, w = galloc(2 * n);
    for (uint32_t k = 0; k < n; k++) W16(w + 2 * k, (uint8_t)s[k]);
    return w;
}

static void display_names(CPU *c, uint32_t tm)
{
    int32_t n = (int32_t)R32(tm + 0x60);
    if (n <= 0 || n > 64 || (int32_t)R32(tm + 0x6c) != n || !R32(tm + 0x68))
        return;                             /* (not localized yet) */
    disp_mgr = tm;
    disp_pc = R32(tm + 0x68);
    uint32_t x = galloc(12 * (uint32_t)n), src = R32(tm + 0x5c);
    for (int i = 0; i < n; i++) {
        uint32_t f = src + 12 * (uint32_t)i, sp = R32(f);
        int32_t len = (int32_t)R32(f + 4);
        char name[96] = "", key[100];
        for (int k = 0; k < len - 1 && k < 94; k++) name[k] = (char)R16(sp + 2 * k);
        snprintf(key, sizeof key, "%s%s", !strncmp(name, "PC", 2) ? "XBox" : "", !strncmp(name, "PC", 2) ? name + 2 : name);
        uint32_t out = x + 12 * (uint32_t)i;
        W32(out, 0); W32(out + 4, 0); W32(out + 8, 0);
        guest_call(c, 0x6f85c0u, 6, out, LOC_SECTION, guest_wstr(key), LOC_PACKAGE, 0u, 0u);
    }
    disp_xbox = x;
    LOG(LOG_ALWAYS, "prompts: %d display names, Xbox table ready", n);
}

static void prompts_tick(CPU *c)
{
    uint32_t ei = R32(ENGINE_INFO);
    if (!ei || R32(ei) != ENGINEINFO_VTABLE)
        return;
    prompts_copy(ei);
    uint32_t tm = R32(ei + 0x2b8);
    if (!tm || R32(tm) != TOKENMGR_VTABLE)
        return;
    if (!disp_xbox) {
        display_names(c, tm);
        if (!disp_xbox) return;
    }
    if (tm != disp_mgr) return;
    uint32_t want = input_controller_last() ? disp_xbox : disp_pc;
    if (R32(tm + 0x68) != want) {
        W32(tm + 0x68, want);
        LOG(LOG_INPUT, "prompts: %s names", want == disp_xbox ? "Xbox" : "PC");
    }
}

static void prompts_restore(void)
{
    if (disp_mgr && R32(disp_mgr) == TOKENMGR_VTABLE)
        W32(disp_mgr + 0x68, disp_pc);      /* (the engine frees its own table at exit) */
}

/* hook at 0x6d1000 (builds/gog.py): while a controller is in use, the key comes from the XBox copy,
   and the two layout entries the PC moved to its own keys (ControllerLayouts[10] = 17 'F',
   [14] = 16 'E') take the 360's (1 Back, 0 Start) [verified: work/script_pc vs script_360
   WooEngineInfo]. Swapping the array pointer instead did not hold: the engine sets the list again
   while it loads a level and expands the prompts inside that same tick [verified: gdb]. */
static int hook_button_name_fn(CPU *c)
{
    if (!input_controller_last() || R32(c->ecx) != ENGINEINFO_VTABLE)
        return 0;
    prompts_copy(c->ecx);
    if (!tok_xbox_data)
        return 0;
    {
        static int said;
        if (!said++) LOG(LOG_ALWAYS, "prompts: Xbox button names in use");
    }
    uint32_t self = c->ecx, out = R32(c->esp + 4), idx = R32(c->esp + 8), col = R32(c->esp + 12) & 0xff;
    uint32_t pos = R32(R32(self + 0x294) + idx * 28 + 0x18) + col;
    uint32_t b = R8(R32(self + 0x1bc) + pos);
    if (pos == 10) b = 1;
    else if (pos == 14) b = 0;
    if ((int32_t)b >= (int32_t)R32(self + 0x28c))
        return 0;
    uint32_t ret = R32(c->esp);
    c->ecx = out;                           /* FString copy (0x401350, thiscall: this, source) */
    guest_call(c, 0x401350u, 1, tok_xbox_data + 12 * b);
    c->eax = out;
    c->ret = ret;
    c->esp += 4 + 12;                       /* (ret 0xc) */
    return 1;
}

static uint32_t engine_call(CPU *c, uint32_t fn)
{
    c->ecx = ENGINE_LOOP;               /* thiscall */
    return guest_call(c, fn, 0);
}

API_CDECL(WXMSW262U, _SetUnrealCallbacks_wxEventLoopBase__SAXPAVwxUnrealCallbacks_1__Z) { return 0; }

/* The program checks its 43 script packages (Core.xxx ... StrangleholdGame.xxx) against SHA-1
   hashes of their uncompressed data in a resource (entries "..\\StrangleholdGame\\CookedPC\\X.xxx",
   NUL, 20 hash bytes; at 0x117fbd8-0x11808b0 with the Coalesced files' entries) and the engine
   quits at start-up when one differs [verified: WooEngineInfo.xxx's entry = SHA-1 of the
   uncompressed package; a recompressed copy starts, the same copy with one value changed quits
   before the first tick]. LoadResource hands the game a pointer into the program image (res.c),
   so a package that tools/upkpatch.py changes gets its new hash from <name>.sha1 beside it in
   the user folder, written here before the engine starts. */
static void package_hashes(void)
{
    static const char prefix[] = "..\\StrangleholdGame\\CookedPC\\";
    for (uint32_t a = 0x117fbd8u; a < 0x11808b0u; a++) {
        if (memcmp(gptr(a), prefix, sizeof prefix - 1))
            continue;
        const char *name = (const char *)gptr(a) + sizeof prefix - 1;
        char path[1024], hex[48] = "";
        snprintf(path, sizeof path, "%s/StrangleholdGame/CookedPC/%s.sha1", user_dir, name);
        uint32_t hash = a + (uint32_t)strlen((const char *)gptr(a)) + 1;
        FILE *f = fopen(path, "r");
        if (f) {
            int ok = fscanf(f, "%40s", hex) == 1 && strlen(hex) == 40;
            fclose(f);
            for (int i = 0; ok && i < 20; i++) {
                unsigned v;
                ok = sscanf(hex + 2 * i, "%2x", &v) == 1;
                W8(hash + (uint32_t)i, v);
            }
            LOG(LOG_ALWAYS, "packages: %s checked against %s%s", name, ok ? "the patched copy's hash " : "?? ", hex);
        }
        a = hash + 19;
    }
}

API_CDECL(WXMSW262U, _wxEntry__YAHPAUHINSTANCE____0PADH_Z)
{
    package_hashes();
    W32(USING_WX, 0);
    uint32_t r = 0;
    if (!R32(REQUESTING_EXIT)) {
        LOG(LOG_ALWAYS, "engine: init");
        r = engine_call(c, 0x407e70u);
        extern int pump_window_messages(CPU *c);
        while (!R32(REQUESTING_EXIT)) {
            if (pump_window_messages(c))        /* (as wx's loop: messages, then the idle tick) */
                W32(REQUESTING_EXIT, 1);
            else {
                pad_tick(c);
                prompts_tick(c);
                images_tick();
                {
                    static double last;
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    double now = ts.tv_sec + ts.tv_nsec / 1e9;
                    rumble_tick(last ? (float)(now - last) : 0.f);
                    last = now;
                }
                static unsigned long ticks;
                engine_call(c, 0x4054e0u);
                find_objects(ticks);
                if (++ticks % 200 == 0)
                    LOG(LOG_WIN, "engine: %lu ticks, %lu frames", ticks, frame_count);
            }
        }
        prompts_restore();
        LOG(LOG_ALWAYS, "engine: exit requested");
    }
    return r;
}
