/* Game controllers: SDL3 gamepads (win.c) and SHOLD_PAD's scripted one, kept as plain state
   (pad.c); game.c turns that state into the engine's own gamepad input (XboxTypeS keys). */
#pragma once
#include <stdint.h>

/* Buttons and axes, numbered as SDL3's SDL_GamepadButton and SDL_GamepadAxis. */
enum { PAD_A, PAD_B, PAD_X, PAD_Y, PAD_BACK, PAD_GUIDE, PAD_START, PAD_LSTICK, PAD_RSTICK, PAD_LB,
       PAD_RB, PAD_DUP, PAD_DDOWN, PAD_DLEFT, PAD_DRIGHT, PAD_MISC1, PAD_BUTTONS = 26 };
enum { PAD_LX, PAD_LY, PAD_RX, PAD_RY, PAD_LT, PAD_RT, PAD_AXES };
#define PAD_SLOTS 4

int pad_attach(const char *id, const char *name);   /* returns the slot, -1 if none free */
void pad_detach(int slot);
void pad_axis(int slot, int axis, int value);        /* sticks -32768..32767 (+ = right/down), triggers 0..32767 */
void pad_button(int slot, int button, int down);
int pad_count(void);                                  /* controllers connected */

/* A copy of a slot's state; 0 if nothing is connected there. */
int pad_read(int slot, uint8_t button[PAD_BUTTONS], int axis[PAD_AXES]);

/* The device used last: 1 a controller, 0 the keyboard or mouse (dinput.c reports those). */
void input_used(int controller);
int input_controller_last(void);

/* SHOLD_PAD: a scripted controller for tests, connected at start-up, fed through the same
   calls as SDL's gamepad events: "frame:BUTTON*hold,frame:AXIS=value*hold,...", e.g.
   "900:A*10,1600:LX=-32768*60,1700:RT=32767*300". Buttons: A B X Y BACK GUIDE START LS RS LB
   RB UP DOWN LEFT RIGHT (the D-pad); axes: LX LY RX RY LT RT (back to rest after the hold).
   "frame:PLUG", "frame:UNPLUG": if the first of these plugs one in, there is no controller at
   start-up. */
void pad_init(void);
void pad_frame(unsigned long frame);                 /* per presented frame, from input_frame */

/* game.c */
void padgame_frame(unsigned long frame);
