/* Resources in the mapped Game.exe (.rsrc): bitmaps, icons, dialogs, version info. */
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include "runtime.h"

static uint32_t rsrc_of(uint32_t base)
{
    uint32_t pe = base + R32(base + 0x3c);
    uint32_t rva = R32(pe + 24 + 96 + 8 * 2);
    return rva ? base + rva : 0;
}

/* does directory entry `name` match the wanted id or string ("#123" counts as id 123)? */
static int name_matches(uint32_t root, uint32_t name, uint32_t want)
{
    if (want < 0x10000)
        return !(name & 0x80000000u) && name == want;
    const char *w = gptr(want);
    if (w[0] == '#')
        return !(name & 0x80000000u) && name == (uint32_t)atoi(w + 1);
    if (!(name & 0x80000000u))
        return 0;
    uint32_t str = root + (name & 0x7fffffffu);
    uint32_t len = R16(str);
    if (strlen(w) != len) return 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t ch = R16(str + 2 + 2 * i);
        if (ch >= 0x80 || toupper((unsigned char)w[i]) != toupper((int)ch)) return 0;
    }
    return 1;
}

static uint32_t dir_find(uint32_t root, uint32_t dir, uint32_t want, int any)
{
    uint32_t named = R16(dir + 12), ids = R16(dir + 14);
    for (uint32_t i = 0; i < named + ids; i++) {
        uint32_t e = dir + 16 + 8 * i;
        if (any || name_matches(root, R32(e), want))
            return R32(e + 4);
    }
    return 0xffffffffu;
}

/* the IMAGE_RESOURCE_DATA_ENTRY of (type, name[, language]) in the module at base, or 0 */
uint32_t resource_entry(uint32_t base, uint32_t type, uint32_t name, int lang)
{
    uint32_t root = rsrc_of(base);
    if (!root) return 0;
    uint32_t o = dir_find(root, root, type, 0);
    if (o == 0xffffffffu || !(o & 0x80000000u)) return 0;
    o = dir_find(root, root + (o & 0x7fffffffu), name, 0);
    if (o == 0xffffffffu || !(o & 0x80000000u)) return 0;
    uint32_t ldir = root + (o & 0x7fffffffu);
    uint32_t l = lang >= 0 ? dir_find(root, ldir, (uint32_t)lang, 0) : 0xffffffffu;
    if (l == 0xffffffffu)
        l = dir_find(root, ldir, 0, 1);                         /* first language */
    if (l == 0xffffffffu || (l & 0x80000000u)) return 0;
    return root + l;
}

int resource_find(uint32_t type, uint32_t name, uint32_t *data, uint32_t *size)
{
    uint32_t de = resource_entry(image_base, type, name, -1);
    if (!de) return 0;
    *data = image_base + R32(de);
    *size = R32(de + 4);
    return 1;
}

static uint32_t res_module(uint32_t h) { return h ? h : image_base; }

static uint32_t find_res(uint32_t mod, uint32_t name, uint32_t type, int lang)
{
    mod = res_module(mod);
    uint32_t r = module_at(mod) ? resource_entry(mod, type, name, lang) : 0;
    LOG(LOG_API, "FindResource(%x, %s%x, type %x) -> %x", mod, name >= 0x10000 ? gptr(name) : "#", name < 0x10000 ? name : 0, type, r);
    if (!r) set_last_error(1814);       /* ERROR_RESOURCE_NAME_NOT_FOUND */
    return r;
}

API(KERNEL32, FindResourceA, 3) { return find_res(ARG(0), ARG(1), ARG(2), -1); }
API(KERNEL32, FindResourceExA, 4) { return find_res(ARG(0), ARG(2), ARG(1), (int)(ARG(3) & 0xffff)); }

API(KERNEL32, LoadResource, 2)
{
    uint32_t de = ARG(1);
    const ModuleEntry *m = module_at(de);
    return de && m ? m->base + R32(de) : 0;
}

API(KERNEL32, LockResource, 1) { return ARG(0); }
API(KERNEL32, FreeResource, 1) { return 0; }
API(KERNEL32, SizeofResource, 2) { return ARG(1) ? R32(ARG(1) + 4) : 0; }

API(KERNEL32, EnumResourceLanguagesA, 5)
{
    uint32_t mod = res_module(ARG(0));
    uint32_t de = resource_entry(mod, ARG(1), ARG(2), -1);
    if (!de) return 0;
    guest_call(c, ARG(3), 5, mod, ARG(1), ARG(2), 0x409u, ARG(4));
    return 1;
}

/* ------------------------------------------------------------------ VERSION.dll
   Only Game.exe's own version block is available (the game reads its version number). */
static int version_block(const char *path, uint32_t *data, uint32_t *size)
{
    const char *base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    if (strcasecmp(base, "Game.exe") && strcasecmp(base, "Game.exe\""))
        return 0;
    return resource_find(16, 1, data, size);
}

API(VERSION, GetFileVersionInfoSizeA, 2)
{
    uint32_t d, n;
    if (ARG(1)) W32(ARG(1), 0);
    int ok = version_block(gstr(ARG(0)), &d, &n);
    LOG(LOG_API, "GetFileVersionInfoSizeA(\"%s\") -> %u", gstr(ARG(0)), ok ? n : 0);
    return ok ? n : 0;
}

API(VERSION, GetFileVersionInfoA, 4)
{
    uint32_t d, n;
    if (!version_block(gstr(ARG(0)), &d, &n))
        return 0;
    memcpy(gptr(ARG(3)), gptr(d), n < ARG(2) ? n : ARG(2));
    return 1;
}

API(VERSION, VerQueryValueA, 4)
{
    const char *q = gstr(ARG(1));
    uint32_t blk = ARG(0);
    if (!strcmp(q, "\\")) {
        /* VS_VERSIONINFO: wLength, wValueLength, wType, L"VS_VERSION_INFO", pad -> VS_FIXEDFILEINFO */
        uint32_t ffi = blk + 40;
        if (R32(ffi) != 0xfeef04bdu) return 0;
        W32(ARG(2), ffi);
        W32(ARG(3), R16(blk + 2));
        return 1;
    }
    LOG(LOG_ALWAYS, "VerQueryValueA(\"%s\") not supported", q);
    return 0;
}
