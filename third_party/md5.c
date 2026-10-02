/* Minimal MD5 (RFC 1321), public domain. */
#include "md5.h"
#include <string.h>

static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static const unsigned char R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void block(romm_md5_CTX *c, const unsigned char *p) {
    uint32_t w[16], a = c->a, b = c->b, cc = c->c, d = c->d;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) | ((uint32_t)p[i * 4 + 2] << 16) |
               ((uint32_t)p[i * 4 + 3] << 24);
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & cc) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ cc ^ d; g = (3 * i + 5) & 15; }
        else { f = cc ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t t = d;
        d = cc;
        cc = b;
        uint32_t x = a + f + K[i] + w[g];
        b = b + ((x << R[i]) | (x >> (32 - R[i])));
        a = t;
    }
    c->a += a; c->b += b; c->c += cc; c->d += d;
}

void romm_md5_Init(romm_md5_CTX *c) {
    c->a = 0x67452301; c->b = 0xefcdab89; c->c = 0x98badcfe; c->d = 0x10325476;
    c->len = 0;
}

void romm_md5_Update(romm_md5_CTX *c, const void *data, unsigned long size) {
    const unsigned char *p = data;
    size_t used = (size_t)(c->len & 63);
    c->len += size;
    if (used) {
        size_t take = 64 - used;
        if (take > size) take = size;
        memcpy(c->buf + used, p, take);
        p += take; size -= take;
        if (used + take < 64) return;
        block(c, c->buf);
    }
    for (; size >= 64; size -= 64, p += 64) block(c, p);
    memcpy(c->buf, p, size);
}

void romm_md5_Final(unsigned char *out, romm_md5_CTX *c) {
    uint64_t bits = c->len * 8;
    unsigned char pad[72] = {0x80};
    size_t used = (size_t)(c->len & 63);
    size_t padlen = (used < 56) ? 56 - used : 120 - used;
    romm_md5_Update(c, pad, (unsigned long)padlen);
    unsigned char lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (unsigned char)(bits >> (8 * i));
    romm_md5_Update(c, lb, 8);
    uint32_t v[4] = {c->a, c->b, c->c, c->d};
    for (int i = 0; i < 16; i++) out[i] = (unsigned char)(v[i / 4] >> (8 * (i % 4)));
}
