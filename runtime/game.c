/* Stranglehold's own hooks and per-frame work. Empty to start: filled as the port needs it. */
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
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

void game_init(void)
{
    if (R32(ENGINEINFO_PC_SLOT) == 0x567a30u)
        W32(ENGINEINFO_PC_SLOT, thunk_addr(engine_is_pc, "UStrangleholdEngineInfo::IsPC (runtime)"));
    else
        LOG(LOG_ALWAYS, "engine info: slot +0xf0 is not the expected stub; left alone");
}

void game_frame(unsigned long frame)
{
    (void)frame;
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
   viewport, controller id, key, delta, delta time) for the sticks every tick [verified: the
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
    /* the sticks every tick, as the 360's viewport did: -1 .. 1, up and right positive */
    send_axis(c, vp, K_LX, a[PAD_LX] / 32768.0f, (float)dt);
    send_axis(c, vp, K_LY, -a[PAD_LY] / 32768.0f, (float)dt);
    send_axis(c, vp, K_RX, a[PAD_RX] / 32768.0f, (float)dt);
    send_axis(c, vp, K_RY, -a[PAD_RY] / 32768.0f, (float)dt);
    had_pad = have;
}

static uint32_t engine_call(CPU *c, uint32_t fn)
{
    c->ecx = ENGINE_LOOP;               /* thiscall */
    return guest_call(c, fn, 0);
}

API_CDECL(WXMSW262U, _SetUnrealCallbacks_wxEventLoopBase__SAXPAVwxUnrealCallbacks_1__Z) { return 0; }

API_CDECL(WXMSW262U, _wxEntry__YAHPAUHINSTANCE____0PADH_Z)
{
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
                static unsigned long ticks;
                engine_call(c, 0x4054e0u);
                if (++ticks % 200 == 0)
                    LOG(LOG_WIN, "engine: %lu ticks, %lu frames", ticks, frame_count);
            }
        }
        LOG(LOG_ALWAYS, "engine: exit requested");
    }
    return r;
}
