#include "switchid.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aes.h" /* tiny-AES-c, here for sigil's PS1 cards: ECB is all XTS needs */
#include "util.h"

#define NCA_SECTOR 0x200
/* A game's NSP has a dozen files, an XCI's secure partition about as many.
 * Anything claiming thousands is lying, or not a Switch file at all. */
#define MAX_ENTRIES 1024
#define MAX_NAMES   (256 << 10)

enum { NCA_PROGRAM = 0, NCA_DATA = 4, NCA_PUBLIC_DATA = 5 };

static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const unsigned char *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static int read_at(int fd, uint64_t off, void *buf, size_t n) {
    return pread(fd, buf, n, (off_t)off) == (ssize_t)n ? 0 : -1;
}

void switchid_xts_decrypt(const unsigned char key[32], uint64_t sector, int nintendo, unsigned char *buf, size_t len,
                          size_t sector_size) {
    struct AES_ctx data, tweak;
    AES_init_ctx(&data, key);
    AES_init_ctx(&tweak, key + 16);
    for (size_t at = 0; sector_size && at + sector_size <= len; at += sector_size, sector++) {
        unsigned char t[16] = {0};
        for (int i = 0; i < 8; i++) t[nintendo ? 15 - i : i] = (unsigned char)(sector >> (8 * i));
        AES_ECB_encrypt(&tweak, t);
        for (size_t b = 0; b + 16 <= sector_size; b += 16) {
            unsigned char *p = buf + at + b;
            for (int i = 0; i < 16; i++) p[i] ^= t[i];
            AES_ECB_decrypt(&data, p);
            for (int i = 0; i < 16; i++) p[i] ^= t[i];
            /* The next block's tweak: times x in GF(2^128), lowest byte first. */
            unsigned carry = 0;
            for (int i = 0; i < 16; i++) {
                const unsigned top = t[i] >> 7;
                t[i] = (unsigned char)(t[i] << 1 | carry);
                carry = top;
            }
            if (carry) t[0] ^= 0x87;
        }
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

int switchid_header_key(const char *prod_keys, unsigned char key[32]) {
    char *text = read_file(prod_keys, NULL), *save = NULL;
    int rc = -1;
    for (char *line = text ? strtok_r(text, "\r\n", &save) : NULL; line && rc != 0; line = strtok_r(NULL, "\r\n", &save)) {
        char name[32], hex[80];
        if (sscanf(line, " %31[^ \t=] = %79s", name, hex) != 2 || !str_ieq(name, "header_key") || strlen(hex) != 64) continue;
        rc = 0;
        for (int i = 0; i < 32 && rc == 0; i++) {
            const int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
            if (hi < 0 || lo < 0) rc = -1;
            else key[i] = (unsigned char)(hi << 4 | lo);
        }
    }
    free(text);
    return rc;
}

/* What one game file has said so far. */
typedef struct {
    int fd;
    const unsigned char *key;
    uint64_t program; /* a Program NCA's title id: the game's, or its update's */
    uint64_t other;   /* another NCA's (meta, control), DLC's left out */
    int locked;       /* NCA headers left unread for want of the key */
} scan;

/* A game's or an update's id, not DLC's. DLC gets the game's id + 0x1000 +
 * its index (1 to 2000), so its last three digits are never 000 or 800. */
static int game_or_update(uint64_t id) { return id && ((id & 0xFFF) == 0 || (id & 0xFFF) == 0x800); }

static int nca_magic(const unsigned char *h) {
    return !memcmp(h, "NCA", 3) && (h[3] == '0' || h[3] == '2' || h[3] == '3');
}

/* One NCA's header at off, or an NCZ's: the same first bytes. Its second
 * sector holds all that matters: magic, content type, title id. */
static void nca_header(scan *s, uint64_t off) {
    unsigned char h[NCA_SECTOR];
    if (read_at(s->fd, off + NCA_SECTOR, h, sizeof h) != 0) return;
    if (!nca_magic(h)) { /* encrypted, unless some tool left it plain */
        if (!s->key) {
            s->locked++;
            return;
        }
        switchid_xts_decrypt(s->key, 1, 1, h, sizeof h, NCA_SECTOR);
        if (!nca_magic(h)) return;
    }
    const unsigned type = h[0x05];
    const uint64_t id = le64(h + 0x10);
    if (type == NCA_PROGRAM && id && !s->program) s->program = id;
    else if (type != NCA_DATA && type != NCA_PUBLIC_DATA && game_or_update(id) && !s->other) s->other = id;
}

typedef struct {
    char name[256];
    uint64_t off;
} fs_entry;

/* The files of a PFS0 (an NSP) or an HFS0 (an XCI's partitions), offsets
 * made absolute. How many, or -1. */
static int fs_list(int fd, uint64_t at, const char *magic, fs_entry **out) {
    unsigned char h[16];
    *out = NULL;
    if (read_at(fd, at, h, sizeof h) != 0 || memcmp(h, magic, 4) != 0) return -1;
    const size_t esz = magic[0] == 'H' ? 0x40 : 0x18;
    const uint32_t count = le32(h + 4), names = le32(h + 8);
    if (count > MAX_ENTRIES || names > MAX_NAMES) return -1;
    const size_t len = count * esz + names;
    unsigned char *t = malloc(len + 1);
    fs_entry *e = calloc(count ? count : 1, sizeof *e);
    if (!t || !e || read_at(fd, at + sizeof h, t, len) != 0) {
        free(t);
        free(e);
        return -1;
    }
    t[len] = 0;
    const uint64_t data = at + sizeof h + len;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *p = t + i * esz;
        if (le32(p + 16) < names) str_copy(e[i].name, sizeof e[i].name, (const char *)t + count * esz + le32(p + 16));
        e[i].off = data + le64(p);
    }
    free(t);
    *out = e;
    return (int)count;
}

/* "<rights id>.tik": the title id is the rights id's first 16 hex digits. */
static uint64_t ticket_id(const char *name) {
    size_t k = 0;
    while (k < 32 && isxdigit((unsigned char)name[k])) k++;
    if (k != 32 || !str_ieq(name + 32, ".tik")) return 0;
    char hex[17];
    memcpy(hex, name, 16);
    hex[16] = 0;
    return strtoull(hex, NULL, 16);
}

/* Where an XCI's secure partition starts: the card header says where the
 * root HFS0 is. Some dumps carry the card's key area first, 0x1000 bytes. */
static int xci_secure(int fd, uint64_t *at) {
    for (uint64_t base = 0; base <= 0x1000; base += 0x1000) {
        unsigned char h[0x40];
        fs_entry *root = NULL;
        if (read_at(fd, base + 0x100, h, sizeof h) != 0 || memcmp(h, "HEAD", 4) != 0) continue;
        const int n = fs_list(fd, base + le64(h + 0x30), "HFS0", &root);
        for (int i = 0; i < n; i++) {
            if (strcmp(root[i].name, "secure") != 0) continue;
            *at = root[i].off;
            free(root);
            return 0;
        }
        free(root);
    }
    return -1;
}

/* One file's base game id, 0 for none. A ticket's name first; the NCA
 * headers only when there's none. */
static uint64_t file_title(const char *path, const unsigned char *key, int *locked) {
    scan s = {open(path, O_RDONLY), key, 0, 0, 0};
    if (s.fd < 0) return 0;
    unsigned char magic[4];
    uint64_t ticket = 0, at = 0;
    fs_entry *e = NULL;
    int n = -1;
    if (read_at(s.fd, 0, magic, sizeof magic) == 0 && !memcmp(magic, "PFS0", 4)) n = fs_list(s.fd, 0, "PFS0", &e);
    else if (xci_secure(s.fd, &at) == 0) n = fs_list(s.fd, at, "HFS0", &e);
    else nca_header(&s, 0); /* an NCA or NCZ on its own */
    for (int i = 0; i < n && !ticket; i++) {
        const uint64_t t = ticket_id(e[i].name);
        if (game_or_update(t)) ticket = t;
    }
    for (int i = 0; i < n && !ticket && !s.program; i++)
        if (ext_in_list(e[i].name, ".nca,.ncz")) nca_header(&s, e[i].off);
    free(e);
    close(s.fd);
    *locked += s.locked;
    const uint64_t id = ticket ? ticket : s.program ? s.program : s.other;
    return id & ~0xFFFULL;
}

int switchid_from_file(const char *path, const unsigned char *header_key, char *out, size_t n) {
    uint64_t id = 0;
    int locked = 0;
    DIR *d = dir_exists(path) ? opendir(path) : NULL;
    if (d) { /* a folder game: the first of its files that says */
        struct dirent *de;
        while (!id && (de = readdir(d))) {
            char p[PATH_MAX_LEN];
            path_join(p, sizeof p, path, de->d_name);
            if (de->d_name[0] != '.' && file_exists(p)) id = file_title(p, header_key, &locked);
        }
        closedir(d);
    } else {
        id = file_title(path, header_key, &locked);
    }
    if (!id) return locked ? -2 : -1;
    snprintf(out, n, "%016llX", (unsigned long long)id);
    return 0;
}
