/* gow_disc: what the launcher needs from the player's disc image before the game is recompiled
 * (gow_rt itself contains the generated code, so it cannot exist yet at that point).
 *
 *   gow_disc --disc game.chd|.iso --id                   prints the disc ID (PARAM.SFO DISC_ID), e.g. UCUS98653
 *   gow_disc --disc game.chd|.iso --decrypt-eboot out    writes the decrypted PSP_GAME/SYSDIR/EBOOT.BIN
 *   gow_disc --disc game.chd|.iso --extract disc0:/path out
 *
 * The decrypted EBOOT is the same file PPSSPP dumps (DumpFileTypes): PrxDecrypter's output buffer of
 * max(elf_size, psp_size) bytes, as sceKernelModule.cpp hands it to the dump. Reads CHD (libchdr) and ISO. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libchdr/chd.h>

#include "rt_prx.h"

#ifdef _WIN32
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#define strcasecmp _stricmp
#else
#include <strings.h>
#define fseek64 fseeko
#define ftell64 ftello
#endif

#define SECTOR 2048u

static FILE *g_iso;
static chd_file *g_chd;
static uint32_t g_hunk_bytes, g_cached_hunk = UINT32_MAX;
static uint8_t *g_hunk;
static uint64_t g_disc_size;

static int open_disc(const char *path) {
    size_t n = strlen(path);
    if (n > 4 && !strcasecmp(path + n - 4, ".chd")) {
        chd_error err = chd_open(path, CHD_OPEN_READ, NULL, &g_chd);
        if (err != CHDERR_NONE) { fprintf(stderr, "gow_disc: cannot open CHD %s (%s)\n", path, chd_error_string(err)); return 0; }
        const chd_header *h = chd_get_header(g_chd);
        g_hunk_bytes = h->hunkbytes;
        g_hunk = (uint8_t *)malloc(g_hunk_bytes);
        g_disc_size = h->logicalbytes;
        return g_hunk != NULL;
    }
    g_iso = fopen(path, "rb");
    if (!g_iso) { fprintf(stderr, "gow_disc: cannot open disc image %s\n", path); return 0; }
    fseek64(g_iso, 0, SEEK_END);
    g_disc_size = (uint64_t)ftell64(g_iso);
    return 1;
}

static uint32_t disc_read(uint64_t off, uint8_t *dst, uint32_t len) {
    if (off >= g_disc_size) return 0;
    if (off + len > g_disc_size) len = (uint32_t)(g_disc_size - off);
    if (g_iso) {
        fseek64(g_iso, (int64_t)off, SEEK_SET);
        return (uint32_t)fread(dst, 1, len, g_iso);
    }
    uint32_t done = 0;
    while (done < len) {
        uint32_t hunk = (uint32_t)((off + done) / g_hunk_bytes), in = (uint32_t)((off + done) % g_hunk_bytes);
        if (hunk != g_cached_hunk) {
            if (chd_read(g_chd, hunk, g_hunk) != CHDERR_NONE) break;
            g_cached_hunk = hunk;
        }
        uint32_t k = g_hunk_bytes - in < len - done ? g_hunk_bytes - in : len - done;
        memcpy(dst + done, g_hunk + in, k);
        done += k;
    }
    return done;
}

/* ISO 9660: finds a '/'-separated path below the root; returns its first sector and size. */
static int iso_lookup(const char *path, uint32_t *lba, uint32_t *size, int *is_dir) {
    uint8_t pvd[SECTOR];
    if (disc_read(16ull * SECTOR, pvd, SECTOR) != SECTOR || memcmp(pvd + 1, "CD001", 5)) return 0;
    memcpy(lba, pvd + 156 + 2, 4);
    memcpy(size, pvd + 156 + 10, 4);
    *is_dir = 1;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *tok = strtok(tmp, "/\\"); tok; tok = strtok(NULL, "/\\")) {
        if (!*is_dir || *size > (64u << 20)) return 0;
        uint8_t *buf = (uint8_t *)malloc(*size ? *size : 1);
        uint32_t n = buf ? disc_read((uint64_t)*lba * SECTOR, buf, *size) : 0;
        int found = 0;
        for (uint32_t pos = 0; pos + 33 < n && !found;) {
            uint8_t len = buf[pos];
            if (len == 0) { pos = (pos / SECTOR + 1) * SECTOR; continue; }
            const uint8_t *r = buf + pos;
            char name[256];
            uint8_t nlen = r[32];
            if (len < 34 || pos + len > n || 33u + nlen > len) break;  /* a damaged image: the record or its name runs out */
            memcpy(name, r + 33, nlen);
            name[nlen] = 0;
            char *semi = strchr(name, ';');
            if (semi) *semi = 0;
            if (!strcasecmp(name, tok)) {
                memcpy(lba, r + 2, 4);
                memcpy(size, r + 10, 4);
                *is_dir = (r[25] & 2) != 0;
                found = 1;
            }
            pos += len;
        }
        free(buf);
        if (!found) return 0;
    }
    return 1;
}

/* Reads a whole file off the disc ("disc0:/" or "umd0:/" prefixes are dropped). */
static uint8_t *read_disc_file(const char *path, uint32_t *size_out) {
    const char *colon = strchr(path, ':');
    if (colon) path = colon + 1;
    uint32_t lba, size;
    int is_dir;
    if (!iso_lookup(path, &lba, &size, &is_dir) || is_dir) {
        fprintf(stderr, "gow_disc: %s not found on the disc\n", path);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc(size ? size : 1);
    if (!buf || disc_read((uint64_t)lba * SECTOR, buf, size) != size) {
        fprintf(stderr, "gow_disc: cannot read %s\n", path);
        free(buf);
        return NULL;
    }
    *size_out = size;
    return buf;
}

static int write_file(const char *out, const uint8_t *p, uint32_t n) {
    FILE *f = fopen(out, "wb");
    int ok = f && fwrite(p, 1, n, f) == n;
    if (f && fclose(f)) ok = 0;
    if (!ok) fprintf(stderr, "gow_disc: cannot write %s\n", out);
    return ok;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* PARAM.SFO: the DISC_ID string value. */
static int print_id(void) {
    uint32_t n;
    uint8_t *sfo = read_disc_file("PSP_GAME/PARAM.SFO", &n);
    if (!sfo) return 0;
    int ok = 0;
    if (n >= 20 && !memcmp(sfo, "\0PSF", 4)) {
        uint32_t keys = rd32(sfo + 8), data = rd32(sfo + 12), count = rd32(sfo + 16);
        for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= n; i++) {
            const uint8_t *e = sfo + 20 + i * 16;
            uint32_t ko = keys + (e[0] | e[1] << 8), len = rd32(e + 4), vo = data + rd32(e + 12);
            if (ko + 8 <= n && !memcmp(sfo + ko, "DISC_ID", 8) && vo + len <= n && len < 64) {
                printf("%.*s\n", (int)strnlen((const char *)sfo + vo, len), (const char *)sfo + vo);
                ok = 1;
                break;
            }
        }
    }
    if (!ok) fprintf(stderr, "gow_disc: no DISC_ID in PARAM.SFO\n");
    free(sfo);
    return ok;
}

static int decrypt_eboot(const char *out) {
    uint32_t n;
    uint8_t *in = read_disc_file("PSP_GAME/SYSDIR/EBOOT.BIN", &n);
    if (!in) return 0;
    if (n >= 4 && !memcmp(in, "\x7f" "ELF", 4)) {  /* not encrypted */
        int ok = write_file(out, in, n);
        free(in);
        return ok;
    }
    if (n < 0x150 || memcmp(in, "~PSP", 4)) {
        fprintf(stderr, "gow_disc: EBOOT.BIN is neither ~PSP nor ELF\n");
        free(in);
        return 0;
    }
    uint32_t elf_size = rd32(in + 0x28), psp_size = rd32(in + 0x2C), comp = in[6] | in[7] << 8;
    uint32_t max = elf_size > psp_size ? elf_size : psp_size;
    if (psp_size > n || max > (64u << 20)) {
        fprintf(stderr, "gow_disc: EBOOT.BIN header sizes do not fit (%u, %u, file %u)\n", elf_size, psp_size, n);
        free(in);
        return 0;
    }
    if (comp & 1) {
        fprintf(stderr, "gow_disc: gzip-compressed EBOOT.BIN is not supported\n");
        free(in);
        return 0;
    }
    uint8_t *dec = (uint8_t *)calloc(max, 1);
    int got = dec ? rt_prx_decrypt(in, dec, psp_size) : -1;
    if (dec && got <= 0 && psp_size > 0x154 && rd32(in + 0x150) == 0x464C457F) {  /* as PPSSPP: an unencrypted ELF behind the header */
        got = (int)(psp_size - 0x150);
        memcpy(dec, in + 0x150, (size_t)got);
    }
    int ok = got > 0 && rd32(dec) == 0x464C457F;
    if (!ok) fprintf(stderr, "gow_disc: cannot decrypt EBOOT.BIN (tag 0x%08X, result %d)\n", rd32(in + 0xD0), got);
    else ok = write_file(out, dec, max);
    if (ok) printf("decrypted EBOOT.BIN (%d bytes of ELF, %u written) to %s\n", got, max, out);
    free(dec);
    free(in);
    return ok;
}

int main(int argc, char **argv) {
    const char *disc = NULL, *eboot = NULL, *ex_from = NULL, *ex_to = NULL;
    int id = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--id")) id = 1;
        else if (!strcmp(argv[i], "--decrypt-eboot") && i + 1 < argc) eboot = argv[++i];
        else if (!strcmp(argv[i], "--extract") && i + 2 < argc) { ex_from = argv[++i]; ex_to = argv[++i]; }
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); disc = NULL; break; }
    }
    if (!disc || (!id && !eboot && !ex_from)) {
        fprintf(stderr, "usage: %s --disc game.chd|.iso [--id] [--decrypt-eboot out.bin] [--extract disc0:/path out]\n", argv[0]);
        return 2;
    }
    if (!open_disc(disc)) return 1;
    if (id && !print_id()) return 1;
    if (eboot && !decrypt_eboot(eboot)) return 1;
    if (ex_from) {
        uint32_t n;
        uint8_t *p = read_disc_file(ex_from, &n);
        int ok = p && write_file(ex_to, p, n);
        free(p);
        if (!ok) return 1;
    }
    return 0;
}