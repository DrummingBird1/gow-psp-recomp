/* sceUtilitySavedata: the PSP's save data utility, after PPSSPP's PSPSaveDialog and
 * SavedataParam (Core/Dialog, v1.20.4).
 *
 * A save is a folder ms0:/PSP/SAVEDATA/<gameName><saveName>/ (below the --ms0 folder on
 * the host) with the data file the game names (DATA.BIN, say), PARAM.SFO and the optional
 * ICON0.PNG, ICON1.PMF, PIC1.PNG and SND0.AT3, laid out as PPSSPP writes them. The data
 * file is stored unencrypted, as PPSSPP does with save encryption off: SAVEDATA_PARAMS in
 * PARAM.SFO stays zero, which is what marks a save as plain when it is loaded again.
 * Encrypted saves (from a PSP, or PPSSPP with encryption on) would need the KIRK engine
 * and are reported as broken instead of being loaded as garbage.
 *
 * Dialog flow, as on PPSSPP: InitStart -> INIT (1), RUNNING (2) 200 ms of virtual time
 * later; the first Update does the work and the next one reports FINISHED (3) and writes
 * the request back; ShutdownStart -> SHUTDOWN (4), then NONE (0) 2 ms later.
 *
 * The port draws no save list. The list dialogs choose for the player: LISTSAVE writes the
 * first empty slot of the game's list, or the oldest save when every slot is used (rolling
 * saves, so a bad save never overwrites the only good one), LISTLOAD loads the newest save,
 * and the delete lists delete nothing (the dialog is cancelled). --save-slot NAME makes
 * LISTSAVE and LISTLOAD use that slot. Yes/no questions (overwrite, load) answer yes.
 * Save times come from the guest's clock (rt_wall_time_us), so "newest" is the same in a
 * replay of an input recording as in the session that was recorded. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "rt_kernel.h"

const char *rt_savedata_slot;

/* --- SceUtilitySavedataParam (offsets in the request) ------------------------------- */
enum {
    P_SIZE = 0x00, P_RESULT = 0x1C, P_MODE = 0x30, P_BIND = 0x34, P_OVERWRITE = 0x38,
    P_GAME = 0x3C, P_SAVE = 0x4C, P_SAVELIST = 0x60, P_FILE = 0x64,
    P_DATABUF = 0x74, P_DATABUFSIZE = 0x78, P_DATASIZE = 0x7C,
    P_TITLE = 0x80, P_SAVETITLE = 0x100, P_DETAIL = 0x180, P_PARENTAL = 0x580,
    P_ICON0 = 0x584, P_ICON1 = 0x594, P_PIC1 = 0x5A4, P_SND0 = 0x5B4,  /* buf, bufSize, size, unknown */
    P_NEWDATA = 0x5C4, P_FOCUS = 0x5C8, P_KEY = 0x5DC, P_SECUREVER = 0x5EC,
    REQ_MAX = 0x600,
};

enum {
    AUTOLOAD, AUTOSAVE, LOAD, SAVE, LISTLOAD, LISTSAVE, LISTDELETE, LISTALLDELETE, SIZES, AUTODELETE,
    DELETE_, LIST, FILES, MAKEDATASECURE, MAKEDATA, READDATASECURE, READDATA, WRITEDATASECURE, WRITEDATA,
    ERASESECURE, ERASE, DELETEDATA, GETSIZE,
};
static const char *const k_mode_names[] = {
    "AUTOLOAD", "AUTOSAVE", "LOAD", "SAVE", "LISTLOAD", "LISTSAVE", "LISTDELETE", "LISTALLDELETE", "SIZES",
    "AUTODELETE", "DELETE", "LIST", "FILES", "MAKEDATASECURE", "MAKEDATA", "READDATASECURE", "READDATA",
    "WRITEDATASECURE", "WRITEDATA", "ERASESECURE", "ERASE", "DELETEDATA", "GETSIZE",
};

#define ERR_INVALID_STATUS     0x80110001u
#define ERR_INVALID_PARAM_SIZE 0x80110004u
#define LOAD_DATA_BROKEN       0x80110306u
#define LOAD_NO_DATA           0x80110307u
#define LOAD_PARAM             0x80110308u
#define LOAD_FILE_NOT_FOUND    0x80110309u
#define RW_DATA_BROKEN         0x80110326u
#define RW_NO_DATA             0x80110327u
#define RW_BAD_PARAMS          0x80110328u
#define RW_FILE_NOT_FOUND      0x80110329u
#define SAVE_MS_NOSPACE        0x80110383u
#define SAVE_PARAM             0x80110388u
#define DELETE_NO_DATA         0x80110347u
#define RESULT_CANCEL          1u

enum { ST_NONE, ST_INIT, ST_RUNNING, ST_FINISHED, ST_SHUTDOWN };
#define INIT_DELAY_US 200000u
#define SHUTDOWN_DELAY_US 2000u

/* --- host files ------------------------------------------------------------------- */
typedef struct { char name[256]; int is_dir; uint64_t size; int64_t mtime_us; } HostEnt;

#ifdef _WIN32
static int64_t filetime_us(FILETIME ft) {
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;  /* 100 ns since 1601 */
    return (int64_t)(t / 10u) - 11644473600000000LL;
}
#endif

/* All entries of a host folder except "." and "..", sorted by name; returns the count or -1. */
static int host_list(const char *dir, HostEnt **out) {
    HostEnt *v = NULL;
    int n = 0, cap = 0;
#ifdef _WIN32
    char pat[1024];
    snprintf(pat, sizeof(pat), "%s/*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { *out = NULL; return -1; }
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; v = (HostEnt *)realloc(v, (size_t)cap * sizeof(*v)); }
        snprintf(v[n].name, sizeof(v[n].name), "%s", fd.cFileName);
        v[n].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        v[n].size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        v[n].mtime_us = filetime_us(fd.ftLastWriteTime);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) { *out = NULL; return -1; }
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1024];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (stat(p, &st)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; v = (HostEnt *)realloc(v, (size_t)cap * sizeof(*v)); }
        snprintf(v[n].name, sizeof(v[n].name), "%s", e->d_name);
        v[n].is_dir = S_ISDIR(st.st_mode);
        v[n].size = (uint64_t)st.st_size;
        v[n].mtime_us = (int64_t)st.st_mtim.tv_sec * 1000000 + st.st_mtim.tv_nsec / 1000;
        n++;
    }
    closedir(d);
#endif
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(v[j].name, v[j - 1].name) < 0; j--) { HostEnt t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    *out = v;
    return n;
}

static int host_is_dir(const char *p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
#endif
}

static int host_is_file(const char *p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return !stat(p, &st) && !S_ISDIR(st.st_mode);
#endif
}

static void host_mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/' || *s == '\\') {
            char c = *s;
            *s = 0;
#ifdef _WIN32
            _mkdir(p);
#else
            mkdir(p, 0755);
#endif
            *s = c;
        }
#ifdef _WIN32
    _mkdir(p);
#else
    mkdir(p, 0755);
#endif
}

/* Sets a file's modification time (microseconds since 1970). */
static void host_set_mtime(const char *p, uint64_t us) {
#ifdef _WIN32
    HANDLE h = CreateFileA(p, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    uint64_t t = (us + 11644473600000000ULL) * 10u;
    FILETIME ft = { (DWORD)t, (DWORD)(t >> 32) };
    SetFileTime(h, NULL, &ft, &ft);
    CloseHandle(h);
#else
    struct timeval tv[2] = { { (time_t)(us / 1000000u), (suseconds_t)(us % 1000000u) },
                             { (time_t)(us / 1000000u), (suseconds_t)(us % 1000000u) } };
    utimes(p, tv);
#endif
}

static uint8_t *host_read(const char *p, uint32_t *size) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    *size = n > 0 ? (uint32_t)fread(b, 1, (size_t)n, f) : 0;
    fclose(f);
    return b;
}

static int host_write(const char *p, const void *data, uint32_t size) {
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, size, f) == size;
    ok = fclose(f) == 0 && ok;
    return ok;
}

/* --- PARAM.SFO (PPSSPP's ParamSFOData: keys kept sorted, each value padded to its max) -- */
enum { SFO_INT, SFO_UTF8, SFO_BIN };  /* formats 0x0404, 0x0204, 0x0004 */
typedef struct { char key[32]; int type; uint32_t i; uint8_t *data; uint32_t len, max; } SfoVal;
typedef struct { SfoVal v[32]; int n; } Sfo;

static void sfo_free(Sfo *s) { for (int i = 0; i < s->n; i++) free(s->v[i].data); s->n = 0; }

static SfoVal *sfo_slot(Sfo *s, const char *key) {
    int i = 0;
    while (i < s->n && strcmp(s->v[i].key, key) < 0) i++;
    if (i < s->n && !strcmp(s->v[i].key, key)) { free(s->v[i].data); s->v[i].data = NULL; return &s->v[i]; }
    if (s->n == 32) return NULL;
    memmove(&s->v[i + 1], &s->v[i], (size_t)(s->n - i) * sizeof(SfoVal));
    s->n++;
    memset(&s->v[i], 0, sizeof(SfoVal));
    snprintf(s->v[i].key, sizeof(s->v[i].key), "%s", key);
    return &s->v[i];
}

static void sfo_set_int(Sfo *s, const char *key, uint32_t v, uint32_t max) {
    SfoVal *e = sfo_slot(s, key);
    if (e) { e->type = SFO_INT; e->i = v; e->max = max; }
}

/* A string from a fixed-size field (not always NUL-terminated); kept below max. */
static void sfo_set_str(Sfo *s, const char *key, const char *str, size_t field, uint32_t max) {
    SfoVal *e = sfo_slot(s, key);
    if (!e) return;
    size_t n = 0;
    while (n < field && str[n]) n++;
    if (n > max - 1) n = max - 1;
    e->type = SFO_UTF8;
    e->data = (uint8_t *)calloc(1, n + 1);
    memcpy(e->data, str, n);
    e->len = (uint32_t)n + 1;
    e->max = max;
}

static void sfo_set_bin(Sfo *s, const char *key, const void *data, uint32_t len, uint32_t max) {
    SfoVal *e = sfo_slot(s, key);
    if (!e) return;
    e->type = SFO_BIN;
    e->data = (uint8_t *)malloc(len ? len : 1);
    memcpy(e->data, data, len);
    e->len = len;
    e->max = max;
}

static const SfoVal *sfo_get(const Sfo *s, const char *key) {
    for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i].key, key)) return &s->v[i];
    return NULL;
}

static int sfo_read(Sfo *s, const char *path) {
    memset(s, 0, sizeof(*s));
    uint32_t n = 0;
    uint8_t *b = host_read(path, &n);
    if (!b) return 0;
    uint32_t h[5];
    if (n < 20 || (memcpy(h, b, 20), h[0] != 0x46535000u) || h[2] > n || h[3] > n) { free(b); return 0; }
    for (uint32_t i = 0; i < h[4] && 20 + i * 16 + 16 <= n; i++) {
        const uint8_t *ix = b + 20 + i * 16;
        uint16_t koff, fmt;
        uint32_t len, max, doff;
        memcpy(&koff, ix, 2); memcpy(&fmt, ix + 2, 2); memcpy(&len, ix + 4, 4); memcpy(&max, ix + 8, 4); memcpy(&doff, ix + 12, 4);
        size_t k = h[2] + koff, d = h[3] + doff;
        if (k >= n || d >= n) break;
        char key[32];
        size_t kl = 0;
        while (k + kl < n && b[k + kl] && kl < sizeof(key) - 1) { key[kl] = (char)b[k + kl]; kl++; }
        key[kl] = 0;
        if (!kl) continue;
        if (fmt == 0x0404 && d + 4 <= n) { uint32_t v; memcpy(&v, b + d, 4); sfo_set_int(s, key, v, max); }
        else if (fmt == 0x0004 && d + len <= n) sfo_set_bin(s, key, b + d, len, max);
        else if (fmt == 0x0204) sfo_set_str(s, key, (const char *)b + d, n - d < max ? n - d : max, max ? max : 1);
    }
    free(b);
    return 1;
}

static int sfo_write(const Sfo *s, const char *path) {
    uint32_t keys = 0, data = 0;
    for (int i = 0; i < s->n; i++) { keys += (uint32_t)strlen(s->v[i].key) + 1; data += s->v[i].max; }
    keys = (keys + 3) & ~3u;
    uint32_t ktab = 20 + 16 * (uint32_t)s->n, dtab = ktab + keys, total = dtab + data;
    uint8_t *b = (uint8_t *)calloc(1, total);
    uint32_t h[5] = { 0x46535000u, 0x00000101u, ktab, dtab, (uint32_t)s->n };
    memcpy(b, h, 20);
    uint32_t ko = 0, dof = 0;
    for (int i = 0; i < s->n; i++) {
        const SfoVal *e = &s->v[i];
        uint16_t koff = (uint16_t)ko, fmt = e->type == SFO_INT ? 0x0404 : e->type == SFO_UTF8 ? 0x0204 : 0x0004;
        uint32_t len = e->type == SFO_INT ? 4 : e->len;
        uint8_t *ix = b + 20 + i * 16;
        memcpy(ix, &koff, 2); memcpy(ix + 2, &fmt, 2); memcpy(ix + 4, &len, 4); memcpy(ix + 8, &e->max, 4); memcpy(ix + 12, &dof, 4);
        memcpy(b + ktab + ko, e->key, strlen(e->key) + 1);
        if (e->type == SFO_INT) memcpy(b + dtab + dof, &e->i, 4);
        else memcpy(b + dtab + dof, e->data, len < e->max ? len : e->max);
        ko += (uint32_t)strlen(e->key) + 1;
        dof += e->max;
    }
    int ok = host_write(path, b, total);
    free(b);
    return ok;
}

/* --- the request ------------------------------------------------------------------------ */
static uint8_t g_rq[REQ_MAX], g_orig[REQ_MAX];  /* working copy and the guest's request as read */
static uint32_t g_req_addr, g_req_size;
static int g_status, g_pending = -1;
static uint64_t g_pending_at;
static int g_action, g_io_done;
static uint64_t g_loads, g_saves, g_inits;

static uint32_t rq32(uint32_t off) { uint32_t v; memcpy(&v, g_rq + off, 4); return v; }
static void wq32(uint32_t off, uint32_t v) { memcpy(g_rq + off, &v, 4); }
static void rq_str(uint32_t off, size_t field, char *out, size_t outsz) {
    size_t n = 0;
    while (n < field && n + 1 < outsz && g_rq[off + n]) { out[n] = (char)g_rq[off + n]; n++; }
    out[n] = 0;
}
static uint32_t mode(void) { return rq32(P_MODE); }
static const char *mode_name(uint32_t m) { return m < sizeof(k_mode_names) / sizeof(k_mode_names[0]) ? k_mode_names[m] : "UNKNOWN"; }
static int valid_ptr(uint32_t p) {
    p &= RT_ADDR_MASK;
    return (p >= 0x08000000u && p < 0x0A000000u) || (p >= 0x04000000u && p < 0x04200000u) || (p >= 0x00010000u && p < 0x00014000u);
}
static int has_key(void) { for (int i = 0; i < 16; i++) if (g_rq[P_KEY + i]) return 1; return 0; }

static void game_name(char *out) { rq_str(P_GAME, 13, out, 14); }
static void save_name(char *out) {  /* "<>" means any name: empty */
    rq_str(P_SAVE, 20, out, 21);
    if (!strcmp(out, "<>")) out[0] = 0;
}
static void file_name(char *out) { rq_str(P_FILE, 13, out, 14); }

static void savedata_root(char *out, size_t n) { snprintf(out, n, "%s/PSP/SAVEDATA", rt_io_ms0()); }
static void save_dir_path(const char *dirname, char *out, size_t n) {
    char game[14];
    game_name(game);
    snprintf(out, n, "%s/PSP/SAVEDATA/%s%s", rt_io_ms0(), game, dirname);
}

/* --- the list of saves the dialog shows (SavedataParam::SetPspParam) ------------------------ */
typedef struct { char name[21]; char dir[256]; uint64_t size; int64_t mtime_us; int broken; } SaveEntry;
#define MAX_LIST 256
static SaveEntry g_list[MAX_LIST];
static int g_count, g_selected;

static int multi_save_name(uint32_t m) {
    switch (m) {
    case LOAD: case AUTOLOAD: case SAVE: case AUTOSAVE: case MAKEDATASECURE: case MAKEDATA: case READDATASECURE:
    case READDATA: case WRITEDATASECURE: case WRITEDATA: case AUTODELETE: case DELETE_: case ERASESECURE: case ERASE:
    case DELETEDATA:
        return 0;
    default:
        return 1;
    }
}

/* Size of every file in a save folder, and the newest file time; 0 if there is no folder. */
static int save_info(const char *path, SaveEntry *e) {
    HostEnt *v;
    int n = host_list(path, &v);
    if (n < 0) return 0;
    e->size = 0;
    e->mtime_us = 0;
    for (int i = 0; i < n; i++)
        if (!v[i].is_dir) {
            e->size += v[i].size;
            if (v[i].mtime_us > e->mtime_us) e->mtime_us = v[i].mtime_us;
        }
    free(v);
    char sfo[1024];
    snprintf(sfo, sizeof(sfo), "%s/PARAM.SFO", path);
    e->broken = !host_is_file(sfo);
    return 1;
}

static void list_add(const char *name, const char *dir, int exists) {
    if (g_count >= MAX_LIST) return;
    SaveEntry *e = &g_list[g_count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->dir, sizeof(e->dir), "%s", dir);
    if (exists) {
        char path[1024], root[600];
        savedata_root(root, sizeof(root));
        snprintf(path, sizeof(path), "%s/%s", root, dir);
        save_info(path, e);
    }
}

static int in_list(const char *name) {
    for (int i = 0; i < g_count; i++) if (!strcmp(g_list[i].name, name)) return 1;
    return 0;
}

static void build_list(void) {
    char root[600], game[14], path[1024];
    savedata_root(root, sizeof(root));
    game_name(game);
    g_count = 0;
    HostEnt *all = NULL;
    int nall = host_list(root, &all);
    uint32_t m = mode();
    if (m == LISTALLDELETE) {  /* every save on the memory stick */
        for (int i = 0; i < nall; i++) if (all[i].is_dir) list_add("", all[i].name, 1);
        free(all);
        return;
    }
    int list_empty = !(m == LISTLOAD || m == LISTDELETE);
    uint32_t names = rq32(P_SAVELIST);
    int multi = 0;
    if (valid_ptr(names) && multi_save_name(m)) {
        int count = 0;
        while (count < 4096 && rt_r8(names + (uint32_t)count * 20)) count++;
        multi = count > 0;
        size_t glen = strlen(game);
        for (int i = 0; i < count; i++) {
            char name[21];
            memcpy(name, rt_ptr(names + (uint32_t)i * 20), 20);
            name[20] = 0;
            if (!strcmp(name, "<>")) {  /* any save of this game that is not listed yet */
                for (int j = 0; j < nall; j++)
                    if (all[j].is_dir && !strncmp(all[j].name, game, glen) && !in_list(all[j].name + glen)) {
                        list_add(all[j].name + glen, all[j].name, 1);
                        break;
                    }
                continue;
            }
            char dir[64];
            snprintf(dir, sizeof(dir), "%s%s", game, name);
            snprintf(path, sizeof(path), "%s/%s", root, dir);
            int exists = host_is_dir(path);
            if (exists || list_empty) list_add(name, dir, exists);
        }
    }
    if (!multi) {
        char name[21], dir[64];
        save_name(name);
        snprintf(dir, sizeof(dir), "%s%s", game, name);
        snprintf(path, sizeof(path), "%s/%s", root, dir);
        int exists = host_is_dir(path);
        list_add(name, dir, exists);
        if (!exists) g_count = 0;  /* as on PPSSPP: an empty entry, but no count */
    }
    free(all);
}

static int newest(void) {
    int idx = -1;
    for (int i = 0; i < g_count; i++)
        if (g_list[i].size && (idx < 0 || g_list[i].mtime_us > g_list[idx].mtime_us)) idx = i;
    return idx;
}
static int oldest(void) {
    int idx = -1;
    for (int i = 0; i < g_count; i++)
        if (g_list[i].size && (idx < 0 || g_list[i].mtime_us < g_list[idx].mtime_us)) idx = i;
    return idx;
}
static int first_empty(void) { for (int i = 0; i < g_count; i++) if (!g_list[i].size) return i; return -1; }
static int named(const char *name) { for (int i = 0; i < g_count; i++) if (!strcmp(g_list[i].name, name)) return i; return -1; }

/* --- Save / Load (SavedataParam::Save, ::Load) ------------------------------------------ */
typedef struct { uint32_t buf, buf_size, size; } FileData;
static FileData file_data(uint32_t off) { FileData d = { rq32(off), rq32(off + 4), rq32(off + 8) }; return d; }

static void put_file(const char *dir, const char *name, uint32_t addr, uint32_t size, uint64_t mtime) {
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    if (host_write(p, rt_ptr(addr), size)) host_set_mtime(p, mtime);
}

static uint32_t do_save(const char *dirname, int secure) {
    FileData icon0 = file_data(P_ICON0), icon1 = file_data(P_ICON1), pic1 = file_data(P_PIC1), snd0 = file_data(P_SND0);
    if (rq32(P_DATASIZE) > rq32(P_DATABUFSIZE)) return RW_BAD_PARAMS;
    const FileData *subs[4] = { &icon0, &icon1, &pic1, &snd0 };
    for (int i = 0; i < 4; i++) if (valid_ptr(subs[i]->buf) && subs[i]->buf_size < subs[i]->size) return RW_BAD_PARAMS;
    uint32_t secure_ver = rq32(P_SECUREVER);
    if (secure_ver > 3) return SAVE_PARAM;
    if (secure_ver != 0 && secure_ver != 1 && !has_key() && secure) return SAVE_PARAM;
    if (secure && (secure_ver || has_key())) {
        static int once;
        if (!once++) rt_log("[savedata] the game asks for encrypted saves (secureVersion %u, key %s): stored unencrypted, "
                            "as PPSSPP does with save encryption off\n", secure_ver, has_key() ? "set" : "none");
    }
    char dir[1024], game[14], fname[14], sfo_path[1100];
    save_dir_path(dirname, dir, sizeof(dir));
    host_mkdirs(dir);
    if (!host_is_dir(dir)) return SAVE_MS_NOSPACE;
    uint64_t now = rt_wall_time_us();
    game_name(game);
    file_name(fname);

    Sfo sfo;
    snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", dir);
    if (!sfo_read(&sfo, sfo_path)) memset(&sfo, 0, sizeof(sfo));
    if (g_rq[P_TITLE]) {
        char sdir[64];
        snprintf(sdir, sizeof(sdir), "%s%s", game, dirname);
        sfo_set_str(&sfo, "TITLE", (const char *)g_rq + P_TITLE, 0x80, 128);
        sfo_set_str(&sfo, "SAVEDATA_TITLE", (const char *)g_rq + P_SAVETITLE, 0x80, 128);
        sfo_set_str(&sfo, "SAVEDATA_DETAIL", (const char *)g_rq + P_DETAIL, 0x400, 1024);
        sfo_set_int(&sfo, "PARENTAL_LEVEL", g_rq[P_PARENTAL], 4);
        sfo_set_str(&sfo, "CATEGORY", "MS", 3, 4);
        sfo_set_str(&sfo, "SAVEDATA_DIRECTORY", sdir, sizeof(sdir), 64);
    }
    /* file list: 99 entries of name[13], hash[16], pad[3]; the hash stays 0 (not encrypted) */
    enum { LIST_BYTES = 99 * 32 };
    uint8_t list[LIST_BYTES];
    memset(list, 0, sizeof(list));
    const SfoVal *old = sfo_get(&sfo, "SAVEDATA_FILE_LIST");
    if (old && old->data) memcpy(list, old->data, old->len < LIST_BYTES ? old->len : LIST_BYTES);
    if (secure && valid_ptr(rq32(P_DATABUF))) {
        for (int i = 0; i < 99; i++) {
            char *e = (char *)list + i * 32;
            if (e[0] && strncmp(e, fname, 13)) continue;
            memset(e, 0, 32);
            memcpy(e, fname, strlen(fname));
            break;
        }
    }
    sfo_set_bin(&sfo, "SAVEDATA_FILE_LIST", list, LIST_BYTES, LIST_BYTES);
    uint8_t zeros[128] = { 0 };
    sfo_set_bin(&sfo, "SAVEDATA_PARAMS", zeros, 128, 128);
    int sfo_ok = sfo_write(&sfo, sfo_path);
    sfo_free(&sfo);
    if (sfo_ok) host_set_mtime(sfo_path, now);

    uint32_t buf = rq32(P_DATABUF);
    if (valid_ptr(buf)) {
        uint32_t size = rq32(P_DATASIZE);
        if (size == 0 || size > rq32(P_DATABUFSIZE)) size = rq32(P_DATABUFSIZE);
        memset(g_rq + P_SAVE, 0, 20);  /* the chosen save's name goes back in the request */
        memcpy(g_rq + P_SAVE, dirname, strlen(dirname) < 20 ? strlen(dirname) : 20);
        if (fname[0]) {
            char p[1100];
            snprintf(p, sizeof(p), "%s/%s", dir, fname);
            if (!host_write(p, rt_ptr(buf), size)) return SAVE_MS_NOSPACE;
            host_set_mtime(p, now);
        }
    }
    if (valid_ptr(icon0.buf)) put_file(dir, "ICON0.PNG", icon0.buf, icon0.size, now);
    if (valid_ptr(icon1.buf) && icon1.size) put_file(dir, "ICON1.PMF", icon1.buf, icon1.size, now);
    if (valid_ptr(pic1.buf)) put_file(dir, "PIC1.PNG", pic1.buf, pic1.size, now);
    if (valid_ptr(snd0.buf) && snd0.size) put_file(dir, "SND0.AT3", snd0.buf, snd0.size, now);
    g_saves++;
    return 0;
}

static void load_sub(const char *dir, const char *name, uint32_t off) {
    FileData d = file_data(off);
    if (!valid_ptr(d.buf)) return;
    char p[1100];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    uint32_t n = 0;
    uint8_t *b = host_read(p, &n);
    if (!b) return;
    if (n > d.buf_size) n = d.buf_size;
    memcpy(rt_ptr(d.buf), b, n);
    wq32(off + 8, n);
    free(b);
}

static uint32_t do_load(const char *dirname, int secure) {
    int rw = mode() == READDATA || mode() == READDATASECURE;
    char dir[1024], fname[14], path[1100];
    save_dir_path(dirname, dir, sizeof(dir));
    file_name(fname);
    snprintf(path, sizeof(path), "%s/%s", dir, fname);
    if (!host_is_dir(dir)) return rw ? RW_NO_DATA : LOAD_NO_DATA;
    if (fname[0] && !host_is_file(path)) return rw ? RW_FILE_NOT_FOUND : LOAD_FILE_NOT_FOUND;
    wq32(P_DATASIZE, 0);
    uint32_t secure_ver = rq32(P_SECUREVER);
    if (secure_ver > 3) return LOAD_PARAM;
    if (secure_ver != 0 && secure_ver != 1 && !has_key() && secure) return LOAD_PARAM;

    Sfo sfo;
    char sfo_path[1100];
    snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", dir);
    int have_sfo = sfo_read(&sfo, sfo_path);
    if (fname[0]) {
        uint32_t n = 0;
        uint8_t *b = host_read(path, &n);
        if (!b) { sfo_free(&sfo); return LOAD_NO_DATA; }
        memset(g_rq + P_SAVE, 0, 20);
        memcpy(g_rq + P_SAVE, dirname, strlen(dirname) < 20 ? strlen(dirname) : 20);
        const SfoVal *params = have_sfo ? sfo_get(&sfo, "SAVEDATA_PARAMS") : NULL;
        if (secure && params && params->len && params->data[0]) {
            rt_log("[savedata] %s/%s is encrypted (SAVEDATA_PARAMS %02X): the port cannot decrypt saves, "
                   "reported as broken\n", dirname, fname, params->data[0]);
            free(b);
            sfo_free(&sfo);
            return LOAD_DATA_BROKEN;
        }
        uint32_t buf = rq32(P_DATABUF);
        if (valid_ptr(buf)) {
            if (n > rq32(P_DATABUFSIZE)) { free(b); sfo_free(&sfo); return LOAD_DATA_BROKEN; }
            memcpy(rt_ptr(buf), b, n);
        }
        free(b);
        wq32(P_DATASIZE, n);
    }
    if (!have_sfo) return rw ? RW_DATA_BROKEN : LOAD_DATA_BROKEN;
    static const struct { const char *key; uint32_t off, size; } strs[] = {
        { "TITLE", P_TITLE, 0x80 }, { "SAVEDATA_TITLE", P_SAVETITLE, 0x80 }, { "SAVEDATA_DETAIL", P_DETAIL, 0x400 },
    };
    for (int i = 0; i < 3; i++) {
        const SfoVal *v = sfo_get(&sfo, strs[i].key);
        memset(g_rq + strs[i].off, 0, strs[i].size);
        if (v && v->type == SFO_UTF8) memcpy(g_rq + strs[i].off, v->data, strlen((const char *)v->data) < strs[i].size ? strlen((const char *)v->data) : strs[i].size);
    }
    const SfoVal *pl = sfo_get(&sfo, "PARENTAL_LEVEL");
    g_rq[P_PARENTAL] = pl && pl->type == SFO_INT ? (uint8_t)pl->i : 0;
    sfo_free(&sfo);
    wq32(P_BIND, 1021);  /* what a PSP answers, per PPSSPP */
    load_sub(dir, "ICON0.PNG", P_ICON0);
    load_sub(dir, "ICON1.PMF", P_ICON1);
    load_sub(dir, "PIC1.PNG", P_PIC1);
    load_sub(dir, "SND0.AT3", P_SND0);
    g_loads++;
    return 0;
}

/* --- what the dialog does ------------------------------------------------------------- */
enum { ACT_IO, ACT_LISTLOAD, ACT_LISTSAVE, ACT_LOAD, ACT_SAVE, ACT_CANCEL_DELETE, ACT_LOAD_NODATA, ACT_DELETE_NODATA,
       ACT_LOAD_BROKEN, ACT_UNSUPPORTED };

static void log_result(const char *what, const char *slot, uint32_t r) {
    char game[14], fname[14];
    game_name(game);
    file_name(fname);
    const char *meaning = r == 0 ? "ok" : r == RESULT_CANCEL ? "cancelled" : r == LOAD_NO_DATA ? "no data"
                        : r == LOAD_DATA_BROKEN ? "data broken" : r == LOAD_FILE_NOT_FOUND ? "file not found" : "error";
    rt_log("[savedata] %s %s%s/%s (%u bytes) -> %08X (%s)\n", what, game, slot, fname, rq32(P_DATASIZE), r, meaning);
}

/* The dialog's choice for the list modes: --save-slot, else the policy described on top. */
static int choose(int save) {
    if (rt_savedata_slot) {
        int i = named(rt_savedata_slot);
        if (i >= 0) return i;
        rt_log("[savedata] --save-slot %s is not in the game's list; choosing as usual\n", rt_savedata_slot);
    }
    if (!save) return newest();
    int i = first_empty();
    return i >= 0 ? i : oldest();
}

static void run_action(void) {
    uint32_t m = mode(), r = 0;
    char name[21];
    switch (g_action) {
    case ACT_IO:
        save_name(name);
        if (m == AUTOLOAD || m == LOAD) { r = do_load(name, 1); log_result(mode_name(m), name, r); }
        else if (m == AUTOSAVE || m == SAVE) { r = do_save(name, 1); log_result(mode_name(m), name, r); }
        else if (m == READDATA || m == READDATASECURE) {
            r = do_load(name, m == READDATASECURE);
            if (r == LOAD_DATA_BROKEN) r = RW_DATA_BROKEN;
            if (r == LOAD_NO_DATA) r = RW_NO_DATA;
            log_result(mode_name(m), name, r);
        } else if (m == WRITEDATA || m == WRITEDATASECURE || m == MAKEDATA || m == MAKEDATASECURE) {
            r = do_save(name, m == WRITEDATASECURE || m == MAKEDATASECURE);
            log_result(mode_name(m), name, r);
        }
        break;
    case ACT_LOAD:
        save_name(name);
        r = do_load(name, 1);
        log_result("LOAD", name, r);  /* a failure keeps its error, as after PPSSPP's DS_LOAD_FAILED */
        break;
    case ACT_SAVE:
        save_name(name);
        r = do_save(name, 1) == 0 ? 0 : RESULT_CANCEL;
        log_result("SAVE", name, r);
        break;
    case ACT_LISTLOAD: {
        int i = choose(0);
        g_selected = i;
        if (i < 0) { r = LOAD_NO_DATA; log_result("LISTLOAD", "", r); break; }
        r = do_load(g_list[i].name, 1);
        log_result("LISTLOAD", g_list[i].name, r);
        if (r) r = RESULT_CANCEL;  /* back to the list after the error, where the player cancels */
        break;
    }
    case ACT_LISTSAVE: {
        int i = choose(1);
        g_selected = i;
        if (i < 0) { r = RESULT_CANCEL; rt_log("[savedata] LISTSAVE: the game listed no slots (cancelled)\n"); break; }
        r = do_save(g_list[i].name, 1);
        log_result("LISTSAVE", g_list[i].name, r);
        if (r) r = RESULT_CANCEL;
        break;
    }
    case ACT_CANCEL_DELETE:
        r = RESULT_CANCEL;
        rt_log("[savedata] %s: %d saves listed; the port deletes nothing (dialog cancelled)\n", mode_name(m), g_count);
        break;
    case ACT_LOAD_NODATA: r = LOAD_NO_DATA; log_result(mode_name(m), "", r); break;
    case ACT_DELETE_NODATA: r = DELETE_NO_DATA; break;
    case ACT_LOAD_BROKEN: r = LOAD_DATA_BROKEN; break;
    default:
        r = 0;
        break;
    }
    wq32(P_RESULT, r);
}

static int get_status(void) {
    if (g_pending >= 0 && k_now >= g_pending_at) { g_status = g_pending; g_pending = -1; }
    return g_status;
}
static void change_status(int st, uint32_t delay_us) {
    if (!delay_us) { g_status = st; g_pending = -1; }
    else { g_pending = st; g_pending_at = k_now + delay_us; }
}

static void read_request(void) {
    memset(g_rq, 0, sizeof(g_rq));
    memcpy(g_rq, rt_ptr(g_req_addr), g_req_size);
    memcpy(g_orig, g_rq, sizeof(g_rq));
    build_list();
}

static void hle_SavedataInitStart(RtCtx *c) {
    if (get_status() != ST_NONE) { rt_log("[savedata] InitStart while a request is running\n"); RET(ERR_INVALID_STATUS); }
    uint32_t size = rt_r32(A0);
    if (size != 1480 && size != 1500 && size != 1536) {
        rt_log("[savedata] InitStart: invalid request size %u\n", size);
        RET(ERR_INVALID_PARAM_SIZE);
    }
    g_req_addr = A0;
    g_req_size = size;
    g_io_done = 0;
    g_selected = -1;
    g_inits++;
    read_request();
    uint32_t m = mode();
    switch (m) {
    case LOAD:
        g_action = g_count && g_list[0].size ? (g_list[0].broken ? ACT_LOAD_BROKEN : ACT_LOAD) : ACT_LOAD_NODATA;
        break;
    case LISTLOAD: g_action = g_count ? ACT_LISTLOAD : ACT_LOAD_NODATA; break;
    case SAVE: g_action = ACT_SAVE; break;
    case LISTSAVE: g_action = ACT_LISTSAVE; break;
    case LISTALLDELETE: case LISTDELETE: g_action = g_count ? ACT_CANCEL_DELETE : ACT_DELETE_NODATA; break;
    case DELETE_: g_action = g_count && g_list[0].size ? ACT_CANCEL_DELETE : ACT_DELETE_NODATA; break;
    case AUTOLOAD: case AUTOSAVE: case READDATA: case READDATASECURE: case WRITEDATA: case WRITEDATASECURE:
    case MAKEDATA: case MAKEDATASECURE:
        g_action = ACT_IO;
        break;
    default:
        g_action = ACT_UNSUPPORTED;
        break;
    }
    char game[14], name[21], fname[14];
    game_name(game);
    rq_str(P_SAVE, 20, name, sizeof(name));
    file_name(fname);
    char range[48] = "";
    if (g_count > 1) snprintf(range, sizeof(range), " (%s..%s)", g_list[0].name, g_list[g_count - 1].name);
    rt_log("[savedata] InitStart %s (%u): game \"%s\" save \"%s\" file \"%s\" buffer %u bytes, %d listed%s, focus %u, "
           "secureVersion %u%s, title \"%.40s\"\n", mode_name(m), m, game, name, fname, rq32(P_DATABUFSIZE), g_count, range,
           rq32(P_FOCUS), rq32(P_SECUREVER), has_key() ? ", key set" : "", (const char *)g_rq + P_TITLE);
    for (int i = 0; i < g_count && rt_hle_verbose; i++)
        rt_log("[savedata]   %2d %-20s %s %llu bytes\n", i, g_list[i].name, g_list[i].dir, (unsigned long long)g_list[i].size);
    if (g_action == ACT_UNSUPPORTED) {  /* PPSSPP: "not coded", result 0 */
        rt_log("[savedata] mode %s (%u) is not implemented in the port: result 0\n", mode_name(m), m);
        wq32(P_RESULT, 0);
    }
    change_status(ST_INIT, 0);
    change_status(ST_RUNNING, INIT_DELAY_US);
    RET(0);
}

static void hle_SavedataGetStatus(RtCtx *c) { RET(get_status()); }

static void hle_SavedataUpdate(RtCtx *c) {
    if (get_status() != ST_RUNNING) RET(ERR_INVALID_STATUS);
    if (memcmp(rt_ptr(g_req_addr), g_orig, g_req_size)) read_request();  /* the game changed the request */
    if (!g_io_done) {
        if (g_action != ACT_UNSUPPORTED) run_action();
        g_io_done = 1;
    } else {
        change_status(ST_FINISHED, 0);
        memcpy(rt_ptr(g_req_addr), g_rq, g_req_size);
    }
    k_wait(WT_DELAY, 0, 300);  /* PPSSPP delays the result by 300 us */
    RET(0);
}

static void hle_SavedataShutdownStart(RtCtx *c) {
    if (get_status() != ST_FINISHED) RET(ERR_INVALID_STATUS);
    change_status(ST_SHUTDOWN, 0);
    change_status(ST_NONE, SHUTDOWN_DELAY_US);
    RET(0);
}

void rt_savedata_report(void) {
    if (g_inits) printf("[savedata] %llu requests: %llu saves written, %llu loaded (folder %s/PSP/SAVEDATA)\n",
                        (unsigned long long)g_inits, (unsigned long long)g_saves, (unsigned long long)g_loads, rt_io_ms0());
}

const HleEntry rt_hle_savedata[] = {
    {"sceUtilitySavedataInitStart", hle_SavedataInitStart}, {"sceUtilitySavedataGetStatus", hle_SavedataGetStatus},
    {"sceUtilitySavedataUpdate", hle_SavedataUpdate}, {"sceUtilitySavedataShutdownStart", hle_SavedataShutdownStart},
    {NULL, NULL},
};
