/* Save zips (src/zip.c): written deflated a buffer at a time, read back the
 * same, and with the content hash RomM gives them, which goes by what's in
 * the files, not by how they're packed. A save an older RommPS zipped stored
 * and the same save zipped now must be one save to everyone, or every console
 * sends all its saves again after the update.
 *   make unit */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "util.h"
#include "zip.h"

static int failures;
#define CHECK(x)                                                                 \
    do {                                                                         \
        if (!(x)) {                                                              \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);              \
            failures++;                                                          \
        }                                                                        \
    } while (0)

#define BIG (64 << 20)

static void put(const char *path, const void *data, size_t n) {
    mkdir_parent(path);
    write_file_atomic(path, data, n);
}

/* A Switch save as Eden makes it: the file at its full size, nearly all zeros.
 * Sparse, so the test doesn't write 64 MB of nothing to make it. */
static void mostly_zeros(const char *path) {
    mkdir_parent(path);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const long at[] = {0, 65535, 65536, 33 << 20, BIG - 5};
    for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
        fseek(f, at[i], SEEK_SET);
        fputs("SNAP", f);
    }
    fclose(f);
    CHECK(truncate(path, BIG) == 0);
}

static int same_files(const char *a, const char *b) {
    size_t na = 0, nb = 0;
    char *x = read_file(a, &na), *y = read_file(b, &nb);
    const int same = x && y && na == nb && !memcmp(x, y, na);
    free(x);
    free(y);
    return same;
}

static int cmp_lines(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* RomM's hash, from the files themselves: md5 of the sorted "name:md5"
 * lines, as bundle_hash makes it. */
static void files_hash(const zip_member *m, int n, char out[33]) {
    char *lines[8], joined[1024] = "";
    for (int i = 0; i < n; i++) {
        char h[33];
        CHECK(md5_file_hex(m[i].path, h) == 0);
        lines[i] = malloc(strlen(m[i].name) + 34);
        sprintf(lines[i], "%s:%s", m[i].name, h);
    }
    qsort(lines, (size_t)n, sizeof *lines, cmp_lines);
    for (int i = 0; i < n; i++) {
        if (i) strcat(joined, "\n");
        strcat(joined, lines[i]);
        free(lines[i]);
    }
    md5_hex(joined, strlen(joined), out);
}

static uint32_t le(const unsigned char *p, int n) {
    uint32_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

int main(void) {
    char root[] = "/tmp/zip_test.XXXXXX";
    if (!mkdtemp(root)) return 1;
    char in[5][PATH_MAX_LEN], zip[PATH_MAX_LEN], out[PATH_MAX_LEN], p[PATH_MAX_LEN];
    const char *names[5] = {"0100F4300BF2C000/meta", "0100F4300BF2C000/empty", "0100F4300BF2C000/noise.bin",
                            "0100F4300BF2C000/text.dat", "0100F4300BF2C000/sub/big.dat"};
    zip_member m[5];
    for (int i = 0; i < 5; i++) {
        snprintf(in[i], sizeof in[i], "%s/in/%s", root, names[i]);
        m[i].path = in[i];
        m[i].name = names[i];
    }
    path_join(zip, sizeof zip, root, "save.zip");
    path_join(out, sizeof out, root, "out");

    put(in[0], "snap meta", 9);
    put(in[1], "", 0);
    /* Noise deflate can't shrink, across several buffers. */
    unsigned char *noise = malloc(300 * 1024);
    uint32_t x = 2463534242u;
    for (size_t i = 0; i < 300 * 1024; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        noise[i] = (unsigned char)x;
    }
    put(in[2], noise, 300 * 1024);
    free(noise);
    char text[4096] = "";
    for (int i = 0; i < 100; i++) strcat(text, "Professor Oak's photo lab. ");
    put(in[3], text, strlen(text));
    mostly_zeros(in[4]);
    const time_t when = 1767225600; /* 2026-01-01 00:00:00 UTC, even seconds for DOS time */
    CHECK(set_file_mtime(in[0], when) == 0);

    CHECK(zip_write(zip, m, 5) == 0);
    /* 64 MB of zeros and 300 KB of noise, in under 1 MB. */
    CHECK(file_size(zip) > 300 * 1024 && file_size(zip) < 1024 * 1024);
    CHECK(zip_first_time(zip) == when);

    char want[33], got[33];
    files_hash(m, 5, want);
    CHECK(zip_content_hash(zip, got) == 0 && !strcmp(got, want));

    CHECK(zip_extract_all(zip, out) == 5);
    for (int i = 0; i < 5; i++) {
        path_join(p, sizeof p, out, names[i]);
        CHECK(same_files(p, in[i]));
    }
    path_join(p, sizeof p, root, "big.again");
    CHECK(zip_extract_entry(zip, names[4], p) == 0 && same_files(p, in[4]));

    /* zip_store_files: each under its base name. */
    const char *two[] = {in[0], in[3]};
    CHECK(zip_store_files(zip, two, 2) == 0);
    path_join(p, sizeof p, root, "text.again");
    CHECK(zip_extract_entry(zip, "text.dat", p) == 0 && same_files(p, in[3]));

    /* The local header has the crc and sizes too (they go in after the data),
     * for readers that go by it rather than the central directory. */
    CHECK(zip_write(zip, &m[3], 1) == 0);
    size_t zn = 0;
    unsigned char *z = (unsigned char *)read_file(zip, &zn);
    const uint32_t crc = (uint32_t)crc32(0L, (const Bytef *)text, (uInt)strlen(text));
    CHECK(z && zn > 30 + 22 && le(z + 8, 2) == 8 && le(z + 14, 4) == crc && le(z + 22, 4) == strlen(text) &&
          le(z + 18, 4) < strlen(text) && 30 + strlen(names[3]) + le(z + 18, 4) == le(z + zn - 22 + 16, 4));
    free(z);

    /* Nothing to zip is still a zip; a file that isn't there is no zip at all. */
    CHECK(zip_write(zip, m, 0) == 0 && file_size(zip) == 22);
    path_join(p, sizeof p, root, "none");
    CHECK(zip_extract_all(zip, p) == 0);
    zip_member gone = {"/nonexistent/zip_test/file", "file"};
    CHECK(zip_write(zip, &gone, 1) == -1 && file_size(zip) < 0);

    char cmd[PATH_MAX_LEN + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", root);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("zip: ok\n");
    return failures != 0;
}
