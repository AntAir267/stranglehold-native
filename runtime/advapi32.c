/* ADVAPI32: the registry, kept in a text file in the user folder (registry.txt), and the
   user name. (LA Rush's installer keys were set here; Stranglehold needs none known yet)
   (the movies are in the install folder: no disc asked for), IsCDROMVersion = 0 and MovieCDNo. */
#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include "runtime.h"

typedef struct { char key[256]; char name[128]; uint32_t type; uint32_t size; uint8_t *data; } Value;
static Value *vals;
static int nvals, capvals, dirty;
static pthread_mutex_t rlock = PTHREAD_MUTEX_INITIALIZER;

#define MAXKEYS 256
static char keys[MAXKEYS][256];        /* open key handles -> full path ("HKLM\...") */

static const char *root_name(uint32_t h)
{
    switch (h) {
    case 0x80000000u: return "HKCR";
    case 0x80000001u: return "HKCU";
    case 0x80000002u: return "HKLM";
    case 0x80000003u: return "HKU";
    default: return NULL;
    }
}

static const char *key_path(uint32_t h)
{
    const char *r = root_name(h);
    if (r) return r;
    uint32_t i = h - 0x5000u;
    if (i < MAXKEYS && keys[i][0]) return keys[i];
    return NULL;
}

static Value *find(const char *key, const char *name)
{
    for (int i = 0; i < nvals; i++)
        if (!strcasecmp(vals[i].key, key) && !strcasecmp(vals[i].name, name))
            return &vals[i];
    return NULL;
}

static void set(const char *key, const char *name, uint32_t type, const void *data, uint32_t size)
{
    Value *v = find(key, name);
    if (!v) {
        if (nvals == capvals) {
            capvals = capvals ? capvals * 2 : 64;
            vals = realloc(vals, capvals * sizeof *vals);
        }
        v = &vals[nvals++];
        memset(v, 0, sizeof *v);
        snprintf(v->key, sizeof v->key, "%s", key);
        snprintf(v->name, sizeof v->name, "%s", name);
    }
    free(v->data);
    v->type = type;
    v->size = size;
    v->data = malloc(size ? size : 1);
    memcpy(v->data, data, size);
    dirty = 1;
}

/* registry.txt: one value per line: key<TAB>name<TAB>type<TAB>hex bytes */
void settings_load(void)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/registry.txt", user_dir);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[8192];
        while (fgets(line, sizeof line, f)) {
            char *k = strtok(line, "\t"), *n = strtok(NULL, "\t"), *t = strtok(NULL, "\t"), *h = strtok(NULL, "\t\r\n");
            if (!k || !n || !t) continue;
            uint8_t buf[4096];
            uint32_t sz = 0;
            for (char *p = h; p && p[0] && p[1] && sz < sizeof buf; p += 2) {
                unsigned b;
                sscanf(p, "%2x", &b);
                buf[sz++] = (uint8_t)b;
            }
            set(k, !strcmp(n, "@") ? "" : n, strtoul(t, NULL, 10), buf, sz);
        }
        fclose(f);
    }
    dirty = 0;
}

/* Host-side access for the Enhanced page (the language choice). */
int registry_get_str(const char *key, const char *name, char *out, int n)
{
    pthread_mutex_lock(&rlock);
    Value *v = find(key, name);
    if (v && n > 0)
        snprintf(out, (size_t)n, "%.*s", (int)v->size, (const char *)v->data);
    pthread_mutex_unlock(&rlock);
    return v != NULL;
}

void registry_set_str(const char *key, const char *name, const char *s)
{
    pthread_mutex_lock(&rlock);
    set(key, name, 1, s, (uint32_t)strlen(s) + 1);
    pthread_mutex_unlock(&rlock);
}

void settings_save(void)
{
    pthread_mutex_lock(&rlock);
    if (dirty) {
        char path[1200], tmp[1300];
        snprintf(path, sizeof path, "%s/registry.txt", user_dir);
        snprintf(tmp, sizeof tmp, "%s.new", path);
        FILE *f = fopen(tmp, "w");
        if (f) {
            for (int i = 0; i < nvals; i++) {
                fprintf(f, "%s\t%s\t%u\t", vals[i].key, vals[i].name[0] ? vals[i].name : "@", vals[i].type);
                for (uint32_t k = 0; k < vals[i].size; k++)
                    fprintf(f, "%02x", vals[i].data[k]);
                fprintf(f, "\n");
            }
            fclose(f);
            rename(tmp, path);
            dirty = 0;
        }
    }
    pthread_mutex_unlock(&rlock);
}

/* The runtime's own access to the store (runtime.h) */
uint32_t settings_get(const char *key, const char *name, uint32_t *type, void *buf, uint32_t cap)
{
    pthread_mutex_lock(&rlock);
    Value *v = find(key, name);
    uint32_t n = v ? v->size : 0;
    if (v) {
        if (type) *type = v->type;
        if (buf) memcpy(buf, v->data, n < cap ? n : cap);
    }
    pthread_mutex_unlock(&rlock);
    return n;
}

int settings_has(const char *key, const char *name)
{
    pthread_mutex_lock(&rlock);
    int r = find(key, name) != NULL;
    pthread_mutex_unlock(&rlock);
    return r;
}

void settings_put(const char *key, const char *name, uint32_t type, const void *data, uint32_t size)
{
    pthread_mutex_lock(&rlock);
    set(key, name, type, data, size);
    pthread_mutex_unlock(&rlock);
}

void settings_delete(const char *key, const char *name)
{
    pthread_mutex_lock(&rlock);
    Value *v = find(key, name);
    if (v) {
        free(v->data);
        *v = vals[--nvals];
        dirty = 1;
    }
    pthread_mutex_unlock(&rlock);
}

static uint32_t open_key(uint32_t parent, const char *sub, uint32_t out)
{
    const char *base = key_path(parent);
    if (!base) return 6;
    char full[256];
    snprintf(full, sizeof full, "%s%s%s", base, sub && *sub ? "\\" : "", sub ? sub : "");
    for (char *p = full; *p; p++) if (*p == '/') *p = '\\';
    pthread_mutex_lock(&rlock);
    int slot = -1;
    for (int i = 0; i < MAXKEYS; i++)
        if (!keys[i][0]) { slot = i; break; }
    if (slot >= 0)
        snprintf(keys[slot], sizeof keys[slot], "%s", full);
    pthread_mutex_unlock(&rlock);
    if (slot < 0) return 14;
    W32(out, 0x5000u + slot);
    return 0;
}

static int key_known(const char *full)
{
    size_t n = strlen(full);
    for (int i = 0; i < nvals; i++)
        if (!strncasecmp(vals[i].key, full, n) && (vals[i].key[n] == 0 || vals[i].key[n] == '\\'))
            return 1;
    return 0;
}

static uint32_t reg_open(uint32_t parent, const char *sub, uint32_t out)
{
    const char *base = key_path(parent);
    char full[256];
    snprintf(full, sizeof full, "%s%s%s", base ? base : "?", sub && *sub ? "\\" : "", sub ? sub : "");
    pthread_mutex_lock(&rlock);
    int known = key_known(full);
    pthread_mutex_unlock(&rlock);
    uint32_t r = known ? open_key(parent, sub, out) : 2;
    LOG(LOG_REG, "RegOpenKeyEx(%s) -> %u", full, r);
    return r;
}

API(ADVAPI32, RegOpenKeyExA, 5) { return reg_open(ARG(0), gstr(ARG(1)), ARG(4)); }
API(ADVAPI32, RegOpenKeyExW, 5)
{
    char b[256];
    return reg_open(ARG(0), ARG(1) ? gwstr(ARG(1), b, sizeof b) : NULL, ARG(4));
}

static uint32_t reg_create(uint32_t parent, const char *sub, uint32_t out, uint32_t disp)
{
    uint32_t r = open_key(parent, sub, out);
    if (disp) W32(disp, 2);              /* REG_OPENED_EXISTING_KEY */
    LOG(LOG_REG, "RegCreateKeyEx(%s\\%s) -> %u", key_path(parent), sub ? sub : "", r);
    return r;
}

API(ADVAPI32, RegCreateKeyExA, 9) { return reg_create(ARG(0), gstr(ARG(1)), ARG(7), ARG(8)); }
API(ADVAPI32, RegCreateKeyExW, 9)
{
    char b[256];
    return reg_create(ARG(0), ARG(1) ? gwstr(ARG(1), b, sizeof b) : NULL, ARG(7), ARG(8));
}

API(ADVAPI32, RegCloseKey, 1)
{
    uint32_t i = ARG(0) - 0x5000u;
    if (i < MAXKEYS) keys[i][0] = 0;
    return 0;
}

API(ADVAPI32, RegFlushKey, 1)
{
    settings_save();
    return 0;
}

/* string values (REG_SZ, REG_EXPAND_SZ, REG_MULTI_SZ) are kept as 8-bit text: the W functions
   convert them to and from UTF-16 */
static int is_string_type(uint32_t t) { return t == 1 || t == 2 || t == 7; }

static uint32_t reg_query(uint32_t hkey, const char *name, uint32_t typep, uint32_t data, uint32_t sizep, int wide)
{
    const char *key = key_path(hkey);
    if (!name) name = "";
    if (!key) return 6;
    pthread_mutex_lock(&rlock);
    Value *v = find(key, name);
    uint32_t r;
    if (!v) {
        r = 2;
    } else {
        int conv = wide && is_string_type(v->type);
        uint32_t size = conv ? 2 * v->size : v->size;
        if (typep) W32(typep, v->type);
        uint32_t cap = sizep ? R32(sizep) : 0;
        if (data && cap < size) r = 234;
        else {
            if (data && conv)
                for (uint32_t i = 0; i < v->size; i++) W16(data + 2 * i, v->data[i]);
            else if (data)
                memcpy(gptr(data), v->data, v->size);
            r = 0;
        }
        if (sizep) W32(sizep, size);
    }
    pthread_mutex_unlock(&rlock);
    LOG(LOG_REG, "RegQueryValueEx%s(%s, %s) -> %u", wide ? "W" : "A", key, name, r);
    return r;
}

API(ADVAPI32, RegQueryValueExA, 6) { return reg_query(ARG(0), gstr(ARG(1)), ARG(3), ARG(4), ARG(5), 0); }
API(ADVAPI32, RegQueryValueExW, 6)
{
    char b[128];
    return reg_query(ARG(0), ARG(1) ? gwstr(ARG(1), b, sizeof b) : NULL, ARG(3), ARG(4), ARG(5), 1);
}

static uint32_t reg_set(uint32_t hkey, const char *name, uint32_t type, uint32_t data, uint32_t size, int wide)
{
    const char *key = key_path(hkey);
    if (!key) return 6;
    if (!name) name = "";
    pthread_mutex_lock(&rlock);
    if (wide && is_string_type(type)) {
        uint8_t *t = malloc(size / 2 + 1);
        for (uint32_t i = 0; i < size / 2; i++) {
            uint32_t ch = R16(data + 2 * i);
            t[i] = ch < 0x100 ? (uint8_t)ch : '?';
        }
        set(key, name, type, t, size / 2);
        free(t);
    } else
        set(key, name, type, gptr(data), size);
    pthread_mutex_unlock(&rlock);
    LOG(LOG_REG, "RegSetValueEx%s(%s, %s, type %u, %u bytes)", wide ? "W" : "A", key, name, type, size);
    return 0;
}

API(ADVAPI32, RegSetValueExA, 6) { return reg_set(ARG(0), gstr(ARG(1)), ARG(3), ARG(4), ARG(5), 0); }
API(ADVAPI32, RegSetValueExW, 6)
{
    char b[128];
    return reg_set(ARG(0), ARG(1) ? gwstr(ARG(1), b, sizeof b) : NULL, ARG(3), ARG(4), ARG(5), 1);
}

API(ADVAPI32, RegDeleteValueA, 2)
{
    const char *key = key_path(ARG(0));
    if (!key) return 6;
    const char *name = ARG(1) ? gstr(ARG(1)) : "";
    pthread_mutex_lock(&rlock);
    int had = find(key, name) != NULL;
    pthread_mutex_unlock(&rlock);
    if (had) settings_delete(key, name);
    LOG(LOG_REG, "RegDeleteValueA(%s, %s) -> %u", key, name, had ? 0 : 2);
    return had ? 0 : 2;
}

/* the key and every value below it */
API(ADVAPI32, RegDeleteKeyA, 2)
{
    const char *base = key_path(ARG(0));
    const char *sub = gstr(ARG(1));
    if (!base) return 6;
    char full[256];
    snprintf(full, sizeof full, "%s%s%s", base, sub && *sub ? "\\" : "", sub ? sub : "");
    size_t n = strlen(full);
    int found = 0;
    pthread_mutex_lock(&rlock);
    for (int i = 0; i < nvals; ) {
        if (!strncasecmp(vals[i].key, full, n) && (vals[i].key[n] == 0 || vals[i].key[n] == '\\')) {
            free(vals[i].data);
            vals[i] = vals[--nvals];
            found = dirty = 1;
        } else
            i++;
    }
    pthread_mutex_unlock(&rlock);
    LOG(LOG_REG, "RegDeleteKeyA(%s) -> %u", full, found ? 0 : 2);
    return found ? 0 : 2;
}

/* the index-th direct subkey of key (in the order first seen), into out; 0 if there is none */
static int subkey_at(const char *key, uint32_t index, char *out, size_t n, uint32_t *count, uint32_t *longest)
{
    size_t kl = strlen(key);
    char seen[64][128];
    uint32_t ns = 0, maxl = 0;
    int got = 0;
    for (int i = 0; i < nvals && ns < 64; i++) {
        const char *k = vals[i].key;
        if (strncasecmp(k, key, kl) || k[kl] != '\\') continue;
        char sub[128];
        snprintf(sub, sizeof sub, "%s", k + kl + 1);
        char *bs = strchr(sub, '\\');
        if (bs) *bs = 0;
        uint32_t j = 0;
        while (j < ns && strcasecmp(seen[j], sub)) j++;
        if (j < ns) continue;
        snprintf(seen[ns], sizeof seen[ns], "%s", sub);
        if (strlen(sub) > maxl) maxl = strlen(sub);
        if (ns == index) { snprintf(out, n, "%s", sub); got = 1; }
        ns++;
    }
    if (count) *count = ns;
    if (longest) *longest = maxl;
    return got;
}

static uint32_t reg_enum_key(uint32_t hkey, uint32_t index, uint32_t name, uint32_t cap)
{
    const char *key = key_path(hkey);
    if (!key) return 6;
    char sub[128];
    pthread_mutex_lock(&rlock);
    int got = subkey_at(key, index, sub, sizeof sub, NULL, NULL);
    pthread_mutex_unlock(&rlock);
    LOG(LOG_REG, "RegEnumKey(%s, %u) -> %s", key, index, got ? sub : "(no more)");
    if (!got) return 259;                /* ERROR_NO_MORE_ITEMS */
    if (strlen(sub) + 1 > cap) return 234;
    gstrncpy(name, sub, cap);
    return 0;
}

API(ADVAPI32, RegEnumKeyA, 4) { return reg_enum_key(ARG(0), ARG(1), ARG(2), ARG(3)); }

API(ADVAPI32, RegEnumKeyExA, 8)
{
    uint32_t sizep = ARG(3);
    uint32_t r = reg_enum_key(ARG(0), ARG(1), ARG(2), R32(sizep));
    if (!r) W32(sizep, strlen(gptr(ARG(2))));
    if (ARG(5) && ARG(6) && R32(ARG(6))) W8(ARG(5), 0);      /* no class */
    if (ARG(6)) W32(ARG(6), 0);
    if (ARG(7)) { W32(ARG(7), 0); W32(ARG(7) + 4, 0); }
    return r;
}

API(ADVAPI32, RegQueryInfoKeyA, 12)
{
    const char *key = key_path(ARG(0));
    if (!key) return 6;
    char sub[128];
    uint32_t nsub = 0, maxsub = 0, nv = 0, maxname = 0, maxdata = 0;
    pthread_mutex_lock(&rlock);
    subkey_at(key, 0xffffffffu, sub, sizeof sub, &nsub, &maxsub);
    for (int i = 0; i < nvals; i++)
        if (!strcasecmp(vals[i].key, key)) {
            nv++;
            if (strlen(vals[i].name) > maxname) maxname = strlen(vals[i].name);
            if (vals[i].size > maxdata) maxdata = vals[i].size;
        }
    pthread_mutex_unlock(&rlock);
    if (ARG(1) && ARG(2) && R32(ARG(2))) W8(ARG(1), 0);
    if (ARG(2)) W32(ARG(2), 0);
    uint32_t outv[] = {nsub, maxsub, 0, nv, maxname, maxdata, 0};
    for (int i = 0; i < 7; i++)
        if (ARG(4 + i)) W32(ARG(4 + i), outv[i]);
    if (ARG(11)) { W32(ARG(11), 0); W32(ARG(11) + 4, 0); }
    LOG(LOG_REG, "RegQueryInfoKeyA(%s) -> %u subkeys, %u values", key, nsub, nv);
    return 0;
}

static int get_user_name(uint32_t buf, uint32_t sizep, int wide)
{
    const char *u = getenv("USER");
    if (!u || !*u)
        u = getenv("USERNAME");             /* Windows */
    if (deterministic)
        u = NULL;                           /* --test runs: the same on every machine and account */
    char name[64];
    snprintf(name, sizeof name, "%s", u && *u ? u : "Player");
    name[0] = (char)toupper((unsigned char)name[0]);
    uint32_t cap = R32(sizep), n = strlen(name) + 1;
    W32(sizep, n);
    if (cap < n) { set_last_error(122); return 0; }
    if (wide) gwput(buf, name, cap);
    else gstrncpy(buf, name, cap);
    return 1;
}

API(ADVAPI32, GetUserNameA, 2) { return get_user_name(ARG(0), ARG(1), 0); }
API(ADVAPI32, GetUserNameW, 2) { return get_user_name(ARG(0), ARG(1), 1); }

API(ADVAPI32, RegOpenKeyA, 3)
{
    const char *base = key_path(ARG(0));
    const char *sub = gstr(ARG(1));
    char full[256];
    snprintf(full, sizeof full, "%s%s%s", base ? base : "?", sub && *sub ? "\\" : "", sub ? sub : "");
    pthread_mutex_lock(&rlock);
    int known = key_known(full);
    pthread_mutex_unlock(&rlock);
    uint32_t r = known ? open_key(ARG(0), sub, ARG(2)) : 2;
    LOG(LOG_REG, "RegOpenKeyA(%s) -> %u", full, r);
    return r;
}
