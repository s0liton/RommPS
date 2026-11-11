/* Switch title ids from inside game files (src/switchid.c): AES-XTS against
 * known answers, then NSP, NSZ, XCI, XCZ, NCA and NCZ files made up here,
 * their NCA headers under a made-up header_key. No Nintendo data, no keys. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "aes.h"
#include "switchid.h"
#include "util.h"

static int failures;
#define CHECK(x)                                                                 \
    do {                                                                         \
        if (!(x)) {                                                              \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);              \
            failures++;                                                          \
        }                                                                        \
    } while (0)

/* 32 bytes that have never been near a Switch. */
static const char FAKE_KEY[] = "made-up header_key, not Nintendo";
#define KEY ((const unsigned char *)FAKE_KEY)

static void unhex(const char *hex, unsigned char *out) {
    for (size_t i = 0; hex[2 * i]; i++) sscanf(hex + 2 * i, "%2hhx", &out[i]);
}

static void put32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}
static void put64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* XTS the other way, written apart from switchid.c's and checked against
 * OpenSSL's (EVP_aes_128_xts) below, so the two can't agree on a mistake. */
static void xts_encrypt(const unsigned char key[32], uint64_t sector, int nintendo, unsigned char *buf, size_t len,
                        size_t sector_size) {
    struct AES_ctx k1, k2;
    AES_init_ctx(&k1, key);
    AES_init_ctx(&k2, key + 16);
    for (size_t s = 0; s < len / sector_size; s++, sector++) {
        unsigned char t[16] = {0};
        for (int i = 0; i < 8; i++) t[nintendo ? 15 - i : i] = (unsigned char)(sector >> (8 * i));
        AES_ECB_encrypt(&k2, t);
        for (size_t b = 0; b < sector_size / 16; b++) {
            unsigned char *p = buf + s * sector_size + b * 16;
            for (int i = 0; i < 16; i++) p[i] ^= t[i];
            AES_ECB_encrypt(&k1, p);
            for (int i = 0; i < 16; i++) p[i] ^= t[i];
            const int carry = t[15] & 0x80;
            for (int i = 15; i > 0; i--) t[i] = (unsigned char)(t[i] << 1 | t[i - 1] >> 7);
            t[0] = (unsigned char)(t[0] << 1 ^ (carry ? 0x87 : 0));
        }
    }
}

/* IEEE 1619-2007, XTS-AES-128 vectors 1 to 3: 32-byte data units, the
 * sector number little-endian in the tweak. */
static void test_xts_vectors(void) {
    static const struct {
        const char *key;
        uint64_t sector;
        const char *pt, *ct;
    } v[] = {
        {"0000000000000000000000000000000000000000000000000000000000000000", 0,
         "0000000000000000000000000000000000000000000000000000000000000000",
         "917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e"},
        {"1111111111111111111111111111111122222222222222222222222222222222", 0x3333333333,
         "4444444444444444444444444444444444444444444444444444444444444444",
         "c454185e6a16936e39334038acef838bfb186fff7480adc4289382ecd6d394f0"},
        {"fffefdfcfbfaf9f8f7f6f5f4f3f2f1f022222222222222222222222222222222", 0x3333333333,
         "4444444444444444444444444444444444444444444444444444444444444444",
         "af85336b597afc1a900b2eb21ec949d292df4c047e0b21532186a5971a227a89"},
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        unsigned char key[32], pt[32], ct[32], buf[32];
        unhex(v[i].key, key), unhex(v[i].pt, pt), unhex(v[i].ct, ct);
        memcpy(buf, ct, 32);
        switchid_xts_decrypt(key, v[i].sector, 0, buf, 32, 32);
        CHECK(!memcmp(buf, pt, 32));
        memcpy(buf, pt, 32);
        xts_encrypt(key, v[i].sector, 0, buf, 32, 32);
        CHECK(!memcmp(buf, ct, 32));
    }

    /* Vector 4: a 512-byte sector, the tweak doubled 31 times. Then the
     * same as an NCA header has it, two sectors from sector 1 with the
     * tweak big-endian; the md5s are OpenSSL's ciphertexts'. */
    unsigned char key[32], buf[1024], want[16];
    char md5[33];
    unhex("2718281828459045235360287471352631415926535897932384626433832795", key);
    for (int i = 0; i < 1024; i++) buf[i] = (unsigned char)i;
    xts_encrypt(key, 0, 0, buf, 512, 512);
    unhex("27a7479befa1d476489f308cd4cfa6e2", want);
    CHECK(!memcmp(buf, want, 16));
    md5_hex(buf, 512, md5);
    CHECK(!strcmp(md5, "213fd2c3093ec042de6870b164de5d16"));
    switchid_xts_decrypt(key, 0, 0, buf, 512, 512);
    for (int i = 0; i < 512; i++) CHECK(buf[i] == (unsigned char)i);

    xts_encrypt(key, 1, 1, buf, 1024, 512);
    md5_hex(buf, 1024, md5);
    CHECK(!strcmp(md5, "c437ed25e9b3ac9c2767f29e31f22f5c"));
    unsigned char wrong[512];
    memcpy(wrong, buf, 512);
    switchid_xts_decrypt(key, 1, 0, wrong, 512, 512); /* IEEE's tweak on Nintendo's sector: garbage */
    CHECK(wrong[0] != 0 || wrong[1] != 1 || wrong[2] != 2);
    switchid_xts_decrypt(key, 1, 1, buf, 1024, 512);
    for (int i = 0; i < 1024; i++) CHECK(buf[i] == (unsigned char)i);
}

static char g_dir[64];

static const char *at(const char *name) {
    static char p[8][PATH_MAX_LEN];
    static int k;
    k = (k + 1) % 8;
    path_join(p[k], sizeof p[k], g_dir, name);
    return p[k];
}

static void write_to(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(data, 1, len, f) == len);
    if (f) fclose(f);
}

/* A growing buffer for the files made up below. */
typedef struct {
    unsigned char *p;
    size_t len;
} blob;

static void add(blob *b, const void *data, size_t len) {
    b->p = realloc(b->p, b->len + len);
    if (len) memcpy(b->p + b->len, data, len);
    b->len += len;
}

enum { PROGRAM = 0, META = 1, CONTROL = 2, DATA = 4 };

/* An NCA's first 0x1000 bytes: the header, its six sectors encrypted as an
 * NCA3's are; plain = 1 leaves it as some tools write it out. An NCZ keeps
 * the NCA's first 0x4000 bytes, then its own sections. */
static blob nca(int type, uint64_t id, int ncz, int plain) {
    blob b = {0};
    unsigned char h[0x4000];
    memset(h, 0, sizeof h);
    memset(h, 0xEE, 0x200); /* the signatures */
    memcpy(h + 0x200, "NCA3", 4);
    h[0x205] = (unsigned char)type;
    put64(h + 0x208, ncz ? 0x4000 : 0x1000);
    put64(h + 0x210, id);
    h[0x220] = 0x10;
    if (!plain) xts_encrypt(KEY, 0, 1, h, 0xC00, 0x200);
    add(&b, h, ncz ? 0x4000 : 0x1000);
    if (ncz) add(&b, "NCZSECTN\1\0\0\0\0\0\0\0", 16);
    return b;
}

typedef struct {
    const char *name;
    blob data;
} entry;

/* A PFS0 (hfs = 0) or HFS0 holding these files; they're freed. */
static blob fs(int hfs, entry *e, int n) {
    const size_t esz = hfs ? 0x40 : 0x18;
    blob names = {0}, table = {0}, data = {0}, out = {0};
    for (int i = 0; i < n; i++) {
        unsigned char ent[0x40] = {0};
        put64(ent, data.len);
        put64(ent + 8, e[i].data.len);
        put32(ent + 16, (uint32_t)names.len);
        add(&table, ent, esz);
        add(&names, e[i].name, strlen(e[i].name) + 1);
        add(&data, e[i].data.p, e[i].data.len);
        free(e[i].data.p);
    }
    while (names.len % 0x20) add(&names, "", 1);
    unsigned char h[16] = {0};
    memcpy(h, hfs ? "HFS0" : "PFS0", 4);
    put32(h + 4, (uint32_t)n);
    put32(h + 8, (uint32_t)names.len);
    add(&out, h, 16);
    add(&out, table.p, table.len);
    add(&out, names.p, names.len);
    add(&out, data.p, data.len);
    free(names.p), free(table.p), free(data.p);
    return out;
}

static blob bytes(const char *s) {
    blob b = {0};
    add(&b, s, strlen(s));
    return b;
}

/* A cartridge dump: the card header, the root HFS0 at 0xF000 as real ones
 * have it, its update and normal partitions empty, the secure one these
 * NCAs. key_area: the 0x1000 bytes some dumpers put first. */
static blob xci(entry *secure, int n, int key_area) {
    blob out = {0}, empty = fs(1, NULL, 0), empty2 = fs(1, NULL, 0);
    entry parts[] = {{"update", empty}, {"normal", empty2}, {"secure", fs(1, secure, n)}};
    blob root = fs(1, parts, 3);
    unsigned char *head = calloc(0xF000, 1);
    memcpy(head + 0x100, "HEAD", 4);
    put64(head + 0x130, 0xF000); /* from the card's start, past any key area */
    put64(head + 0x138, 0x200);
    if (key_area) {
        unsigned char area[0x1000];
        memset(area, 0x5A, sizeof area);
        add(&out, area, sizeof area);
    }
    add(&out, head, 0xF000);
    add(&out, root.p, root.len);
    free(head), free(root.p);
    return out;
}

static void save(const char *name, blob b) {
    write_to(at(name), b.p, b.len);
    free(b.p);
}

/* What switchid_from_file says: the id, or "-1"/"-2". */
static const char *id_of(const char *name, const unsigned char *key) {
    static char out[32];
    const int rc = switchid_from_file(at(name), key, out, sizeof out);
    if (rc != 0) snprintf(out, sizeof out, "%d", rc);
    return out;
}

#define SNAP        0x0100F4300BF2C000ULL /* a made-up game, its update and its DLC */
#define SNAP_UPDATE 0x0100F4300BF2C800ULL
#define SNAP_DLC    0x0100F4300BF2D001ULL
#define WANT        "0100F4300BF2C000"

static void test_files(void) {
    /* An NSP with its ticket: the name says it, no key needed. */
    entry nsp[] = {{"0100f4300bf2c0000000000000000005.tik", bytes("ticket")},
                   {"0100f4300bf2c0000000000000000005.cert", bytes("cert")},
                   {"0123456789abcdef0123456789abcdef.nca", nca(PROGRAM, SNAP, 0, 0)}};
    save("ticket.nsp", fs(0, nsp, 3));
    CHECK(!strcmp(id_of("ticket.nsp", NULL), WANT));

    /* An update's ticket names its game; DLC's doesn't, and loses to the game's. */
    entry upd[] = {{"0100F4300BF2C8000000000000000005.tik", bytes("ticket")}};
    save("update.nsp", fs(0, upd, 1));
    CHECK(!strcmp(id_of("update.nsp", NULL), WANT));
    entry both[] = {{"0100F4300BF2D0010000000000000005.tik", bytes("ticket")},
                    {"0100F4300BF2C0000000000000000005.tik", bytes("ticket")}};
    save("with-dlc.nsp", fs(0, both, 2));
    CHECK(!strcmp(id_of("with-dlc.nsp", NULL), WANT));
    entry dlc[] = {{"0100F4300BF2D0010000000000000005.tik", bytes("ticket")},
                   {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.nca", nca(DATA, SNAP_DLC, 0, 0)},
                   {"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.cnmt.nca", nca(META, SNAP_DLC, 0, 0)}};
    save("dlc.nsp", fs(0, dlc, 3));
    CHECK(!strcmp(id_of("dlc.nsp", KEY), "-1"));

    /* No ticket: the NCA headers, the Program one over DLC's; without the key, -2. */
    entry bare[] = {{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.nca", nca(DATA, SNAP_DLC, 0, 0)},
                    {"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.cnmt.nca", nca(META, SNAP_DLC, 0, 0)},
                    {"cccccccccccccccccccccccccccccccc.nca", nca(CONTROL, SNAP_UPDATE, 0, 0)},
                    {"dddddddddddddddddddddddddddddddd.nca", nca(PROGRAM, SNAP_UPDATE, 0, 0)}};
    save("noticket.nsp", fs(0, bare, 4));
    CHECK(!strcmp(id_of("noticket.nsp", KEY), WANT));
    CHECK(!strcmp(id_of("noticket.nsp", NULL), "-2"));
    unsigned char other[32];
    memcpy(other, KEY, 32);
    other[0] ^= 1;
    CHECK(!strcmp(id_of("noticket.nsp", other), "-1")); /* the wrong key reads nothing */
    entry control[] = {{"cccccccccccccccccccccccccccccccc.nca", nca(CONTROL, SNAP, 0, 0)}};
    save("control.nsp", fs(0, control, 1));
    CHECK(!strcmp(id_of("control.nsp", KEY), WANT));
    /* A game's second program (its id + 1): a Program NCA counts as it is. */
    entry second[] = {{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.nca", nca(DATA, SNAP_DLC, 0, 0)},
                      {"dddddddddddddddddddddddddddddddd.nca", nca(PROGRAM, SNAP + 1, 0, 0)}};
    save("second.nsp", fs(0, second, 2));
    CHECK(!strcmp(id_of("second.nsp", KEY), WANT));
    entry plain[] = {{"dddddddddddddddddddddddddddddddd.nca", nca(PROGRAM, SNAP, 0, 1)}};
    save("plain.nsp", fs(0, plain, 1));
    CHECK(!strcmp(id_of("plain.nsp", NULL), WANT));

    entry nsz[] = {{"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.cnmt.nca", nca(META, SNAP, 0, 0)},
                   {"dddddddddddddddddddddddddddddddd.ncz", nca(PROGRAM, SNAP, 1, 0)}};
    save("game.nsz", fs(0, nsz, 2));
    CHECK(!strcmp(id_of("game.nsz", KEY), WANT));

    /* Cartridge dumps, the No-Intro way: no tag in the name. */
    const char *name = "New Pokemon Snap (World) (En,Ja,Fr,De,Es,It,Zh,Ko).xci";
    entry sec[] = {{"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.cnmt.nca", nca(META, SNAP, 0, 0)},
                   {"dddddddddddddddddddddddddddddddd.nca", nca(PROGRAM, SNAP, 0, 0)}};
    save(name, xci(sec, 2, 0));
    CHECK(!strcmp(id_of(name, KEY), WANT));
    CHECK(!strcmp(id_of(name, NULL), "-2"));
    entry sec2[] = {{"dddddddddddddddddddddddddddddddd.nca", nca(PROGRAM, SNAP, 0, 0)}};
    save("keyarea.xci", xci(sec2, 1, 1));
    CHECK(!strcmp(id_of("keyarea.xci", KEY), WANT));
    entry sec3[] = {{"dddddddddddddddddddddddddddddddd.ncz", nca(PROGRAM, SNAP_UPDATE, 1, 0)}};
    save("game.xcz", xci(sec3, 1, 0));
    CHECK(!strcmp(id_of("game.xcz", KEY), WANT));

    save("loose.nca", nca(PROGRAM, SNAP_UPDATE, 0, 0));
    CHECK(!strcmp(id_of("loose.nca", KEY), WANT));
    save("loose.ncz", nca(PROGRAM, SNAP, 1, 0));
    CHECK(!strcmp(id_of("loose.ncz", KEY), WANT));

    /* A folder game: the file that says, past the ones that don't. */
    CHECK(mkdir(at("Folder Game"), 0755) == 0);
    save("Folder Game/readme.txt", bytes("not a game"));
    entry dlc2[] = {{"0100F4300BF2D0010000000000000005.tik", bytes("ticket")},
                    {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.nca", nca(DATA, SNAP_DLC, 0, 0)}};
    save("Folder Game/dlc.nsp", fs(0, dlc2, 2));
    entry sec4[] = {{"dddddddddddddddddddddddddddddddd.nca", nca(PROGRAM, SNAP, 0, 0)}};
    save("Folder Game/game.xci", xci(sec4, 1, 0));
    CHECK(!strcmp(id_of("Folder Game", KEY), WANT));

    /* Nonsense: nothing found, nothing crashed. */
    unsigned char junk[0x3000];
    for (size_t i = 0; i < sizeof junk; i++) junk[i] = (unsigned char)(i * 131 + 7);
    write_to(at("junk.nsp"), junk, sizeof junk);
    CHECK(!strcmp(id_of("junk.nsp", KEY), "-1"));
    save("tiny.xci", bytes("PFS0"));
    CHECK(!strcmp(id_of("tiny.xci", KEY), "-1"));
    memcpy(junk, "PFS0\xff\xff\xff\xff\x10\0\0\0", 12); /* four billion files */
    write_to(at("liar.nsp"), junk, sizeof junk);
    CHECK(!strcmp(id_of("liar.nsp", KEY), "-1"));
    memcpy(junk, "PFS0\2\0\0\0\x10\0\0\0\0\0\0\0", 16); /* names past the name table */
    put32(junk + 16 + 16, 0x7FFFFFFF);
    put32(junk + 16 + 0x18 + 16, 0x7FFFFFFF);
    write_to(at("badnames.nsp"), junk, sizeof junk);
    CHECK(!strcmp(id_of("badnames.nsp", KEY), "-1"));
    CHECK(!strcmp(id_of("missing.nsp", KEY), "-1"));
}

static void test_keys(void) {
    unsigned char key[32];
    char hex[65], text[512];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", KEY[i]);
    snprintf(text, sizeof text, "; made up, all of it\r\nmaster_key_00 = 00112233445566778899aabbccddeeff\r\nheader_key = %s\r\n"
             "titlekek_00 = ffeeddccbbaa99887766554433221100\r\n", hex);
    write_to(at("prod.keys"), text, strlen(text));
    memset(key, 0, sizeof key);
    CHECK(switchid_header_key(at("prod.keys"), key) == 0 && !memcmp(key, KEY, 32));
    for (int i = 0; hex[i]; i++) hex[i] = (char)toupper((unsigned char)hex[i]);
    snprintf(text, sizeof text, "HEADER_KEY=%s\n", hex);
    write_to(at("upper.keys"), text, strlen(text));
    CHECK(switchid_header_key(at("upper.keys"), key) == 0 && !memcmp(key, KEY, 32));
    write_to(at("short.keys"), "header_key = 0011\n", 18);
    CHECK(switchid_header_key(at("short.keys"), key) == -1);
    snprintf(text, sizeof text, "header_key = %.62szz\n", hex);
    write_to(at("bad.keys"), text, strlen(text));
    CHECK(switchid_header_key(at("bad.keys"), key) == -1);
    write_to(at("none.keys"), "master_key_00 = 00\n", 19);
    CHECK(switchid_header_key(at("none.keys"), key) == -1);
    CHECK(switchid_header_key(at("missing.keys"), key) == -1);
}

int main(void) {
    snprintf(g_dir, sizeof g_dir, "%s/switchid.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 1;
    }
    test_xts_vectors();
    test_keys();
    test_files();
    remove_tree(g_dir);
    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    puts("switchid: ok");
    return 0;
}
