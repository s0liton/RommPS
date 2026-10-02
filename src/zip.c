#include "zip.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "util.h"

static uint32_t crc_table[256];

static void crc_init(void) {
    if (crc_table[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc_file(FILE *f, unsigned char *buf, size_t bufsize, uint32_t *size) {
    uint32_t c = 0xFFFFFFFFu, total = 0;
    size_t n;
    while ((n = fread(buf, 1, bufsize, f)) > 0) {
        for (size_t i = 0; i < n; i++) c = crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
        total += (uint32_t)n;
    }
    *size = total;
    return c ^ 0xFFFFFFFFu;
}

static void put16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}
static uint16_t get16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const unsigned char *p) { return (uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16; }

static void dos_time(uint16_t *t, uint16_t *d) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    *t = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    *d = (uint16_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
}

int zip_write(const char *zip_path, const zip_member *members, int count) {
    crc_init();
    mkdir_parent(zip_path);
    FILE *out = fopen(zip_path, "wb");
    if (!out) return -1;
    unsigned char *central = NULL;
    size_t central_len = 0;
    uint32_t offset = 0;
    uint16_t t, d;
    dos_time(&t, &d);
    int rc = 0;
    const size_t bufsize = 64 * 1024;
    unsigned char *buf = malloc(bufsize);
    if (!buf) rc = -1;

    for (int i = 0; i < count && rc == 0; i++) {
        FILE *in = fopen(members[i].path, "rb");
        if (!in) {
            rc = -1;
            break;
        }
        uint32_t size;
        uint32_t crc = crc_file(in, buf, bufsize, &size);
        rewind(in);
        const char *name = members[i].name;
        uint16_t nl = (uint16_t)strlen(name);

        unsigned char lh[30] = {0};
        put32(lh, 0x04034b50);
        put16(lh + 4, 20);
        put16(lh + 10, t);
        put16(lh + 12, d);
        put32(lh + 14, crc);
        put32(lh + 18, size);
        put32(lh + 22, size);
        put16(lh + 26, nl);
        fwrite(lh, 1, 30, out);
        fwrite(name, 1, nl, out);
        size_t n;
        while ((n = fread(buf, 1, bufsize, in)) > 0)
            if (fwrite(buf, 1, n, out) != n) rc = -1;
        fclose(in);

        central = realloc(central, central_len + 46 + nl);
        unsigned char *ch = central + central_len;
        memset(ch, 0, 46);
        put32(ch, 0x02014b50);
        put16(ch + 4, 20);
        put16(ch + 6, 20);
        put16(ch + 12, t);
        put16(ch + 14, d);
        put32(ch + 16, crc);
        put32(ch + 20, size);
        put32(ch + 24, size);
        put16(ch + 28, nl);
        put32(ch + 42, offset);
        memcpy(ch + 46, name, nl);
        central_len += 46 + nl;
        offset += 30 + nl + size;
    }

    if (rc == 0) {
        fwrite(central, 1, central_len, out);
        unsigned char end[22] = {0};
        put32(end, 0x06054b50);
        put16(end + 8, (uint16_t)count);
        put16(end + 10, (uint16_t)count);
        put32(end + 12, (uint32_t)central_len);
        put32(end + 16, offset);
        fwrite(end, 1, 22, out);
    }
    free(central);
    free(buf);
    if (fclose(out) != 0) rc = -1;
    if (rc) unlink(zip_path);
    return rc;
}

int zip_store_files(const char *zip_path, const char *const *files, int count) {
    zip_member *m = calloc((size_t)count, sizeof *m);
    if (!m) return -1;
    for (int i = 0; i < count; i++) {
        m[i].path = files[i];
        m[i].name = path_basename(files[i]);
    }
    int rc = zip_write(zip_path, m, count);
    free(m);
    return rc;
}

typedef struct {
    char name[512];
    uint16_t method;
    uint32_t csize, usize;
    size_t data; /* offset of the entry's data */
} zip_entry;

/* Parses the central directory. Returns the entry count, or -1. */
static int zip_entries(const unsigned char *z, size_t len, zip_entry **out) {
    long eocd = -1;
    for (long i = (long)len - 22; i >= 0 && eocd < 0; i--)
        if (get32(z + i) == 0x06054b50) eocd = i;
    if (eocd < 0) return -1;
    uint16_t count = get16(z + eocd + 10);
    uint32_t cd = get32(z + eocd + 16);
    zip_entry *e = calloc(count ? count : 1, sizeof *e);
    if (!e) return -1;
    int n = 0;
    for (uint16_t i = 0; i < count && cd + 46 <= len; i++) {
        const unsigned char *ch = z + cd;
        if (get32(ch) != 0x02014b50) break;
        uint16_t nl = get16(ch + 28), xl = get16(ch + 30), cl = get16(ch + 32);
        uint32_t lo = get32(ch + 42);
        if (cd + 46 + nl > len || lo + 30 > len) break;
        zip_entry *x = &e[n];
        size_t l = nl < sizeof x->name - 1 ? nl : sizeof x->name - 1;
        memcpy(x->name, ch + 46, l);
        x->name[l] = 0;
        x->method = get16(ch + 10);
        x->csize = get32(ch + 20);
        x->usize = get32(ch + 24);
        x->data = lo + 30 + get16(z + lo + 26) + get16(z + lo + 28);
        if (x->data + x->csize <= len) n++;
        cd += 46 + nl + xl + cl;
    }
    *out = e;
    return n;
}

/* Entry data, inflated when needed. Returns malloc'd bytes. */
static unsigned char *entry_data(const unsigned char *z, const zip_entry *e) {
    unsigned char *buf = malloc(e->usize ? e->usize : 1);
    if (!buf) return NULL;
    if (e->method == 0 && e->csize == e->usize) {
        memcpy(buf, z + e->data, e->usize);
        return buf;
    }
    if (e->method == 8) {
        z_stream s;
        memset(&s, 0, sizeof s);
        if (inflateInit2(&s, -MAX_WBITS) != Z_OK) {
            free(buf);
            return NULL;
        }
        s.next_in = (unsigned char *)z + e->data;
        s.avail_in = e->csize;
        s.next_out = buf;
        s.avail_out = e->usize;
        int r = inflate(&s, Z_FINISH);
        inflateEnd(&s);
        if (r == Z_STREAM_END && s.total_out == e->usize) return buf;
    }
    free(buf);
    return NULL;
}

static int safe_name(const char *name) {
    if (!name[0] || name[0] == '/' || strchr(name, '\\') || strchr(name, ':')) return 0;
    for (const char *p = name; *p;) {
        const char *slash = strchr(p, '/');
        size_t l = slash ? (size_t)(slash - p) : strlen(p);
        if (l == 2 && !strncmp(p, "..", 2)) return 0;
        if (!slash) break;
        p = slash + 1;
    }
    return 1;
}

int zip_extract_entry(const char *zip_path, const char *name, const char *dest) {
    size_t len;
    unsigned char *z = (unsigned char *)read_file(zip_path, &len);
    if (!z) return -1;
    zip_entry *e = NULL;
    int n = zip_entries(z, len, &e), rc = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(e[i].name, name) != 0) continue;
        unsigned char *data = entry_data(z, &e[i]);
        if (data) rc = write_file_atomic(dest, data, e[i].usize);
        free(data);
        break;
    }
    free(e);
    free(z);
    return rc;
}

int zip_extract_all(const char *zip_path, const char *dest_dir) {
    size_t len;
    unsigned char *z = (unsigned char *)read_file(zip_path, &len);
    if (!z) return -1;
    zip_entry *e = NULL;
    int n = zip_entries(z, len, &e), count = 0;
    for (int i = 0; i < n && count >= 0; i++) {
        size_t nl = strlen(e[i].name);
        if (nl && e[i].name[nl - 1] == '/') continue;
        if (!safe_name(e[i].name)) {
            LOGW("refusing zip entry %s", e[i].name);
            count = -1;
            break;
        }
        unsigned char *data = entry_data(z, &e[i]);
        char dest[PATH_MAX_LEN];
        path_join(dest, sizeof dest, dest_dir, e[i].name);
        if (!data || write_file_atomic(dest, data, e[i].usize) != 0) count = -1;
        else count++;
        free(data);
    }
    free(e);
    free(z);
    return n < 0 ? -1 : count;
}

static int cmp_lines(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int zip_content_hash(const char *zip_path, char out[33]) {
    size_t len;
    unsigned char *z = (unsigned char *)read_file(zip_path, &len);
    if (!z) return -1;
    zip_entry *e = NULL;
    int n = zip_entries(z, len, &e), rc = -1;
    if (n >= 0) {
        char **lines = calloc((size_t)n + 1, sizeof *lines);
        int k = 0;
        for (int i = 0; i < n; i++) {
            size_t nl = strlen(e[i].name);
            if (nl && e[i].name[nl - 1] == '/') continue;
            unsigned char *data = entry_data(z, &e[i]);
            if (!data) continue;
            char h[33];
            md5_hex(data, e[i].usize, h);
            free(data);
            lines[k] = malloc(nl + 34);
            snprintf(lines[k++], nl + 34, "%s:%s", e[i].name, h);
        }
        qsort(lines, (size_t)k, sizeof *lines, cmp_lines);
        size_t total = 0;
        for (int i = 0; i < k; i++) total += strlen(lines[i]) + 1;
        char *joined = malloc(total + 1), *p = joined;
        for (int i = 0; i < k; i++) p += sprintf(p, i ? "\n%s" : "%s", lines[i]);
        *p = 0;
        md5_hex(joined, strlen(joined), out);
        for (int i = 0; i < k; i++) free(lines[i]);
        free(lines);
        free(joined);
        rc = 0;
    }
    free(e);
    free(z);
    return rc;
}
