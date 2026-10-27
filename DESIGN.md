# Stranglehold native: design notes

A native Linux port of John Woo Presents Stranglehold (Midway, PC 2007) by static
recompilation, the method of larush-native and 1nsane-native (whose translator and Windows
stand-in this started from, copied 2026-10-26). Personal use first; built as "bring your own
game": the repository holds only our tools and runtime, and each user's own files are
translated and compiled on their machine.

## Decisions

### 1. Goal (Ant, 2026-10-26)
Run on Linux with Vulkan, with native controller support at parity with the Xbox 360
version's controls.

### 2. The game (GOG 1.1 v2, installer setup_stranglehold_1.1_v2_gog_(84418))
Unpacked with innoextract (never run) to ~/.local/share/stranglehold-native/game (13 GB).
- Binaries/Retail-Stranglehold.exe: linked 2007-11-13, base 0x400000, 9.5 MB of code
  (.text 0x401000-0xd1744a), a BINK section, no packer (GOG copy has no SecuROM) [verified:
  section entropy, imports readable]. Unreal Engine 3 (2007-era) with Midway's layers
  (MwyInput, MwyPhysics, "Woo" game classes). Imports: wxmsw262u, dbghelp, kernel32, user32,
  msvcp80, msvcr80, DFEngine, ws2_32, binkw32, wsock32, d3d9, d3dx9_28, dinput8, avifil32,
  msacm32, gdi32, advapi32, shell32, ole32, winmm.
- Sound: FMOD Ex linked into the exe, loading dsound.dll itself [verified: FMOD class names,
  fmod_output_dsound.cpp strings]. No XAudio/OpenAL.
- No XInput: the exe neither imports nor names XInput [verified: strings, imports]; DirectInput8
  for keyboard and mouse. Binaries/xinput9_1_0.dll ships but is unused.
- d3dx9_28: only D3DXMatrixInverse, D3DXCompileShader, D3DXDisassembleShader are imported.
- DFEngine.dll: Double Fusion in-game advertising (export CreateDFEngine; data in
  StrangleholdGame/PC-only/DynamicAdvertising).
- wxWidgets 2.6.2: the engine's application loop starts through wxEntry (279 imports, mostly
  base-class methods its wxApp/wxFrame subclasses' vtables point at).
- Binaries/dinput8.dll + patches/main.patch: GOG's 2019 patch loader. Its patches (to redo
  natively later, not by loading it): unlock all resolutions (a 640x480 check), a 16:9 HUD
  layout, FOV on F5/F6/F7 saved in Binaries/Settings.ini.

### 3. What is translated (builds/gog.py)
Five modules, each at its own preferred base, as one program: Retail-Stranglehold.exe
(0x400000), MSVCR80.dll (0x78130000), MSVCP80.dll (0x7c420000), wxmsw262u.dll (0x10000000),
binkw32.dll (0x18000000). Not translated: d3dx9_28 (its preferred base 0x400000 overlaps the
exe, and it brings msvcrt.dll; the runtime answers the three imports), DFEngine (stood in for),
GOG's dinput8.dll.

### 4. Controllers (Ant: parity with the Xbox controls)
The PC data still carries the console's control scheme [verified: strings in the cooked
packages]: ui_common/ui_ingame/ui_shell hold 360_ControlConfig, the Xbox button icons
(Generic_Elements-Xbox_RT, _LB, _Start, _Back ...), D-pad Tequila Bomb art; Startup_eng holds
MwyInput expressions on XBoxTypeS_* keys; the exe keeps aim friction (friction multipliers,
fade-out, factor at range), MwyInput dead-zone ops, Get/SetControllerVibration and Get/SetAimAssist
[verified: strings]. The PC's own controller configs are separate objects (dPCControllerConfigs,
m_pcKeyboardConfig). Plan [guess until read in the code]: SDL3 gamepads feed XboxTypeS_* keys and
axes into the engine where the Windows client sends keyboard/mouse input, the console controller
configs and Xbox prompts are chosen while a pad is in use, and rumble goes to SDL.
Input path found so far [verified: disassembly]: the input keys are FName globals built at
start-up from their wide names (0x7a11c0 and on: MouseX at 0x10ae0f0, MouseY 0x10ae0f8,
XboxTypeS_A at 0x10ae528; each FName is 8 bytes). The viewport's input step (0x8f4a60, called
from 0x6ef336; esi = the viewport, [esi+0x38] its ViewportClient, esi+4 the FViewport passed
on) sends mouse motion as ViewportClient vtable +0x28 (InputAxis: viewport, controller id -1,
key FName (2 dwords), delta, delta time) [guess: names from UE3 of the time]; +0x24 is likely
InputKey. Nothing in the exe reads the XboxTypeS FNames by address, so a pad's keys would be
sent the same way from the runtime. Open: which controller id the local player's PlayerInput
answers to (mouse uses -1).
The Xbox 360 version (Ant's copy, if he dumps it) is the reference to check parity against:
its Coalesced ini and MwyInput packages, and its executable through REA for any console-only
code the PC build left out.

### 5. Graphics
Direct3D 9 through DXVK 3.1.1 native (SDL3 window system), as larush-native: the program's COM
objects are ours, calling DXVK's by vtable index. third_party/dxvk-native and third_party/sdl3
were copied from larush-native's builds.

### 6. The engine's main loop (runtime/game.c)
The shipped game drops its command-line switches before WinMain checks -nowxwindows (ParseParam
sees an empty appCmdLine [verified: gdb on the translated ParseParam 0x6f3000]), so it always
starts wxWidgets. The runtime's wxEntry instead runs the -nowxwindows branch: GEngineLoop
(0x10619a4) Init 0x407e70, Tick 0x4054e0 until GIsRequestingExit 0x107147c [verified:
disassembly of 0x401ae2-0x401b93]. SetUnrealCallbacks is a no-op. Imported wx variables
(wxDefaultSize, wxAppConsole::ms_appInitFn) get blocks of program memory (main.c).

## Progress
- 2026-10-26: project set up; game unpacked; builds/gog.py; first translator run (74,050
  functions, ~100 s discovery, ~6 GB RAM, ~9-10 min to write 9.2 M lines in 301 files; a full
  build at JOBS=6 ~25 min, 700 MB binary with debug info).
- Translator: rdtsc (the runtime clock at QueryPerformanceCounter's 10 MHz); "repz ret" and
  other prefixed control transfers decode as the plain instruction (MSVC 2005's
  __security_check_cookie was taken for a no-return function); recomp.py writes only files whose
  contents change (gen/.stamp marks the run), so a change rebuilds only the files it touches -
  but a new or removed function changes funcs.h and rebuilds everything.
- Runtime: memory pool 0x01300000-0x74000000 (above the 0x400000-0x12c2000 image, below the
  stand-in system DLLs); ~185 missing imports added (Unicode W versions, Winsock that finds no
  network, OLEAUT32 BSTRs, console, affinity, dbghelp/avifil32/msacm32 failing cleanly);
  RaiseException 0x406d1388 (thread names) continues; DirectInput8 W interfaces accepted;
  engine message boxes (DialogBoxIndirectParamW) logged with their text.
- First runs: all DLLs start, the engine reads Coalesced.ini and the localization, lists the
  980 packages, creates the DirectInput mouse and the D3D9 device on DXVK. Harmless: "Failed to
  bind to port 9989 for listening for propagation messages" (no network). Stopped at 0xb89650
  (a function discovery missed: builds/gog.py EXTRA_ENTRIES says why).
