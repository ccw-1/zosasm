/* zio.h - the integer and string encoding shared by the ZSYM and ZSRC streams:
 * LEB128 integers ("uv" unsigned, "sv" zigzag signed), strings (uv length,
 * bytes), big-endian u32, and a bounds-checked reader. */
#ifndef ZA_ZIO_H
#define ZA_ZIO_H

#include <stdint.h>
#include <string.h>

#include "util.h"
#include "zsym.h"

static inline void put_uv(Buf *b, uint64_t v) {
    while (v >= 0x80) {
        buf_putc(b, (int)(v & 0x7F) | 0x80);
        v >>= 7;
    }
    buf_putc(b, (int)v);
}

static inline void put_sv(Buf *b, int64_t v) {
    put_uv(b, v < 0 ? ((~(uint64_t)v) << 1) | 1 : (uint64_t)v << 1);
}

static inline void put_str(Buf *b, const char *s) {
    size_t n = strlen(s);
    put_uv(b, n);
    buf_write(b, s, n);
}

static inline void put_u32(Buf *b, uint32_t v) {
    buf_putc(b, (int)(v >> 24 & 0xFF));
    buf_putc(b, (int)(v >> 16 & 0xFF));
    buf_putc(b, (int)(v >> 8 & 0xFF));
    buf_putc(b, (int)(v & 0xFF));
}

/* ---- reader ---- */

typedef struct {
    const uint8_t *p, *end;
    int bad;
} Rd;

static inline uint64_t get_uv(Rd *r) {
    uint64_t v = 0;
    int sh = 0;
    for (;;) {
        uint8_t c;
        if (r->p >= r->end || sh > 63) {
            r->bad = 1;
            return 0;
        }
        c = *r->p++;
        v |= (uint64_t)(c & 0x7F) << sh;
        if (!(c & 0x80)) return v;
        sh += 7;
    }
}

static inline int64_t get_sv(Rd *r) {
    uint64_t u = get_uv(r);
    return (int64_t)((u >> 1) ^ (~(u & 1) + 1));
}

static inline int get_u8(Rd *r) {
    if (r->p >= r->end) {
        r->bad = 1;
        return 0;
    }
    return *r->p++;
}

static inline uint32_t get_u32(Rd *r) {
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++) v = v << 8 | (uint32_t)get_u8(r);
    return v;
}

static inline char *get_str(Rd *r) {
    uint64_t n = get_uv(r);
    char *s;
    if (r->bad || n > (uint64_t)(r->end - r->p)) {
        r->bad = 1;
        return xstrdup("");
    }
    s = xstrndup((const char *)r->p, (size_t)n);
    r->p += n;
    return s;
}

/* ---- chunks: 'Z' 'S' version:u8 kind:u8 length:u32 payload crc32:u32 ---- */

static inline uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* A chunk with a good checksum at b[i..n): its total size, with its version,
 * kind and payload (a reader over it); 0 when there is none. */
static inline size_t chunk_at(const uint8_t *b, size_t n, size_t i, int *ver, int *kind, Rd *payload) {
    uint32_t len;
    if (n - i < 12 || b[i] != 'Z' || b[i + 1] != 'S') return 0;
    len = be32(b + i + 4);
    if (len > n - i - 12 || zsym_crc(b + i, n - i, NULL, 0, (int64_t)len + 8) != be32(b + i + 8 + len)) return 0;
    *ver = b[i + 2];
    *kind = b[i + 3];
    payload->p = b + i + 8;
    payload->end = payload->p + len;
    payload->bad = 0;
    return 12 + (size_t)len;
}

#endif
