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
