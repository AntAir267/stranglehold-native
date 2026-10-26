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

EXTRA_ENTRIES = []
NOT_ENTRIES = []
JUMP_TABLES = {}
NONLOCAL_JUMPS = []
NORETURN_IMPORTS = ['ExitProcess', 'ExitThread', 'TerminateProcess', 'FatalExit', 'FatalAppExitA']

MEM_OVERRIDES = {}
HOOKS = {}
