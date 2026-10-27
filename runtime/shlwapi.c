/* SHLWAPI: the path helpers MFC uses. SHELL32: special folders, folder creation, and
   ShellExecute (nothing is launched). */
#include <strings.h>
#include "runtime.h"

static uint32_t file_name_of(uint32_t p)
{
    uint32_t r = p;
    for (uint32_t q = p; R8(q); q++)
        if ((R8(q) == '\\' || R8(q) == '/' || R8(q) == ':') && R8(q + 1))
            r = q + 1;
    return r;
}

API(SHLWAPI, PathFindFileNameA, 1) { return ARG(0) ? file_name_of(ARG(0)) : 0; }

API(SHLWAPI, PathFindExtensionA, 1)
{
    if (!ARG(0)) return 0;
    uint32_t dot = 0, q = file_name_of(ARG(0));
    for (; R8(q); q++) {
        if (R8(q) == '.') dot = q;
        else if (R8(q) == ' ') { /* spaces keep the extension */ }
    }
    return dot ? dot : q;                       /* the terminating zero when there is none */
}

API(SHLWAPI, PathRemoveExtensionA, 1)
{
    if (!ARG(0)) return 0;
    uint32_t q = file_name_of(ARG(0)), dot = 0;
    for (; R8(q); q++) if (R8(q) == '.') dot = q;
    if (dot) W8(dot, 0);
    return 0;
}

API(SHLWAPI, PathIsUNCA, 1) { return ARG(0) && R8(ARG(0)) == '\\' && R8(ARG(0) + 1) == '\\'; }

API(SHLWAPI, PathStripToRootA, 1)
{
    char *p = gptr(ARG(0));
    if (p[0] && p[1] == ':') { p[2] = p[2] == '\\' ? '\\' : 0; if (p[2]) p[3] = 0; return 1; }
    if (p[0] == '\\') { p[1] = p[1] == '\\' ? p[1] : 0; return 1; }
    p[0] = 0;
    return 0;
}

API(SHLWAPI, UrlUnescapeA, 4) { return 0; }

/* ------------------------------------------------------------------ SHELL32 */
/* Special folders as on Windows XP, for one user "Player"; outside C:\Stranglehold they lie in
   the user folder's drive_c (files.c). */
extern uint32_t create_dir_tree(const char *win);

static const char *special_folder(uint32_t csidl)
{
    switch (csidl & 0xff) {
    case 0x00: case 0x10: return "C:\\Documents and Settings\\Player\\Desktop";
    case 0x05: return "C:\\Documents and Settings\\Player\\My Documents";            /* CSIDL_PERSONAL */
    case 0x27: return "C:\\Documents and Settings\\Player\\My Documents\\My Pictures";
    case 0x1a: return "C:\\Documents and Settings\\Player\\Application Data";        /* CSIDL_APPDATA */
    case 0x1c: return "C:\\Documents and Settings\\Player\\Local Settings\\Application Data";
    case 0x23: return "C:\\Documents and Settings\\All Users\\Application Data";     /* CSIDL_COMMON_APPDATA */
    case 0x2e: return "C:\\Documents and Settings\\All Users\\Documents";            /* CSIDL_COMMON_DOCUMENTS */
    case 0x24: return "C:\\WINDOWS";
    case 0x25: case 0x29: return "C:\\WINDOWS\\system32";
    case 0x14: return "C:\\WINDOWS\\Fonts";
    case 0x26: return "C:\\Program Files";
    case 0x2b: return "C:\\Program Files\\Common Files";
    default: return NULL;
    }
}

API(SHELL32, SHGetFolderPathW, 5)
{
    uint32_t csidl = ARG(1), out = ARG(4);
    const char *p = special_folder(csidl);
    LOG(LOG_FILE, "SHGetFolderPathW(%x) -> %s", csidl, p ? p : "(none)");
    if (!p) { W16(out, 0); return 0x80070057u; }                    /* E_INVALIDARG */
    if (csidl & 0x8000) create_dir_tree(p);                         /* CSIDL_FLAG_CREATE */
    gwput(out, p, 260);
    return 0;
}

API(SHELL32, SHCreateDirectoryExW, 3)
{
    char b[1024];
    return create_dir_tree(gwstr(ARG(1), b, sizeof b));
}

/* opening documents or web pages: nothing is started, the program is told it was (33 > 32) */
API(SHELL32, ShellExecuteA, 6)
{
    LOG(LOG_ALWAYS, "ShellExecuteA(\"%s\", \"%s\", \"%s\"): not done", gstr(ARG(1)) ? gstr(ARG(1)) : "open",
        gstr(ARG(2)) ? gstr(ARG(2)) : "", gstr(ARG(3)) ? gstr(ARG(3)) : "");
    return 33;
}

API(SHELL32, ShellExecuteW, 6)
{
    char op[64], file[1024], params[1024];
    LOG(LOG_ALWAYS, "ShellExecuteW(\"%s\", \"%s\", \"%s\"): not done", ARG(1) ? gwstr(ARG(1), op, sizeof op) : "open",
        gwstr(ARG(2), file, sizeof file), gwstr(ARG(3), params, sizeof params));
    return 33;
}
