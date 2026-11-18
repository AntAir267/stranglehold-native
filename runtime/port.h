/* port.h: what this port tells the AntiStatic runtime about Stranglehold (GOG 1.1 v2). Every
   value describes the game as the program sees it under Windows, or names the port on the host.
   None of it is the game's code or data. */
#pragma once

#define PORT_TITLE "Stranglehold"
#define PORT_SLUG "stranglehold-native"
/* SHOLD_PAD, SHOLD_KEYS, SHOLD_SHOT, ...: the names this port's tests have always used */
#define PORT_ENV "SHOLD"

/* The game's folder as the program sees it; the program starts in its Binaries folder and finds
   everything else as ..\StrangleholdGame. Its command line asks for the engine's own loop instead
   of wxWidgets' (WinMain 0x401ae2 reads -nowxwindows [verified: disassembly]). */
#define PORT_VROOT "C:\\Stranglehold"
#define PORT_START_DIR "Binaries"
#define PORT_EXE "Retail-Stranglehold.exe"
#define PORT_CMDLINE_ARGS "-nowxwindows"
#define PORT_VOLUME_LABEL "STRANGLEHOLD"

/* above Retail-Stranglehold.exe (0x400000-0x12c2000) */
#define PORT_ARENA_START 0x01300000u

/* Direct3D 9 through the toolkit's DXVK bridge (runtime/d3d9): a 4:3 back buffer on a 16:10
   window shown centred with black bars, not stretched (DESIGN.md, 2026-10-27); a DYNAMIC or
   SYSTEMMEM texture's lock gives the same program copy each time (Bink decodes into its earlier
   frames: the movies, 2026-10-26) */
#define PORT_D3D9_KEEP_ASPECT 1
#define PORT_D3D9_KEEP_LOCK_COPIES 1
