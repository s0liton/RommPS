#include "zip.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "util.h"

static void put16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}
static uint16_t get16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const unsigned char *p) { return (uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16; }

/* An entry's time: when its file was last written, so whoever unpacks it
 * knows how old it is (memory cards, src/memcard.c). */
static void dos_time(time_t when, uint16_t *t, uint16_t *d) {
    struct tm tm;
    localtime_r(&when, &tm);
    *t = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    *d = (uint16_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
}

/* Entries are deflated, at zlib's fastest. Stored, a save is as big as its
 * files, and some emulators make those at full size up front: Eden's save for
 * New Pokemon Snap is 1.6 GB, about 1 MB of it not zeros, which RomM turns
 * away. Deflated, it's a few MB. The slower levels shrink zeros further, but
 * spend seconds on a console doing it. */
#define ZIP_LEVEL Z_BEST_SPEED

/* Deflates the rest of in onto out a buffer at a time, as a file can run to
 * gigabytes, with its crc and both sizes. */
static int deflate_entry(z_stream *zs, FILE *in, FILE *out, unsigned char *ibuf, unsigned char *obuf, size_t bufsize,
                         uint32_t *crc, uint64_t *usize, uint64_t *csize) {
    if (deflateReset(zs) != Z_OK) return -1;
    uLong c = crc32(0L, Z_NULL, 0);
    uint64_t u = 0, z = 0;
    int flush;
    do {
        const size_t n = fread(ibuf, 1, bufsize, in);
        if (ferror(in)) return -1;
        flush = feof(in) ? Z_FINISH : Z_NO_FLUSH;
        c = crc32(c, ibuf, (uInt)n);
        u += n;
        zs->next_in = ibuf;
        zs->avail_in = (uInt)n;
        do {
            zs->next_out = obuf;
            zs->avail_out = (uInt)bufsize;
            if (deflate(zs, flush) == Z_STREAM_ERROR) return -1;
            const size_t have = bufsize - zs->avail_out;
            if (have && fwrite(obuf, 1, have, out) != have) return -1;
            z += have;
        } while (zs->avail_out == 0);
    } while (flush != Z_FINISH);
    *crc = (uint32_t)c;
    *usize = u;
    *csize = z;
    return 0;
}

/* The local header goes out before its data, so its crc and sizes are
 * filled in once they're known. */
static int local_sizes(FILE *out, uint32_t at, uint32_t crc, uint32_t csize, uint32_t usize) {
    unsigned char p[12];
    put32(p, crc);
    put32(p + 4, csize);
    put32(p + 8, usize);
    const off_t end = ftello(out);
    if (end < 0 || fseeko(out, (off_t)at + 14, SEEK_SET) != 0 || fwrite(p, 1, sizeof p, out) != sizeof p) return -1;
    return fseeko(out, end, SEEK_SET);
}

int zip_write(const char *zip_path, const zip_member *members, int count) {
    mkdir_parent(zip_path);
    FILE *out = fopen(zip_path, "wb");
    if (!out) return -1;
    unsigned char *central = NULL;
    size_t central_len = 0;
    uint32_t offset = 0;
    int rc = 0;
    const size_t bufsize = 64 * 1024;
    unsigned char *buf = malloc(bufsize), *zbuf = malloc(bufsize);
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (!buf || !zbuf || deflateInit2(&zs, ZIP_LEVEL, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) rc = -1;

    for (int i = 0; i < count && rc == 0; i++) {
        FILE *in = fopen(members[i].path, "rb");
        if (!in) {
            rc = -1;
            break;
        }
        struct stat st;
        uint16_t t, d;
        dos_time(fstat(fileno(in), &st) == 0 ? st.st_mtime : time(NULL), &t, &d);
        const char *name = members[i].name;
        uint16_t nl = (uint16_t)strlen(name);

        unsigned char lh[30] = {0};
        put32(lh, 0x04034b50);
        put16(lh + 4, 20);
        put16(lh + 8, 8);
        put16(lh + 10, t);
        put16(lh + 12, d);
        put16(lh + 26, nl);
        fwrite(lh, 1, 30, out);
        fwrite(name, 1, nl, out);
        uint32_t crc = 0;
        uint64_t usize = 0, csize = 0;
        if (deflate_entry(&zs, in, out, buf, zbuf, bufsize, &crc, &usize, &csize) != 0) {
            rc = -1;
        } else if (usize >= 0xFFFFFFFFu || (uint64_t)offset + 30 + nl + csize >= 0xFFFFFFFFu) {
            /* Past 4 GB a zip needs zip64, and RomM gives up on saves long before that. */
            LOGE("%s: too big with %s, over the 4 GB a zip without zip64 holds", zip_path, members[i].path);
            rc = -2;
        } else if (local_sizes(out, offset, crc, (uint32_t)csize, (uint32_t)usize) != 0) {
            rc = -1;
        }
        fclose(in);

        unsigned char *grown = realloc(central, central_len + 46 + nl);
        if (!grown) {
            rc = -1;
            break;
        }
        central = grown;
        unsigned char *ch = central + central_len;
        memset(ch, 0, 46);
        put32(ch, 0x02014b50);
        put16(ch + 4, 20);
        put16(ch + 6, 20);
        put16(ch + 10, 8);
        put16(ch + 12, t);
        put16(ch + 14, d);
        put32(ch + 16, crc);
        put32(ch + 20, (uint32_t)csize);
        put32(ch + 24, (uint32_t)usize);
        put16(ch + 28, nl);
        put32(ch + 42, offset);
        memcpy(ch + 46, name, nl);
        central_len += 46 + nl;
        offset += 30 + nl + (uint32_t)csize;
    }

    if (rc == 0) {
        if (central_len) fwrite(central, 1, central_len, out);
        unsigned char end[22] = {0};
        put32(end, 0x06054b50);
        put16(end + 8, (uint16_t)count);
        put16(end + 10, (uint16_t)count);
        put32(end + 12, (uint32_t)central_len);
        put32(end + 16, offset);
        fwrite(end, 1, 22, out);
        if (ferror(out)) rc = -1; /* a full disk makes a zip RomM won't open */
    }
    deflateEnd(&zs);
    free(central);
    free(buf);
    free(zbuf);
    if (fclose(out) != 0 && rc == 0) rc = -1;
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

/* An entry written out a buffer at a time, through a .tmp beside dest: a
 * save that's mostly zeros can come out of a few MB of zip as gigabytes. */
static int entry_to_file(const unsigned char *z, const zip_entry *e, const char *dest) {
    if (e->method != 8 && !(e->method == 0 && e->csize == e->usize)) return -1;
    char tmp[PATH_MAX_LEN];
    snprintf(tmp, sizeof tmp, "%s.tmp", dest);
    mkdir_parent(dest);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = 1;
    if (e->method == 0) {
        ok = fwrite(z + e->data, 1, e->usize, f) == e->usize;
    } else {
        const size_t bufsize = 64 * 1024;
        unsigned char *buf = malloc(bufsize);
        z_stream s;
        memset(&s, 0, sizeof s);
        if (!buf || inflateInit2(&s, -MAX_WBITS) != Z_OK) {
            free(buf);
            fclose(f);
            unlink(tmp);
            return -1;
        }
        s.next_in = (unsigned char *)z + e->data;
        s.avail_in = e->csize;
        int r = Z_OK;
        while (ok && r == Z_OK) {
            s.next_out = buf;
            s.avail_out = (uInt)bufsize;
            r = inflate(&s, Z_NO_FLUSH);
            const size_t have = bufsize - s.avail_out;
            if ((r != Z_OK && r != Z_STREAM_END) || fwrite(buf, 1, have, f) != have) ok = 0;
        }
        ok = ok && r == Z_STREAM_END && s.total_out == e->usize;
        inflateEnd(&s);
        free(buf);
    }
    if (ok && (fflush(f) != 0 || fsync(fileno(f)) != 0)) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, dest) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
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
        rc = entry_to_file(z, &e[i], dest);
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
        char dest[PATH_MAX_LEN];
        path_join(dest, sizeof dest, dest_dir, e[i].name);
        if (entry_to_file(z, &e[i], dest) != 0) count = -1;
        else count++;
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

static uint64_t get64(const unsigned char *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }

static int read_at64(FILE *f, uint64_t at, void *buf, size_t n) {
    return fseeko(f, (off_t)at, SEEK_SET) == 0 && fread(buf, 1, n, f) == n ? 0 : -1;
}

/* A central directory entry's sizes and offset, from its zip64 extra field
 * where the 32-bit ones say 0xFFFFFFFF (in that order, only those that do). */
static void zip64_fields(const unsigned char *x, size_t xl, uint64_t *usize, uint64_t *csize, uint64_t *lo) {
    for (size_t p = 0; p + 4 <= xl;) {
        const uint16_t id = get16(x + p), l = get16(x + p + 2);
        if (p + 4 + l > xl) return;
        if (id == 0x0001) {
            size_t o = p + 4;
            uint64_t *want[] = {usize, csize, lo};
            for (int i = 0; i < 3; i++) {
                if (*want[i] != 0xFFFFFFFFu) continue;
                if (o + 8 > p + 4 + l) return;
                *want[i] = get64(x + o);
                o += 8;
            }
            return;
        }
        p += 4 + (size_t)l;
    }
}

int zip_read_suffix(const char *zip_path, const char *suffix, size_t max, unsigned char **out, size_t *len) {
    *out = NULL;
    *len = 0;
    FILE *f = fopen(zip_path, "rb");
    if (!f) return -1;
    int rc = -1;
    unsigned char *tail = NULL, *cdir = NULL, *data = NULL, *buf = NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size < 22) goto out;
    const uint64_t size = (uint64_t)st.st_size;
    /* The end record is in the last 22 bytes, plus a comment of up to 64 KB. */
    const size_t tl = size < 22 + 65535 ? (size_t)size : 22 + 65535;
    tail = malloc(tl);
    if (!tail || read_at64(f, size - tl, tail, tl) != 0) goto out;
    long eocd = -1;
    for (long i = (long)tl - 22; i >= 0 && eocd < 0; i--)
        if (get32(tail + i) == 0x06054b50) eocd = i;
    if (eocd < 0) goto out;
    uint64_t count = get16(tail + eocd + 10), cd_size = get32(tail + eocd + 12), cd = get32(tail + eocd + 16);
    if (count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd == 0xFFFFFFFFu) {
        /* Zip64: a locator right before the end record points at its own. */
        unsigned char loc[20], z64[56];
        const uint64_t at = size - tl + (uint64_t)eocd;
        if (at < 20 || read_at64(f, at - 20, loc, sizeof loc) != 0 || get32(loc) != 0x07064b50 ||
            read_at64(f, get64(loc + 8), z64, sizeof z64) != 0 || get32(z64) != 0x06064b50)
            goto out;
        count = get64(z64 + 32);
        cd_size = get64(z64 + 40);
        cd = get64(z64 + 48);
    }
    if (cd_size > 64u << 20 || cd > size || cd_size > size - cd) goto out;
    cdir = malloc(cd_size ? (size_t)cd_size : 1);
    if (!cdir || read_at64(f, cd, cdir, (size_t)cd_size) != 0) goto out;

    const size_t sl = strlen(suffix);
    int best_depth = -1;
    size_t best_nl = 0;
    uint64_t b_lo = 0, b_cs = 0, b_us = 0;
    uint16_t b_method = 0;
    for (uint64_t i = 0, p = 0; i < count && p + 46 <= cd_size; i++) {
        const unsigned char *ch = cdir + p;
        if (get32(ch) != 0x02014b50) break;
        const uint16_t nl = get16(ch + 28), xl = get16(ch + 30), cl = get16(ch + 32);
        if (p + 46 + nl + xl > cd_size) break;
        const char *name = (const char *)ch + 46;
        p += 46 + (uint64_t)nl + xl + cl;
        if (nl < sl || strncasecmp(name + nl - sl, suffix, sl) != 0 || (nl > sl && name[nl - sl - 1] != '/')) continue;
        int depth = 0;
        for (size_t k = 0; k < nl; k++) depth += name[k] == '/';
        if (best_depth >= 0 && (depth > best_depth || (depth == best_depth && nl >= best_nl))) continue;
        uint64_t us = get32(ch + 24), cs = get32(ch + 20), lo = get32(ch + 42);
        zip64_fields(ch + 46 + nl, xl, &us, &cs, &lo);
        best_depth = depth;
        best_nl = nl;
        b_lo = lo;
        b_cs = cs;
        b_us = us;
        b_method = get16(ch + 10);
    }
    unsigned char lh[30];
    if (best_depth < 0 || b_us > max || b_cs > (uint64_t)max + 65536 || b_lo > size - 30 ||
        read_at64(f, b_lo, lh, sizeof lh) != 0 || get32(lh) != 0x04034b50)
        goto out;
    const uint64_t at = b_lo + 30 + get16(lh + 26) + get16(lh + 28);
    if (at > size || b_cs > size - at) goto out;
    data = malloc(b_cs ? (size_t)b_cs : 1);
    buf = malloc(b_us ? (size_t)b_us : 1);
    if (!data || !buf || read_at64(f, at, data, (size_t)b_cs) != 0) goto out;
    if (b_method == 0 && b_cs == b_us) {
        memcpy(buf, data, (size_t)b_us);
        rc = 0;
    } else if (b_method == 8) {
        z_stream s;
        memset(&s, 0, sizeof s);
        if (inflateInit2(&s, -MAX_WBITS) == Z_OK) {
            s.next_in = data;
            s.avail_in = (uInt)b_cs;
            s.next_out = buf;
            s.avail_out = (uInt)b_us;
            const int r = inflate(&s, Z_FINISH);
            if (r == Z_STREAM_END && s.total_out == b_us) rc = 0;
            inflateEnd(&s);
        }
    }
    if (rc == 0) {
        *out = buf;
        *len = (size_t)b_us;
        buf = NULL;
    }
out:
    free(tail);
    free(cdir);
    free(data);
    free(buf);
    fclose(f);
    return rc;
}

time_t zip_first_time(const char *zip_path) {
    unsigned char lh[30];
    FILE *fp = fopen(zip_path, "rb");
    if (!fp) return 0;
    const size_t n = fread(lh, 1, sizeof lh, fp);
    fclose(fp);
    if (n != sizeof lh || get32(lh) != 0x04034b50) return 0;
    const uint16_t t = get16(lh + 10), d = get16(lh + 12);
    if (!d) return 0;
    struct tm tm = {0};
    tm.tm_year = (d >> 9) + 80;
    tm.tm_mon = ((d >> 5) & 15) - 1;
    tm.tm_mday = d & 31;
    tm.tm_hour = t >> 11;
    tm.tm_min = (t >> 5) & 63;
    tm.tm_sec = (t & 31) * 2;
    tm.tm_isdst = -1;
    const time_t when = mktime(&tm);
    return when < 0 ? 0 : when;
}
