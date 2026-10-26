/* KERNEL32 functions the C runtime (msvcr71), MFC and LA Rush use beyond what 1NSANE needed:
   version, time, memory status, system information, locale and string helpers, atoms,
   messages, the console (none) and child processes (none). */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "runtime.h"

/* ------------------------------------------------------------------ UTF-16 helpers */
const char *gwstr(uint32_t w, char *buf, size_t n)
{
    size_t i = 0;
    if (!w) { buf[0] = 0; return buf; }
    for (; i + 1 < n; i++) {
        uint32_t ch = R16(w + 2 * i);
        if (!ch) break;
        buf[i] = ch < 0x100 ? (char)ch : '?';
    }
    buf[i] = 0;
    return buf;
}

uint32_t gwput(uint32_t dst, const char *s, uint32_t cap)
{
    if (!cap) return 0;
    uint32_t i = 0;
    for (; s[i] && i + 1 < cap; i++)
        W16(dst + 2 * i, (uint8_t)s[i]);
    W16(dst + 2 * i, 0);
    return i;
}

/* ------------------------------------------------------------------ version */
/* Windows XP SP3, what the game was made for */
API(KERNEL32, GetVersionExA, 1)
{
    uint32_t v = ARG(0), size = R32(v);
    if (size != 148 && size != 156) { set_last_error(122); return 0; }
    memset(gptr(v + 4), 0, size - 4);
    W32(v + 4, 5);
    W32(v + 8, 1);
    W32(v + 12, 2600);
    W32(v + 16, 2);                     /* VER_PLATFORM_WIN32_NT */
    gstrncpy(v + 20, "Service Pack 3", 128);
    if (size == 156) {
        W16(v + 148, 3);                /* wServicePackMajor */
        W8(v + 154, 1);                 /* VER_NT_WORKSTATION */
    }
    return 1;
}

API(KERNEL32, IsProcessorFeaturePresent, 1)
{
    switch (ARG(0)) {
    case 2: case 3: case 6: case 8: return 1;   /* CMPXCHG8B, MMX, SSE, RDTSC */
    default: return 0;                          /* SSE2, 3DNow!, the FDIV erratum ... */
    }
}

API(KERNEL32, GetSystemInfo, 1)
{
    uint32_t b = ARG(0);
    memset(gptr(b), 0, 36);
    W16(b, 0);                          /* PROCESSOR_ARCHITECTURE_INTEL */
    W32(b + 4, 4096);
    W32(b + 8, 0x10000);
    W32(b + 12, 0x7ffeffffu);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    n = n < 1 ? 1 : n > 4 ? 4 : n;
    W32(b + 16, (1u << n) - 1);
    W32(b + 20, (uint32_t)n);
    W32(b + 24, 586);
    W32(b + 28, 0x10000);
    W16(b + 32, 6);
    W16(b + 34, 0x0806);
    return 0;
}

API(KERNEL32, GlobalMemoryStatus, 1)
{
    uint32_t b = ARG(0);
    W32(b, 32);
    W32(b + 4, 30);
    W32(b + 8, 1024u << 20);            /* a 2006-sized machine: 1 GB, most of it free */
    W32(b + 12, 768u << 20);
    W32(b + 16, 2048u << 20);
    W32(b + 20, 1536u << 20);
    W32(b + 24, 0x7ffe0000u);
    W32(b + 28, 0x60000000u);
    return 0;
}

/* ------------------------------------------------------------------ time */
static uint64_t filetime_now(void)
{
    if (deterministic)      /* test runs: 2006-08-04 (the program's link date) on the virtual clock */
        return (1154649600ull + 11644473600ull) * 10000000ull + (uint64_t)(now_ms() * 10000.0);
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return ((uint64_t)t.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)t.tv_nsec / 100;
}

static void put_filetime(uint32_t at, uint64_t ft) { W32(at, (uint32_t)ft); W32(at + 4, (uint32_t)(ft >> 32)); }
static uint64_t get_filetime(uint32_t at) { return R32(at) | ((uint64_t)R32(at + 4) << 32); }

static void put_systemtime(uint32_t at, const struct tm *tm, int ms)
{
    W16(at, tm->tm_year + 1900);
    W16(at + 2, tm->tm_mon + 1);
    W16(at + 4, tm->tm_wday);
    W16(at + 6, tm->tm_mday);
    W16(at + 8, tm->tm_hour);
    W16(at + 10, tm->tm_min);
    W16(at + 12, tm->tm_sec);
    W16(at + 14, ms);
}

static int64_t tz_offset_100ns(time_t t)
{
    struct tm lt;
    localtime_r(&t, &lt);
    return (int64_t)lt.tm_gmtoff * 10000000ll;
}

/* a game hook for GetTickCount at chosen call sites (game.c: the frame limiter's wait loop) */
void (*tick_hook)(CPU *c, uint32_t ret);
API(KERNEL32, GetTickCount, 0)
{
    if (tick_hook)
        tick_hook(c, R32(c->esp));
    return (uint32_t)(uint64_t)now_ms();
}

API(KERNEL32, QueryPerformanceFrequency, 1)
{
    W32(ARG(0), 10000000u);
    W32(ARG(0) + 4, 0);
    return 1;
}

uint64_t cpu_rdtsc(void) { return (uint64_t)(now_ms() * 10000.0); }

API(KERNEL32, QueryPerformanceCounter, 1)
{
    uint64_t v = cpu_rdtsc();
    W32(ARG(0), (uint32_t)v);
    W32(ARG(0) + 4, (uint32_t)(v >> 32));
    return 1;
}

API(KERNEL32, GetSystemTimeAsFileTime, 1) { put_filetime(ARG(0), filetime_now()); return 0; }

API(KERNEL32, GetSystemTime, 1)
{
    uint64_t ft = filetime_now();
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm tm;
    gmtime_r(&t, &tm);
    put_systemtime(ARG(0), &tm, (int)(ft / 10000 % 1000));
    return 0;
}

API(KERNEL32, GetLocalTime, 1)
{
    uint64_t ft = filetime_now();
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm tm;
    localtime_r(&t, &tm);
    put_systemtime(ARG(0), &tm, (int)(ft / 10000 % 1000));
    return 0;
}

API(KERNEL32, SetLocalTime, 1) { return 1; }

API(KERNEL32, FileTimeToSystemTime, 2)
{
    uint64_t ft = get_filetime(ARG(0));
    time_t t = (time_t)(ft / 10000000ull) - (time_t)11644473600ll;
    struct tm tm;
    gmtime_r(&t, &tm);
    put_systemtime(ARG(1), &tm, (int)(ft / 10000 % 1000));
    return 1;
}

API(KERNEL32, SystemTimeToFileTime, 2)
{
    uint32_t s = ARG(0);
    struct tm tm = {0};
    tm.tm_year = R16(s) - 1900;
    tm.tm_mon = R16(s + 2) - 1;
    tm.tm_mday = R16(s + 6);
    tm.tm_hour = R16(s + 8);
    tm.tm_min = R16(s + 10);
    tm.tm_sec = R16(s + 12);
    time_t t = timegm(&tm);
    put_filetime(ARG(1), ((uint64_t)t + 11644473600ull) * 10000000ull + R16(s + 14) * 10000ull);
    return 1;
}

API(KERNEL32, FileTimeToLocalFileTime, 2)
{
    uint64_t ft = get_filetime(ARG(0));
    put_filetime(ARG(1), ft + tz_offset_100ns((time_t)(ft / 10000000ull - 11644473600ull)));
    return 1;
}

API(KERNEL32, LocalFileTimeToFileTime, 2)
{
    uint64_t ft = get_filetime(ARG(0));
    put_filetime(ARG(1), ft - tz_offset_100ns((time_t)(ft / 10000000ull - 11644473600ull)));
    return 1;
}

API(KERNEL32, GetTimeZoneInformation, 1)
{
    memset(gptr(ARG(0)), 0, 172);
    time_t t = time(NULL);
    W32(ARG(0), (uint32_t)(int32_t)(-tz_offset_100ns(t) / 600000000ll));   /* Bias in minutes */
    return 0;                                                               /* TIME_ZONE_ID_UNKNOWN */
}

/* ------------------------------------------------------------------ memory */
API(KERNEL32, GetProcessHeap, 0) { return process_heap; }
API(KERNEL32, HeapValidate, 3) { return 1; }
API(KERNEL32, HeapCompact, 2) { return 0x100000; }
API(KERNEL32, HeapWalk, 2) { set_last_error(259); return 0; }

API(KERNEL32, LocalAlloc, 2) { return heap_alloc(process_heap, ARG(1) ? ARG(1) : 1, (ARG(0) & 0x40) != 0); }
API(KERNEL32, LocalFree, 1) { if (ARG(0)) heap_free(process_heap, ARG(0)); return 0; }
API(KERNEL32, LocalLock, 1) { return ARG(0); }
API(KERNEL32, LocalUnlock, 1) { return 0; }
API(KERNEL32, LocalReAlloc, 3) { return heap_realloc(process_heap, ARG(0), ARG(1) ? ARG(1) : 1, (ARG(2) & 0x40) != 0, 0); }
API(KERNEL32, GlobalReAlloc, 3) { return heap_realloc(process_heap, ARG(0), ARG(1) ? ARG(1) : 1, (ARG(2) & 0x40) != 0, 0); }
API(KERNEL32, GlobalSize, 1) { return heap_size(process_heap, ARG(0)); }
API(KERNEL32, GlobalFlags, 1) { return 0; }

API(KERNEL32, VirtualProtect, 4)
{
    if (ARG(3)) W32(ARG(3), 0x04);      /* PAGE_READWRITE: all program memory is */
    return 1;
}

API(KERNEL32, VirtualQuery, 3)
{
    uint32_t a = ARG(0), b = ARG(1);
    memset(gptr(b), 0, 28);
    const ModuleEntry *m = module_at(a);
    uint32_t base = m ? m->base : vm_query_base(a);
    if (!base) {
        W32(b, a & ~0xfffu);
        W32(b + 12, 0x1000);
        W32(b + 16, 0x10000);           /* MEM_FREE */
        W32(b + 20, 0x01);
        return 28;
    }
    W32(b, a & ~0xfffu);
    W32(b + 4, base);
    W32(b + 8, 0x04);
    W32(b + 12, 0x1000);
    W32(b + 16, 0x1000);                /* MEM_COMMIT */
    W32(b + 20, 0x04);
    W32(b + 24, m ? 0x1000000 : 0x20000); /* MEM_IMAGE / MEM_PRIVATE */
    return 28;
}

/* ------------------------------------------------------------------ errors, misc */
API(KERNEL32, SetLastError, 1) { set_last_error(ARG(0)); return 0; }
API(KERNEL32, SetErrorMode, 1) { return 0; }
API(KERNEL32, DisableThreadLibraryCalls, 1) { return 1; }
API(KERNEL32, DebugBreak, 0) { LOG(LOG_ALWAYS, "DebugBreak from %08x (ignored)", R32(c->esp)); return 0; }
API(KERNEL32, Beep, 2) { return 1; }
API(KERNEL32, FatalExit, 1) { LOG(LOG_ALWAYS, "FatalExit(%d)", (int)ARG(0)); process_exit((int)ARG(0)); }

API(KERNEL32, MulDiv, 3)
{
    int32_t a = (int32_t)ARG(0), b = (int32_t)ARG(1), d = (int32_t)ARG(2);
    if (!d) return 0xffffffffu;
    int64_t p = (int64_t)a * b;
    int64_t r = (p + ((p < 0) != (d < 0) ? -d / 2 : d / 2)) / d;   /* rounded, as Windows does */
    if (r > INT32_MAX || r < INT32_MIN) return 0xffffffffu;
    return (uint32_t)(int32_t)r;
}

API(KERNEL32, GetEnvironmentVariableA, 3) { set_last_error(203); return 0; }
API(KERNEL32, GetEnvironmentVariableW, 3) { set_last_error(203); return 0; }
API(KERNEL32, SetEnvironmentVariableA, 2) { return 1; }
API(KERNEL32, SetEnvironmentVariableW, 2) { return 1; }

API(KERNEL32, GetCommandLineW, 0)
{
    static uint32_t w;
    if (!w) {
        char b[600];
        snprintf(b, sizeof b, "\"%s\\Binaries\\Retail-Stranglehold.exe\" -nowxwindows", virtual_game_root());
        w = galloc(2 * (strlen(b) + 1));
        gwput(w, b, strlen(b) + 1);
    }
    return w;
}

API(KERNEL32, FormatMessageA, 7)
{
    uint32_t flags = ARG(0), id = ARG(2), buf = ARG(4), n = ARG(5);
    char text[128];
    snprintf(text, sizeof text, "Error %u.\r\n", id);
    if (flags & 0x100) {                /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
        uint32_t p = heap_alloc(process_heap, strlen(text) + 1, 0);
        gstrncpy(p, text, strlen(text) + 1);
        W32(buf, p);
    } else
        gstrncpy(buf, text, n);
    return strlen(text);
}

/* ------------------------------------------------------------------ atoms */
static char *atoms[0x1000];
static pthread_mutex_t alock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t atom_find(const char *s, int add)
{
    if (s && s[0] == '#')
        return (uint32_t)atoi(s + 1);
    pthread_mutex_lock(&alock);
    for (int i = 1; i < 0x1000; i++)
        if (atoms[i] && !strcasecmp(atoms[i], s)) {
            pthread_mutex_unlock(&alock);
            return 0xc000u + i;
        }
    uint32_t r = 0;
    if (add)
        for (int i = 1; i < 0x1000; i++)
            if (!atoms[i]) {
                atoms[i] = strdup(s);
                r = 0xc000u + i;
                break;
            }
    pthread_mutex_unlock(&alock);
    return r;
}

uint32_t atom_add(const char *s) { return atom_find(s, 1); }

API(KERNEL32, GlobalAddAtomA, 1) { return ARG(0) < 0x10000 ? ARG(0) : atom_find(gstr(ARG(0)), 1); }
API(KERNEL32, GlobalFindAtomA, 1) { return ARG(0) < 0x10000 ? ARG(0) : atom_find(gstr(ARG(0)), 0); }
API(KERNEL32, GlobalDeleteAtom, 1) { return 0; }

API(KERNEL32, GlobalGetAtomNameA, 3)
{
    uint32_t a = ARG(0) & 0xffff;
    char b[300];
    if (a >= 0xc000 && a < 0xd000 && atoms[a - 0xc000])
        snprintf(b, sizeof b, "%s", atoms[a - 0xc000]);
    else
        snprintf(b, sizeof b, "#%u", a);
    gstrncpy(ARG(1), b, ARG(2));
    return strlen(gptr(ARG(1)));
}

/* ------------------------------------------------------------------ locale (US English) */
API(KERNEL32, GetUserDefaultLCID, 0) { return 0x409; }
API(KERNEL32, GetThreadLocale, 0) { return 0x409; }
API(KERNEL32, ConvertDefaultLocale, 1) { return 0x409; }
API(KERNEL32, IsValidLocale, 2) { return 1; }
API(KERNEL32, IsValidCodePage, 1) { return ARG(0) == 1252 || ARG(0) == 437 || ARG(0) == 65001 || ARG(0) == 850; }
API(KERNEL32, IsDBCSLeadByte, 1) { return 0; }
API(KERNEL32, IsDBCSLeadByteEx, 2) { return 0; }

static const char *locale_info(uint32_t type)
{
    switch (type & 0xffff) {
    case 0x0001: return "0409";          /* LOCALE_ILANGUAGE */
    case 0x0002: return "English (United States)";
    case 0x0003: return "ENU";
    case 0x1001: return "English";
    case 0x0004: return "English";
    case 0x0005: return "1";
    case 0x0006: return "United States";
    case 0x0007: return "USA";
    case 0x000b: return "437";           /* LOCALE_IDEFAULTCODEPAGE */
    case 0x1004: return "1252";          /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x000c: return ",";
    case 0x000e: return ".";
    case 0x000f: return ",";
    case 0x0010: return "3;0";
    case 0x0014: return "$";
    case 0x0016: return ".";
    case 0x0017: return ",";
    case 0x001d: return "/";
    case 0x001e: return ":";
    case 0x001f: return "M/d/yyyy";
    case 0x0020: return "dddd, MMMM dd, yyyy";
    case 0x1003: return "h:mm:ss tt";
    case 0x0028: return "AM";
    case 0x0029: return "PM";
    case 0x005a: return "en";
    case 0x005b: return "US";
    case 0x1009: return "0";             /* LOCALE_IFIRSTDAYOFWEEK? (ok) */
    default: return NULL;
    }
}

API(KERNEL32, GetLocaleInfoA, 4)
{
    const char *v = locale_info(ARG(1));
    if (!v) { LOG(LOG_API, "GetLocaleInfoA(%x, %x) unknown", ARG(0), ARG(1)); v = ""; }
    uint32_t n = strlen(v) + 1;
    if (!ARG(3)) return n;
    if (ARG(3) < n) { set_last_error(122); return 0; }
    gstrncpy(ARG(2), v, ARG(3));
    return n;
}

API(KERNEL32, GetLocaleInfoW, 4)
{
    const char *v = locale_info(ARG(1));
    if (!v) v = "";
    uint32_t n = strlen(v) + 1;
    if (!ARG(3)) return n;
    if (ARG(3) < n) { set_last_error(122); return 0; }
    gwput(ARG(2), v, ARG(3));
    return n;
}

API(KERNEL32, EnumSystemLocalesA, 2)
{
    uint32_t s = gstrdup("00000409");
    guest_call(c, ARG(0), 1, s);
    return 1;
}

static int cmp_strings(const char *a, int na, const char *b, int nb, uint32_t flags)
{
    size_t la = na < 0 ? strlen(a) : (size_t)na, lb = nb < 0 ? strlen(b) : (size_t)nb;
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) {
        int x = (uint8_t)a[i], y = (uint8_t)b[i];
        if (flags & 1) { x = tolower(x); y = tolower(y); }     /* NORM_IGNORECASE */
        if (x != y) return x < y ? 1 : 3;
    }
    return la == lb ? 2 : la < lb ? 1 : 3;     /* CSTR_LESS_THAN / EQUAL / GREATER_THAN */
}

API(KERNEL32, CompareStringA, 6)
{
    return cmp_strings(gstr(ARG(2)), (int)ARG(3), gstr(ARG(4)), (int)ARG(5), ARG(1));
}

API(KERNEL32, CompareStringW, 6)
{
    char a[2048], b[2048];
    int na = (int)ARG(3), nb = (int)ARG(5);
    gwstr(ARG(2), a, sizeof a);
    gwstr(ARG(4), b, sizeof b);
    if (na >= 0 && na < (int)sizeof a) a[na] = 0;
    if (nb >= 0 && nb < (int)sizeof b) b[nb] = 0;
    return cmp_strings(a, -1, b, -1, ARG(1));
}

API(KERNEL32, GetStringTypeExA, 5)
{
    /* CT_CTYPE1 classes of each character */
    const char *s = gstr(ARG(2));
    int n = (int)ARG(3);
    if (n < 0) n = strlen(s) + 1;
    for (int i = 0; i < n; i++) {
        int ch = (uint8_t)s[i];
        uint16_t t = 0;
        if (isupper(ch)) t |= 0x001;
        if (islower(ch)) t |= 0x002;
        if (isdigit(ch)) t |= 0x004;
        if (isspace(ch)) t |= 0x008;
        if (ispunct(ch)) t |= 0x010;
        if (iscntrl(ch)) t |= 0x020;
        if (ch == ' ') t |= 0x040;
        if (isxdigit(ch)) t |= 0x080;
        if (isalpha(ch)) t |= 0x100;
        W16(ARG(4) + 2 * i, t);
    }
    return 1;
}

API(KERNEL32, GetStringTypeExW, 5)
{
    int n = (int)ARG(3);
    if (n < 0) { n = 0; while (R16(ARG(2) + 2 * n)) n++; n++; }
    for (int i = 0; i < n; i++) {
        uint32_t ch = R16(ARG(2) + 2 * i);
        uint16_t t = 0;
        if (ch < 0x80) {
            if (isupper(ch)) t |= 0x001;
            if (islower(ch)) t |= 0x002;
            if (isdigit(ch)) t |= 0x004;
            if (isspace(ch)) t |= 0x008;
            if (ispunct(ch)) t |= 0x010;
            if (iscntrl(ch)) t |= 0x020;
            if (ch == ' ') t |= 0x040;
            if (isxdigit(ch)) t |= 0x080;
            if (isalpha(ch)) t |= 0x100;
        } else
            t = 0x100;
        W16(ARG(4) + 2 * i, t);
    }
    return 1;
}

static int format_date(char *out, size_t n, const struct tm *tm, int timefmt)
{
    if (timefmt)
        return snprintf(out, n, "%d:%02d:%02d %s", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, tm->tm_min,
                        tm->tm_sec, tm->tm_hour < 12 ? "AM" : "PM");
    return snprintf(out, n, "%d/%d/%d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900);
}

static void tm_from(uint32_t st, struct tm *tm)
{
    time_t t = time(NULL);
    localtime_r(&t, tm);
    if (st) {
        tm->tm_year = R16(st) - 1900; tm->tm_mon = R16(st + 2) - 1; tm->tm_mday = R16(st + 6);
        tm->tm_hour = R16(st + 8); tm->tm_min = R16(st + 10); tm->tm_sec = R16(st + 12);
    }
}

API(KERNEL32, GetDateFormatA, 6)
{
    struct tm tm;
    char b[64];
    tm_from(ARG(2), &tm);
    int n = format_date(b, sizeof b, &tm, 0) + 1;
    if (!ARG(5)) return n;
    gstrncpy(ARG(4), b, ARG(5));
    return n;
}

API(KERNEL32, GetTimeFormatA, 6)
{
    struct tm tm;
    char b[64];
    tm_from(ARG(2), &tm);
    int n = format_date(b, sizeof b, &tm, 1) + 1;
    if (!ARG(5)) return n;
    gstrncpy(ARG(4), b, ARG(5));
    return n;
}

/* ------------------------------------------------------------------ strings */
API(KERNEL32, lstrcatA, 2) { strcat(gptr(ARG(0)), gstr(ARG(1))); return ARG(0); }
API(KERNEL32, lstrcmpiA, 2) { int r = strcasecmp(gstr(ARG(0)), gstr(ARG(1))); return (uint32_t)(r < 0 ? -1 : r > 0); }

API(KERNEL32, lstrcpynA, 3)
{
    if (!ARG(2)) return ARG(0);
    gstrncpy(ARG(0), gstr(ARG(1)), ARG(2));
    return ARG(0);
}

API(KERNEL32, lstrlenW, 1)
{
    uint32_t n = 0;
    if (ARG(0)) while (R16(ARG(0) + 2 * n)) n++;
    return n;
}

API(KERNEL32, lstrcpyW, 2)
{
    uint32_t i = 0;
    do W16(ARG(0) + 2 * i, R16(ARG(1) + 2 * i)); while (R16(ARG(1) + 2 * i++));
    return ARG(0);
}

API(KERNEL32, lstrcpynW, 3)
{
    uint32_t n = ARG(2), i = 0;
    if (!n) return ARG(0);
    for (; i + 1 < n && R16(ARG(1) + 2 * i); i++)
        W16(ARG(0) + 2 * i, R16(ARG(1) + 2 * i));
    W16(ARG(0) + 2 * i, 0);
    return ARG(0);
}

static int wcmp(uint32_t a, uint32_t b, int icase)
{
    for (uint32_t i = 0;; i++) {
        uint32_t x = R16(a + 2 * i), y = R16(b + 2 * i);
        if (icase) { if (x < 0x80) x = tolower(x); if (y < 0x80) y = tolower(y); }
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}

API(KERNEL32, lstrcmpW, 2) { return (uint32_t)wcmp(ARG(0), ARG(1), 0); }
API(KERNEL32, lstrcmpiW, 2) { return (uint32_t)wcmp(ARG(0), ARG(1), 1); }

/* ------------------------------------------------------------------ console: none */
API(KERNEL32, GetConsoleCP, 0) { return 437; }
API(KERNEL32, GetConsoleOutputCP, 0) { return 437; }
API(KERNEL32, GetConsoleMode, 2) { set_last_error(6); return 0; }
API(KERNEL32, SetConsoleMode, 2) { set_last_error(6); return 0; }
API(KERNEL32, SetConsoleCtrlHandler, 2) { return 1; }
API(KERNEL32, GetNumberOfConsoleInputEvents, 2) { set_last_error(6); return 0; }
API(KERNEL32, PeekConsoleInputA, 4) { set_last_error(6); return 0; }
API(KERNEL32, ReadConsoleInputA, 4) { set_last_error(6); return 0; }
API(KERNEL32, ReadConsoleInputW, 4) { set_last_error(6); return 0; }
API(KERNEL32, ReadConsoleA, 5) { set_last_error(6); return 0; }
API(KERNEL32, ReadConsoleW, 5) { set_last_error(6); return 0; }

API(KERNEL32, WriteConsoleA, 5)
{
    LOG(LOG_ALWAYS, "console: %.*s", (int)ARG(2), gstr(ARG(1)));
    if (ARG(3)) W32(ARG(3), ARG(2));
    return 1;
}

API(KERNEL32, WriteConsoleW, 5)
{
    char b[2048];
    gwstr(ARG(1), b, sizeof b);
    if (ARG(2) < sizeof b) b[ARG(2)] = 0;
    LOG(LOG_ALWAYS, "console: %s", b);
    if (ARG(3)) W32(ARG(3), ARG(2));
    return 1;
}

/* ------------------------------------------------------------------ processes, pipes: none */
API(KERNEL32, CreateProcessA, 10) { LOG(LOG_ALWAYS, "CreateProcessA(\"%s\", \"%s\") refused", gstr(ARG(0)), gstr(ARG(1))); set_last_error(2); return 0; }
API(KERNEL32, CreateProcessW, 10) { LOG(LOG_ALWAYS, "CreateProcessW refused"); set_last_error(2); return 0; }
API(KERNEL32, GetExitCodeProcess, 2) { if (ARG(1)) W32(ARG(1), 0); return 1; }
API(KERNEL32, CreatePipe, 4) { set_last_error(5); return 0; }
API(KERNEL32, PeekNamedPipe, 6) { set_last_error(6); return 0; }
