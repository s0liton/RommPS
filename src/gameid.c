#include "gameid.h"

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include "cJSON.h"
#include "state.h" /* jget_str */
#include "util.h"

#define SECTOR 2048

static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* A disc image read by offset: plain files, or CSO with deflate blocks. */
typedef struct {
    FILE *f;
    int cso;
    uint32_t block_size, align, nblocks;
    uint32_t *index;
    unsigned char *block;
    long cached;
} image;

static int image_open(image *im, const char *path) {
    memset(im, 0, sizeof *im);
    im->cached = -1;
    im->f = fopen(path, "rb");
    if (!im->f) return -1;
    unsigned char h[24];
    if (fread(h, 1, sizeof h, im->f) == sizeof h && !memcmp(h, "CISO", 4) && str_ends_with_ci(path, ".cso")) {
        uint64_t total = (uint64_t)le32(h + 8) | (uint64_t)le32(h + 12) << 32;
        im->cso = 1;
        im->block_size = le32(h + 16);
        im->align = h[21];
        if (!im->block_size || im->block_size > 1 << 20) return -1;
        im->nblocks = (uint32_t)((total + im->block_size - 1) / im->block_size);
        im->index = malloc(((size_t)im->nblocks + 1) * 4);
        im->block = malloc(im->block_size);
        if (!im->index || !im->block) return -1;
        fseek(im->f, 24, SEEK_SET);
        unsigned char b[4];
        for (uint32_t i = 0; i <= im->nblocks; i++) {
            if (fread(b, 1, 4, im->f) != 4) return -1;
            im->index[i] = le32(b);
        }
    }
    return 0;
}

static void image_close(image *im) {
    if (im->f) fclose(im->f);
    free(im->index);
    free(im->block);
}

static int cso_block(image *im, uint32_t i) {
    if ((long)i == im->cached) return 0;
    if (i >= im->nblocks) return -1;
    uint32_t a = im->index[i], b = im->index[i + 1];
    int plain = a & 0x80000000u;
    uint64_t start = (uint64_t)(a & 0x7FFFFFFFu) << im->align, end = (uint64_t)(b & 0x7FFFFFFFu) << im->align;
    size_t len = (size_t)(end - start);
    if (len > im->block_size * 2 + 64) return -1;
    unsigned char *raw = malloc(len ? len : 1);
    if (!raw) return -1;
    fseek(im->f, (long)start, SEEK_SET);
    int rc = -1;
    if (fread(raw, 1, len, im->f) == len) {
        if (plain) {
            memcpy(im->block, raw, len < im->block_size ? len : im->block_size);
            rc = 0;
        } else {
            z_stream s;
            memset(&s, 0, sizeof s);
            if (inflateInit2(&s, -MAX_WBITS) == Z_OK) {
                s.next_in = raw;
                s.avail_in = (uInt)len;
                s.next_out = im->block;
                s.avail_out = im->block_size;
                int r = inflate(&s, Z_FINISH);
                rc = (r == Z_STREAM_END || r == Z_OK || r == Z_BUF_ERROR) ? 0 : -1;
                inflateEnd(&s);
            }
        }
    }
    free(raw);
    if (rc == 0) im->cached = (long)i;
    return rc;
}

static int image_read(image *im, uint64_t off, void *out, size_t len) {
    if (!im->cso) {
        fseek(im->f, (long)off, SEEK_SET);
        return fread(out, 1, len, im->f) == len ? 0 : -1;
    }
    unsigned char *o = out;
    while (len) {
        uint32_t blk = (uint32_t)(off / im->block_size), in = (uint32_t)(off % im->block_size);
        if (cso_block(im, blk) != 0) return -1;
        size_t take = im->block_size - in;
        if (take > len) take = len;
        memcpy(o, im->block + in, take);
        o += take;
        off += take;
        len -= take;
    }
    return 0;
}

/* Finds a named entry in an ISO9660 directory. */
static int iso_find(image *im, uint32_t lba, uint32_t size, const char *name, uint32_t *out_lba, uint32_t *out_size) {
    unsigned char *dir = malloc(size ? size : 1);
    if (!dir || image_read(im, (uint64_t)lba * SECTOR, dir, size) != 0) {
        free(dir);
        return -1;
    }
    size_t nl = strlen(name);
    int rc = -1;
    for (uint32_t p = 0; p < size;) {
        unsigned rl = dir[p];
        if (!rl) {
            p = (p / SECTOR + 1) * SECTOR; /* records never cross a sector, skip the padding */
            continue;
        }
        if (p + 33 > size || p + rl > size) break;
        unsigned fl = dir[p + 32];
        const char *fn = (const char *)dir + p + 33;
        if (fl >= nl && !strncasecmp(fn, name, nl) && (fl == nl || fn[nl] == ';')) {
            *out_lba = le32(dir + p + 2);
            *out_size = le32(dir + p + 10);
            rc = 0;
            break;
        }
        p += rl;
    }
    free(dir);
    return rc;
}

int sfo_get(const unsigned char *sfo, size_t len, const char *key, char *out, size_t n) {
    if (len < 20 || memcmp(sfo, "\0PSF", 4) != 0) return -1;
    uint32_t keys = le32(sfo + 8), data = le32(sfo + 12), count = le32(sfo + 16);
    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= len; i++) {
        const unsigned char *e = sfo + 20 + i * 16;
        uint32_t ko = keys + le16(e), dl = le32(e + 4), dof = data + le32(e + 12);
        if (ko >= len || dof + dl > len) continue;
        if (strcmp((const char *)sfo + ko, key) != 0) continue;
        size_t l = dl && sfo[dof + dl - 1] == 0 ? dl - 1 : dl;
        if (l >= n) l = n - 1;
        memcpy(out, sfo + dof, l);
        out[l] = 0;
        return 0;
    }
    return -1;
}

int sfo_file_get(const char *path, const char *key, char *out, size_t n) {
    size_t len;
    char *d = read_file(path, &len);
    if (!d) return -1;
    int rc = sfo_get((unsigned char *)d, len, key, out, n);
    free(d);
    return rc;
}

static void clean_id(char *id) {
    size_t o = 0;
    for (size_t i = 0; id[i]; i++)
        if (isalnum((unsigned char)id[i])) id[o++] = (char)toupper((unsigned char)id[i]);
    id[o] = 0;
}

int gameid_psp(const char *path, char *out, size_t n) {
    if (!str_ends_with_ci(path, ".iso") && !str_ends_with_ci(path, ".cso")) return -1;
    image im;
    int rc = -1;
    unsigned char pvd[SECTOR];
    if (image_open(&im, path) == 0 && image_read(&im, 16 * SECTOR, pvd, sizeof pvd) == 0 && !memcmp(pvd + 1, "CD001", 5)) {
        uint32_t root_lba = le32(pvd + 156 + 2), root_size = le32(pvd + 156 + 10), lba, size, slba, ssize;
        if (iso_find(&im, root_lba, root_size, "PSP_GAME", &lba, &size) == 0 &&
            iso_find(&im, lba, size, "PARAM.SFO", &slba, &ssize) == 0 && ssize < 64 * 1024) {
            unsigned char *sfo = malloc(ssize);
            if (sfo && image_read(&im, (uint64_t)slba * SECTOR, sfo, ssize) == 0 && sfo_get(sfo, ssize, "DISC_ID", out, n) == 0) {
                clean_id(out);
                rc = out[0] ? 0 : -1;
            }
            free(sfo);
        }
    }
    image_close(&im);
    return rc;
}

int gameid_dolphin(const char *path, char *out, size_t n) {
    long off;
    if (str_ends_with_ci(path, ".rvz") || str_ends_with_ci(path, ".wia"))
        off = 0x58; /* disc header copy after the WIA file header */
    else if (str_ends_with_ci(path, ".ciso"))
        off = 0x8000;
    else if (str_ends_with_ci(path, ".wbfs"))
        off = 0x200;
    else if (str_ends_with_ci(path, ".iso") || str_ends_with_ci(path, ".gcm"))
        off = 0;
    else
        return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char id[6];
    int ok = fseek(f, off, SEEK_SET) == 0 && fread(id, 1, 6, f) == 6;
    fclose(f);
    if (!ok || n < 7) return -1;
    for (int i = 0; i < 6; i++)
        if (!isalnum(id[i])) return -1;
    memcpy(out, id, 6);
    out[6] = 0;
    return 0;
}

/* ---- PlayStation discs ---------------------------------------------------
 * A PS1 game's serial ("SCUS-94194") from SYSTEM.CNF's BOOT line, read from a
 * 2048-byte ISO, a raw 2352-byte BIN (directly or through its .cue) or a CHD
 * (libchdr). Emulators that keep one memory card per game name it by serial. */

#include "libchdr/chd.h"

typedef struct {
    FILE *f;
    int raw;               /* 2352-byte sectors */
    chd_file *chd;
    uint32_t hunk_bytes, frame_bytes, frames_per_hunk;
    unsigned char *hunk;
    long cached_hunk;
} psx_disc;

static int psx_open_file(psx_disc *d, const char *path) {
    d->f = fopen(path, "rb");
    if (!d->f) return -1;
    static const unsigned char sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    unsigned char h[12];
    d->raw = fread(h, 1, sizeof h, d->f) == sizeof h && !memcmp(h, sync, sizeof sync);
    return 0;
}

/* The file a .cue names index-th (0 for its first), beside it. */
static int cue_file(const char *cue, int index, char *out, size_t n) {
    char *txt = read_file(cue, NULL);
    if (!txt) return -1;
    int rc = -1;
    char *p = txt;
    while ((p = strstr(p, "FILE"))) {
        char *q = strchr(p, '"'), *e = q ? strchr(q + 1, '"') : NULL;
        if (!q || !e) break;
        if (index--) {
            p = e + 1;
            continue;
        }
        *e = 0;
        char dir[PATH_MAX_LEN];
        str_copy(dir, sizeof dir, cue);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        else str_copy(dir, sizeof dir, ".");
        path_join(out, n, dir, q + 1);
        rc = 0;
        break;
    }
    free(txt);
    return rc;
}

static int psx_open(psx_disc *d, const char *path) {
    memset(d, 0, sizeof *d);
    d->cached_hunk = -1;
    if (str_ends_with_ci(path, ".chd")) {
        if (chd_open(path, CHD_OPEN_READ, NULL, &d->chd) != CHDERR_NONE) return -1;
        const chd_header *h = chd_get_header(d->chd);
        d->hunk_bytes = h->hunkbytes;
        d->frame_bytes = h->unitbytes ? h->unitbytes : 2448;
        d->frames_per_hunk = d->hunk_bytes / d->frame_bytes;
        d->hunk = malloc(d->hunk_bytes);
        return d->hunk && d->frames_per_hunk ? 0 : -1;
    }
    if (str_ends_with_ci(path, ".cue")) {
        char bin[PATH_MAX_LEN];
        if (cue_file(path, 0, bin, sizeof bin) != 0) return -1;
        return psx_open_file(d, bin);
    }
    return psx_open_file(d, path);
}

static void psx_close(psx_disc *d) {
    if (d->f) fclose(d->f);
    if (d->chd) chd_close(d->chd);
    free(d->hunk);
}

/* One 2048-byte user-data sector. A DVD's CHD (PS2) keeps 2048-byte frames
 * with no header; a CD's keeps whole 2352-byte frames (and subcode). */
static int psx_sector(psx_disc *d, uint32_t lba, unsigned char *out) {
    unsigned char frame[2352];
    if (d->chd) {
        long hunk = (long)(lba / d->frames_per_hunk);
        if (hunk != d->cached_hunk) {
            if (chd_read(d->chd, (uint32_t)hunk, d->hunk) != CHDERR_NONE) return -1;
            d->cached_hunk = hunk;
        }
        const unsigned char *at = d->hunk + (size_t)(lba % d->frames_per_hunk) * d->frame_bytes;
        if (d->frame_bytes < sizeof frame) {
            if (d->frame_bytes < SECTOR) return -1;
            memcpy(out, at, SECTOR);
            return 0;
        }
        memcpy(frame, at, sizeof frame);
    } else if (d->raw) {
        if (fseeko(d->f, (off_t)lba * 2352, SEEK_SET) != 0 || fread(frame, 1, sizeof frame, d->f) != sizeof frame) return -1;
    } else {
        return fseeko(d->f, (off_t)lba * SECTOR, SEEK_SET) == 0 && fread(out, 1, SECTOR, d->f) == SECTOR ? 0 : -1;
    }
    /* Mode 2 (PS1 discs) has an 8-byte subheader after the 16-byte header. */
    memcpy(out, frame + (frame[15] == 2 ? 24 : 16), SECTOR);
    return 0;
}

/* A file on the disc by its path ("SYSTEM.CNF", "\\DATA\\SLUS_123.45;1"):
 * where it starts and how long it is. Names match without their ";1". */
static int psx_find(psx_disc *d, const char *path, uint32_t *lba, uint32_t *size) {
    unsigned char pvd[SECTOR];
    if (psx_sector(d, 16, pvd) != 0 || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return -1;
    uint32_t at = le32(pvd + 156 + 2), len = le32(pvd + 156 + 10);
    int is_dir = 1;
    while (*path == '\\' || *path == '/') path++;
    while (*path) {
        size_t nl = strcspn(path, "\\/;");
        if (!is_dir || !nl) return -1;
        if (len > 64 * SECTOR) len = 64 * SECTOR;
        unsigned char *dir = malloc(len + SECTOR);
        uint32_t got = 0;
        while (dir && got < len && psx_sector(d, at + got / SECTOR, dir + got) == 0) got += SECTOR;
        int found = 0;
        for (uint32_t p = 0; dir && p + 33 < got && !found;) {
            unsigned rl = dir[p];
            if (!rl) {
                p = (p / SECTOR + 1) * SECTOR;
                continue;
            }
            unsigned fl = dir[p + 32];
            const char *name = (const char *)dir + p + 33;
            size_t base = 0;
            while (base < fl && name[base] != ';') base++;
            if (p + 33 + fl <= got && base == nl && !strncasecmp(name, path, nl)) {
                at = le32(dir + p + 2);
                len = le32(dir + p + 10);
                is_dir = (dir[p + 25] & 2) != 0;
                found = 1;
            }
            p += rl;
        }
        free(dir);
        if (!found) return -1;
        path += nl;
        if (*path == ';') path += strcspn(path, "\\/");
        while (*path == '\\' || *path == '/') path++;
    }
    if (is_dir) return -1;
    *lba = at;
    *size = len;
    return 0;
}

/* SYSTEM.CNF's text, 0-terminated. */
static int read_cnf(psx_disc *d, char *out, size_t n) {
    uint32_t lba, len;
    unsigned char cnf[SECTOR];
    if (n < 2 || psx_find(d, "SYSTEM.CNF", &lba, &len) != 0 || psx_sector(d, lba, cnf) != 0) return -1;
    size_t l = len < SECTOR ? len : SECTOR;
    if (l > n - 1) l = n - 1;
    memcpy(out, cnf, l);
    out[l] = 0;
    return 0;
}

/* "BOOT = cdrom:\SCUS_941.94;1" -> "SCUS-94194". */
static int serial_from_cnf(const char *cnf, char *out, size_t n) {
    const char *b = strstr(cnf, "BOOT");
    if (!b) return -1;
    const char *eol = b + strcspn(b, "\r\n");
    const char *name = NULL;
    for (const char *p = b; p < eol; p++)
        if (*p == '\\' || *p == ':') name = p + 1;
    if (!name) return -1;
    char letters[8] = "", digits[16] = "";
    size_t nl = 0, nd = 0;
    for (const char *p = name; p < eol && *p != ';'; p++) {
        if (isalpha((unsigned char)*p) && nd == 0 && nl < 6) letters[nl++] = (char)toupper((unsigned char)*p);
        else if (isdigit((unsigned char)*p) && nd < 10) digits[nd++] = *p;
    }
    letters[nl] = 0;
    digits[nd] = 0;
    if (nl < 3 || nd < 4) return -1;
    snprintf(out, n, "%s-%s", letters, digits);
    return 0;
}

int gameid_psx(const char *path, char *out, size_t n) {
    psx_disc d;
    char cnf[SECTOR + 1];
    int rc = psx_open(&d, path) == 0 && read_cnf(&d, cnf, sizeof cnf) == 0 ? serial_from_cnf(cnf, out, n) : -1;
    psx_close(&d);
    return rc;
}

int gameid_ps2_crc(const char *path, uint32_t *crc) {
    psx_disc d;
    char cnf[SECTOR + 1], boot[256];
    uint32_t lba, len;
    int rc = -1;
    if (psx_open(&d, path) != 0 || read_cnf(&d, cnf, sizeof cnf) != 0) goto out;
    /* "BOOT2 = cdrom0:\SLUS_205.52;1": the file after the device. */
    const char *b = strstr(cnf, "BOOT2"), *colon;
    if (!b || !(colon = strchr(b, ':'))) goto out;
    size_t bl = strcspn(colon + 1, "\r\n");
    if (bl >= sizeof boot) goto out;
    memcpy(boot, colon + 1, bl);
    boot[bl] = 0;
    if (psx_find(&d, boot, &lba, &len) != 0 || len < 4 || len > 64u << 20) goto out;
    uint32_t x = 0, left = len & ~3u;
    unsigned char sec[SECTOR];
    for (uint32_t s = 0; left; s++) {
        if (psx_sector(&d, lba + s, sec) != 0) goto out;
        uint32_t take = left < SECTOR ? left : SECTOR;
        for (uint32_t i = 0; i < take; i += 4) x ^= le32(sec + i);
        left -= take;
    }
    *crc = x;
    rc = 0;
out:
    psx_close(&d);
    return rc;
}

/* ---- Dreamcast ----
 * A disc's product number ("MK-51035") from its IP.BIN, at the start of the
 * high-density area (a GD-ROM's track 3): a .gdi's third track, a .cue's
 * third file, or in a CHD, frame by frame from the start, since its tracks
 * sit end to end and the third is only a little way in. A one-track image
 * (a CD-ROM game, or an .iso) has it in its first sector. Flycast names each
 * game's own memory card by it. .cdi images aren't read. */
#define DC_CHD_FRAMES 30000 /* track 3 starts within the first few thousand */

/* Flycast's game id: the product number, trailing spaces trimmed and the
 * characters a file name can't hold made "_" ("T13008D 05" -> "T13008D_05"),
 * as it names the game's own card (oslib.cpp getVmuPath). */
static void flycast_id(char *id) {
    for (char *c = id; *c; c++)
        if (strchr(" /\\:*?|<>\"", *c)) *c = '_';
}

static int ipbin_product(const unsigned char *s, char *out, size_t n) {
    if (memcmp(s, "SEGA SEGAKATANA ", 16) != 0) return -1;
    size_t len = 10;
    while (len && s[0x40 + len - 1] == ' ') len--;
    if (!len || len + 1 > n) return -1;
    for (size_t i = 0; i < len; i++)
        if (!isprint(s[0x40 + i])) return -1;
    memcpy(out, s + 0x40, len);
    out[len] = 0;
    flycast_id(out);
    return 0;
}

/* Track 3's file in a .gdi ("3 45000 4 2352 track03.bin 0", the name maybe
 * quoted), else its first track's. */
static int gdi_track_file(const char *gdi, char *out, size_t n) {
    char *txt = read_file(gdi, NULL);
    if (!txt) return -1;
    char dir[PATH_MAX_LEN], first[PATH_MAX_LEN] = "", name[PATH_MAX_LEN];
    str_copy(dir, sizeof dir, gdi);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    else str_copy(dir, sizeof dir, ".");
    int rc = -1;
    char *save = NULL;
    strtok_r(txt, "\r\n", &save); /* the track count */
    for (char *line = strtok_r(NULL, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        int track = 0, used = 0;
        if (sscanf(line, "%d %*d %*d %*d %n", &track, &used) < 1 || !used) continue;
        const char *f = line + used;
        if (*f == '"') {
            const char *e = strchr(f + 1, '"');
            if (!e) continue;
            snprintf(name, sizeof name, "%.*s", (int)(e - f - 1), f + 1);
        } else if (sscanf(f, "%1023s", name) != 1) {
            continue;
        }
        if (track == 1) path_join(first, sizeof first, dir, name);
        if (track == 3) {
            path_join(out, n, dir, name);
            rc = 0;
            break;
        }
    }
    free(txt);
    if (rc && first[0]) {
        str_copy(out, n, first);
        rc = 0;
    }
    return rc;
}

static int dc_from_file(const char *path, char *out, size_t n) {
    psx_disc d;
    memset(&d, 0, sizeof d);
    unsigned char s[SECTOR];
    int rc = psx_open_file(&d, path) == 0 && psx_sector(&d, 0, s) == 0 ? ipbin_product(s, out, n) : -1;
    psx_close(&d);
    return rc;
}

int gameid_dreamcast(const char *path, char *out, size_t n) {
    char track[PATH_MAX_LEN];
    if (str_ends_with_ci(path, ".gdi"))
        return gdi_track_file(path, track, sizeof track) == 0 ? dc_from_file(track, out, n) : -1;
    if (str_ends_with_ci(path, ".cue"))
        return cue_file(path, 2, track, sizeof track) == 0 || cue_file(path, 0, track, sizeof track) == 0
                   ? dc_from_file(track, out, n)
                   : -1;
    if (!str_ends_with_ci(path, ".chd")) return str_ends_with_ci(path, ".cdi") ? -1 : dc_from_file(path, out, n);
    psx_disc d;
    if (psx_open(&d, path) != 0) {
        psx_close(&d);
        return -1;
    }
    int rc = -1;
    for (uint32_t f = 0; f < DC_CHD_FRAMES && rc; f++) {
        long hunk = (long)(f / d.frames_per_hunk);
        if (hunk != d.cached_hunk) {
            if (chd_read(d.chd, (uint32_t)hunk, d.hunk) != CHDERR_NONE) break;
            d.cached_hunk = hunk;
        }
        const unsigned char *frame = d.hunk + (size_t)(f % d.frames_per_hunk) * d.frame_bytes;
        rc = ipbin_product(frame + 16, out, n) == 0 || ipbin_product(frame, out, n) == 0 ? 0 : -1;
    }
    psx_close(&d);
    return rc;
}

int gameid_from_list(const char *list, const char *app_root, const char *path, char *out, size_t n) {
    char *txt = read_file(list, NULL);
    if (!txt) return -1;
    int rc = -1;
    const size_t plen = strlen(path);
    char *save = NULL;
    for (char *line = strtok_r(txt, "\r\n", &save); line && rc; line = strtok_r(NULL, "\r\n", &save)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        const char *id = tab + 1;
        char file[PATH_MAX_LEN];
        if (!strncmp(line, "/app0/", 6)) path_join(file, sizeof file, app_root, line + 6);
        else str_copy(file, sizeof file, line);
        /* The game's file, or one inside its folder (a game on several files). */
        if (strcmp(file, path) != 0 && !(!strncmp(file, path, plen) && file[plen] == '/')) continue;
        size_t len = strlen(id);
        if (!len || len + 1 > n) continue;
        str_copy(out, n, id);
        flycast_id(out); /* it names files */
        rc = 0;
    }
    free(txt);
    return rc;
}

static uint64_t le64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

static int read_at(FILE *f, long off, unsigned char *buf, size_t len) {
    return fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, len, f) == len ? 0 : -1;
}

int gameid_3ds(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f || n < 17) {
        if (f) fclose(f);
        return -1;
    }
    unsigned char h[0x200];
    uint64_t id = 0;
    if (read_at(f, 0, h, sizeof h) == 0) {
        if (!memcmp(h + 0x100, "NCSD", 4)) {
            id = le64(h + 0x108); /* the media id: the title's */
        } else if (!memcmp(h + 0x100, "NCCH", 4)) {
            id = le64(h + 0x118); /* the program id */
        } else if (le32(h) == 0x2020) {
            /* CIA: header, certificates, ticket, then the TMD, each 64-byte
             * aligned; the TMD (RSA-2048 signed) has the title id at 0x18C,
             * big-endian. */
            uint32_t cert = le32(h + 0x08), ticket = le32(h + 0x0C);
            long tmd = (long)(((0x2020 + 63) & ~63u));
            tmd = (long)((((uint32_t)tmd + cert + 63) & ~63u));
            tmd = (long)((((uint32_t)tmd + ticket + 63) & ~63u));
            unsigned char t[8];
            if (read_at(f, tmd + 0x18C, t, 8) == 0)
                for (int i = 0; i < 8; i++) id = id << 8 | t[i];
        }
    }
    fclose(f);
    if (!id) return -1;
    snprintf(out, n, "%016llX", (unsigned long long)id);
    return 0;
}

int gameid_from_library(const char *library_json, const char *system, const char *path, char *out, size_t n) {
    char *txt = read_file(library_json, NULL);
    cJSON *lib = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    const cJSON *g;
    int found = -1;
    cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(lib, system)) {
        const char *tid = jget_str(g, "titleId", "");
        if (strcmp(jget_str(g, "path", ""), path) != 0 || strlen(tid) != 16) continue;
        size_t o = 0;
        for (; *tid && o + 1 < n; tid++) out[o++] = (char)toupper((unsigned char)*tid);
        out[o] = 0;
        found = 0;
        break;
    }
    cJSON_Delete(lib);
    return found;
}

/* XDVDFS, the Xbox disc filesystem: a root file's offset and size. The game
 * partition sits at 0 in a trimmed .xiso, or one video partition in on a full
 * disc image (XGD1). The magic is checked at both ends of the volume
 * descriptor, so a stray copy inside game data doesn't pass for one. */
static int xdvdfs_root(FILE *f, uint64_t *base, unsigned char **table, uint32_t *table_size) {
    static const uint64_t bases[] = {0, 34078720, 265879552, 405798912};
    unsigned char vd[SECTOR];
    for (size_t i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        if (read_at(f, (long)(bases[i] + 0x10000), vd, sizeof vd) != 0 || memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20) != 0 ||
            memcmp(vd + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20) != 0)
            continue;
        const uint32_t size = le32(vd + 0x18);
        if (!size || size > 512 * 1024) return -1;
        unsigned char *t = malloc(size);
        if (!t || read_at(f, (long)(bases[i] + (uint64_t)le32(vd + 0x14) * SECTOR), t, size) != 0) {
            free(t);
            return -1;
        }
        *base = bases[i];
        *table = t;
        *table_size = size;
        return 0;
    }
    return -1;
}

static int xdvdfs_root_file(FILE *f, const char *name, uint64_t *off, uint32_t *size) {
    uint64_t base;
    unsigned char *table;
    uint32_t table_size;
    if (xdvdfs_root(f, &base, &table, &table_size) != 0) return -1;
    /* A binary tree, ordered by the uppercased name, a prefix first.
     * Children are in 4-byte units; 0 or 0xff is none. */
    uint32_t at = 0;
    int rc = -1;
    for (int depth = 0; depth < 64 && at + 14 <= table_size; depth++) {
        const unsigned char *e = table + at;
        const size_t len = e[13];
        if (at + 14 + len > table_size) break;
        int cmp = 0;
        for (size_t k = 0; k < len && !cmp; k++) {
            if (!name[k]) cmp = -1;
            else cmp = toupper((unsigned char)name[k]) - toupper(e[14 + k]);
        }
        if (!cmp && name[len]) cmp = 1;
        if (!cmp) {
            if (!(e[12] & 0x10)) {
                *off = base + (uint64_t)le32(e + 4) * SECTOR;
                *size = le32(e + 8);
                rc = 0;
            }
            break;
        }
        const uint16_t next = le16(e + (cmp < 0 ? 0 : 2));
        if (!next || next == 0xff) break;
        at = (uint32_t)next * 4;
    }
    free(table);
    return rc;
}

int gameid_xbox_short(const char *path, uint64_t *need, uint64_t *have) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint64_t base;
    unsigned char *table;
    uint32_t table_size;
    int rc = 0;
    if (xdvdfs_root(f, &base, &table, &table_size) == 0) {
        /* Every entry of the root's tree, each visited once: where the
         * furthest of them ends is as long as the image must be at least. */
        uint32_t stack[256];
        unsigned char *seen = calloc(table_size / 4 + 1, 1);
        int sp = 0;
        uint64_t end = 0;
        stack[sp++] = 0;
        while (seen && sp > 0) {
            const uint32_t at = stack[--sp];
            if (at + 14 > table_size || seen[at / 4]) continue;
            seen[at / 4] = 1;
            const unsigned char *e = table + at;
            const uint64_t stop = base + (uint64_t)le32(e + 4) * SECTOR + le32(e + 8);
            if (stop > end) end = stop;
            for (int c = 0; c < 2; c++) {
                const uint16_t next = le16(e + c * 2);
                if (next && next != 0xff && next != 0xffff && sp < 256) stack[sp++] = (uint32_t)next * 4;
            }
        }
        free(seen);
        free(table);
        if (fseek(f, 0, SEEK_END) == 0) {
            const long size = ftell(f);
            if (size >= 0 && end > (uint64_t)size) {
                *need = end;
                *have = (uint64_t)size;
                rc = 1;
            }
        }
    }
    fclose(f);
    return rc;
}

int gameid_xbox(const char *path, char *out, size_t n) {
    char xbe[PATH_MAX_LEN];
    str_copy(xbe, sizeof xbe, path);
    if (dir_exists(path)) path_join(xbe, sizeof xbe, path, "default.xbe");
    FILE *f = fopen(xbe, "rb");
    if (!f || n < 9) {
        if (f) fclose(f);
        return -1;
    }
    unsigned char h[0x11C], cert[12];
    uint64_t at = 0;
    uint32_t size = sizeof h;
    int rc = -1;
    if (read_at(f, 0, h, 4) == 0 && (!memcmp(h, "XBEH", 4) || xdvdfs_root_file(f, "default.xbe", &at, &size) == 0) &&
        size >= sizeof h && read_at(f, (long)at, h, sizeof h) == 0 && !memcmp(h, "XBEH", 4)) {
        /* The certificate, by the address the image loads at. */
        const uint32_t base = le32(h + 0x104), addr = le32(h + 0x118);
        if (addr >= base && read_at(f, (long)(at + (addr - base)), cert, sizeof cert) == 0 && le32(cert) >= 12 && le32(cert) <= 0x1000) {
            snprintf(out, n, "%08X", (unsigned)le32(cert + 8));
            rc = 0;
        }
    }
    fclose(f);
    return rc;
}


static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* An STFS package's title id: "LIVE", "PIRS" or "CON " first, the title id at
 * 0x360. A GOD game's header is one ("<title id>/00007000/<media id>"), and
 * an Xbox Live Arcade game is one file of its own. */
static int stfs_file(const char *path, char *out, size_t n) {
    unsigned char h[0x364];
    FILE *f = fopen(path, "rb");
    int rc = -1;
    if (f && read_at(f, 0, h, sizeof h) == 0 && (!memcmp(h, "LIVE", 4) || !memcmp(h, "PIRS", 4) || !memcmp(h, "CON ", 4)) &&
        be32(h + 0x360)) {
        snprintf(out, n, "%08X", (unsigned)be32(h + 0x360));
        rc = 0;
    }
    if (f) fclose(f);
    return rc;
}

/* The title id of the STFS header somewhere below dir. */
static int stfs_title(const char *dir, char *out, size_t n, int depth) {
    DIR *d = opendir(dir);
    struct dirent *de;
    int rc = -1;
    while (d && rc && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[PATH_MAX_LEN];
        path_join(path, sizeof path, dir, de->d_name);
        if (dir_exists(path)) {
            if (depth < 3 && !str_ends_with_ci(de->d_name, ".data")) rc = stfs_title(path, out, n, depth + 1);
        } else {
            rc = stfs_file(path, out, n);
        }
    }
    if (d) closedir(d);
    return rc;
}

int gameid_x360(const char *path, char *out, size_t n) {
    char xex[PATH_MAX_LEN];
    str_copy(xex, sizeof xex, path);
    if (dir_exists(path)) {
        path_join(xex, sizeof xex, path, "default.xex");
        if (!file_exists(xex)) return n >= 9 ? stfs_title(path, out, n, 0) : -1;
    } else if (n >= 9 && stfs_file(path, out, n) == 0) {
        return 0;
    }
    FILE *f = fopen(xex, "rb");
    if (!f || n < 9) {
        if (f) fclose(f);
        return -1;
    }
    unsigned char h[0x18 + 64 * 8], info[16];
    uint64_t at = 0;
    uint32_t size = 0;
    int rc = -1;
    if (read_at(f, 0, h, 4) == 0 && (!memcmp(h, "XEX2", 4) || xdvdfs_root_file(f, "default.xex", &at, &size) == 0) &&
        read_at(f, (long)at, h, 0x18) == 0 && !memcmp(h, "XEX2", 4)) {
        /* The optional headers: key and value pairs, big-endian. The
         * execution info's value is its offset from the XEX's start; its
         * title id is 12 bytes in. */
        uint32_t count = be32(h + 0x14);
        if (count > 64) count = 64;
        if (read_at(f, (long)at + 0x18, h + 0x18, count * 8) == 0)
            for (uint32_t i = 0; i < count && rc; i++)
                if (be32(h + 0x18 + i * 8) == 0x00040006 &&
                    read_at(f, (long)(at + be32(h + 0x1C + i * 8)), info, sizeof info) == 0 && be32(info + 12)) {
                    snprintf(out, n, "%08X", (unsigned)be32(info + 12));
                    rc = 0;
                }
    }
    fclose(f);
    return rc;
}
