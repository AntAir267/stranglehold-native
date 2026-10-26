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
The Xbox 360 version (Ant's copy, if he dumps it) is the reference to check parity against:
its Coalesced ini and MwyInput packages, and its executable through REA for any console-only
code the PC build left out.

### 5. Graphics
Direct3D 9 through DXVK 3.1.1 native (SDL3 window system), as larush-native: the program's COM
objects are ours, calling DXVK's by vtable index. third_party/dxvk-native and third_party/sdl3
were copied from larush-native's builds.

## Progress
- 2026-10-26: project set up; game unpacked; builds/gog.py; first translator run.
