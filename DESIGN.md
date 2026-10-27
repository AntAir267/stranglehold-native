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
Xbox 360 disc (Ant's ISO, unpacked 2026-10-26 with extract-xiso to
~/.local/share/stranglehold-native/xbox360: default.xex, CookedXenon/ in level folders, Config/Xenon,
Localization/Xenon) [verified]. tools/coalesced.py unpacks a Coalesced file (count = strings, a name
and a text per file; lengths little- or big-endian). The final input ini differs where it matters
[verified: work/ini_pc vs work/ini_360, PC-/Xe-StrangleholdInput.ini]: MwyInput's
InitialControlConfigs is PCControllerConfigs.PCConfig on PC, ControllerConfigs.XBoxConfig on 360,
which also lists XBoxConfigAlt1-3 (the selectable layouts) and XBoxConfig_GamersDay07; the rest of
the list (UIStartButton0-3, UISelectButton0-3, ScreenEngineConfig0-3, CinemaCamera, CommonActions,
FreeCamera, GuitarCase, MoviePlayer, HelicopterTurret, MexicanStandoff) has the same names under
each package. The PC's Startup_eng package holds both packages' objects: ControllerConfigs with
XBoxConfig, XBoxConfigAlt1-3, XBoxConfigX06, XBoxConfig_GamersDay07 beside PCControllerConfigs/PCConfig
[verified: tools/upkdecomp.py (LZO1X blocks through the system liblzo2), names in the decompressed
package]. So pad parity = select ControllerConfigs.* while a pad is in use; still to check that
the PC copies of those objects equal the 360's (360 packages are XMem LZX-compressed; the system libmspack does not export its LZX decoder, so that needs our own decoder; open, not urgent: PC cooked Sep 2007 from the same code, 360 May-Jul 2007 [verified: the ini timestamp lists]).
The Xbox 360 version (Ant's copy) is the reference to check parity against:
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

### 7. Reading tools (Ant, 2026-10-26: UE Viewer and an UnrealScript decompiler)
All local to the project, never committed (third_party/ is ignored; only our own scripts and
tools/uedump's source are in the repository).
- tools/upkdecomp.py: a fully compressed package (.xxx with .uncompressed_size) to a plain .upk.
  PC and Xbox 360 blocks are both LZO1X (the system liblzo2 through ctypes); the 360's headers
  are big-endian, and its 830 level packages are stored uncompressed [verified: all 1491 blocks of
  the 48 compressed 360 packages decode to their listed sizes]. No LZX anywhere.
- tools/coalesced.py: a Coalesced.ini / Coalesced.<lang> to its .ini files.
- tools/upkparse.py: package header, names, imports, exports (both byte orders). Midway adds
  "WOO " + build + a second number after the header size: PC build 530/35, 360 523/34.
- UELib (UE Explorer's library, MIT): third_party/UELib, release 1.12.0 (master needs the
  .NET 10 compiler), built on the .NET 8 SDK 8.0.427 in third_party/dotnet (Microsoft's
  linux-x64 archive, sha512 checked; no system install) with -p:LangVersion=12
  -p:TargetFrameworks=net8.0. It knows Stranglehold (package 375/25). tools/uelib.sh runs its
  CLI ("obj list", "obj decompile <Group.Object>"; give it </dev/null or it waits for more).
- tools/uedump (ours, C# on UELib; build: third_party/dotnet/dotnet build tools/uedump/uedump.csproj
  -c Release -p:LangVersion=12 -p:TargetFrameworks=net8.0, output in work/uedump):
  tools/uedump.sh <script package folder> <package.upk> <out> <path prefix>... | --classes.
  It reads every script package first to learn each array property's element type (UELib alone
  only knows types of classes in the same package), then writes one file per object (properties)
  or per class (UnrealScript). Script packages decompressed in work/upk_pc, work/upk_360.
- UE Viewer (umodel), official Linux build of 2022-01-06 (539 KB, gildor.org) in
  third_party/umodel: it is 32-bit and needs libpng12.so.0 (i386), which this system lacks, so it
  does not start yet [verified: ldd]. Not needed for the work so far (UELib covers objects and
  script); options if wanted later (each needs Ant's yes): build UE Viewer from its source, or a
  32-bit libpng12 beside it.

Findings (2026-10-26):
- Controller configs: all 3364 objects of package ControllerConfigs present on both (XBoxConfig,
  XBoxConfigAlt1-3, Targeting, Movement, XBoxActions, VehicleActions, the UI/screen configs and
  every MwyInput op under them) decompile to identical text on PC and 360; the PC only adds
  CinemaCameraConfigPS3Japan (16 objects) [verified: work/ctrl_pc vs work/ctrl_360].
  XBoxConfig = Includes CommonMacros, KeyboardActions, Movement, Targeting, XBoxActions,
  KeyboardMovement, CommonActions, DevelopmentActions, VehicleActions.
- Scripts: 2145 classes decompiled per version (work/script_pc, work/script_360); 76 differ
  (work/script_diff_sizes.txt), mostly defaults. Control-related differences, all data, no logic:
  MwyInputHandler (its config lists: PCControllerConfigs.* vs ControllerConfigs.* as in the ini);
  WooControllerTokenManager (prompt names PC*ButtonName vs XBox*ButtonName, and the PC's extra
  %CROUCH%, %MOVEMENT2%, %ENTER% tokens); WooProfileSettings (PC version 27 adds settings 700-722:
  resolution and key bindings; setting 116 "Auto Aim", levels 0-3, defaults to 0 on PC and 2 on
  360); WooEngineInfo.ControllerLayouts[10] and [14] (PC pictures 17 and 16, 360 1 and 0);
  Engine.PlayerInput's keyboard bindings.
- Pad parity therefore needs [guess until built]: ControllerConfigs.XBoxConfig (or the chosen
  Alt layout) while a pad is in use, the XBox* prompt names, Auto Aim 2 as the pad default,
  and XboxTypeS key events from SDL3.
- Files: the 360 has nothing newer. All 864 packages both versions share are build 530 on PC and
  523 on 360 [verified: work/woo_builds.py]. Only on 360: 11 Tai O streaming chunks (the PC splits
  that level differently, 13 of its own), ui_*xenon packages, Xbox360TOC.txt. Only on PC: the ad
  data, Russian, more level/cinema chunks, the PC UI packages [verified: work/files_only_*.txt].
- Still to dump: the controller-layout screen (in the ui_* screen packages, not script) and the
  native aim friction (exe strings: friction multipliers, WooTargetingSystem in script).

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
- 2026-10-26: FIRST SCREEN. With 0xb89650 added the engine runs 90 s without a fault (2717 frames
  headless), and a hidden-window run (--hidden, SHOLD_SHOT=frames) saves "Midway Presents" at
  frame 1500 (1024x768, X8R8G8B8, presented from the render thread, thread 6) [verified: the PNG].
  Earlier frames are black (the fade-in). Two D3DXCompileShader(main, ps_2_0) calls at start-up
  are refused (the shader cache missed two shaders: which ones is open). The Double Fusion
  module's own Engine.Log (user folder drive_c/.../Midway/Stranglehold) shows it found no network
  (WSA 10050) [verified]. Shipping UE3 writes no Launch.log.
- 2026-10-26: TITLE SCREEN. A 200 s hidden-window run reaches "JOHN WOO PRESENTS STRANGLEHOLD /
  Press ENTER" by frame 3600 [verified: work/testuser/shot_3600.png]. ~19 frames a second over the
  run (3810 frames in 200 s, intro movies included): speed not looked at yet. Next: input into the
  window (keyboard, then the pad path of section 4), the two missed shaders, frame rate.
- 2026-10-26: KEYBOARD. The engine never pumps window messages itself (wx did, between its idle
  ticks): runtime/user32.c pump_window_messages runs before each tick in the wxEntry loop (WM_QUIT
  ends it). Host (SDL) events are taken only on the thread that started SDL: the render thread
  presents here, and two threads in libwayland crashed. Scripted Enter on the title opens the main
  menu (3D tea-house scene behind it) and Down moves its highlight [verified: shot_3750,
  shot_4100]. During intro movies the game thread sits in render-fence waits (0x6e40d0 -> Sleep),
  so a tick there can take ~13 s and keys wait for it. Frame rate: 20 fps exactly in a hidden
  window, 30 headless, while the engine ticks 40/s: the hidden window's presentation is held back
  by the compositor, not the game [guess: check in a visible window]. --test (deterministic
  turns) deadlocks with UE3's threads: real-time --hidden runs are used instead.
- 2026-10-26: CONTROLLER (core). runtime/pad.c rewritten for this game (state only: SDL gamepads,
  SHOLD_PAD's scripted pad, no dead zone - the configs apply their own); game.c pad_tick, before
  each engine tick on the game thread, sends button changes to the first viewport's client as
  InputKey (controller 0, the XboxTypeS FName globals 0x10ae4e8 on) and the four stick axes as
  InputAxis every tick (Y negated: up positive), triggers as buttons past XInput's 30/255.
  Findings [verified: disassembly + runs]: the client's InputKey (0x661f70) also feeds the
  keyboard (-3) and mouse (-1) straight to the screen engine, then the base (0x4d5b60) gives every
  key to the MwyInput handler (0x4f41b0, queued with its controller id) and to the engine; the
  game finds control configs by SHORT NAME, first loaded wins (the exe pushes "PCConfig" at
  0x6a7240, "UIStartButton%d"/"UISelectButton%d"/"ScreenEngineConfig%d" at 0x6a62c0 on), and an
  object's Includes resolve by short name too. UStrangleholdEngineInfo's vtable +0xf0 returns 1
  (a hard "PC" answer; 0x5dab63 picks PC vs Xbox CinemaCameraConfig by it) - not changed.
  Tried and dropped: the 360's MwyInputHandler list in a Coalesced copy (the loader crashes at
  0x4a31d0 without the PC entries); the union with the Xbox entries first (pad works, keyboard
  no longer passes the title); Xbox twins in Includes under their own names (resolved back to the
  PC objects: no effect).
  Done: tools/upkpatch.py writes the user's Startup_<lang> packages into the user folder with
  the 360 twins renamed (name number 100, "UI0_99") and included by the PC configs: PCConfig +
  Movement, Targeting, XBoxActions, VehicleActions; UIStartButton0-3, UISelectButton0-3, UI0-3,
  CinemaCamera, MoviePlayer, FreeCamera, GuitarCase, HelicopterTurret, MexicanStandoff each + its
  twin. Changed objects are appended, only their export entries rewritten, then LZO1X-1 repacked
  (make pad-data, part of make all; USERDIR = ~/.local/share/stranglehold-native). Result
  [verified: shots]: the pad's Start opens the main menu, its D-pad moves the highlight, and the
  keyboard still works beside it (Enter on the title, Down in the menu).
  Next: gameplay with the pad (sticks, triggers, aim), Xbox prompts while a pad is in use
  (WooControllerTokenManager ButtonMappings), Auto Aim 2 as the pad default, the layout choice
  (XBoxConfigAlt1-3; the profile load always loads PCConfig), rumble.

- 2026-10-26: MOVIES. The two D3DXCompileShader calls are Bink's YUV -> RGB pixel shaders (RAD's
  HLSL, logged: 3 planes, and 4 with alpha; registers fixed, c0-c4 set by the caller): the runtime
  returns our ps_2_0 equivalents (data/bink/*.psh, assembled into runtime/bink_shaders.h) in an
  ID3DXBuffer. Block corruption: Bink's frame buffers are SYSTEMMEM L8 textures (Y 1280x720, Cr/Cb
  640x360, two sets) it decodes into between locks, redrawing only changed blocks; the bridge gave
  a fresh copy per lock. DYNAMIC and SYSTEMMEM textures now keep one copy per level between locks
  (dropped on release) [verified: the market intro plays clean, shot_1200]. Test aid: a user folder
  whose Coalesced.ini starts at Map=market_global_persistent (work/testplay) reaches the first
  level's intro in ~10 s instead of ~3 min of title movies.
