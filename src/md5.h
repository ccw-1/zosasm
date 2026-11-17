/* md5.h - MD5 (RFC 1321), used for the signature that ties an object to its
 * .dbg file (dbg.c); not a security feature. */
#ifndef ZA_MD5_H
#define ZA_MD5_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t s[4];
    uint64_t len;
    uint8_t buf[64];
} Md5;

void md5_init(Md5 *c);
void md5_update(Md5 *c, const void *p, size_t n);
void md5_final(Md5 *c, uint8_t out[16]);

#endif
