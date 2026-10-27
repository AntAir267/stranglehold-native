/* The host window, keyboard, mouse and controllers on SDL3 (design point 3).
   Without --window everything here is inert (headless runs for tests and the first milestone).

   Keys go two ways, like on Windows: to DirectInput by physical position (input.h) and to the
   window procedure as WM_KEYDOWN/WM_KEYUP (+ WM_CHAR via TranslateMessage). Alt+Enter toggles
   between a window and borderless fullscreen and is not passed on. */
#include <math.h>
#include <stdlib.h>
#include <strings.h>
#include <SDL3/SDL.h>
#include <pthread.h>
#include "runtime.h"
#include "options.h"
#include "win.h"
#include "input.h"
#include "pad.h"
#include "sound.h"

static SDL_Window *window;
static int sdl_ok;
static pthread_t sdl_thread;         /* the thread that started SDL: the one that takes host events */
static uint8_t vk_state[256];
static int fullscreen;
int start_fullscreen;                   /* --fullscreen: go fullscreen at the first display mode */

/* ------------------------------------------------------------------ sound output
   The mixer (dsound.c) runs in SDL's audio callback; its consumption drives DirectSound's
   play cursors from then on. */
static SDL_AudioStream *audio;
static float *mixbuf;
static int mixcap, audio_rate;

static void audio_cb(void *ud, SDL_AudioStream *s, int additional, int total)
{
    (void)ud; (void)total;
    int frames = additional / (int)(2 * sizeof(float));
    if (frames <= 0) return;
    if (frames > mixcap) {
        mixcap = frames * 2;
        mixbuf = realloc(mixbuf, mixcap * 2 * sizeof(float));
    }
    sound_mix(mixbuf, frames, audio_rate);
    SDL_PutAudioStreamData(s, mixbuf, frames * 2 * (int)sizeof(float));
}

static void audio_open(void)
{
    SDL_AudioSpec dev;
    int sample_frames = 512;
    if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &dev, &sample_frames)) {
        LOG(LOG_ALWAYS, "no sound output: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, dev.freq > 0 ? dev.freq : 48000};
    audio_rate = spec.freq;
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
    if (!audio) {
        LOG(LOG_ALWAYS, "no sound output: %s", SDL_GetError());
        return;
    }
    sound_output_attach(audio_rate, sample_frames > 0 ? sample_frames * 2 : 1024);
    SDL_ResumeAudioStreamDevice(audio);
    LOG(LOG_SOUND, "sound output %d Hz, %d-frame chunks", audio_rate, sample_frames);
}

/* ------------------------------------------------------------------ rumble */
#define MAXPADS 8
static SDL_Gamepad *pad_of_slot[MAXPADS];

static int rumble_on = 1;            /* Enhanced option "rumble" */

static void rumble(int slot, uint16_t low, uint16_t high, uint32_t ms)
{
    if (!rumble_on || slot < 0 || slot >= MAXPADS || !pad_of_slot[slot])
        return;
    SDL_RumbleGamepad(pad_of_slot[slot], low, high, ms == 0xffffffffu ? 0xffffffffu : ms);
}

/* ------------------------------------------------------------------ controllers (pad.c) */
_Static_assert((int)SDL_GAMEPAD_BUTTON_SOUTH == PAD_A && (int)SDL_GAMEPAD_BUTTON_START == PAD_START &&
               (int)SDL_GAMEPAD_BUTTON_DPAD_UP == PAD_DUP && (int)SDL_GAMEPAD_BUTTON_MISC1 == PAD_MISC1 &&
               (int)SDL_GAMEPAD_BUTTON_COUNT <= PAD_BUTTONS, "pad.h numbers buttons as SDL3");
_Static_assert((int)SDL_GAMEPAD_AXIS_LEFTX == PAD_LX && (int)SDL_GAMEPAD_AXIS_RIGHT_TRIGGER == PAD_RT &&
               (int)SDL_GAMEPAD_AXIS_COUNT == PAD_AXES, "pad.h numbers axes as SDL3");

static int gamepad_slot(SDL_JoystickID id)
{
    SDL_Gamepad *g = SDL_GetGamepadFromID(id);
    if (!g) return -1;
    return (int)(intptr_t)SDL_GetPointerProperty(SDL_GetGamepadProperties(g), "insane.slot", NULL) - 1;
}

static void gamepad_added(SDL_JoystickID id)
{
    if (gamepad_slot(id) >= 0)
        return;                                 /* attached at start-up already */
    if (getenv("SHOLD_PAD"))
        return;                                 /* a scripted test: its controller is the only one */
    SDL_Gamepad *g = SDL_OpenGamepad(id);
    if (!g) return;
    char guid[64];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof guid);
    const char *serial = SDL_GetGamepadSerial(g);
    char key[160];
    snprintf(key, sizeof key, "%s/%s", guid, serial ? serial : "");
    const char *name = SDL_GetGamepadName(g);
    int slot = pad_attach(key, name ? name : "Controller");
    LOG(LOG_INPUT, "controller \"%s\" connected as joystick %d", name ? name : "?", slot);
    if (slot >= 0 && slot < MAXPADS)
        pad_of_slot[slot] = g;
    SDL_SetPointerProperty(SDL_GetGamepadProperties(g), "insane.slot", (void *)(intptr_t)(slot + 1));
}

/* Scripted keys (SHOLD_KEYS) reach the window procedure too, like real ones. */
static void script_key(int dik, int down)
{
    int ext = dik >= 0x80;
    uint32_t scan = (dik & 0x7f) | (ext ? 0x100 : 0);
    uint32_t vk = scan_to_vk(scan);
    if (!vk)
        return;
    vk_state[vk] = down;
    uint32_t lparam = 1 | ((scan & 0xff) << 16) | (ext ? 1u << 24 : 0) | (down ? 0 : 0xc0000000u);
    post_message(down ? 0x0100 : 0x0101, vk, lparam);
}

void win_init(void)
{
    input_script_key_hook = script_key;
    if (headless)
        return;
    SDL_SetHint(SDL_HINT_APP_NAME, "Stranglehold");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    /* the window's app ID (Wayland) / class (X11) matches the applications-menu entry
       (stranglehold-native.desktop, make install), so the dock shows the game's icon */
    SDL_SetAppMetadata("Stranglehold", NULL, "stranglehold-native");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS | SDL_INIT_AUDIO))
        fatal("SDL_Init: %s", SDL_GetError());
    sdl_ok = 1;
    sdl_thread = pthread_self();
    input_set_rumble_handler(rumble);
    if (!strcasecmp(option_str("display", ""), "fullscreen"))
        start_fullscreen = 1;
    rumble_on = option_bool("rumble", 1);
    if (!deterministic)
        audio_open();
    /* controllers already connected: attached now, since the game looks for joysticks once,
       before its first frame (their "added" events then find them attached) */
    int n = 0;
    SDL_JoystickID *ids = deterministic ? NULL : SDL_GetGamepads(&n);   /* (test runs: scripted only) */
    for (int i = 0; ids && i < n; i++)
        gamepad_added(ids[i]);
    SDL_free(ids);
}

static void desktop_probe(void);

void host_window_create(const char *title, int w, int h)
{
    LOG(LOG_WIN, "host window \"%s\" %dx%d", title, w, h);
    if (!sdl_ok)
        return;
    if (window) {                       /* made early to find the screen (desktop_probe) */
        SDL_SetWindowTitle(window, title);
        return;
    }
    window = SDL_CreateWindow(title, w, h,
                              SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
        fatal("SDL_CreateWindow: %s", SDL_GetError());
}

void host_window_show(int visible)
{
    if (!window) return;
    if (visible && !hidden_window) SDL_ShowWindow(window);
    else SDL_HideWindow(window);
}

/* Window sizes are in points, the game's in pixels: they differ on a scaled screen (200 %) */
static int points(int px)
{
    float d = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return (int)lroundf(px / (d > 0 ? d : 1.0f));
}

void host_window_resize(int w, int h)
{
    if (window && !fullscreen && w > 0 && h > 0)
        SDL_SetWindowSize(window, points(w), points(h));
}

void host_window_title(const char *title)
{
    if (window) SDL_SetWindowTitle(window, title);
}

/* The screen the game plays on. On Wayland the compositor places a window (on the screen with
   the pointer, say) and a program can't ask where, or which screen is the main one: so the game's
   window is made and shown here, full screen (or maximized for display = window), and its size in
   pixels is the desktop size. The window is then the game's own (host_window_create keeps it). */
static int desk_w, desk_h;

static void desktop_probe(void)
{
    if (window && !fullscreen) {        /* already made, in a window: the first screen SDL lists */
        desk_w = 0;
        goto fallback;
    }
    if (window)
        goto measure;
    window = SDL_CreateWindow("Stranglehold", 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN |
                              SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!window)
        fatal("SDL_CreateWindow: %s", SDL_GetError());
    if (start_fullscreen) {
        SDL_SetWindowFullscreen(window, true);
        start_fullscreen = 0;
        fullscreen = 1;
    } else {
        SDL_MaximizeWindow(window);
    }
    if (!hidden_window)
        SDL_ShowWindow(window);
measure:
    SDL_SyncWindow(window);
    SDL_GetWindowSizeInPixels(window, &desk_w, &desk_h);
    LOG(LOG_WIN, "screen: %dx%d pixels (%s, density %.2f)", desk_w, desk_h,
        fullscreen ? "full screen" : "maximized", SDL_GetWindowPixelDensity(window));
fallback:
    if (desk_w < 320 || desk_h < 240) {             /* no answer: the first screen SDL lists */
        const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        desk_w = m ? (int)lroundf(m->w * m->pixel_density) : 1920;
        desk_h = m ? (int)lroundf(m->h * m->pixel_density) : 1080;
    }
}

void host_desktop_size(int *w, int *h)
{
    *w = 1920;
    *h = 1080;
    /* (test runs: always 1920 x 1080, whichever screen a window would open on, or none if hidden:
       the Options screen's "desktop" entry is built from it) */
    if (!sdl_ok || deterministic) return;
    if (!desk_w)
        desktop_probe();
    *w = desk_w;
    *h = desk_h;
}

/* the window's size in pixels now (0 x 0 without one) */
void host_window_pixels(int *w, int *h)
{
    *w = *h = 0;
    if (sdl_ok && window)
        SDL_GetWindowSizeInPixels(window, w, h);
}

int host_refresh_hz(void)
{
    if (!sdl_ok)
        return 60;
    SDL_DisplayID d = window ? SDL_GetDisplayForWindow(window) : 0;
    /* a full-screen window is on the screen of its size (Wayland does not say which screen a
       window is on, and SDL's guess can be another: desktop_probe) */
    if (window && fullscreen) {
        int pw, ph, n = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        SDL_DisplayID *ids = SDL_GetDisplays(&n), hit = 0;
        int hits = 0;
        for (int i = 0; ids && i < n; i++) {
            const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(ids[i]);
            if (dm && lroundf(dm->w * dm->pixel_density) == pw && lroundf(dm->h * dm->pixel_density) == ph) {
                hit = ids[i];
                hits++;
            }
        }
        if (hits == 1) d = hit;
        SDL_free(ids);
    }
    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(d ? d : SDL_GetPrimaryDisplay());
    return m && m->refresh_rate >= 20.0f ? (int)lroundf(m->refresh_rate) : 60;
}

void *host_sdl_window(void) { return window; }
int host_fullscreen(void) { return window ? fullscreen || start_fullscreen : start_fullscreen; }

/* Enhanced options changed in the game's menu: display (only once set) and rumble */
void win_options(void)
{
    rumble_on = option_bool("rumble", 1);
    const char *d = option_str("display", NULL);
    if (!window || !d || start_fullscreen) return;
    int want = !strcasecmp(d, "fullscreen");
    if (want != fullscreen) {
        fullscreen = want;
        SDL_SetWindowFullscreen(window, want);
    }
}

void host_display_mode(int w, int h)
{
    LOG(LOG_WIN, "display mode %dx%d", w, h);
    if (window && start_fullscreen) {
        start_fullscreen = 0;
        fullscreen = 1;
        SDL_SetWindowFullscreen(window, true);
        return;
    }
    if (!window || fullscreen || w <= 0 || h <= 0)
        return;
    int dw, dh;
    host_desktop_size(&dw, &dh);
    int k = 1;
    while ((k + 1) * w <= dw * 9 / 10 && (k + 1) * h <= dh * 9 / 10)
        k++;
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) {
        if (w == dw && h == dh)
            return;                     /* the desktop size: stays maximized */
        SDL_RestoreWindow(window);
    }
    SDL_SetWindowSize(window, points(k * w), points(k * h));
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void host_cursor_visible(int visible)
{
    if (!sdl_ok) return;
    if (visible) SDL_ShowCursor();
    else SDL_HideCursor();
}

void host_message_box(const char *caption, const char *text)
{
    if (sdl_ok)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, caption, text, window);
}

void host_show_bitmap(uint32_t dib, int x, int y, int w, int h)
{
    (void)dib; (void)x; (void)y; (void)w; (void)h;   /* the splash screen: shown by the renderer later */
}

int host_vk_down(int vk) { return vk >= 0 && vk < 256 && vk_state[vk]; }
int host_shift_down(void) { return vk_state[0x10]; }

/* ------------------------------------------------------------------ events */
static void key_event(const SDL_KeyboardEvent *k, int down)
{
    if (down && k->scancode == SDL_SCANCODE_RETURN && (k->mod & SDL_KMOD_ALT)) {
        fullscreen = !fullscreen;
        SDL_SetWindowFullscreen(window, fullscreen);
        option_set("display", fullscreen ? "fullscreen" : "window");
        options_save();
        return;
    }
    if (down && !k->repeat && k->scancode == SDL_SCANCODE_ESCAPE)
        video_skip_press();             /* (an intro video: skipped when the game next looks) */
    int dik = input_dik_from_hid(k->scancode);
    if (!dik)
        return;
    if (!k->repeat)
        input_key(dik, down);
    int ext = dik >= 0x80;
    uint32_t scan = (dik & 0x7f) | (ext ? 0x100 : 0);
    uint32_t vk = scan_to_vk(scan);
    if (!vk)
        return;
    uint32_t generic = vk;
    if (vk == 0xa1 || vk == 0x10) generic = 0x10;      /* VK_SHIFT */
    if (vk == 0xa3 || vk == 0x11) generic = 0x11;      /* VK_CONTROL */
    if (vk == 0xa5 || vk == 0x12) generic = 0x12;      /* VK_MENU */
    vk_state[generic] = down;
    vk_state[vk] = down;
    uint32_t lparam = 1 | ((scan & 0xff) << 16) | (ext ? 1u << 24 : 0);
    if (!down) lparam |= 0xc0000000u;
    else if (k->repeat) lparam |= 0x40000000u;
    int alt = (k->mod & SDL_KMOD_ALT) != 0;
    post_message(down ? (alt ? 0x0104 : 0x0100) : (alt ? 0x0105 : 0x0101), generic, lparam);
}

/* Host events are taken on the thread that started SDL only (the program's main thread, which
   pumps its window messages): the engine presents from its render thread, and two threads
   reading the Wayland display at once crashed in libwayland [verified: a run's backtrace]. */
void host_pump(void)
{
    if (!sdl_ok || !pthread_equal(pthread_self(), sdl_thread))
        return;
    if (window)
        SDL_SetWindowRelativeMouseMode(window, input_mouse_captured() != 0);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        /* test runs take only their scripted input: the desktop's (keys, mouse, focus, a real
           controller's stick noise) would change them from run to run */
        if (deterministic && e.type != SDL_EVENT_QUIT && e.type != SDL_EVENT_WINDOW_DISPLAY_CHANGED &&
            e.type != SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED)
            continue;
        switch (e.type) {
        case SDL_EVENT_QUIT:
            post_message(0x0010, 0, 0);                     /* WM_CLOSE */
            break;
        case SDL_EVENT_KEY_DOWN: key_event(&e.key, 1); break;
        case SDL_EVENT_KEY_UP: key_event(&e.key, 0); break;
        case SDL_EVENT_MOUSE_MOTION:
            input_mouse_move((int)e.motion.xrel, (int)e.motion.yrel);
            post_message(0x0200, 0, ((uint32_t)(int)e.motion.y << 16) | ((uint32_t)(int)e.motion.x & 0xffff));
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            int down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 :
                    e.button.button == SDL_BUTTON_MIDDLE ? 2 : e.button.button == SDL_BUTTON_X1 ? 3 : 4;
            input_mouse_button(b, down);
            static const uint32_t msgs[3][2] = {{0x0202, 0x0201}, {0x0205, 0x0204}, {0x0208, 0x0207}};
            if (b < 3)
                post_message(msgs[b][down], 0, ((uint32_t)(int)e.button.y << 16) | ((uint32_t)(int)e.button.x & 0xffff));
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            input_mouse_wheel((int)(e.wheel.y * 120));
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            input_focus(1);
            post_message(0x001c, 1, 0);                     /* WM_ACTIVATEAPP */
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            input_focus(0);
            memset(vk_state, 0, sizeof vk_state);
            post_message(0x001c, 0, 0);
            break;
        /* FRAME LIMIT "screen" reads the refresh of the window's screen again: on Wayland the window
           has none until the compositor first shows it on one (the primary screen's until then) */
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED: {
            frame_limit_options();
            extern void frame_rate_reapply(void);
            if (!deterministic) frame_rate_reapply();       /* (render_rate = screen) */
            break;
        }
        case SDL_EVENT_GAMEPAD_ADDED:
            gamepad_added(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED: {
            int slot = gamepad_slot(e.gdevice.which);
            if (slot >= 0) pad_detach(slot);
            if (slot >= 0 && slot < MAXPADS) pad_of_slot[slot] = NULL;
            SDL_Gamepad *g = SDL_GetGamepadFromID(e.gdevice.which);
            if (g) SDL_CloseGamepad(g);
            break;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            pad_axis(gamepad_slot(e.gaxis.which), e.gaxis.axis, e.gaxis.value);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            pad_button(gamepad_slot(e.gbutton.which), e.gbutton.button, e.gbutton.down);
            break;
        default:
            break;
        }
    }
}

void win_pump(void)
{
    host_pump();
}
