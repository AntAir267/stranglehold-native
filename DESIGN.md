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

### 8. Menus: the WooScreenSet format (2026-10-27; Ant: "decode the midway binary screen format!")

Tool: tools/screenset.py (dump / roundtrip / diff / rows / patch). Dumps (JSON + text tree) in
work/screenset/<package>.json/.txt for PC ui_shell, ui_common, ui_ingame, ui_unlockshop and the
360's ui_shellxenon, ui_ingamexenon, ui_unlockshopxenon, ui_commonxenon (inside the 360
Startup_eng). PC-vs-360 structural diffs: work/screenset/diff_{shell,ingame,unlockshop,common}.txt.

Format [verified: the exe's load-time fix-up code 0x9b7dc0 (SSET), 0x9b7ab0 (SDAT), 0x9b7730
(nodes), 0x9b76e0/0x9b7650 (handlers, arguments), 0x9b79d0/0x9b78d0 (animations); round trip of
all 8 screen sets byte-identical; every handler entry of every screen reachable through the tree
model below (0 unreached)]:
- Export = properties ReferencedTextures (Texture2D list) and RawData (bytes). RawData = SSET:
  screen names, an offset table, then one SDAT block per screen. All pointers are byte offsets
  (from SSET resp. SDAT) that the loader turns into addresses; byte order = package order.
- SDAT: +0xc root node, +0x10 animation table, +0x14 string table (count, offsets). Every name,
  image, localization key, action string and expression is an index into it.
- Nodes: tag ('OBJ ', 'GROP', 'POLY', 'TEXT'), name, flags (bit 0 = visible; PC node flags also
  carry 0x8000). OBJ/GROP (0x20): handler list, transform block (20 floats: scale [3..5],
  position [6..8], colour [12..15] [guess for the rest]), child list, X block = native widget
  (4CC + arguments: LIST = option row (list-names var, value var, ..., value TEXT), SLDP = slider,
  SPSH = the 360 controller-layout picker, TEXT, CNTR). POLY (0x74): 4 vertices, 4 colours,
  4 UVs, image name. TEXT (0x34): text key, font, size/box, colour, alignment.
- Handler = one event's script: event id, entry count, top-level count; entries {op, first
  child, child count, argument list}. If/Else/IfVar run their child range (0x9b3690); an If
  that ran disables the following Else/ElseIf.
- Events [verified: signal table 0x6a7577..., 0x9b5a90]: 1006/1007/1008/1009 up/down/left/right,
  1010 A, 1014 + 1035 B, 1015 X, 1011 Y, 1013 Start, 1012 Back, 1017/1019 LT/RT, 2090/2091
  LB/RB; 1031 init and 1004/1005 focus in/out [guess from content]. Footer objects carry a
  "button" event 2102 (B), 2105 (A), 2110 (Start), 2112 (X), 2113 (Y), 2109 (Back), 2114/2115
  (LB/RB), 2116/2117 (LT/RT): a mouse click on the object is turned into that button's event
  (table 0x9b5bb8; dispatch 0x9b58f0).
- Actions: engine ops 1000-1092 (factory 0x9b8ea0, classes from RTTI: 1003 OpenScreen,
  1002 Visible, 1014 SetFocus, 1039 If / 1040 ElseIf (integer expression), 1035/1036 float If,
  1065 Else, 1021/1022 anim ...); game ops (FWooScreenClient 0x6aba80, FWooGameVars 0x6d2100):
  11000 SetVar(var, value), 11001 IfVar(var, cmp 0..5 = == != > >= < <=, value),
  11017 IfConfigBool(section, key, ini), 3004 Command string ('causeevent ...'), 5002 sound,
  11045 profile -> vars, 11041 vars -> profile, 11085 revert resolution.
- Expressions (If args; evaluator 0x9c1c30/0x9c0ce0): first byte 01vsssss = target slot
  (0 = evaluate only), then operands/operators: 01... slot, 10... operator (EXPR_OPS),
  11... literal (c0 u8, c1 u16, c2 i32, c3 float, big-endian). Literal payloads may be 0: the
  cooker stores '@\xc0' 0 as 40 c0 00 00.
- Screen vars: 0x6a5250 GetVar -> engine store 0x9b6990 (declared id ranges from a 'NAM' blob)
  or a provider by id range (FWooGameVars, FWooScreenVars). An undeclared id reads 0 and
  SetVar to it is dropped [verified: 0x9b68b0/0x9b6a40].

Findings:
- (a) Control Config [verified]: the cooker folded the platform test into literals. In every
  copy of the Settings menu (m_Settings and the copies in m_AudioSettings, m_controllerSetup,
  m_MultiplayerSettings) the Control Config accept handler is If(Xbox) -> m_controllerSetup,
  If(PS3) -> m_controllerSetup, If(PC) -> m_pcKeyboardConfig; PC data has 0,0,1, the 360 1,0,0.
  Same pattern for Audio (m_AudioSettings / m_pcAudioOptions) and Video. No runtime condition.
- (b) Button images on PC [verified]: two things hide them. (1) FWooScreenPoly draw (0x6dc770,
  vtable 0xdff8c4 slot 3) refuses to draw any POLY whose image _stricmp-matches one of 13 names
  (Generic_Elements-Nav_A/X/B/Y, _HudElements-SB_IconBackingFill, Generic_Elements-Xbox_Back/
  LA/LB/LT/RA/RB/RT/Start; strings 0xdff75c-0xdff8a0, compares 0x6dc8a3-0x6dc9d8, drawing at
  0x6dc9da). No platform query: unconditional. (2) The PC data hides the A ("Select") footer
  objects (flags 2 instead of 3, Nav_A POLY flags 0) in m_chapterSelect, m_MainMenu (also its
  B footer), m_Settings, m_extras. The footer scripts themselves are platform-neutral:
  If(PS3) {var 9218 picks PS3 X/O} Else {Nav_A/Nav_B visible}, same on both. The PC-only
  screens m_pcKeyboardConfig/VideoOptions/AudioOptions have Back (Nav_B), Reset (Nav_Y) and a
  text-only Apply (2110 = Start). Localized: Module_Accept "Select", module_Back "Back".
- (c) Video Apply [verified: data; guess: the effect]: footers are not focusable (no
  1004/1006-1009 handlers; the last row Decals' Down sets focus to itself). Apply is the
  Start-button alias (event 2110): clicking it = pressing Start = keyboard Enter; the focused
  row's Start handler runs op 11086, which no C++ switch handles by constant (0x6aba80's table
  sends it to the shared exit) [guess: settings apply as each LIST changes, through the vars;
  check in a run: pad Start on m_pcVideoOptions should do what Enter does].
- (d) Auto Aim [verified]: the in-game pause menu (ui_ingame ig_PauseMenu, both versions) has
  an options page with rows Invert Up/Down, Invert Left/Right, Vertical/Horizontal Speed, Music,
  SFX, Gamma, Lookspring, Auto Tequila Time, Aim Assist, Controller Vibration, TV Type, View
  Tips. Aim Assist = OBJ "Toggle_AimAssist" (0x11edc), LIST(1, #9074 level names, #9144 value):
  op 11045 copies the profile to vars, 11041 back; var 9144 <-> script GetAutoAim/SetAutoAim
  (0x563760/0x563d10, FNames at 0x565052/0x565426). On PC the Vibration row is hidden (flags
  0x8000; 360 3) but still in the up/down chain. The front end has no such rows: the 360's
  m_controllerSetup holds only the layout picker (SPSH widget, vars #9029/#9093). So Auto Aim is
  already an in-menu setting (Pause > Options); the default 2 is a profile default (setting 116),
  not screen data.

Patches (tools/screenset.py patch <pkg> <out> <names>; each re-parses and round-trips). Installed
by tools/upkpatch.py patch_ui (make pad-data): ui_shell controlconfig-pad, footers-visible,
video-apply-icon; ui_ingame pause-vibration:
- controlconfig-always: swap the literals in the 4 menus (m_controllerSetup always).
- controlconfig-pad: the Xbox and PC branches become IfVar(9999 == 1) / IfVar(9999 == 0). Needs
  a runtime hook so GetVar (0x6a5250, thiscall (id, x), ret 8) answers id 9999 with "pad in use"
  (9999 is undeclared, so SetVar alone would be dropped) [guess until run].
- footers-visible: show the 5 A/B footer objects (+ their Nav images) the PC hides.
- video-apply-icon: Apply footer gets the backing + Generic_Elements-Xbox_Start image (cloned
  from Reset's polys, appended) and its text moves to x 24 like the others.
- pause-vibration (ui_ingame): show the Controller Vibration row.
- Runtime (not data): while a pad is in use, game.c images_tick changes the first byte of each of
  the 13 refused names (0x6dc770 compares against them only), so nothing matches and the Nav_*/
  Xbox_* images draw; with the keyboard the names go back and the PC look stays (text only).
  Chosen over a hook on 0x6dc770: no rebuild-time change to a drawing function, one write per
  device switch.
Writing: changed screens grow at their end (strings/lists/nodes appended; old copies left
unused), later screens shift, the export is appended at the end of the package with only its
export entry changed (as tools/upkpatch.py does).

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
- 2026-10-26: PAD IN GAMEPLAY [verified: shots in the market level, work/testplay]: Start/A skip
  the intro cutscene, A dismisses the tutorial box, the left stick walks, the right trigger fires
  (ammo 48 -> 41), the right stick turns the camera once UStrangleholdEngineInfo's "PC controls?"
  slot (vtable 0xdb85c4 +0xf0) answers from the device used last (game.c engine_is_pc): with the
  fixed PC answer the camera only took the mouse's absolute orbit. Still PC: the tutorial text's
  prompts ("Press SPACEBAR"), Auto Aim off, no rumble, layout always the default.
- 2026-10-26: XBOX PROMPTS [verified: shot, the market tutorial reads "Press [LT] to dive and
  press [RT] to shoot"]. The prompt text comes from the controller token manager
  (UWooControllerTokenManager, vtable 0xdbbcdc, [engine info +0x2b8]): at engine init 0x6bdc70
  localizes its names (+0x5c) into display strings (+0x68) with Localize("Controls", name,
  "ControllerSettings") [verified: gdb backtrace]. game.c makes an XBox table once (the engine's
  own Localize 0x6f85c0, so every language) and points +0x68 at it while a controller is in use,
  back to the engine's before exit. The PC localization still has the XBox names as font glyphs.
  Also hooked (0x6d1000, builds/gog.py HOOKS) for the action-key path: XBox names and the 360's
  ControllerLayouts[10]/[14]. Tried: swapping the engine info's ButtonMappings (+0x288) - the
  engine resets it during level load and the display table is built at init anyway.
  Still PC text: five PC-only strings with keys written in (strings.eng Press_Start "Press ENTER",
  Press_Start_Button, Press_A_button_To_Begin_Match, tips_PressAContinue "Press SPACEBAR to
  Continue", Demo\Controller_Layout); the 360 has no such keys (its UI used images). Plan: a hook
  on Localize (0x6f85c0) giving glyph text for those while a pad is in use (needs a full rebuild).
- RUMBLE [verified: log, firing pulses the motors at 7710 (30/255) about every 100 ms]: the PC config comments out the force feedback manager
  ([Engine.PlayerController] ForceFeedbackManagerClassName=WinDrv.XnaForceFeedbackManager; the 360
  has XeDrv.XnaForceFeedbackManager); tools/upkpatch.py now also writes the user's Coalesced.ini
  with it on. UXnaForceFeedbackManager (vtable 0xe5e4d4) keeps the base's empty
  ApplyForceFeedback (slot 55, +0xdc, ret 8) [verified: vtables]; game.c points that slot at
  ffm_apply, which plays UE3 waveforms (samples: left/right amplitude, left/right function bytes,
  duration float [verified: the decompiled WooDamageType/WooWeaponPatternFireMode defaults]) on
  the pads' motors (left = low). Sources: weapons build a firing waveform (BuiltWaveform),
  WooDamageType's damaged (100/40 noise, 0.5 s) and killed (100/100, 0.75 s) waveforms.
- LAYOUTS: the 360's four controller configurations differ only in the action set (XBoxActions,
  XBoxActionsAlt1-3). tools/upkpatch.py makes XBoxConfigAlt1-3_99 complete PC configs with that
  layout's actions [verified: uedump of the patched package] and lists them in the config;
  hook_load_profile_config (0x6a7240) pushes the chosen one in place of PCConfig [verified: log,
  "controller layout 2: XBoxConfigAlt1_99"]. Choice: option controller_layout (1-4) for now - the
  PC's Settings > Control Config opens the keyboard remapping screen, not the 360's layout screen,
  even with a pad [verified: shot]; the menus are one WooScreenSet object (Midway's binary screen
  format, not decoded), whose data still holds 360_ControlConfig.
- x87: a shared forwarder (0x40be60, a virtual in 17 vtables) calls this->vtable[+0x10c] whose
  callee returns a float in some classes and nothing in others; builds/gog.py ICALL_FPU (new
  translator option) declares that call None, so the forwarder keeps the x87 stack in the record.
  The engine never calls ApplyForceFeedback on PC (the slot never ran while a live manager
  existed: SHOLD_FIND=e5e4d4@1500 found the class default and one instance), so game.c finds the
  managers by scanning program memory for their vtable, 16 MB a tick (a full pass about every
  1.5 s), steps every one found each tick (rumble_tick) and drives the motors from the strongest.
  The manager's FFWaveform field is at +0x3c [verified: found on the first shot]. Improvement for
  later: reach the manager through the local player instead of the scan.
- Dead ends noted: StrangleholdSeqCond_Platform (a PC-only Kismet condition with Xbox360/PS3/PC
  outputs) is used by no map or menu package [verified: work/find_cls.py over all CookedPC
  packages]; WorldInfo.IsConsoleBuild exists but nothing found choosing the control screen by it.
  The Settings > Control Config screen choice lives in the WooScreenSet data (ui_shell /
  ui_common: "causeevent settingsContr", Pc_Keyboard_Config, 360_ControlConfig).
- 2026-10-26: PC-only key texts done [verified: shots]: hook on Localize (0x6f85c0) - with a pad,
  the title reads "Press START" (its font has no glyphs: the word), the tutorial "Press [A] to
  Continue" (glyph). Rumble scan slowed to 4 MB a tick once a manager is known. Options help
  trimmed to this port's (display, rumble, controller_layout, frame_limit); xbox_data (LA Rush's)
  off. skip_intro (tests) now skips Stranglehold's logo/legal movies and the opening cinematic
  (intro_c0010): the title in ~60 s instead of ~190 s (work/testmenu has it on).
- 2026-10-26: a visible window (--window) runs at ~100 fps on the laptop (2588 frames in 25 s at the
  title) [verified: run log]: the 20 fps of the --hidden runs is the compositor holding back an
  unshown window. make install -> ~/.local/opt/stranglehold-native + applications-menu entry
  "Stranglehold" (started with no arguments it plays, user folder ~/.local/share/stranglehold-native
  with the controller data from make pad-data).
- 2026-10-26 (Ant's first play): AUDIO - FMOD Ex looks up DirectSoundEnumerateA,
  DirectSoundCaptureEnumerateA and DirectSoundCaptureCreate8 by GetProcAddress and played nothing
  without them (both DirectSound objects in the log were Bink's; a WAV of the run was silent after
  the intro). Added (dsound.c): enumeration of the primary driver + one device, no capture devices
  [verified: SHOLD_SOUND_WAV, title and menu music from ~50 s]. STICKS: the 360's sense is Y DOWN
  positive (the menu config moves down on LeftY > 0.5; PC keyboard Desire_MoveY = S - W); the
  bridge had up positive (Ant: menu stick upside down) - fixed for both sticks.
- 2026-10-27 (Ant's first play, continued): STRETCHED - the game's 4:3 1024x768 back buffer was
  scaled to the 16:10 window. d3d9.c Present now passes a centred destination rectangle when the
  window's shape differs from the back buffer's by more than 1% (bars at the sides) [verified: the
  rectangle in the log; the look itself is Ant's to see]. The default resolution stays 1024x768;
  any resolution in Video Options fills the window when its shape matches. CRASH after switching
  to 2560x1600: 0x659620 (sets the viewport's widescreen flag) notifies a chain that is empty in
  the menus and read address 0x38; hook widescreen_flag sets the flag and skips the notification
  then [verified: disassembly + Ant's backtrace; guess until run: no side effect from the skipped
  notification]. AUTO AIM - Ant: "default it 2 but make it a setting": the profile default
  (WooEngineInfo.xxx, setting 116 Value1) is patched to 2 by make pad-data; the setting is the
  game's own Pause > Options > Aim Assist row (section 8 d). A profile saved before keeps its
  value. PACKAGE HASHES: the program checks its 43 script packages (WooEngineInfo.xxx is one;
  Startup_*, ui_* and the maps are not) against SHA-1 hashes of their uncompressed data in a
  resource and quits at start-up on a mismatch [verified: the table entry = SHA-1 of the
  package; recompressed-unchanged starts, any changed value quits before the first tick]. So
  upkpatch writes WooEngineInfo.xxx.sha1 and game.c package_hashes puts it into the table (in
  the program image: LoadResource returns a pointer into it) before the engine starts
  [verified: run75 starts]. The Coalesced files are listed too but the patched Coalesced.ini
  loads without this. MENUS - the screen format decoded (section 8); patched menus installed by
  make pad-data; hook screen_var (GetVar id 9999 = pad in use) and images_tick (Xbox button
  images) in game.c [verified: run75/76/77 shots with a scripted pad: Settings footer B Back /
  A Select; Control Config opens the Xbox 360 Controller Config screen (layout picker); Video
  Options footer B Back / Y Reset / Start Apply; Start applies the resolution, the game's own
  "keep these settings?" dialog follows and right + A accepts it (kept 1920x1080), no crash].
- 2026-10-27 (Ant: "it says A is exit game on main menu and the game is running in 4:3"):
  EXIT GAME - m_MainMenu's PC-only Exit Game footer answers Y (event 2113) but carried the A
  image (never drawn on PC) at the A Select footer's place; screenset.py main-exit-y gives it the
  backing + Y image and the hidden storage footer's place [verified: run81 shot "(Y) Exit Game"].
  4:3 - Ant's game runs full screen on his primary monitor, 2560x1440 (16:9; the Win Max panel is
  2560x1600, plus a portrait 1080x1920) [verified: kscreen-doctor], with the game set to 2560x1600,
  so the letterbox drew side bars. The game's own picture is full 16:10 [verified: run78 gameplay
  shot]. The Video Options list kept only 15 sizes (no 2560x1440); builds/gog.py CODE_PATCHES (new
  translator feature, tools/recomp.py, original bytes checked) drops that filter as GOG's loader
  did [verified: run80, the list steps 1920x1080, 1920x1200, 1920x1440, 2048x1152, 2048x1536].
  present_dest logs the window and back buffer sizes when they change. Open: a first-run default
  resolution matching the screen (the game starts at 1024x768).
- 2026-11-01: SHADER CACHE (GAME-GOALS "Shaders"): on and working, nothing to fix. DXVK 3.1.1 keeps its
  own shader cache in ~/.cache/dxvk (<hash>.dxvk.bin/.lut), and RADV its pipeline cache in
  ~/.cache/mesa_shader_cache. [verified: run with both empty "Created cache file", 39 MB + 58 MB
  written; next run "Found cache file ... 6272 shaders"; the installed copy found Ant's real cache
  and added to it.] DXVK_STATE_CACHE_PATH in d3d9.c names a variable this DXVK no longer reads
  (it has DXVK_SHADER_CACHE_PATH) [verified: the library's strings]: harmless. First-run stutter,
  title + menus + start of the first level, hidden window, machine under load: empty caches slowest
  frame after the first 300 = 57.1 ms, filled 69.6 ms, both ~50 ms a frame (the hidden window's
  20 fps hold), so no compile hitch showed and no pipeline list is needed so far. The first 300
  frames' worst (403 ms empty, 899 ms filled) is start-up loading, not shaders [guess: it isn't
  worse with empty caches]. Limits: hitches under 50 ms can't show at that hold, and later levels
  weren't measured; re-measure in a visible window if Ant sees stutter. Open: Ant's log of
  2026-10-30 says "Created cache file" for the real cache, though that file's date and contents
  weren't reset; unexplained, and today's installed run found it normally.
