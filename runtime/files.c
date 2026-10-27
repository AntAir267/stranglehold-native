/* Files: Windows paths -> host paths, file handles, directory searches.

   The program sees its folder as C:\Stranglehold (starting in C:\Stranglehold\Binaries). Reads look in the user folder first and then in
   the game folder (which is never written); anything opened for writing lives in the user
   folder, copied there first if it only exists in the game folder. Lookups ignore case. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>
#include "options.h"
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "runtime.h"

#define VROOT "C:\\Stranglehold"
/* the program starts in its Binaries folder: the engine finds everything as ..\\StrangleholdGame */
static char cwd[512] = VROOT "\\Binaries";            /* the program's current directory (Windows form) */
static pthread_mutex_t flock_ = PTHREAD_MUTEX_INITIALIZER;

const char *virtual_game_root(void) { return VROOT; }

/* Normalise to a path relative to the virtual root, '/' separated, no "." or "..";
   returns 0 if outside the root (then *rel holds a path under the user folder's drive_c). */
static int to_relative(const char *win, char *rel, size_t n, int *outside)
{
    char full[1024];
    *outside = 0;
    if (win[0] && win[1] == ':')
        snprintf(full, sizeof full, "%s", win);
    else if (win[0] == '\\' || win[0] == '/')
        snprintf(full, sizeof full, "C:%s", win);
    else
        snprintf(full, sizeof full, "%s\\%s", cwd, win);
    for (char *p = full; *p; p++)
        if (*p == '/') *p = '\\';
    /* split into components and resolve . and .. */
    char *comp[128];
    int nc = 0;
    char *save = NULL;
    char *drive = strtok_r(full, "\\", &save);
    for (char *t = strtok_r(NULL, "\\", &save); t; t = strtok_r(NULL, "\\", &save)) {
        if (!strcmp(t, ".") || !*t) continue;
        if (!strcmp(t, "..")) { if (nc) nc--; continue; }
        if (nc < 128) comp[nc++] = t;
    }
    int start = 0;
    if (drive && !strcasecmp(drive, "C:") && nc >= 1 && !strcasecmp(comp[0], "Stranglehold"))
        start = 1;
    else
        *outside = 1;
    size_t k = 0;
    rel[0] = 0;
    if (*outside) {
        k = snprintf(rel, n, "drive_%c", drive && drive[0] ? (char)tolower((unsigned char)drive[0]) : 'c');
    }
    for (int i = start; i < nc && k < n; i++)
        k += snprintf(rel + k, n - k, "%s%s", k ? "/" : "", comp[i]);
    return 1;
}

/* Find `rel` under `base` ignoring case; writes the real host path. Returns 1 if it exists.
   If only the parent exists, writes the path with the last component as given. */
static int resolve_ci(const char *base, const char *rel, char *out, size_t n)
{
#ifdef _WIN32
    /* Windows' folders already ignore case: a path that is not there is not there in any case,
       so one look at the whole path does (not a look at each part and a folder listing; the
       game opens thousands of files at start-up with fan content) */
    if (!*rel) {
        snprintf(out, n, "%s", base);
        return 1;
    }
    snprintf(out, n, "%s/%s", base, rel);
    struct stat st;
    return stat(out, &st) == 0;
#else
    char cur[2048];
    snprintf(cur, sizeof cur, "%s", base);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", rel);
    char *save = NULL;
    int exists = 1;
    for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        char next[2048];
        snprintf(next, sizeof next, "%s/%s", cur, t);
        struct stat st;
        if (exists && stat(next, &st) != 0) {
            DIR *d = opendir(cur);
            int found = 0;
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) {
                    if (!strcasecmp(e->d_name, t)) {
                        snprintf(next, sizeof next, "%s/%s", cur, e->d_name);
                        found = 1;
                        break;
                    }
                }
                closedir(d);
            }
            if (!found)
                exists = 0;
        }
        snprintf(cur, sizeof cur, "%s", next);
    }
    snprintf(out, n, "%s", cur);
    return exists;
#endif
}

static void mkdirs_for(const char *path)
{
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
}

static int copy_file(const char *from, const char *to)
{
    int a = open(from, O_RDONLY);
    if (a < 0) return 0;
    mkdirs_for(to);
    int b = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (b < 0) { close(a); return 0; }
    char buf[65536];
    ssize_t r;
    while ((r = read(a, buf, sizeof buf)) > 0)
        if (write(b, buf, r) != r) break;
    close(a);
    close(b);
    return 1;
}

/* XBOX DATA (option xbox_data = on, the default): files the Xbox version has better, prepared
   by tools/xbox_*.py in the user folder's xbox_data/ at the game's own paths, are read in place of
   the game's (writes still go to the user folder). */
static int xbox_data_file(const char *rel, char *out, size_t n)
{
    static int on = -1;
    static char dir[1100];
    if (on < 0) {
        on = strcasecmp(option_str("xbox_data", "off"), "off") != 0;   /* (LA Rush's; unused here) */
        snprintf(dir, sizeof dir, "%s/xbox_data", user_dir);
    }
    return on && resolve_ci(dir, rel, out, n);
}

/* MODS (option mods = on, the default): each folder in the user folder's mods/ holds files at the
   game's own paths, loose or whole .dir/.res packs, read in place of the game's and the Xbox data's
   (writes still go to the user folder). Taken in name order: the first holding a file wins. */
#define MAXMODS 32
static char mod_dirs[MAXMODS][1024];
static int nmods = -1;

static int cmp_str(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void mods_scan(void)
{
    if (nmods >= 0) return;
    nmods = 0;
    if (!option_bool("mods", 1)) return;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/mods", user_dir);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nmods < MAXMODS) {
        if (e->d_name[0] == '.') continue;
        char p[1100 + 256];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) || !S_ISDIR(st.st_mode) || strlen(p) >= sizeof mod_dirs[0]) continue;
        snprintf(mod_dirs[nmods++], sizeof mod_dirs[0], "%s", p);
    }
    closedir(d);
    qsort(mod_dirs, (size_t)nmods, sizeof mod_dirs[0], cmp_str);
    for (int i = 0; i < nmods; i++) LOG(LOG_ALWAYS, "mod: %s", mod_dirs[i]);
}

static int mod_file(const char *rel, char *out, size_t n)
{
    mods_scan();
    for (int i = 0; i < nmods; i++)
        if (resolve_ci(mod_dirs[i], rel, out, n)) return 1;
    return 0;
}

int path_to_host(const char *win, char *out, size_t n, int for_write)
{
    char rel[1024];
    int outside;
    to_relative(win, rel, sizeof rel, &outside);
    char user_path[2048], game_path[2048];
    int in_user = resolve_ci(user_dir, rel, user_path, sizeof user_path);
    if (outside) {
        snprintf(out, n, "%s", user_path);
        return in_user;
    }
    int in_game = resolve_ci(game_dir, rel, game_path, sizeof game_path);
    if (for_write) {
        if (!in_user && in_game && for_write == 2)
            copy_file(game_path, user_path);      /* opened for update: keep its contents */
        mkdirs_for(user_path);
        snprintf(out, n, "%s", user_path);
        return 1;
    }
    if (in_user) {
        snprintf(out, n, "%s", user_path);
        return 1;
    }
    if (mod_file(rel, out, n) || xbox_data_file(rel, out, n))
        return 1;
    snprintf(out, n, "%s", game_path);
    return in_game;
}

/* ------------------------------------------------------------------ handles */
typedef struct { int fd; char path[1024]; int is_std; } File;
static uint32_t std_handles[3];

static File *file_of(uint32_t h)
{
    return handle_get(h, H_FILE);
}

void files_init(void)
{
    for (int i = 0; i < 3; i++) {
        File *f = calloc(1, sizeof *f);
        f->fd = i;
        f->is_std = 1;
        snprintf(f->path, sizeof f->path, "<std%d>", i);
        std_handles[i] = handle_new(H_FILE, f);
    }
}

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u

/* SKIP INTRO (options.txt skip_intro = on; for testing): the start-up logo and legal movies are
   not found [guess: the game then moves on; check the time to the title] */
static int intro_skipped(const char *name)
{
    static int on = -1;
    if (on < 0) on = option_bool("skip_intro", 0);
    if (!on || !name) return 0;
    const char *b = strrchr(name, '\\');
    b = b ? b + 1 : name;
    static const char *const intro[] = {"AMD64.bik", "intro_Logo_Midway_HD.bik", "intro_Logo_TigerHill_HD.bik",
                                        "intro_Logo_Unreal.bik", "intro_Logo_Shared.bik", "intro_Legal_US.bik",
                                        "intro_Legal_Euro.bik", "RP_Logo_bink.bik", "ndgames.bik", "intro_c0010_bink.bik"};
    for (size_t i = 0; i < sizeof intro / sizeof *intro; i++)
        if (!strcasecmp(b, intro[i]))
            return 1;
    return 0;
}

static uint32_t create_file(const char *name, uint32_t access, uint32_t disp, const char *fn)
{
    if (intro_skipped(name)) {
        LOG(LOG_FILE, "%s(\"%s\"): skipped (skip_intro)", fn, name);
        set_last_error(2);
        return 0xffffffffu;
    }
    int writing = (access & GENERIC_WRITE) != 0;
    char host[2048];
    int keep = disp == 3 /* OPEN_EXISTING */ || disp == 4 /* OPEN_ALWAYS */;
    int exists = path_to_host(name, host, sizeof host, writing ? (keep ? 2 : 1) : 0);
    int fl = writing ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; break;      /* CREATE_NEW */
    case 2: fl |= O_CREAT | O_TRUNC; break;     /* CREATE_ALWAYS */
    case 3: break;                              /* OPEN_EXISTING */
    case 4: fl |= O_CREAT; break;               /* OPEN_ALWAYS */
    case 5: fl |= O_TRUNC; break;               /* TRUNCATE_EXISTING */
    }
    (void)exists;
    int fd = open(host, fl | O_CLOEXEC, 0644);
    if (fd < 0) {
        LOG(LOG_FILE, "%s(\"%s\", %x, disp %u) -> failed (%s)", fn, name, access, disp, host);
        set_last_error(errno == ENOENT ? 2 : errno == EEXIST ? 80 : 5);
        return 0xffffffffu;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        set_last_error(5);
        return 0xffffffffu;
    }
    File *f = calloc(1, sizeof *f);
    f->fd = fd;
    snprintf(f->path, sizeof f->path, "%s", host);
    uint32_t h = handle_new(H_FILE, f);
    LOG(LOG_FILE, "%s(\"%s\", %s%s, disp %u) -> %x (%s)", fn, name,
        access & GENERIC_READ ? "R" : "", writing ? "W" : "", disp, h, host);
    set_last_error((disp == 2 || disp == 4) && exists ? 183 : 0);
    return h;
}

API(KERNEL32, CreateFileA, 7) { return create_file(gstr(ARG(0)), ARG(1), ARG(4), "CreateFileA"); }
API(KERNEL32, CreateFileW, 7) { char b[1024]; return create_file(gwstr(ARG(0), b, sizeof b), ARG(1), ARG(4), "CreateFileW"); }

/* OpenFile (16-bit style): OF_EXIST asks only whether the file is there; the OFSTRUCT gets
   the full path. Returns the handle, 1 for OF_EXIST, or HFILE_ERROR. */
static void full_path(const char *p, char *out, size_t n);
API(KERNEL32, OpenFile, 3)
{
    const char *name = gstr(ARG(0));
    uint32_t of = ARG(1), style = ARG(2);
    char full[1024], host[2048];
    full_path(name, full, sizeof full);
    memset(gptr(of), 0, 136);
    W8(of, 136);
    W8(of + 1, 1);                                      /* fFixedDisk */
    gstrncpy(of + 8, full, 128);
    if (style & 0x4000) {                               /* OF_EXIST */
        int ok = path_to_host(name, host, sizeof host, 0) && access(host, F_OK) == 0;
        LOG(LOG_FILE, "OpenFile(\"%s\", OF_EXIST) -> %d", name, ok);
        if (!ok) { W16(of + 2, 2); set_last_error(2); return 0xffffffffu; }
        return 1;
    }
    if (style & 0x200) {                                /* OF_DELETE */
        LOG(LOG_FILE, "OpenFile(\"%s\", OF_DELETE) refused", name);
        W16(of + 2, 5);
        set_last_error(5);
        return 0xffffffffu;
    }
    uint32_t access_ = (style & 3) == 1 ? GENERIC_WRITE : (style & 3) == 2 ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    uint32_t h = create_file(name, access_, (style & 0x1000) ? 2 : 3, "OpenFile");   /* OF_CREATE */
    if (h == 0xffffffffu) W16(of + 2, (uint16_t)get_last_error());
    return h;
}

/* no devices to talk to */
API(KERNEL32, DeviceIoControl, 8)
{
    LOG(LOG_FILE, "DeviceIoControl(%x, code %x) -> not supported", ARG(0), ARG(1));
    set_last_error(1);                                  /* ERROR_INVALID_FUNCTION */
    return 0;
}

/* An OVERLAPPED read or write completes at once: its result goes into the OVERLAPPED record
   (Internal = STATUS_SUCCESS, InternalHigh = bytes) and its event is set, which is what Windows
   does when a request completes synchronously. The offset comes from the record. */
extern int event_set(uint32_t h);

static void overlapped_done(uint32_t ov, uint32_t n, uint32_t status)
{
    W32(ov, status);
    W32(ov + 4, n);
    if (R32(ov + 16))
        event_set(R32(ov + 16));
}

API(KERNEL32, ReadFile, 5)
{
    File *f = file_of(ARG(0));
    uint32_t ov = ARG(4);
    if (ARG(3)) W32(ARG(3), 0);
    if (!f) { set_last_error(6); return 0; }
    uint32_t want = ARG(2), got = 0;
    if (ov) {
        off_t at = (off_t)(R32(ov + 8) | ((uint64_t)R32(ov + 12) << 32));
        while (got < want) {
            ssize_t r = pread(f->fd, gptr(ARG(1) + got), want - got, at + got);
            if (r <= 0) break;
            got += r;
        }
        overlapped_done(ov, got, got || !want ? 0 : 0xc0000011u);        /* STATUS_END_OF_FILE */
        if (ARG(3)) W32(ARG(3), got);
        LOG(LOG_FILE, "ReadFile(%x, %u at %lld, overlapped) -> %u", ARG(0), want, (long long)at, got);
        if (!got && want) { set_last_error(38); return 0; }               /* ERROR_HANDLE_EOF */
        return 1;
    }
    while (got < want) {
        ssize_t r = read(f->fd, gptr(ARG(1) + got), want - got);
        if (r <= 0) break;
        got += r;
    }
    if (ARG(3)) W32(ARG(3), got);
    LOG(LOG_FILE, "ReadFile(%x, %u) -> %u", ARG(0), want, got);
    return 1;
}

API(KERNEL32, GetOverlappedResult, 4)
{
    uint32_t ov = ARG(1);
    if (ARG(2)) W32(ARG(2), R32(ov + 4));
    if (R32(ov) == 0x103u) { set_last_error(996); return 0; }            /* still pending: never */
    if (R32(ov)) { set_last_error(R32(ov) == 0xc0000011u ? 38 : 31); return 0; }
    return 1;
}

API(KERNEL32, WriteFile, 5)
{
    File *f = file_of(ARG(0));
    if (ARG(3)) W32(ARG(3), 0);
    if (!f) { set_last_error(6); return 0; }
    uint32_t n = ARG(2);
    if (f->is_std) {
        LOG(LOG_ALWAYS, "program wrote to std%d: %.*s", f->fd, (int)n, gptr(ARG(1)));
        if (ARG(3)) W32(ARG(3), n);
        return 1;
    }
    ssize_t r;
    if (ARG(4)) {
        uint32_t ov = ARG(4);
        r = pwrite(f->fd, gptr(ARG(1)), n, (off_t)(R32(ov + 8) | ((uint64_t)R32(ov + 12) << 32)));
        overlapped_done(ov, r > 0 ? (uint32_t)r : 0, r < 0 ? 0xc0000001u : 0);
    } else
        r = write(f->fd, gptr(ARG(1)), n);
    if (r < 0) { set_last_error(5); return 0; }
    if (ARG(3)) W32(ARG(3), (uint32_t)r);
    LOG(LOG_FILE, "WriteFile(%x, %u) -> %zd", ARG(0), n, r);
    return 1;
}

API(KERNEL32, SetFilePointer, 4)
{
    File *f = file_of(ARG(0));
    if (!f) { set_last_error(6); return 0xffffffffu; }
    int64_t dist = (int32_t)ARG(1);
    if (ARG(2))
        dist = (int64_t)(((uint64_t)R32(ARG(2)) << 32) | ARG(1));
    int whence = ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END;
    off_t r = lseek(f->fd, dist, whence);
    if (r < 0) { set_last_error(131); return 0xffffffffu; }
    if (ARG(2)) W32(ARG(2), (uint32_t)((uint64_t)r >> 32));
    set_last_error(0);
    return (uint32_t)r;
}

API(KERNEL32, SetEndOfFile, 1)
{
    File *f = file_of(ARG(0));
    if (!f) return 0;
    off_t p = lseek(f->fd, 0, SEEK_CUR);
    return ftruncate(f->fd, p) == 0;
}

API(KERNEL32, FlushFileBuffers, 1)
{
    return file_of(ARG(0)) != NULL;
}

API(KERNEL32, GetFileType, 1)
{
    File *f = file_of(ARG(0));
    if (!f) { set_last_error(6); return 0; }
    return f->is_std ? 2 : 1;                   /* FILE_TYPE_CHAR / FILE_TYPE_DISK */
}

API(KERNEL32, GetStdHandle, 1)
{
    int32_t n = (int32_t)ARG(0);
    if (n >= -12 && n <= -10)
        return std_handles[-10 - n];
    return 0xffffffffu;
}

API(KERNEL32, SetStdHandle, 2)
{
    int32_t n = (int32_t)ARG(0);
    if (n >= -12 && n <= -10)
        std_handles[-10 - n] = ARG(1);
    return 1;
}

API(KERNEL32, SetHandleCount, 1) { return ARG(0); }

int close_file_handle(uint32_t h)
{
    File *f = file_of(h);
    if (!f) return 0;
    if (!f->is_std)
        close(f->fd);
    free(f);
    handle_close(h);
    return 1;
}

static int delete_file(const char *name)
{
    char host[2048];
    path_to_host(name, host, sizeof host, 0);
    char user[2048];
    snprintf(user, sizeof user, "%s/", user_dir);
    int ok = 0;
    if (!strncmp(host, user, strlen(user)))
        ok = unlink(host) == 0;
    LOG(LOG_FILE, "DeleteFile(\"%s\") -> %d", name, ok);
    if (!ok) set_last_error(2);
    return ok;
}
API(KERNEL32, DeleteFileA, 1) { return delete_file(gstr(ARG(0))); }
API(KERNEL32, DeleteFileW, 1) { char b[1024]; return delete_file(gwstr(ARG(0), b, sizeof b)); }

static int move_file(const char *src, const char *dst)
{
    char from[2048], to[2048];
    int exists = path_to_host(src, from, sizeof from, 0);
    path_to_host(dst, to, sizeof to, 1);
    char user[2048];
    snprintf(user, sizeof user, "%s/", user_dir);
    int ok = 0;
    if (exists) {
        if (!strncmp(from, user, strlen(user)))
            ok = rename(from, to) == 0;
        else
            ok = copy_file(from, to);           /* never remove from the game folder */
    }
    LOG(LOG_FILE, "MoveFile(\"%s\", \"%s\") -> %d", src, dst, ok);
    if (!ok) set_last_error(2);
    return ok;
}
API(KERNEL32, MoveFileA, 2) { return move_file(gstr(ARG(0)), gstr(ARG(1))); }
API(KERNEL32, MoveFileW, 2)
{
    char a[1024], b[1024];
    return move_file(gwstr(ARG(0), a, sizeof a), gwstr(ARG(1), b, sizeof b));
}

static int set_cwd(const char *p)
{
    char host[2048];
    int exists = path_to_host(p, host, sizeof host, 0);
    LOG(LOG_FILE, "SetCurrentDirectory(\"%s\") -> %d", p, exists);
    if (!exists) { set_last_error(3); return 0; }
    pthread_mutex_lock(&flock_);
    if (p[0] && p[1] == ':')
        snprintf(cwd, sizeof cwd, "%s", p);
    else {
        char t[512];
        snprintf(t, sizeof t, "%s\\%s", cwd, p);
        snprintf(cwd, sizeof cwd, "%s", t);
    }
    size_t k = strlen(cwd);
    while (k > 3 && (cwd[k - 1] == '\\' || cwd[k - 1] == '/'))
        cwd[--k] = 0;
    pthread_mutex_unlock(&flock_);
    return 1;
}
API(KERNEL32, SetCurrentDirectoryA, 1) { return set_cwd(gstr(ARG(0))); }
API(KERNEL32, SetCurrentDirectoryW, 1) { char b[1024]; return set_cwd(gwstr(ARG(0), b, sizeof b)); }

API(KERNEL32, GetTempFileNameA, 4)
{
    static unsigned counter = 1;
    const char *dir = gstr(ARG(0)), *pre = gstr(ARG(1));
    unsigned u = ARG(2) ? ARG(2) & 0xffff : counter++;
    char name[600];
    snprintf(name, sizeof name, "%s\\%.3s%X.TMP", dir, pre ? pre : "", u);
    if (!ARG(2)) {
        char host[2048];
        path_to_host(name, host, sizeof host, 1);
        int fd = open(host, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
    }
    gstrncpy(ARG(3), name, 260);
    LOG(LOG_FILE, "GetTempFileNameA -> \"%s\"", name);
    return u;
}

/* ------------------------------------------------------------------ directory search */
typedef struct {
    char names[4096][64];
    char from[4096];                    /* where each name was listed: FROM_USER, FROM_GAME, FROM_XBOX, FROM_MOD + i */
    int n, pos;
    char dir_user[2048], dir_game[2048], dir_xbox[2048], dir_mod[MAXMODS][1024];
} Find;

static void unix_time_to_filetime(time_t t, uint32_t at)
{
    uint64_t ft = ((uint64_t)t + 11644473600ull) * 10000000ull;
    W32(at, (uint32_t)ft);
    W32(at + 4, (uint32_t)(ft >> 32));
}

enum { FROM_USER, FROM_GAME, FROM_XBOX, FROM_MOD };

/* WIN32_FIND_DATAA (320 bytes) or, wide, WIN32_FIND_DATAW (592: the names in UTF-16) */
static int fill_find_data(Find *f, uint32_t data, int wide)
{
    while (f->pos < f->n) {
        int from = f->from[f->pos];
        const char *nm = f->names[f->pos++];
        char p[2200];
        struct stat st;
        /* looked for where it was listed only (a name in the user folder hides the game's) */
        snprintf(p, sizeof p, "%s/%s", from == FROM_USER ? f->dir_user : from == FROM_XBOX ? f->dir_xbox :
                 from >= FROM_MOD ? f->dir_mod[from - FROM_MOD] : f->dir_game, nm);
        if (stat(p, &st) != 0)
            continue;
        memset(gptr(data), 0, wide ? 592 : 320);
        W32(data, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
        unix_time_to_filetime(st.st_mtime, data + 4);
        unix_time_to_filetime(st.st_atime, data + 12);
        unix_time_to_filetime(st.st_mtime, data + 20);
        W32(data + 28, (uint32_t)((uint64_t)st.st_size >> 32));
        W32(data + 32, (uint32_t)st.st_size);
        if (wide) gwput(data + 44, nm, 260);
        else gstrncpy(data + 44, nm, 260);
        return 1;
    }
    return 0;
}

static void add_matches(Find *f, const char *dir, const char *pat, int from)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && f->n < 4096) {
        if (fnmatch(pat, e->d_name, FNM_CASEFOLD))
            continue;
        if (strlen(e->d_name) >= 64)
            continue;
        int dup = 0;
        for (int i = 0; i < f->n && !dup; i++)
            dup = !strcasecmp(f->names[i], e->d_name);
        if (!dup) {
            f->from[f->n] = (char)from;
            snprintf(f->names[f->n++], 64, "%s", e->d_name);
        }
    }
    closedir(d);
}

/* sorted by name, case ignored (the names are unique so), each keeping where it was listed */
static Find *sorting;
static int cmp_idx(const void *a, const void *b)
{
    return strcasecmp(sorting->names[*(const int *)a], sorting->names[*(const int *)b]);
}

static void sort_names(Find *f)
{
    static int idx[4096];
    static char names[4096][64], from[4096];
    for (int i = 0; i < f->n; i++)
        idx[i] = i;
    sorting = f;
    qsort(idx, f->n, sizeof idx[0], cmp_idx);
    for (int i = 0; i < f->n; i++) {
        memcpy(names[i], f->names[idx[i]], 64);
        from[i] = f->from[idx[i]];
    }
    memcpy(f->names, names, (size_t)f->n * 64);
    memcpy(f->from, from, (size_t)f->n);
}

static uint32_t find_first(const char *spec, uint32_t data, int wide)
{
    char dirpart[600], pat[260];
    const char *slash = strrchr(spec, '\\');
    if (!slash) slash = strrchr(spec, '/');
    if (slash) {
        snprintf(dirpart, sizeof dirpart, "%.*s", (int)(slash - spec), spec);
        snprintf(pat, sizeof pat, "%s", slash + 1);
    } else {
        snprintf(dirpart, sizeof dirpart, ".");
        snprintf(pat, sizeof pat, "%s", spec);
    }
    if (!strcmp(pat, "*.*"))
        snprintf(pat, sizeof pat, "*");
    Find *f = calloc(1, sizeof *f);
    char rel[1024];
    int outside;
    to_relative(dirpart, rel, sizeof rel, &outside);
    resolve_ci(user_dir, rel, f->dir_user, sizeof f->dir_user);
    if (outside)
        snprintf(f->dir_game, sizeof f->dir_game, "%s", f->dir_user);
    else
        resolve_ci(game_dir, rel, f->dir_game, sizeof f->dir_game);
    add_matches(f, f->dir_user, pat, FROM_USER);
    if (!outside) {                                     /* (each before the game's: it hides them) */
        mods_scan();
        for (int i = 0; i < nmods; i++)
            if (resolve_ci(mod_dirs[i], rel, f->dir_mod[i], sizeof f->dir_mod[i]))
                add_matches(f, f->dir_mod[i], pat, FROM_MOD + i);
        if (xbox_data_file(rel, f->dir_xbox, sizeof f->dir_xbox))
            add_matches(f, f->dir_xbox, pat, FROM_XBOX);
    }
    add_matches(f, f->dir_game, pat, FROM_GAME);
    sort_names(f);
    LOG(LOG_FILE, "FindFirstFile(\"%s\") -> %d matches", spec, f->n);
    if (!fill_find_data(f, data, wide)) {
        free(f);
        set_last_error(2);
        return 0xffffffffu;
    }
    return handle_new(H_FIND, f);
}

API(KERNEL32, FindFirstFileA, 2) { return find_first(gstr(ARG(0)), ARG(1), 0); }
API(KERNEL32, FindFirstFileW, 2) { char b[1024]; return find_first(gwstr(ARG(0), b, sizeof b), ARG(1), 1); }

static int find_next(uint32_t h, uint32_t data, int wide)
{
    Find *f = handle_get(h, H_FIND);
    if (!f || !fill_find_data(f, data, wide)) {
        set_last_error(18);                     /* ERROR_NO_MORE_FILES */
        return 0;
    }
    return 1;
}

API(KERNEL32, FindNextFileA, 2) { return find_next(ARG(0), ARG(1), 0); }
API(KERNEL32, FindNextFileW, 2) { return find_next(ARG(0), ARG(1), 1); }

API(KERNEL32, FindClose, 1)
{
    Find *f = handle_get(ARG(0), H_FIND);
    if (!f) return 0;
    free(f);
    handle_close(ARG(0));
    return 1;
}

/* ------------------------------------------------------------------ more file functions (LA Rush) */
static uint32_t attrs_of(const char *host)
{
    struct stat st;
    if (stat(host, &st) != 0)
        return 0xffffffffu;
    uint32_t a = S_ISDIR(st.st_mode) ? 0x10 : 0x20;             /* DIRECTORY / ARCHIVE */
    char user[2048];
    snprintf(user, sizeof user, "%s/", user_dir);
    if (!S_ISDIR(st.st_mode) && strncmp(host, user, strlen(user)))
        a |= 0x01;                                              /* the game folder is read-only */
    return a;
}

static uint32_t get_attrs(const char *win)
{
    char host[2048];
    if (!path_to_host(win, host, sizeof host, 0)) {
        set_last_error(2);
        return 0xffffffffu;
    }
    uint32_t a = attrs_of(host);
    if (a == 0xffffffffu)
        set_last_error(2);
    LOG(LOG_FILE, "GetFileAttributes(\"%s\") -> %x", win, a);
    return a;
}

API(KERNEL32, GetFileAttributesA, 1) { return get_attrs(gstr(ARG(0))); }
API(KERNEL32, GetFileAttributesW, 1) { char b[1024]; return get_attrs(gwstr(ARG(0), b, sizeof b)); }
API(KERNEL32, SetFileAttributesA, 2) { return get_attrs(gstr(ARG(0))) != 0xffffffffu; }
API(KERNEL32, SetFileAttributesW, 2) { char b[1024]; return get_attrs(gwstr(ARG(0), b, sizeof b)) != 0xffffffffu; }

static int make_dir(const char *win)
{
    char host[2048];
    path_to_host(win, host, sizeof host, 1);
    int ok = mkdir(host, 0755) == 0;
    if (!ok) set_last_error(errno == EEXIST ? 183 : 3);
    LOG(LOG_FILE, "CreateDirectory(\"%s\") -> %d", win, ok);
    return ok;
}
API(KERNEL32, CreateDirectoryA, 2) { return make_dir(gstr(ARG(0))); }
API(KERNEL32, CreateDirectoryW, 2) { char b[1024]; return make_dir(gwstr(ARG(0), b, sizeof b)); }

static int remove_dir(const char *win)
{
    char host[2048], user[2048];
    path_to_host(win, host, sizeof host, 0);
    snprintf(user, sizeof user, "%s/", user_dir);
    int ok = !strncmp(host, user, strlen(user)) && rmdir(host) == 0;
    if (!ok) set_last_error(5);
    return ok;
}
API(KERNEL32, RemoveDirectoryA, 1) { return remove_dir(gstr(ARG(0))); }
API(KERNEL32, RemoveDirectoryW, 1) { char b[1024]; return remove_dir(gwstr(ARG(0), b, sizeof b)); }

/* the full path of a name against the program's current directory */
static void full_path(const char *p, char *out, size_t n)
{
    pthread_mutex_lock(&flock_);
    if (p[0] && p[1] == ':')
        snprintf(out, n, "%s", p);
    else if (p[0] == '\\' || p[0] == '/')
        snprintf(out, n, "%.2s%s", cwd, p);
    else
        snprintf(out, n, "%s\\%s", cwd, p);
    pthread_mutex_unlock(&flock_);
    for (char *q = out; *q; q++)
        if (*q == '/') *q = '\\';
}

API(KERNEL32, GetFullPathNameA, 4)
{
    char b[1024];
    full_path(gstr(ARG(0)), b, sizeof b);
    uint32_t len = strlen(b);
    if (len + 1 > ARG(1)) return len + 1;
    gstrncpy(ARG(2), b, ARG(1));
    if (ARG(3)) {
        const char *s = strrchr(b, '\\');
        W32(ARG(3), s ? ARG(2) + (uint32_t)(s + 1 - b) : 0);
    }
    return len;
}

API(KERNEL32, GetFullPathNameW, 4)
{
    char in[1024], b[1024];
    full_path(gwstr(ARG(0), in, sizeof in), b, sizeof b);
    uint32_t len = strlen(b);
    if (len + 1 > ARG(1)) return len + 1;
    gwput(ARG(2), b, ARG(1));
    if (ARG(3)) {
        const char *s = strrchr(b, '\\');
        W32(ARG(3), s ? ARG(2) + 2 * (uint32_t)(s + 1 - b) : 0);
    }
    return len;
}

API(KERNEL32, GetCurrentDirectoryA, 2)
{
    pthread_mutex_lock(&flock_);
    uint32_t len = strlen(cwd);
    if (len + 1 <= ARG(0)) gstrncpy(ARG(1), cwd, ARG(0));
    pthread_mutex_unlock(&flock_);
    return len + 1 <= ARG(0) ? len : len + 1;
}

API(KERNEL32, GetCurrentDirectoryW, 2)
{
    pthread_mutex_lock(&flock_);
    uint32_t len = strlen(cwd);
    if (len + 1 <= ARG(0)) gwput(ARG(1), cwd, ARG(0));
    pthread_mutex_unlock(&flock_);
    return len + 1 <= ARG(0) ? len : len + 1;
}

API(KERNEL32, GetFileSize, 2)
{
    File *f = file_of(ARG(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) { set_last_error(6); return 0xffffffffu; }
    if (ARG(1)) W32(ARG(1), (uint32_t)((uint64_t)st.st_size >> 32));
    return (uint32_t)st.st_size;
}

API(KERNEL32, GetFileTime, 4)
{
    File *f = file_of(ARG(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) { set_last_error(6); return 0; }
    for (int i = 1; i <= 3; i++)
        if (ARG(i)) unix_time_to_filetime(st.st_mtime, ARG(i));
    return 1;
}

API(KERNEL32, SetFileTime, 4) { return file_of(ARG(0)) != NULL; }

API(KERNEL32, GetFileInformationByHandle, 2)
{
    File *f = file_of(ARG(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) { set_last_error(6); return 0; }
    uint32_t b = ARG(1);
    memset(gptr(b), 0, 52);
    W32(b, attrs_of(f->path));
    unix_time_to_filetime(st.st_mtime, b + 4);
    unix_time_to_filetime(st.st_atime, b + 12);
    unix_time_to_filetime(st.st_mtime, b + 20);
    W32(b + 28, 0x1234);                                        /* volume serial */
    W32(b + 32, (uint32_t)((uint64_t)st.st_size >> 32));
    W32(b + 36, (uint32_t)st.st_size);
    W32(b + 40, 1);
    W32(b + 44, (uint32_t)((uint64_t)st.st_ino >> 32));
    W32(b + 48, (uint32_t)st.st_ino);
    return 1;
}

static int copy_file_to(const char *src, const char *dst, int fail_if_exists)
{
    char from[2048], to[2048];
    if (!path_to_host(src, from, sizeof from, 0)) { set_last_error(2); return 0; }
    int exists = path_to_host(dst, to, sizeof to, 1);
    struct stat st;
    if (fail_if_exists && exists && stat(to, &st) == 0) { set_last_error(80); return 0; }
    int ok = copy_file(from, to);
    LOG(LOG_FILE, "CopyFile(\"%s\", \"%s\") -> %d", src, dst, ok);
    return ok;
}
API(KERNEL32, CopyFileA, 3) { return copy_file_to(gstr(ARG(0)), gstr(ARG(1)), ARG(2) != 0); }
API(KERNEL32, CopyFileW, 3)
{
    char a[1024], b[1024];
    return copy_file_to(gwstr(ARG(0), a, sizeof a), gwstr(ARG(1), b, sizeof b), ARG(2) != 0);
}

API(KERNEL32, LockFile, 5) { return 1; }
API(KERNEL32, UnlockFile, 5) { return 1; }

API(KERNEL32, GetDriveTypeA, 1) { return 3; }              /* DRIVE_FIXED */
API(KERNEL32, GetDriveTypeW, 1) { return 3; }
API(KERNEL32, GetLogicalDrives, 0) { return 1u << 2; }      /* C: */
API_CDECL(MSVCRT, _getdrives) { return 1u << 2; }

API(KERNEL32, GetDiskFreeSpaceA, 5)
{
    if (ARG(1)) W32(ARG(1), 8);
    if (ARG(2)) W32(ARG(2), 512);
    if (ARG(3)) W32(ARG(3), 0x100000);
    if (ARG(4)) W32(ARG(4), 0x200000);
    return 1;
}

API(KERNEL32, GetDiskFreeSpaceExA, 4)
{
    uint64_t v = 16ull << 30;
    for (int i = 1; i <= 3; i++)
        if (ARG(i)) { W32(ARG(i), (uint32_t)v); W32(ARG(i) + 4, (uint32_t)(v >> 32)); }
    return 1;
}

API(KERNEL32, GetVolumeInformationA, 8)
{
    if (ARG(1) && ARG(2)) gstrncpy(ARG(1), "STRANGLEHOLD", ARG(2));
    if (ARG(3)) W32(ARG(3), 0x1234abcdu);
    if (ARG(4)) W32(ARG(4), 255);
    if (ARG(5)) W32(ARG(5), 0x3);
    if (ARG(6) && ARG(7)) gstrncpy(ARG(6), "NTFS", ARG(7));
    return 1;
}

API(KERNEL32, GetTempPathA, 2)
{
    char b[600];
    snprintf(b, sizeof b, "%s\\TEMP\\", virtual_game_root());
    uint32_t len = strlen(b);
    if (len + 1 <= ARG(0)) gstrncpy(ARG(1), b, ARG(0));
    return len;
}

API(KERNEL32, GetWindowsDirectoryA, 2) { gstrncpy(ARG(0), "C:\\WINDOWS", ARG(1)); return 10; }
API(KERNEL32, GetSystemDirectoryA, 2) { gstrncpy(ARG(0), "C:\\WINDOWS\\system32", ARG(1)); return 19; }
API(KERNEL32, GetSystemDirectoryW, 2) { gwput(ARG(0), "C:\\WINDOWS\\system32", ARG(1)); return 19; }

API(KERNEL32, GetShortPathNameA, 3)
{
    const char *s = gstr(ARG(0));
    uint32_t len = strlen(s);
    if (len + 1 <= ARG(2)) gstrncpy(ARG(1), s, ARG(2));
    return len + 1 <= ARG(2) ? len : len + 1;
}

/* names are kept as given (no 8.3 names exist) */
API(KERNEL32, GetLongPathNameW, 3)
{
    char s[1024];
    gwstr(ARG(0), s, sizeof s);
    uint32_t len = strlen(s);
    if (len + 1 <= ARG(2)) gwput(ARG(1), s, ARG(2));
    return len + 1 <= ARG(2) ? len : len + 1;
}

/* every missing folder of a path (SHCreateDirectoryEx, SHGetFolderPath's CSIDL_FLAG_CREATE):
   0, or ERROR_ALREADY_EXISTS / ERROR_PATH_NOT_FOUND */
uint32_t create_dir_tree(const char *win)
{
    char host[2048];
    struct stat st;
    if (path_to_host(win, host, sizeof host, 0) && stat(host, &st) == 0)
        return S_ISDIR(st.st_mode) ? 183 : 80;
    path_to_host(win, host, sizeof host, 1);
    mkdirs_for(host);
    int ok = mkdir(host, 0755) == 0 || errno == EEXIST;
    LOG(LOG_FILE, "create folders \"%s\" -> %d", win, ok);
    return ok ? 0 : 3;
}

API(KERNEL32, SearchPathA, 6) { set_last_error(2); return 0; }

/* ------------------------------------------------------------------ INI files */
/* The program's .ini files: read from the game or user folder, written to the user folder. */
static int ini_read(const char *file, char **text)
{
    char host[2048];
    *text = NULL;
    char name[600];
    if (!strchr(file, '\\') && !strchr(file, '/'))
        snprintf(name, sizeof name, "C:\\WINDOWS\\%s", file);
    else
        snprintf(name, sizeof name, "%s", file);
    if (!path_to_host(name, host, sizeof host, 0))
        return 0;
    FILE *f = fopen(host, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *text = malloc(n + 1);
    n = fread(*text, 1, n, f);
    (*text)[n] = 0;
    fclose(f);
    return 1;
}

/* the value of key in section (NULL if absent), into out */
static int ini_get(const char *text, const char *sect, const char *key, char *out, size_t n)
{
    int in = 0;
    for (const char *l = text; l && *l; ) {
        const char *e = strpbrk(l, "\r\n");
        size_t len = e ? (size_t)(e - l) : strlen(l);
        char line[1024];
        snprintf(line, sizeof line, "%.*s", (int)(len < 1023 ? len : 1023), l);
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            char *q = strchr(p, ']');
            if (q) *q = 0;
            in = !strcasecmp(p + 1, sect);
        } else if (in && *p && *p != ';') {
            char *eq = strchr(p, '=');
            if (eq) {
                *eq = 0;
                char *k = p, *v = eq + 1;
                size_t kl = strlen(k);
                while (kl && (k[kl - 1] == ' ' || k[kl - 1] == '\t')) k[--kl] = 0;
                while (*v == ' ' || *v == '\t') v++;
                if (!strcasecmp(k, key)) {
                    snprintf(out, n, "%s", v);
                    return 1;
                }
            }
        }
        l = e ? e + 1 : NULL;
        while (l && (*l == '\r' || *l == '\n')) l++;
    }
    return 0;
}

API(KERNEL32, GetPrivateProfileStringA, 6)
{
    const char *sect = gstr(ARG(0)), *key = gstr(ARG(1)), *def = gstr(ARG(2)), *file = gstr(ARG(5));
    uint32_t out = ARG(3), cap = ARG(4);
    char *text, v[1024];
    const char *r = def ? def : "";
    if (sect && key && ini_read(file, &text)) {
        if (ini_get(text, sect, key, v, sizeof v))
            r = v;
        free(text);
    } else if (!sect || !key) {
        LOG(LOG_ALWAYS, "GetPrivateProfileStringA: listing sections/keys of %s not supported", file);
    }
    LOG(LOG_FILE, "GetPrivateProfileStringA([%s] %s, %s) -> \"%s\"", sect, key, file, r);
    if (!cap) return 0;
    gstrncpy(out, r, cap);
    return strlen(gptr(out));
}

API(KERNEL32, GetPrivateProfileIntA, 4)
{
    const char *sect = gstr(ARG(0)), *key = gstr(ARG(1)), *file = gstr(ARG(3));
    char *text, v[1024];
    uint32_t r = ARG(2);
    if (ini_read(file, &text)) {
        if (ini_get(text, sect, key, v, sizeof v))
            r = (uint32_t)strtol(v, NULL, 10);
        free(text);
    }
    LOG(LOG_FILE, "GetPrivateProfileIntA([%s] %s, %s) -> %d", sect, key, file, (int)r);
    return r;
}

API(KERNEL32, GetProfileIntA, 3) { return ARG(2); }

API(KERNEL32, WritePrivateProfileStringA, 4)
{
    const char *sect = gstr(ARG(0)), *key = gstr(ARG(1)), *val = gstr(ARG(2)), *file = gstr(ARG(3));
    char *text = NULL;
    ini_read(file, &text);
    /* rebuild: copy lines, replacing or adding key in section */
    size_t cap = (text ? strlen(text) : 0) + 4096, n = 0;
    char *out = malloc(cap);
    int in = 0, done = 0, seen = 0;
    out[0] = 0;
#define PUT(...) n += snprintf(out + n, cap - n, __VA_ARGS__)
    for (const char *l = text; l && *l; ) {
        const char *e = strpbrk(l, "\r\n");
        size_t len = e ? (size_t)(e - l) : strlen(l);
        char line[1024];
        snprintf(line, sizeof line, "%.*s", (int)(len < 1023 ? len : 1023), l);
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            if (in && !done && key && val) { PUT("%s=%s\r\n", key, val); done = 1; }
            char s[256];
            snprintf(s, sizeof s, "%s", p + 1);
            char *q = strchr(s, ']');
            if (q) *q = 0;
            in = !strcasecmp(s, sect);
            seen |= in;
            if (!(in && !key)) PUT("%s\r\n", line);
        } else if (in) {
            const char *eq = strchr(p, '=');
            if (!key)
                ;                                               /* the section is deleted */
            else if (eq && (size_t)(eq - p) == strlen(key) && !strncasecmp(p, key, eq - p)) {
                if (val && !done) PUT("%s=%s\r\n", key, val);
                done = 1;
            } else
                PUT("%s\r\n", line);
        } else if (*p)
            PUT("%s\r\n", line);
        l = e ? e + 1 : NULL;
        while (l && (*l == '\r' || *l == '\n')) l++;
    }
    if (key && val && !done) {
        if (!in) PUT("[%s]\r\n", sect);
        PUT("%s=%s\r\n", key, val);
    }
#undef PUT
    (void)seen;
    char name[600], host[2048];
    if (!strchr(file, '\\') && !strchr(file, '/'))
        snprintf(name, sizeof name, "C:\\WINDOWS\\%s", file);
    else
        snprintf(name, sizeof name, "%s", file);
    path_to_host(name, host, sizeof host, 1);
    FILE *f = fopen(host, "wb");
    if (f) { fwrite(out, 1, n, f); fclose(f); }
    LOG(LOG_FILE, "WritePrivateProfileStringA([%s] %s=%s, %s)", sect, key ? key : "-", val ? val : "-", file);
    free(out);
    free(text);
    return f != NULL;
}
