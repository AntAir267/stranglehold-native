/* Stranglehold's own hooks and per-frame work. Empty to start: filled as the port needs it. */
#include "runtime.h"

void game_init(void)
{
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
void padgame_frame(unsigned long frame) { (void)frame; }
int ifc_rumble_wanted(void) { return 0; }
void ifc_frame(unsigned long frame) { (void)frame; }
void frame_rate_reapply(void) {}

/* ------------------------------------------------------------------ the engine's main loop
   WinMain's engine part (0x401940) runs the engine under wxWidgets unless its command line has
   -nowxwindows, and the shipped game never passes command-line switches through (ParseParam
   0x6f3000 sees an empty appCmdLine there [verified: gdb on the translated ParseParam]). So
   wxEntry stands in for the -nowxwindows branch (0x401b4f) [verified: disassembly]: the flag
   0x10619a0 cleared, then GEngineLoop (0x10619a4) Init (0x407e70) and Tick (0x4054e0) until
   GIsRequestingExit (0x107147c); WinMain calls Exit (0x404e90) itself after. */
#define ENGINE_LOOP        0x10619a4u
#define USING_WX           0x10619a0u
#define REQUESTING_EXIT    0x107147cu

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
        while (!R32(REQUESTING_EXIT))
            engine_call(c, 0x4054e0u);
        LOG(LOG_ALWAYS, "engine: exit requested");
    }
    return r;
}
