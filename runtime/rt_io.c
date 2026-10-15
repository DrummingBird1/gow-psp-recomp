/* File I/O: the user's disc image (CHD via libchdr, or a plain ISO), ISO 9660, and the
 * IoFileMgrForUser / sceUmdUser HLE functions.
 *
 * Devices: disc0:/umd0:/umd1: read from the disc (including "sce_lbn0x<lba>_size0x<n>"
 * raw-sector paths); ms0: maps to a local folder (--ms0). Asynchronous calls complete
 * immediately; PollAsync/WaitAsync return the stored result, as on PPSSPP. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_host(p) _mkdir(p)
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
#include <sys/stat.h>
#define mkdir_host(p) mkdir(p, 0755)
#define fseek64 fseeko
#define ftell64 ftello
#endif

#include <libchdr/chd.h>

#include "rt_kernel.h"

#define SECTOR 2048u
#define MAX_FD 64

/* --- disc image --------------------------------------------------------------- */
static FILE *g_iso;
static chd_file *g_chd;
static uint32_t g_hunk_bytes, g_cached_hunk = UINT32_MAX;
static uint8_t *g_hunk;
static uint64_t g_disc_size;
static char g_ms0[512] = "ms0";

int rt_io_open_disc(const char *path) {
    size_t n = strlen(path);
    if (n > 4 && (!strcmp(path + n - 4, ".chd") || !strcmp(path + n - 4, ".CHD"))) {
        chd_error err = chd_open(path, CHD_OPEN_READ, NULL, &g_chd);
        if (err != CHDERR_NONE) { fprintf(stderr, "rt: cannot open CHD %s (%s)\n", path, chd_error_string(err)); return 0; }
        const chd_header *h = chd_get_header(g_chd);
        g_hunk_bytes = h->hunkbytes;
        g_hunk = (uint8_t *)malloc(g_hunk_bytes);
        g_disc_size = h->logicalbytes;
    } else {
        g_iso = fopen(path, "rb");
        if (!g_iso) { fprintf(stderr, "rt: cannot open disc image %s\n", path); return 0; }
        fseek64(g_iso, 0, SEEK_END);
        g_disc_size = (uint64_t)ftell64(g_iso);
    }
    rt_log("[io] disc image %s: %.1f MB%s\n", path, (double)g_disc_size / (1 << 20), g_chd ? " (CHD)" : "");
    return 1;
}

void rt_io_set_ms0(const char *dir) { strncpy(g_ms0, dir, sizeof(g_ms0) - 1); }

static uint32_t disc_read(uint64_t off, uint8_t *dst, uint32_t len) {
    if (off >= g_disc_size) return 0;
    if (off + len > g_disc_size) len = (uint32_t)(g_disc_size - off);
    if (g_iso) {
        fseek64(g_iso, (int64_t)off, SEEK_SET);
        return (uint32_t)fread(dst, 1, len, g_iso);
    }
    if (!g_chd) return 0;
    uint32_t done = 0;
    while (done < len) {
        uint32_t hunk = (uint32_t)((off + done) / g_hunk_bytes), in = (uint32_t)((off + done) % g_hunk_bytes);
        if (hunk != g_cached_hunk) {
            if (chd_read(g_chd, hunk, g_hunk) != CHDERR_NONE) break;
            g_cached_hunk = hunk;
        }
        uint32_t n = g_hunk_bytes - in < len - done ? g_hunk_bytes - in : len - done;
        memcpy(dst + done, g_hunk + in, n);
        done += n;
    }
    return done;
}

/* --- ISO 9660 ----------------------------------------------------------------- */
typedef struct { uint32_t lba, size; int is_dir; char name[256]; } IsoEntry;

static int iso_root(IsoEntry *root) {
    uint8_t pvd[SECTOR];
    if (disc_read(16ull * SECTOR, pvd, SECTOR) != SECTOR || memcmp(pvd + 1, "CD001", 5)) return 0;
    const uint8_t *r = pvd + 156;
    memcpy(&root->lba, r + 2, 4);
    memcpy(&root->size, r + 10, 4);
    root->is_dir = 1;
    strcpy(root->name, "");
    return 1;
}

/* Calls fn for every entry of a directory; stops early when fn returns nonzero. */
static int iso_list(const IsoEntry *dir, int (*fn)(const IsoEntry *, void *), void *arg) {
    uint8_t *buf = (uint8_t *)malloc(dir->size ? dir->size : SECTOR);
    uint32_t n = disc_read((uint64_t)dir->lba * SECTOR, buf, dir->size);
    int stop = 0;
    for (uint32_t pos = 0; pos < n && !stop;) {
        uint8_t len = buf[pos];
        if (len == 0) { pos = (pos / SECTOR + 1) * SECTOR; continue; }
        const uint8_t *r = buf + pos;
        uint8_t nlen = r[32];
        if (!(nlen == 1 && (r[33] == 0 || r[33] == 1))) {
            IsoEntry e;
            memcpy(&e.lba, r + 2, 4);
            memcpy(&e.size, r + 10, 4);
            e.is_dir = (r[25] & 2) != 0;
            memcpy(e.name, r + 33, nlen);
            e.name[nlen] = 0;
            char *semi = strchr(e.name, ';');
            if (semi) *semi = 0;
            stop = fn(&e, arg);
        }
        pos += len;
    }
    free(buf);
    return stop;
}

typedef struct { const char *want; IsoEntry found; int ok; } FindArg;
static int find_cb(const IsoEntry *e, void *p) {
    FindArg *f = (FindArg *)p;
#ifdef _WIN32
    if (!_stricmp(e->name, f->want)) { f->found = *e; f->ok = 1; return 1; }
#else
    if (!strcasecmp(e->name, f->want)) { f->found = *e; f->ok = 1; return 1; }
#endif
    return 0;
}

/* path: components below the disc root, '/'-separated, already normalized */
static int iso_lookup(const char *path, IsoEntry *out) {
    IsoEntry cur;
    if (!iso_root(&cur)) return 0;
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    for (char *tok = strtok(tmp, "/"); tok; tok = strtok(NULL, "/")) {
        if (!cur.is_dir) return 0;
        FindArg f = { tok, {0}, 0 };
        iso_list(&cur, find_cb, &f);
        if (!f.ok) return 0;
        cur = f.found;
    }
    *out = cur;
    return 1;
}

/* --- paths -------------------------------------------------------------------- */
static char g_cwd[512] = "disc0:/";
enum { DEV_NONE, DEV_DISC, DEV_MS0 };

/* Splits "dev:/a/b" (or a cwd-relative path) into a device and a normalized path below it. */
static int resolve(const char *in, char *out, size_t outsz) {
    char full[1024];
    if (strchr(in, ':')) snprintf(full, sizeof(full), "%s", in);
    else snprintf(full, sizeof(full), "%s/%s", g_cwd, in);
    char *colon = strchr(full, ':');
    *colon = 0;
    const char *dev = full, *rest = colon + 1;
    int kind = !strncmp(dev, "disc", 4) || !strncmp(dev, "umd", 3) ? DEV_DISC : !strncmp(dev, "ms", 2) || !strncmp(dev, "ef", 2) ? DEV_MS0 : DEV_NONE;
    /* normalize: collapse separators, resolve "." and ".." */
    char parts[64][128];
    int np = 0;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", rest);
    for (char *p = tmp; *p; p++) if (*p == '\\') *p = '/';
    for (char *tok = strtok(tmp, "/"); tok && np < 64; tok = strtok(NULL, "/")) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (np) np--; continue; }
        snprintf(parts[np++], sizeof(parts[0]), "%s", tok);
    }
    out[0] = 0;
    for (int i = 0; i < np; i++) {
        strncat(out, parts[i], outsz - strlen(out) - 1);
        if (i + 1 < np) strncat(out, "/", outsz - strlen(out) - 1);
    }
    return kind;
}

static int parse_lbn(const char *path, uint32_t *lba, uint32_t *size) {
    const char *p = strstr(path, "sce_lbn");
    if (!p) return 0;
    unsigned a = 0, b = 0;
    if (sscanf(p, "sce_lbn0x%x_size0x%x", &a, &b) != 2 && sscanf(p, "sce_lbn%x_size%x", &a, &b) != 2) return 0;
    *lba = a; *size = b;
    return 1;
}

/* --- file descriptors ----------------------------------------------------------- */
typedef struct {
    int used, is_dir, close_pending;
    uint32_t lba;
    uint64_t size, pos;
    FILE *host;
    int has_async;
    int64_t async_result;
    IsoEntry *ents;
    int nents, dirpos;
    char path[256];
} Fd;
static Fd g_fd[MAX_FD];

static int fd_alloc(void) {
    for (int i = 3; i < MAX_FD; i++) if (!g_fd[i].used) { memset(&g_fd[i], 0, sizeof(Fd)); g_fd[i].used = 1; return i; }
    return -1;
}
static Fd *fd_get(uint32_t fd) { return fd < MAX_FD && g_fd[fd].used ? &g_fd[fd] : NULL; }
static void fd_free(int fd) {
    Fd *f = &g_fd[fd];
    if (f->host) fclose(f->host);
    free(f->ents);
    memset(f, 0, sizeof(*f));
}

static void host_path(const char *rel, char *out, size_t n) { snprintf(out, n, "%s/%s", g_ms0, rel); }

/* Returns fd (>= 3) or a negative PSP error */
static int32_t io_open(const char *path, uint32_t flags) {
    char rel[512];
    int dev = resolve(path, rel, sizeof(rel));
    int fd = fd_alloc();
    if (fd < 0) return (int32_t)0x80020320u;  /* too many open files */
    Fd *f = &g_fd[fd];
    snprintf(f->path, sizeof(f->path), "%s", path);
    if (dev == DEV_DISC) {
        uint32_t lba, size;
        IsoEntry e;
        if (parse_lbn(rel, &lba, &size)) { f->lba = lba; f->size = size; }
        else if (iso_lookup(rel, &e) && !e.is_dir) { f->lba = e.lba; f->size = e.size; }
        else { fd_free(fd); return (int32_t)SCE_ERR_FILE_NOT_FOUND; }
        return fd;
    }
    if (dev == DEV_MS0) {
        char hp[1024];
        host_path(rel, hp, sizeof(hp));
        const char *mode = (flags & 0x0200) ? ((flags & 0x0002) ? "w+b" : "wb") : (flags & 0x0002) ? "r+b" : "rb";
        f->host = fopen(hp, mode);
        if (!f->host && (flags & 0x0200)) f->host = fopen(hp, "w+b");
        if (!f->host) { fd_free(fd); return (int32_t)SCE_ERR_FILE_NOT_FOUND; }
        fseek(f->host, 0, SEEK_END);
        f->size = (uint64_t)ftell(f->host);
        fseek(f->host, 0, SEEK_SET);
        if (flags & 0x0100) { fseek(f->host, 0, SEEK_END); f->pos = f->size; }  /* append */
        return fd;
    }
    fd_free(fd);
    return (int32_t)SCE_ERR_FILE_NOT_FOUND;
}

static int64_t io_read(Fd *f, uint32_t buf, uint32_t len) {
    if (f->is_dir) return (int32_t)SCE_ERR_BADF;
    if (f->pos >= f->size) return 0;
    if (f->pos + len > f->size) len = (uint32_t)(f->size - f->pos);
    uint32_t got;
    if (f->host) { fseek(f->host, (long)f->pos, SEEK_SET); got = (uint32_t)fread(rt_ptr(buf), 1, len, f->host); }
    else got = disc_read((uint64_t)f->lba * SECTOR + f->pos, rt_ptr(buf), len);
    f->pos += got;
    return got;
}

static int64_t io_seek(Fd *f, int64_t off, uint32_t whence) {
    int64_t base = whence == 1 ? (int64_t)f->pos : whence == 2 ? (int64_t)f->size : 0;
    if (base + off < 0) return (int32_t)0x80010016u;  /* EINVAL */
    f->pos = (uint64_t)(base + off);
    return (int64_t)f->pos;
}

/* --- IoFileMgrForUser ------------------------------------------------------------- */
static uint64_t g_opens, g_reads, g_read_bytes;

static void hle_IoOpen(RtCtx *c) {
    const char *path = A0 ? (const char *)rt_ptr(A0) : "";
    int32_t fd = io_open(path, A1);
    g_opens++;
    if (fd >= 0) rt_log("[io] open \"%s\" -> fd %d (%llu bytes)\n", path, fd, (unsigned long long)g_fd[fd].size);
    else rt_log("[io] open \"%s\" -> not found\n", path);
    RET((uint32_t)fd);
}
static void hle_IoOpenAsync(RtCtx *c) {
    const char *path = A0 ? (const char *)rt_ptr(A0) : "";
    int32_t r = io_open(path, A1);
    g_opens++;
    int fd = r;
    if (r < 0) {  /* like the PSP, hand out a descriptor that reports the error */
        fd = fd_alloc();
        if (fd < 0) RET(0x80020320u);
        g_fd[fd].close_pending = 1;
        rt_log("[io] open async \"%s\" -> not found (fd %d)\n", path, fd);
    } else {
        rt_log("[io] open async \"%s\" -> fd %d (%llu bytes)\n", path, fd, (unsigned long long)g_fd[fd].size);
    }
    g_fd[fd].has_async = 1;
    g_fd[fd].async_result = r < 0 ? r : fd;
    RET((uint32_t)fd);
}
static void hle_IoClose(RtCtx *c) {
    if (!fd_get(A0)) RET(SCE_ERR_BADF);
    fd_free((int)A0);
    RET(0);
}
static void hle_IoCloseAsync(RtCtx *c) {
    Fd *f = fd_get(A0);
    if (!f) RET(SCE_ERR_BADF);
    f->has_async = 1; f->async_result = 0; f->close_pending = 1;
    RET(0);
}
static void hle_IoRead(RtCtx *c) {
    Fd *f = fd_get(A0);
    if (!f) RET(SCE_ERR_BADF);
    int64_t r = io_read(f, A1, A2);
    g_reads++; if (r > 0) g_read_bytes += (uint64_t)r;
    RET((uint32_t)r);
}
static void hle_IoReadAsync(RtCtx *c) {
    Fd *f = fd_get(A0);
    if (!f) RET(SCE_ERR_BADF);
    f->async_result = io_read(f, A1, A2);
    f->has_async = 1;
    g_reads++; if (f->async_result > 0) g_read_bytes += (uint64_t)f->async_result;
    RET(0);
}
static void hle_IoWrite(RtCtx *c) {
    if (A0 == 1 || A0 == 2) {
        fwrite(rt_ptr(A1), 1, A2, stdout);
        RET(A2);
    }
    Fd *f = fd_get(A0);
    if (!f) RET(SCE_ERR_BADF);
    if (!f->host) RET(0x8001000Du);  /* read-only medium */
    fseek(f->host, (long)f->pos, SEEK_SET);
    uint32_t n = (uint32_t)fwrite(rt_ptr(A1), 1, A2, f->host);
    f->pos += n;
    if (f->pos > f->size) f->size = f->pos;
    RET(n);
}
static void hle_IoLseek(RtCtx *c) {
    Fd *f = fd_get(A0);
    if (!f) RET64((int64_t)(int32_t)SCE_ERR_BADF);
    int64_t off = (int64_t)(((uint64_t)A3 << 32) | A2);
    RET64(io_seek(f, off, T0));
}
static void hle_IoLseekAsync(RtCtx *c) {
    Fd *f = fd_get(A0);
    if (!f) RET(SCE_ERR_BADF);
    int64_t off = (int64_t)(((uint64_t)A3 << 32) | A2);
    f->async_result = io_seek(f, off, T0);
    f->has_async = 1;
    RET(0);
}
static void async_result(RtCtx *c, int wait) {
    Fd *f = fd_get(A0);
    if (!f) RET(SCE_ERR_BADF);
    if (!f->has_async) RET(SCE_ERR_NOASYNC);
    if (wait) k_wait(WT_IO, 0, 100);  /* let other threads run, as real I/O would */
    f = fd_get(A0);
    if (!f) RET(SCE_ERR_BADF);
    if (A1) { rt_w32(A1, (uint32_t)f->async_result); rt_w32(A1 + 4, (uint32_t)((uint64_t)f->async_result >> 32)); }
    f->has_async = 0;
    if (f->close_pending) fd_free((int)A0);
    RET(0);
}
static void hle_IoPollAsync(RtCtx *c) { async_result(c, 0); }
static void hle_IoWaitAsync(RtCtx *c) { async_result(c, 1); }

static void fill_stat(uint32_t p, int is_dir, uint64_t size, uint32_t lba) {
    memset(rt_ptr(p), 0, 0x58);
    rt_w32(p, is_dir ? 0x11FFu : 0x21FFu);  /* FIO_S_IFDIR/IFREG | 0777 */
    rt_w32(p + 4, is_dir ? 0x10u : 0x20u);
    rt_w32(p + 8, (uint32_t)size);
    rt_w32(p + 12, (uint32_t)(size >> 32));
    rt_w32(p + 64, lba);                    /* st_private[0]: start sector, as on PPSSPP */
}
static void hle_IoGetstat(RtCtx *c) {
    char rel[512];
    const char *path = A0 ? (const char *)rt_ptr(A0) : "";
    int dev = resolve(path, rel, sizeof(rel));
    if (dev == DEV_DISC) {
        IsoEntry e;
        if (!iso_lookup(rel, &e)) RET(SCE_ERR_FILE_NOT_FOUND);
        fill_stat(A1, e.is_dir, e.size, e.lba);
        RET(0);
    }
    if (dev == DEV_MS0) {
        char hp[1024];
        host_path(rel, hp, sizeof(hp));
        FILE *h = fopen(hp, "rb");
        if (!h) RET(SCE_ERR_FILE_NOT_FOUND);
        fseek(h, 0, SEEK_END);
        fill_stat(A1, 0, (uint64_t)ftell(h), 0);
        fclose(h);
        RET(0);
    }
    RET(SCE_ERR_FILE_NOT_FOUND);
}
static void hle_IoChdir(RtCtx *c) {
    char rel[512];
    const char *path = A0 ? (const char *)rt_ptr(A0) : "";
    int dev = resolve(path, rel, sizeof(rel));
    const char *colon = strchr(path, ':');
    if (colon) snprintf(g_cwd, sizeof(g_cwd), "%.*s:/%s", (int)(colon - path), path, rel);
    else snprintf(g_cwd, sizeof(g_cwd), "%s", rel);
    rt_log("[io] chdir \"%s\" -> cwd %s%s\n", path, g_cwd, dev == DEV_NONE ? " (unknown device)" : "");
    RET(0);
}

typedef struct { IsoEntry *ents; int n, cap; } ListArg;
static int collect_cb(const IsoEntry *e, void *p) {
    ListArg *l = (ListArg *)p;
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 32; l->ents = (IsoEntry *)realloc(l->ents, (size_t)l->cap * sizeof(IsoEntry)); }
    l->ents[l->n++] = *e;
    return 0;
}
static void hle_IoDopen(RtCtx *c) {
    char rel[512];
    const char *path = A0 ? (const char *)rt_ptr(A0) : "";
    IsoEntry dir;
    if (resolve(path, rel, sizeof(rel)) != DEV_DISC || !iso_lookup(rel, &dir) || !dir.is_dir) {
        rt_log("[io] dopen \"%s\" -> not found\n", path);
        RET(SCE_ERR_FILE_NOT_FOUND);
    }
    int fd = fd_alloc();
    if (fd < 0) RET(0x80020320u);
    ListArg l = { NULL, 0, 0 };
    iso_list(&dir, collect_cb, &l);
    g_fd[fd].is_dir = 1;
    g_fd[fd].ents = l.ents;
    g_fd[fd].nents = l.n;
    rt_log("[io] dopen \"%s\" -> fd %d (%d entries)\n", path, fd, l.n);
    RET(fd);
}
static void hle_IoDread(RtCtx *c) {
    Fd *f = fd_get(A0);
    if (!f || !f->is_dir) RET(SCE_ERR_BADF);
    if (f->dirpos >= f->nents) RET(0);
    IsoEntry *e = &f->ents[f->dirpos++];
    fill_stat(A1, e->is_dir, e->size, e->lba);
    snprintf((char *)rt_ptr(A1 + 0x58), 256, "%s", e->name);
    RET(1);
}
static void hle_IoDclose(RtCtx *c) { if (!fd_get(A0)) RET(SCE_ERR_BADF); fd_free((int)A0); RET(0); }
static void hle_IoMkdir(RtCtx *c) {
    char rel[512], hp[1024];
    if (resolve((const char *)rt_ptr(A0), rel, sizeof(rel)) != DEV_MS0) RET(0x8001000Du);
    host_path(rel, hp, sizeof(hp));
    mkdir_host(hp);
    RET(0);
}
static void hle_IoRemove(RtCtx *c) {
    char rel[512], hp[1024];
    if (resolve((const char *)rt_ptr(A0), rel, sizeof(rel)) != DEV_MS0) RET(0x8001000Du);
    host_path(rel, hp, sizeof(hp));
    RET(remove(hp) == 0 ? 0 : SCE_ERR_FILE_NOT_FOUND);
}
static void hle_IoIoctl(RtCtx *c) {
    rt_log("[io] ioctl fd %d cmd %08X in %08X/%u out %08X/%u (not implemented, returns 0)\n", (int)A0, A1, A2, A3, T0, T1);
    RET(0);
}
static void hle_Zero(RtCtx *c) { RET(0); }
static void hle_One(RtCtx *c) { RET(1); }

void rt_io_report(void) {
    printf("[io] %llu opens, %llu reads, %.1f MB read from disc\n", (unsigned long long)g_opens, (unsigned long long)g_reads,
           (double)g_read_bytes / (1 << 20));
}

const HleEntry rt_hle_io[] = {
    {"sceIoOpen", hle_IoOpen}, {"sceIoOpenAsync", hle_IoOpenAsync}, {"sceIoClose", hle_IoClose},
    {"sceIoCloseAsync", hle_IoCloseAsync}, {"sceIoRead", hle_IoRead}, {"sceIoReadAsync", hle_IoReadAsync},
    {"sceIoWrite", hle_IoWrite}, {"sceIoLseek", hle_IoLseek}, {"sceIoLseekAsync", hle_IoLseekAsync},
    {"sceIoPollAsync", hle_IoPollAsync}, {"sceIoWaitAsync", hle_IoWaitAsync}, {"sceIoWaitAsyncCB", hle_IoWaitAsync},
    {"sceIoChangeAsyncPriority", hle_Zero}, {"sceIoGetstat", hle_IoGetstat}, {"sceIoChdir", hle_IoChdir},
    {"sceIoDopen", hle_IoDopen}, {"sceIoDread", hle_IoDread}, {"sceIoDclose", hle_IoDclose},
    {"sceIoMkdir", hle_IoMkdir}, {"sceIoRmdir", hle_IoRemove}, {"sceIoRemove", hle_IoRemove},
    {"sceIoRename", hle_Zero}, {"sceIoIoctl", hle_IoIoctl},
    {"sceUmdCheckMedium", hle_One}, {"sceUmdActivate", hle_Zero}, {"sceUmdWaitDriveStatWithTimer", hle_Zero},
    {"sceUmdWaitDriveStat", hle_Zero}, {"sceUmdGetDriveStat", hle_Zero},
    {NULL, NULL},
};
