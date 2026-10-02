/* Minimal MD5 (RFC 1321), public domain. */
#ifndef ROMM_MD5_H
#define ROMM_MD5_H
#include <stdint.h>
typedef struct {
    uint32_t a, b, c, d;
    uint64_t len;
    unsigned char buf[64];
} romm_md5_CTX;
void romm_md5_Init(romm_md5_CTX *ctx);
void romm_md5_Update(romm_md5_CTX *ctx, const void *data, unsigned long size);
void romm_md5_Final(unsigned char *result, romm_md5_CTX *ctx);
#endif
