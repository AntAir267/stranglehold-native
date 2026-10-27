/* KERNEL32: process, memory, strings, code pages, modules, errors. Files are in files.c,
   threads and synchronisation in sync.c. */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include "runtime.h"
#include "image.h"

extern int close_file_handle(uint32_t h);
extern void set_current_cpu(CPU *c);

/* ------------------------------------------------------------------ errors */
void set_last_error(uint32_t e)
{
    CPU *c = current_cpu();
    if (c) W32(c->fs_base + 0x34, e);
}

uint32_t get_last_error(void)
{
    CPU *c = current_cpu();
    return c ? R32(c->fs_base + 0x34) : 0;
}

API(KERNEL32, GetLastError, 0) { return get_last_error(); }

API(KERNEL32, CloseHandle, 1)
{
    uint32_t h = ARG(0);
    switch (handle_type(h)) {
    case H_FILE: return close_file_handle(h);
    case H_EVENT: case H_MUTEX: case H_THREAD: case H_SEMAPHORE: case H_MISC:
        LOG(LOG_THREAD, "CloseHandle(%x)", h);
        handle_close(h);               /* the objects stay allocated: waiters may still hold them */
        return 1;
    default:
        set_last_error(6);
        return 0;
    }
}

/* ------------------------------------------------------------------ process */
static uint32_t cmdline_a, cmdline_w_env, env_a, module_name;

API(KERNEL32, GetCommandLineA, 0)
{
    if (!cmdline_a) {
        char b[600];
        snprintf(b, sizeof b, "\"%s\\Binaries\\Retail-Stranglehold.exe\" -nowxwindows", virtual_game_root());
        cmdline_a = gstrdup(b);
    }
    return cmdline_a;
}

API(KERNEL32, GetEnvironmentStrings, 0)
{
    if (!env_a) {
        static const char env[] = "WINDIR=C:\\WINDOWS\0\0";
        env_a = galloc(sizeof env);
        memcpy(gptr(env_a), env, sizeof env);
    }
    return env_a;
}

API(KERNEL32, GetEnvironmentStringsW, 0)
{
    if (!cmdline_w_env) {
        static const char env[] = "WINDIR=C:\\WINDOWS";
        uint32_t n = sizeof env;          /* includes one terminator; add the block's second */
        cmdline_w_env = galloc(2 * (n + 1));
        for (uint32_t i = 0; i < n; i++)
            W16(cmdline_w_env + 2 * i, (uint8_t)env[i]);
        W16(cmdline_w_env + 2 * n, 0);
    }
    return cmdline_w_env;
}

API(KERNEL32, FreeEnvironmentStringsA, 1) { return 1; }
API(KERNEL32, FreeEnvironmentStringsW, 1) { return 1; }

API(KERNEL32, GetStartupInfoA, 1)
{
    memset(gptr(ARG(0)), 0, 68);
    W32(ARG(0), 68);
    return 0;
}

API(KERNEL32, GetVersion, 0)
{
    return 0x0a280105u;                  /* Windows XP (5.1 build 2600): GOG's compatibility mode */
}

/* Modules: the translated ones at their real bases; the system DLLs the runtime stands in for
   get handles of their usual addresses (nothing is mapped there: their functions are found by
   name, GetProcAddress below). */
static const struct { const char *name; uint32_t base; } sysdlls[] = {
    {"kernel32", 0x7c800000u}, {"user32", 0x7e410000u}, {"gdi32", 0x77f10000u},
    {"advapi32", 0x77dd0000u}, {"shlwapi", 0x77f60000u}, {"winmm", 0x76b40000u},
    {"ntdll", 0x7c900000u}, {"ole32", 0x774e0000u}, {"oleaut32", 0x77120000u},
    {"comctl32", 0x773d0000u}, {"version", 0x77c00000u}, {"d3d9", 0x75000000u},
    {"dsound", 0x75100000u}, {"dinput8", 0x75200000u}, {"shell32", 0x7c9c0000u},
};

/* each stand-in DLL's handle points at a minimal PE header (no sections, no exports): code that
   reads a module's headers through its handle finds a valid, empty image */
#include <sys/mman.h>
void sysdlls_map(void)
{
    for (size_t i = 0; i < sizeof sysdlls / sizeof *sysdlls; i++) {
        uint32_t b = sysdlls[i].base;
        void *m = mmap((void *)(uintptr_t)b, 0x1000, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (m != (void *)(uintptr_t)b) {
            LOG(LOG_ALWAYS, "cannot place the %s header at %08x", sysdlls[i].name, b);
            continue;
        }
        W16(b, 0x5a4d);                 /* MZ */
        W32(b + 0x3c, 0x40);
        W32(b + 0x40, 0x4550);          /* PE */
        W16(b + 0x44, 0x14c);           /* i386 */
        W16(b + 0x54, 0xe0);            /* SizeOfOptionalHeader */
        W16(b + 0x56, 0x2102);          /* DLL, 32-bit, executable */
        uint32_t o = b + 0x58;
        W16(o, 0x10b);
        W8(o + 2, 7);
        W32(o + 28, b);                 /* ImageBase */
        W32(o + 32, 0x1000); W32(o + 36, 0x200);
        W16(o + 40, 5); W16(o + 42, 1); W16(o + 48, 5); W16(o + 50, 1);
        W32(o + 56, 0x1000);            /* SizeOfImage */
        W32(o + 60, 0x400);
        W16(o + 68, 2);
        W32(o + 92, 16);                /* NumberOfRvaAndSizes: all empty */
    }
}

static int sysdll_match(const char *n, const char *base)
{
    const char *b = strrchr(n, '\\');
    b = b ? b + 1 : n;
    size_t k = strlen(base);
    return !strncasecmp(b, base, k) && (!b[k] || !strcasecmp(b + k, ".dll"));
}

static uint32_t module_handle(const char *n)
{
    if (!n)
        return image_base;
    const ModuleEntry *m = module_by_name(n);
    if (m)
        return m->base;
    for (size_t i = 0; i < sizeof sysdlls / sizeof *sysdlls; i++)
        if (sysdll_match(n, sysdlls[i].name))
            return sysdlls[i].base;
    return 0;
}

static const char *sysdll_name(uint32_t h)
{
    for (size_t i = 0; i < sizeof sysdlls / sizeof *sysdlls; i++)
        if (sysdlls[i].base == h)
            return sysdlls[i].name;
    return NULL;
}

static uint32_t get_module_handle(const char *n)
{
    uint32_t r = module_handle(n);
    LOG(LOG_API, "GetModuleHandle(%s) -> %x", n ? n : "NULL", r);
    if (!r) set_last_error(126);
    return r;
}

API(KERNEL32, GetModuleHandleA, 1) { return get_module_handle(gstr(ARG(0))); }
API(KERNEL32, GetModuleHandleW, 1)
{
    char b[600];
    return get_module_handle(ARG(0) ? gwstr(ARG(0), b, sizeof b) : NULL);
}

static void module_path(uint32_t h, char *b, size_t n)
{
    /* the program and the DLLs translated with it all live in the game's Binaries folder */
    const ModuleEntry *m = h ? module_at(h) : &module_table[0];
    const char *sys = sysdll_name(h);
    if (m && m != &module_table[0])
        snprintf(b, n, "%s\\Binaries\\%s", virtual_game_root(), m->name);
    else if (sys)
        snprintf(b, n, "C:\\WINDOWS\\system32\\%s.dll", sys);
    else
        snprintf(b, n, "%s\\Binaries\\Retail-Stranglehold.exe", virtual_game_root());
}

API(KERNEL32, GetModuleFileNameA, 3)
{
    char b[600];
    module_path(ARG(0), b, sizeof b);
    uint32_t n = ARG(2);
    if (!n) return 0;
    gstrncpy(ARG(1), b, n);
    (void)module_name;
    return strlen(gptr(ARG(1)));
}

API(KERNEL32, GetModuleFileNameW, 3)
{
    char b[600];
    module_path(ARG(0), b, sizeof b);
    return gwput(ARG(1), b, ARG(2));
}

API(KERNEL32, ExitProcess, 1)
{
    process_exit((int)ARG(0));
}

API(KERNEL32, GetCurrentProcess, 0) { return 0xffffffffu; }

API(KERNEL32, TerminateProcess, 2)
{
    if (ARG(0) == 0xffffffffu)
        process_exit((int)ARG(1));
    return 0;
}

const char *computer_name(void) { return "PLAYER-PC"; }    /* (also Winsock's gethostname) */

static int get_computer_name(uint32_t buf, uint32_t sizep, int wide)
{
    const char *name = computer_name();
    uint32_t cap = R32(sizep);
    uint32_t n = strlen(name);
    if (cap <= n) { W32(sizep, n + 1); set_last_error(111); return 0; }
    if (wide) gwput(buf, name, cap);
    else gstrncpy(buf, name, cap);
    W32(sizep, n);
    return 1;
}

API(KERNEL32, GetComputerNameA, 2) { return get_computer_name(ARG(0), ARG(1), 0); }
API(KERNEL32, GetComputerNameW, 2) { return get_computer_name(ARG(0), ARG(1), 1); }

API(KERNEL32, OutputDebugStringA, 1)
{
    LOG(LOG_ALWAYS, "debug output: %s", gstr(ARG(0)));
    return 0;
}

API(KERNEL32, OutputDebugStringW, 1)
{
    char b[2048];
    LOG(LOG_ALWAYS, "debug output: %s", gwstr(ARG(0), b, sizeof b));
    return 0;
}

API(KERNEL32, IsDebuggerPresent, 0) { return 0; }
API(KERNEL32, FlushInstructionCache, 3) { return 1; }  /* (no code is generated at run time) */

/* ------------------------------------------------------------------ libraries */
#include <dlfcn.h>

static uint32_t load_library(const char *n)
{
    uint32_t r = n ? module_handle(n) : 0;
    LOG(LOG_ALWAYS, "LoadLibrary(\"%s\") -> %x", n ? n : "(null)", r);
    if (!r)
        set_last_error(126);              /* ERROR_MOD_NOT_FOUND */
    return r;
}

API(KERNEL32, LoadLibraryA, 1) { return load_library(gstr(ARG(0))); }
API(KERNEL32, LoadLibraryExA, 3) { return load_library(gstr(ARG(0))); }   /* (flags make no difference) */
API(KERNEL32, LoadLibraryW, 1)
{
    char b[600];
    return load_library(ARG(0) ? gwstr(ARG(0), b, sizeof b) : NULL);
}

API(KERNEL32, FreeLibrary, 1) { return 1; }

/* an export of a translated module (by name, or ordinal when name < 0x10000) */
static uint32_t module_export(const ModuleEntry *m, uint32_t np)
{
    uint32_t pe = m->base + R32(m->base + 0x3c);
    uint32_t dir = R32(pe + 24 + 96), dsize = R32(pe + 24 + 100);
    if (!dir) return 0;
    uint32_t ed = m->base + dir;
    uint32_t ordbase = R32(ed + 16), nfun = R32(ed + 20), nnames = R32(ed + 24);
    uint32_t funcs = m->base + R32(ed + 28), names = m->base + R32(ed + 32), ords = m->base + R32(ed + 36);
    uint32_t idx = 0xffffffffu;
    if (np < 0x10000)
        idx = np - ordbase;
    else
        for (uint32_t i = 0; i < nnames; i++)
            if (!strcmp(gptr(m->base + R32(names + 4 * i)), gptr(np))) {
                idx = R16(ords + 2 * i);
                break;
            }
    if (idx >= nfun) return 0;
    uint32_t rva = R32(funcs + 4 * idx);
    if (!rva) return 0;
    if (rva >= dir && rva < dir + dsize) {
        /* forwarded: "DLL.name" */
        char fw[256], dll[256];
        snprintf(fw, sizeof fw, "%s", gptr(m->base + rva));
        char *dot = strchr(fw, '.');
        if (!dot) return 0;
        *dot = 0;
        snprintf(dll, sizeof dll, "%s.dll", fw);
        const ModuleEntry *t = module_by_name(dll);
        if (t) return module_export(t, gstrdup(dot + 1));
        LOG(LOG_ALWAYS, "export of %s forwarded to %s.%s, not provided", m->name, fw, dot + 1);
        return 0;
    }
    return m->base + rva;
}

/* a function of a DLL the runtime stands in for: its import slot if a module imports it,
   else the runtime's implementation found by its symbol name (w_DLL_name) */
static uint32_t runtime_proc(const char *dll, const char *name)
{
    /* functions the runtime has under their ordinal (as the game imports them) */
    static const struct { const char *dll, *name, *ord; } aliases[] = {
        {"dsound", "DirectSoundCreate", "1"}, {"dsound", "DirectSoundCreate8", "11"},
    };
    for (size_t i = 0; i < sizeof aliases / sizeof *aliases; i++)
        if (!strcasecmp(dll, aliases[i].dll) && !strcmp(name, aliases[i].name))
            name = aliases[i].ord;
    char d[64];
    snprintf(d, sizeof d, "%s.dll", dll);
    for (int i = 0; i < import_count; i++)
        if (!strcasecmp(import_table[i].dll, d) && !strcmp(import_table[i].name, name))
            return R32(import_table[i].iat);
    char sym[300], up[64];
    size_t k = 0;
    for (; dll[k] && k < sizeof up - 1; k++) up[k] = (char)toupper((unsigned char)dll[k]);
    up[k] = 0;
    snprintf(sym, sizeof sym, "w_%s_%s", up, name);
    void (*fn)(CPU *) = (void (*)(CPU *))dlsym(RTLD_DEFAULT, sym);
    if (fn) {
        char *nm = malloc(strlen(d) + strlen(name) + 2);
        sprintf(nm, "%s!%s", d, name);
        return thunk_addr(fn, nm);
    }
    return 0;
}

API(KERNEL32, GetProcAddress, 2)
{
    uint32_t h = ARG(0), np = ARG(1);
    char ord[16];
    const char *name = np < 0x10000 ? (snprintf(ord, sizeof ord, "#%u", np), ord) : gstr(np);
    uint32_t r = 0;
    const ModuleEntry *m = module_at(h);
    const char *sys = sysdll_name(h);
    if (m && m->base == h)
        r = module_export(m, np);
    else if (sys)
        r = runtime_proc(sys, name);
    LOG(r ? LOG_API : LOG_ALWAYS, "GetProcAddress(%x, \"%s\") -> %x", h, name, r);
    if (!r) set_last_error(127);
    return r;
}

/* ------------------------------------------------------------------ memory */
API(KERNEL32, HeapCreate, 3)
{
    uint32_t h = heap_create(ARG(1), ARG(2));
    LOG(LOG_MEM, "HeapCreate(%x, %x, %x) -> %x", ARG(0), ARG(1), ARG(2), h);
    return h;
}

API(KERNEL32, HeapDestroy, 1) { return heap_destroy(ARG(0)); }

API(KERNEL32, HeapAlloc, 3)
{
    return heap_alloc(ARG(0), ARG(2), (ARG(1) & 8) != 0);
}

API(KERNEL32, HeapFree, 3)
{
    return heap_free(ARG(0), ARG(2));
}

API(KERNEL32, HeapReAlloc, 4)
{
    return heap_realloc(ARG(0), ARG(2), ARG(3), (ARG(1) & 8) != 0, (ARG(1) & 0x10) != 0);
}

API(KERNEL32, HeapSize, 3)
{
    return heap_size(ARG(0), ARG(2));
}

API(KERNEL32, VirtualAlloc, 4)
{
    uint32_t r = vm_alloc(ARG(0), ARG(1), !(ARG(2) & 0x1000));
    LOG(LOG_MEM, "VirtualAlloc(%08x, %x, %x, %x) -> %08x", ARG(0), ARG(1), ARG(2), ARG(3), r);
    if (!r) set_last_error(8);
    return r;
}

API(KERNEL32, VirtualFree, 3)
{
    return vm_free(ARG(0), ARG(1), (ARG(2) & 0x8000) != 0);
}

API(KERNEL32, GlobalAlloc, 2)
{
    uint32_t p = heap_alloc(process_heap, ARG(1) ? ARG(1) : 1, (ARG(0) & 0x40) != 0);
    LOG(LOG_MEM, "GlobalAlloc(%x, %u) -> %08x", ARG(0), ARG(1), p);
    return p;
}

API(KERNEL32, GlobalLock, 1) { return ARG(0); }
API(KERNEL32, GlobalUnlock, 1) { return 1; }
API(KERNEL32, GlobalHandle, 1) { return ARG(0); }
API(KERNEL32, GlobalFree, 1) { return heap_free(process_heap, ARG(0)) ? 0 : ARG(0); }

static int mapped(uint32_t p, uint32_t n)
{
    const ModuleEntry *m = module_at(p);
    if (m && (uint64_t)p + n <= (uint64_t)m->base + m->size)
        return 1;
    uint32_t b = vm_query_base(p);
    return b != 0 && (n == 0 || vm_query_base(p + n - 1) == b);
}

API(KERNEL32, IsBadReadPtr, 2) { return ARG(1) && !mapped(ARG(0), ARG(1)); }
API(KERNEL32, IsBadWritePtr, 2) { return ARG(1) && !mapped(ARG(0), ARG(1)); }
API(KERNEL32, IsBadCodePtr, 1)
{
    uint32_t p = ARG(0);
    const ModuleEntry *m = module_at(p);
    for (int i = 0; m && i < m->ntext; i++)
        if (p - m->text[i][0] < m->text[i][1] - m->text[i][0])
            return 0;
    return !thunk_name(p);
}

/* ------------------------------------------------------------------ strings */
API(KERNEL32, lstrlenA, 1) { return ARG(0) ? strlen(gptr(ARG(0))) : 0; }

API(KERNEL32, lstrcpyA, 2)
{
    strcpy(gptr(ARG(0)), gptr(ARG(1)));
    return ARG(0);
}

/* lstrcmpA compares with the user locale's word sort; for the program's ASCII names a
   case-sensitive comparison with lower case first is close enough. */
API(KERNEL32, lstrcmpA, 2)
{
    const unsigned char *a = (const unsigned char *)gptr(ARG(0)), *b = (const unsigned char *)gptr(ARG(1));
    if (!a || !b) return a ? 1 : b ? -1 : 0;
    for (;; a++, b++) {
        int x = tolower(*a), y = tolower(*b);
        if (x != y) return x < y ? -1 : 1;
        if (*a != *b) return islower(*a) ? -1 : 1;
        if (!*a) return 0;
    }
}

/* Windows-1252 bytes 0x80-0x9f */
static const uint16_t cp1252_hi[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039,
    0x0152, 0x008d, 0x017d, 0x008f, 0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178};

static uint16_t to_wide(uint8_t b) { return b >= 0x80 && b < 0xa0 ? cp1252_hi[b - 0x80] : b; }
static uint8_t to_narrow(uint16_t w)
{
    if (w < 0x80 || (w >= 0xa0 && w < 0x100)) return (uint8_t)w;
    for (int i = 0; i < 32; i++)
        if (cp1252_hi[i] == w) return 0x80 + i;
    return '?';
}

API(KERNEL32, MultiByteToWideChar, 6)
{
    uint32_t src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = strlen(gptr(src)) + 1;
    if (!cap) return n;
    if ((uint32_t)n > cap) { set_last_error(122); return 0; }
    for (int i = 0; i < n; i++)
        W16(dst + 2 * i, to_wide(R8(src + i)));
    return n;
}

API(KERNEL32, WideCharToMultiByte, 8)
{
    uint32_t src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) { n = 0; while (R16(src + 2 * n)) n++; n++; }
    if (ARG(7)) W32(ARG(7), 0);
    if (!cap) return n;
    if ((uint32_t)n > cap) { set_last_error(122); return 0; }
    for (int i = 0; i < n; i++)
        W8(dst + i, to_narrow(R16(src + 2 * i)));
    return n;
}

API(KERNEL32, GetACP, 0) { return 1252; }
API(KERNEL32, GetOEMCP, 0) { return 437; }

API(KERNEL32, GetCPInfo, 2)
{
    uint32_t p = ARG(1);
    memset(gptr(p), 0, 20);
    W32(p, 1);
    W8(p + 4, '?');
    return 1;
}

static uint16_t ctype1(uint16_t ch)
{
    uint16_t f = 0;
    unsigned c = ch;
    if (c < 0x20 || c == 0x7f || (c >= 0x80 && c < 0xa0)) f |= 0x20;        /* C1_CNTRL */
    if (c == ' ' || (c >= 9 && c <= 13) || c == 0xa0) f |= 0x08;             /* C1_SPACE */
    if (c == ' ' || c == '\t' || c == 0xa0) f |= 0x40;                       /* C1_BLANK */
    if (c >= '0' && c <= '9') f |= 0x04 | 0x80;                              /* DIGIT|XDIGIT */
    if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) f |= 0x80;
    int upper = (c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7) ||
                c == 0x160 || c == 0x152 || c == 0x17d || c == 0x178;
    int lower = (c >= 'a' && c <= 'z') || (c >= 0xdf && c <= 0xff && c != 0xf7) ||
                c == 0x161 || c == 0x153 || c == 0x17e || c == 0x192 || c == 0xaa || c == 0xb5 || c == 0xba;
    if (upper) f |= 0x01 | 0x100;
    if (lower) f |= 0x02 | 0x100;
    if (!(f & (0x20 | 0x08 | 0x04 | 0x100)) && c > ' ' && c != 0x7f) f |= 0x10;   /* C1_PUNCT */
    if (c == 0xa0) f &= ~0x10;
    return f;
}

API(KERNEL32, GetStringTypeA, 5)
{
    uint32_t type = ARG(1), src = ARG(2), out = ARG(4);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = strlen(gptr(src)) + 1;
    for (int i = 0; i < n; i++)
        W16(out + 2 * i, type == 1 ? ctype1(to_wide(R8(src + i))) : 0);
    return 1;
}

API(KERNEL32, GetStringTypeW, 4)
{
    uint32_t type = ARG(0), src = ARG(1), out = ARG(3);
    int32_t n = (int32_t)ARG(2);
    if (n < 0) { n = 0; while (R16(src + 2 * n)) n++; n++; }
    for (int i = 0; i < n; i++)
        W16(out + 2 * i, type == 1 ? ctype1(R16(src + 2 * i)) : 0);
    return 1;
}

static uint16_t case_map(uint16_t ch, uint32_t flags)
{
    if (flags & 0x100) {                 /* LCMAP_LOWERCASE */
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 0xc0 && ch <= 0xde && ch != 0xd7)) return ch + 0x20;
    } else if (flags & 0x200) {          /* LCMAP_UPPERCASE */
        if ((ch >= 'a' && ch <= 'z') || (ch >= 0xe0 && ch <= 0xfe && ch != 0xf7)) return ch - 0x20;
    }
    return ch;
}

API(KERNEL32, LCMapStringA, 6)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = strlen(gptr(src)) + 1;
    if (flags & ~0x300u)
        LOG(LOG_ALWAYS, "LCMapStringA: flags %x not supported", flags);
    if (!cap) return n;
    if ((uint32_t)n > cap) { set_last_error(122); return 0; }
    for (int i = 0; i < n; i++)
        W8(dst + i, to_narrow(case_map(to_wide(R8(src + i)), flags)));
    return n;
}

API(KERNEL32, LCMapStringW, 6)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) { n = 0; while (R16(src + 2 * n)) n++; n++; }
    if (flags & ~0x300u)
        LOG(LOG_ALWAYS, "LCMapStringW: flags %x not supported", flags);
    if (!cap) return n;
    if ((uint32_t)n > cap) { set_last_error(122); return 0; }
    for (int i = 0; i < n; i++)
        W16(dst + 2 * i, case_map(R16(src + 2 * i), flags));
    return n;
}

/* ------------------------------------------------------------------ exceptions */
static uint32_t unhandled_filter;

API(KERNEL32, SetUnhandledExceptionFilter, 1)
{
    uint32_t old = unhandled_filter;
    unhandled_filter = ARG(0);
    return old;
}

API(KERNEL32, UnhandledExceptionFilter, 1) { return 0; }

API(KERNEL32, RaiseException, 4)
{
    uint32_t code = ARG(0);
    if (code == 0x406d1388u) {
        /* MSVC's "set thread name" for a debugger (args: type 0x1000, name, thread id, flags): the
           program catches it itself when no debugger takes it, so it simply continues */
        uint32_t info = ARG(3);
        if (ARG(2) >= 3 && R32(info) == 0x1000 && R32(info + 4))
            LOG(LOG_THREAD, "thread %x named \"%s\"", R32(info + 8), gstr(R32(info + 4)));
        return 0;
    }
    if (code == 0xe06d7363u && ARG(2) >= 3) {
        /* C++ throw: args = magic, object, ThrowInfo -> CatchableTypeArray -> type name */
        uint32_t args = ARG(3), ti = R32(args + 8);
        const char *tname = "?";
        if (ti) {
            uint32_t cta = R32(ti + 12);
            if (cta && R32(cta) > 0) {
                uint32_t ct = R32(cta + 4), td = R32(ct + 4);
                if (td) tname = gstr(td + 8);
            }
        }
        fatal("the program threw a C++ exception of type %s (exceptions are not supported yet)", tname);
    }
    fatal("RaiseException(%08x) (exceptions are not supported yet)", code);
}

API(KERNEL32, RtlUnwind, 4)
{
    fatal("RtlUnwind (exceptions are not supported yet)");
}
