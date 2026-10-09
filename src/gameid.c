#include "gameid.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

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

/* The first data file a .cue names, beside it. */
static int cue_first_file(const char *cue, char *out, size_t n) {
    char *txt = read_file(cue, NULL);
    if (!txt) return -1;
    int rc = -1;
    char *p = txt;
    while ((p = strstr(p, "FILE"))) {
        char *q = strchr(p, '"'), *e = q ? strchr(q + 1, '"') : NULL;
        if (!q || !e) break;
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
        if (cue_first_file(path, bin, sizeof bin) != 0) return -1;
        return psx_open_file(d, bin);
    }
    return psx_open_file(d, path);
}

static void psx_close(psx_disc *d) {
    if (d->f) fclose(d->f);
    if (d->chd) chd_close(d->chd);
    free(d->hunk);
}

/* One 2048-byte user-data sector. */
static int psx_sector(psx_disc *d, uint32_t lba, unsigned char *out) {
    unsigned char frame[2352];
    if (d->chd) {
        long hunk = (long)(lba / d->frames_per_hunk);
        if (hunk != d->cached_hunk) {
            if (chd_read(d->chd, (uint32_t)hunk, d->hunk) != CHDERR_NONE) return -1;
            d->cached_hunk = hunk;
        }
        memcpy(frame, d->hunk + (size_t)(lba % d->frames_per_hunk) * d->frame_bytes, sizeof frame);
    } else if (d->raw) {
        if (fseek(d->f, (long)lba * 2352, SEEK_SET) != 0 || fread(frame, 1, sizeof frame, d->f) != sizeof frame) return -1;
    } else {
        return fseek(d->f, (long)lba * SECTOR, SEEK_SET) == 0 && fread(out, 1, SECTOR, d->f) == SECTOR ? 0 : -1;
    }
    /* Mode 2 (PS1 discs) has an 8-byte subheader after the 16-byte header. */
    memcpy(out, frame + (frame[15] == 2 ? 24 : 16), SECTOR);
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
    if (psx_open(&d, path) != 0) {
        psx_close(&d);
        return -1;
    }
    int rc = -1;
    unsigned char pvd[SECTOR], *dir = NULL;
    if (psx_sector(&d, 16, pvd) == 0 && pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5)) {
        uint32_t lba = le32(pvd + 156 + 2), size = le32(pvd + 156 + 10);
        if (size > 64 * SECTOR) size = 64 * SECTOR;
        dir = malloc(size + SECTOR);
        uint32_t got = 0;
        while (dir && got < size && psx_sector(&d, lba + got / SECTOR, dir + got) == 0) got += SECTOR;
        for (uint32_t p = 0; dir && p + 33 < got;) {
            unsigned rl = dir[p];
            if (!rl) {
                p = (p / SECTOR + 1) * SECTOR;
                continue;
            }
            unsigned fl = dir[p + 32];
            if (fl >= 10 && !strncasecmp((const char *)dir + p + 33, "SYSTEM.CNF", 10)) {
                unsigned char cnf[SECTOR + 1];
                if (psx_sector(&d, le32(dir + p + 2), cnf) == 0) {
                    uint32_t len = le32(dir + p + 10);
                    cnf[len < SECTOR ? len : SECTOR] = 0;
                    rc = serial_from_cnf((const char *)cnf, out, n);
                }
                break;
            }
            p += rl;
        }
    }
    free(dir);
    psx_close(&d);
    return rc;
}
