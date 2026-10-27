/* Controllers (pad.h): the state SDL's gamepad events (win.c) and SHOLD_PAD write, which game.c
   reads on the game thread and sends to the engine as its own gamepad keys. No dead zone here:
   the game's control configs apply their own (MwyInputDeadZoneOp), as on the Xbox 360. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "input.h"
#include "pad.h"

typedef struct {
    int connected;
    char id[160];
    uint8_t button[PAD_BUTTONS];
    int axis[PAD_AXES];
} Pad;

static pthread_mutex_t pad_lock = PTHREAD_MUTEX_INITIALIZER;
static Pad pads[PAD_SLOTS];

static int last_controller;             /* the device used last was a controller */
void input_used(int controller) { last_controller = controller != 0; }
int input_controller_last(void) { return last_controller; }

/* Plugging a controller in counts as using it; unplugging the last one hands back to the
   keyboard. A controller seen before gets its old slot back when it is free. */
int pad_attach(const char *id, const char *name)
{
    int slot = -1;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < PAD_SLOTS && slot < 0; s++)
        if (!pads[s].connected && !strcmp(pads[s].id, id))
            slot = s;
    for (int s = 0; s < PAD_SLOTS && slot < 0; s++)
        if (!pads[s].connected)
            slot = s;
    if (slot >= 0) {
        memset(&pads[slot], 0, sizeof pads[slot]);
        pads[slot].connected = 1;
        snprintf(pads[slot].id, sizeof pads[slot].id, "%s", id);
    }
    pthread_mutex_unlock(&pad_lock);
    if (slot >= 0) {
        LOG(LOG_INPUT, "controller \"%s\" in slot %d", name, slot);
        input_used(1);
    }
    return slot;
}

void pad_detach(int slot)
{
    if (slot < 0 || slot >= PAD_SLOTS)
        return;
    pthread_mutex_lock(&pad_lock);
    pads[slot].connected = 0;
    memset(pads[slot].button, 0, sizeof pads[slot].button);
    memset(pads[slot].axis, 0, sizeof pads[slot].axis);
    pthread_mutex_unlock(&pad_lock);
    if (!pad_count())
        input_used(0);
}

void pad_axis(int slot, int axis, int value)
{
    if (slot < 0 || slot >= PAD_SLOTS || axis < 0 || axis >= PAD_AXES)
        return;
    pthread_mutex_lock(&pad_lock);
    pads[slot].axis[axis] = value;
    pthread_mutex_unlock(&pad_lock);
    if (abs(value) > 12000)
        input_used(1);
}

void pad_button(int slot, int button, int down)
{
    if (slot < 0 || slot >= PAD_SLOTS || button < 0 || button >= PAD_BUTTONS)
        return;
    pthread_mutex_lock(&pad_lock);
    pads[slot].button[button] = down != 0;
    pthread_mutex_unlock(&pad_lock);
    if (down)
        input_used(1);
}

int pad_read(int slot, uint8_t button[PAD_BUTTONS], int axis[PAD_AXES])
{
    if (slot < 0 || slot >= PAD_SLOTS)
        return 0;
    pthread_mutex_lock(&pad_lock);
    int c = pads[slot].connected;
    memcpy(button, pads[slot].button, PAD_BUTTONS);
    memcpy(axis, pads[slot].axis, sizeof pads[slot].axis);
    pthread_mutex_unlock(&pad_lock);
    return c;
}

int pad_count(void)
{
    int n = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < PAD_SLOTS; s++)
        n += pads[s].connected;
    pthread_mutex_unlock(&pad_lock);
    return n;
}

/* ------------------------------------------------------------------ SHOLD_PAD */
/* axis: 0 a button, 1 an axis, 2 plugging (index 0 unplug, 1 plug) */
typedef struct { unsigned long at, release; int axis, index, value, hold, state; } PadPress;
static PadPress *script;
static int nscript, script_slot = -1;

static const struct { const char *name; int button; } button_names[] = {
    {"A", PAD_A}, {"B", PAD_B}, {"X", PAD_X}, {"Y", PAD_Y}, {"BACK", PAD_BACK}, {"VIEW", PAD_BACK},
    {"GUIDE", PAD_GUIDE}, {"START", PAD_START}, {"MENU", PAD_START}, {"LS", PAD_LSTICK},
    {"RS", PAD_RSTICK}, {"LB", PAD_LB}, {"RB", PAD_RB}, {"UP", PAD_DUP}, {"DOWN", PAD_DDOWN},
    {"LEFT", PAD_DLEFT}, {"RIGHT", PAD_DRIGHT},
};
static const char *const axis_names[PAD_AXES] = {"LX", "LY", "RX", "RY", "LT", "RT"};

void pad_init(void)
{
    const char *env = getenv("SHOLD_PAD");
    if (!env || !*env)
        return;
    char *copy = strdup(env), *save = NULL;
    int first_plug = -1;
    for (char *tok = strtok_r(copy, ", \t\n", &save); tok; tok = strtok_r(NULL, ", \t\n", &save)) {
        char *colon = strchr(tok, ':'), *end;
        unsigned long at = colon ? strtoul(tok, &end, 10) : 0;
        if (!colon || end != colon) {
            LOG(LOG_ALWAYS, "SHOLD_PAD: \"%s\" is not frame:BUTTON or frame:AXIS=value", tok);
            continue;
        }
        char *name = colon + 1, *star = strchr(name, '*'), *eq = strchr(name, '=');
        int hold = 3;
        if (star) {
            *star = 0;
            hold = atoi(star + 1) > 0 ? atoi(star + 1) : 1;
        }
        PadPress p = {at, 0, 0, -1, 1, hold, 0};
        if (!eq && (!strcasecmp(name, "PLUG") || !strcasecmp(name, "UNPLUG"))) {
            p.axis = 2;
            p.index = !strcasecmp(name, "PLUG");
            if (first_plug < 0) first_plug = p.index;
        } else if (eq) {
            *eq = 0;
            p.axis = 1;
            p.value = atoi(eq + 1);
            for (int a = 0; a < PAD_AXES; a++)
                if (!strcasecmp(name, axis_names[a]))
                    p.index = a;
        } else {
            for (size_t k = 0; k < sizeof button_names / sizeof *button_names; k++)
                if (!strcasecmp(name, button_names[k].name))
                    p.index = button_names[k].button;
        }
        if (p.index < 0) {
            LOG(LOG_ALWAYS, "SHOLD_PAD: unknown %s \"%s\"", eq ? "axis" : "button", name);
            continue;
        }
        script = realloc(script, (nscript + 1) * sizeof *script);
        script[nscript++] = p;
    }
    free(copy);
    if (first_plug != 1)
        script_slot = pad_attach("shold-pad-script", "Scripted controller");
    LOG(LOG_ALWAYS, "SHOLD_PAD: %d scripted inputs, controller %s", nscript,
        script_slot >= 0 ? "connected" : "not connected yet");
}

void pad_frame(unsigned long frame)
{
    for (int k = 0; k < nscript; k++) {
        PadPress *s = &script[k];
        int down = -1;
        if (s->state == 0 && frame >= s->at) {
            s->state = 1;
            s->release = frame + (unsigned long)s->hold;
            down = 1;
        } else if (s->state == 1 && frame >= s->release) {
            s->state = 2;
            down = 0;
        }
        if (down < 0)
            continue;
        if (s->axis == 2) {
            if (!down) continue;
            if (s->index && script_slot < 0)
                script_slot = pad_attach("shold-pad-script", "Scripted controller");
            else if (!s->index && script_slot >= 0) {
                pad_detach(script_slot);
                script_slot = -1;
            }
            LOG(LOG_ALWAYS, "SHOLD_PAD: frame %lu: %s", frame, s->index ? "plugged in" : "unplugged");
        } else if (s->axis) {
            LOG(LOG_ALWAYS, "SHOLD_PAD: frame %lu: %s = %d", frame, axis_names[s->index], down ? s->value : 0);
            pad_axis(script_slot, s->index, down ? s->value : 0);
        } else {
            const char *n = "?";
            for (size_t b = 0; b < sizeof button_names / sizeof *button_names; b++)
                if (button_names[b].button == s->index) {
                    n = button_names[b].name;
                    break;
                }
            LOG(LOG_ALWAYS, "SHOLD_PAD: frame %lu: %s %s", frame, n, down ? "down" : "up");
            pad_button(script_slot, s->index, down);
        }
    }
    padgame_frame(frame);
}
