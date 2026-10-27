"""Per-build knowledge for Stranglehold PC, GOG 1.1 v2 (Retail-Stranglehold.exe linked 2007-11-13).

Addresses only: no code or data from the game lives in this repository.
"""
NAME = 'gog-1.1v2'

# The modules translated together, the program first: (name as imported, file, sha256).
# All ship in the game's Binaries folder. msvcr80.dll and msvcp80.dll are Microsoft's Visual C++
# 2005 runtime; binkw32.dll plays the movies.
# Not translated: d3dx9_28.dll (the program uses only D3DXMatrixInverse, D3DXCompileShader and
# D3DXDisassembleShader: the runtime answers them), dinput8.dll in the folder (GOG's
# patch loader for Binaries/patches/main.patch; the real DirectInput is the runtime's),
# wxmsw262u.dll (wxWidgets 2.6.2: the program runs its own loop when its command line has
# -nowxwindows, WinMain 0x401ae2 [verified: disassembly], so the runtime passes that and only
# answers wx's imports with stubs).
MODULES = [
    ('Retail-Stranglehold.exe', 'Binaries/Retail-Stranglehold.exe', '59b9f06a30560ed1a635ea80c027972bd5203a98dd45b2bfbdb31ac85b2758d6'),
    ('MSVCR80.dll', 'Binaries/msvcr80.dll', '02bdde38e4c6bd795a092d496b8d6060cdbe71e22ef4d7a204e3050c1be44fa9'),
    ('MSVCP80.dll', 'Binaries/msvcp80.dll', '2026f3c4f830dff6883b88e2647272a52a132f25eb42c0d423e36b3f65a94d08'),
    ('binkw32.dll', 'Binaries/binkw32.dll', 'feeb17a6f9a8287061fe350c08ff017d8d92bec46773448101ef1e771c9a0118'),
    # Double Fusion in-game advertising: translated so the program gets the real object it calls
    # without checking (0x45e6a7 stores CreateDFEngine's result, 0x45e794 calls through it
    # [verified: disassembly]); its network calls fail in the runtime, so no ads are fetched.
    ('DFEngine.dll', 'Binaries/DFEngine.dll', 'b88e12a3fa79f102f7bef1bea7e3fb0cadc0db5f26ab4be19b9dd8013ed47344'),
]

# Functions discovery turned away. 0xb89650: a virtual method (vtable pointers at 0xd5f638 and
# three more, all relocated) with an SEH prologue, rejected as "inside the code of another
# function" (the preceding function's bounded-by-scan jump table runs into it) [verified: the
# translated program called it, 0xb8e776]. The other 50 relocated pointers rejected that way
# point into functions (exception handlers' continuations, catch blocks) [verified: their bytes];
# the 833 padded starts nothing references are dead library code (every pointer here is
# relocated, and direct calls are followed) [verified: work/why_rejected.py].
EXTRA_ENTRIES = [0xb89650]
NOT_ENTRIES = []
JUMP_TABLES = {}
NONLOCAL_JUMPS = []
NORETURN_IMPORTS = ['ExitProcess', 'ExitThread', 'TerminateProcess', 'FatalExit', 'FatalAppExitA']

# Calls through a pointer whose callee changes the x87 stack (the translator assumes they don't and
# checks at run time): call-site address -> effect, or None when it depends on the callee (the
# function then keeps the x87 stack in the CPU record, right for any depth)
ICALL_FPU = {
    # a forwarder (0x40be60: this->vtable[+0x10c](float, int), then ret 8; itself a virtual method
    # shared by 17 vtables, identical code folded) whose callee returns a float in some classes and
    # nothing in others [verified: fpu_depth_fail at 0x40be75 with top 1, and with top 0 once
    # declared +1, in the market level with a pad]
    0x40be75: None,
}

MEM_OVERRIDES = {}
# Functions the runtime may hook (hook_<name>, run at the function's entry; returns 1 if it did the
# call itself). Hooks with no handler set cost nothing.
HOOKS = {
    # (this = the engine info; &out FString, action index, layout column): out = the
    # ControllerSettings key of the button bound to an action, ButtonMappings[ControllerLayouts[
    # [[this+0x294] + idx*28 + 0x18] + [this+0x1bc] + column]] (ret 0xc) [verified: disassembly]:
    # the Xbox names while a controller is in use (game.c)
    0x6d1000: 'button_name',
    # the profile's control config pushed by name, always "PCConfig" on PC [verified: the ANSI
    # string 0xdf8404 pushed there, then MwyInputHandler 0x4a3130]: the layout choice (game.c)
    0x6a7240: 'load_profile_config',
    # Localize (cdecl: &out FString, section, key, package, 0, 0; returns &out) [verified: the calls
    # at 0x6bdd4f, 0x6d417b]: the few PC-only strings with keyboard keys written in get the Xbox
    # buttons while a controller is in use (game.c)
    0x6f85c0: 'localize',
    # (this, bool wide): sets the viewport client's widescreen flag (+0x1b8) and, when it changes,
    # tells [[[GEngine 0x10ae0e8]+0x2d8] -> 0x6419d0 -> +0xe4]+0x38 (two 0x7fa notifications); with
    # that chain empty it reads address 0x38 and the game crashed (Ant's session: 20 s after
    # switching to 2560x1600 in the menus) [verified: disassembly of 0x659620 and the crash
    # backtrace 0x8f5100 -> 0x663100 -> 0x662380 -> 0x659620]: the hook skips the notification then
    0x659620: 'widescreen_flag',
    # the screen engine's GetVar (thiscall: id, x; ret 8) [verified: disassembly, then the engine's
    # variable store 0x9b6990]: id 9999 (undeclared in the game) answers "a pad is in use" for the
    # patched Settings menus (tools/screenset.py controlconfig-pad) (game.c)
    0x6a5250: 'screen_var',
}
