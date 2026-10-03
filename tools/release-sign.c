/* Signs releases for in-app updates (see src/update.c). Built with "make tools".
 *
 *   release-sign keygen                       new key: secret seed and public key, hex
 *   release-sign manifest VERSION romm-sync.elf  > manifest.json
 *   RELEASE_SIGNING_KEY=<seed hex> release-sign sign manifest.json > manifest.sig
 *   release-sign check manifest.json manifest.sig   signed by a key the payload trusts?
 *
 * The secret is the 32-byte Ed25519 seed as 64 hex characters. It never goes
 * in the repo, only in the RELEASE_SIGNING_KEY secret and an offline backup. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "monocypher-ed25519.h"
#include "update_keys.h"

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    size_t cap = 1 << 20, n = 0, r;
    unsigned char *buf = malloc(cap);
    while (buf && (r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) buf = realloc(buf, cap *= 2);
    }
    fclose(f);
    if (!buf) { fprintf(stderr, "out of memory\n"); exit(1); }
    *len = n;
    return buf;
}

static void hex(const uint8_t *b, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

static int unhex(const char *s, uint8_t *out, size_t n) {
    if (!s || strlen(s) != 2 * n) return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static int usage(void) {
    fprintf(stderr, "usage: release-sign keygen | manifest VERSION ELF | sign FILE | check FILE SIG\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    if (!strcmp(argv[1], "keygen")) {
        uint8_t seed[32], keep[32], sk[64], pk[32];
        char sh[65], ph[65];
        FILE *r = fopen("/dev/urandom", "rb");
        if (!r || fread(seed, 1, 32, r) != 32) { fprintf(stderr, "no randomness\n"); return 1; }
        fclose(r);
        memcpy(keep, seed, 32);
        crypto_ed25519_key_pair(sk, pk, seed); /* wipes seed */
        hex(keep, 32, sh);
        hex(pk, 32, ph);
        printf("secret: %s\npublic: %s\n", sh, ph);
        return 0;
    }
    if (!strcmp(argv[1], "manifest") && argc == 4) {
        size_t len;
        unsigned char *elf = slurp(argv[3], &len);
        uint8_t h[64];
        char hh[129];
        crypto_sha512(h, elf, len);
        hex(h, 64, hh);
        const char *v = argv[2][0] == 'v' ? argv[2] + 1 : argv[2];
        printf("{\"version\":\"%s\",\"file\":\"romm-sync.elf\",\"size\":%zu,\"sha512\":\"%s\"}\n", v, len, hh);
        return 0;
    }
    if (!strcmp(argv[1], "sign") && argc == 3) {
        uint8_t seed[32], sk[64], pk[32], sig[64];
        if (unhex(getenv("RELEASE_SIGNING_KEY"), seed, 32)) {
            fprintf(stderr, "RELEASE_SIGNING_KEY must hold the 64-character hex secret\n");
            return 1;
        }
        crypto_ed25519_key_pair(sk, pk, seed);
        size_t len;
        unsigned char *msg = slurp(argv[2], &len);
        crypto_ed25519_sign(sig, sk, msg, len);
        fwrite(sig, 1, 64, stdout);
        char ph[65];
        hex(pk, 32, ph);
        fprintf(stderr, "signed %s with key %s\n", argv[2], ph);
        return 0;
    }
    if (!strcmp(argv[1], "check") && argc == 4) {
        size_t len, slen;
        unsigned char *msg = slurp(argv[2], &len), *sig = slurp(argv[3], &slen);
        for (size_t i = 0; slen == 64 && i < sizeof UPDATE_KEYS / sizeof UPDATE_KEYS[0]; i++) {
            if (crypto_ed25519_check(sig, UPDATE_KEYS[i], msg, len) == 0) {
                fprintf(stderr, "signature OK (trusted key %zu)\n", i);
                return 0;
            }
        }
        fprintf(stderr, "signature does NOT match any key in src/update_keys.h\n");
        return 1;
    }
    return usage();
}
