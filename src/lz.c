/* lz.c - LZ4 block format encoder and decoder (see lz.h). */
#include <stdlib.h>
#include <string.h>

#include "lz.h"

#define HASH_BITS 14
#define MFLIMIT 12    /* a match may not start in the last 12 bytes */
#define LASTLIT 5     /* the last 5 bytes are literals */
#define MINMATCH 4
#define MAXOFF 65535

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t hash4(uint32_t v) { return (v * 2654435761u) >> (32 - HASH_BITS); }

/* the length extension bytes of a count that did not fit the 4-bit field */
static void put_ext(Buf *out, size_t rem) {
    while (rem >= 255) {
        buf_putc(out, 255);
        rem -= 255;
    }
    buf_putc(out, (int)rem);
}

/* One sequence: literals src[from..to), then a match (offset, mlen >= 4) or none (mlen 0). */
static void put_seq(Buf *out, const uint8_t *src, size_t from, size_t to, size_t off, size_t mlen) {
    size_t lit = to - from, m = mlen ? mlen - MINMATCH : 0;
    buf_putc(out, (int)((lit >= 15 ? 15 : lit) << 4 | (mlen ? (m >= 15 ? 15 : m) : 0)));
    if (lit >= 15) put_ext(out, lit - 15);
    buf_write(out, src + from, lit);
    if (mlen) {
        buf_putc(out, (int)(off & 0xFF));
        buf_putc(out, (int)(off >> 8));
        if (m >= 15) put_ext(out, m - 15);
    }
}

void lz_compress(const uint8_t *src, size_t n, Buf *out) {
    size_t ip = 0, anchor = 0;
    uint32_t *table = NULL;
    if (n > MFLIMIT) {
        table = xcalloc(((size_t)1 << HASH_BITS) * sizeof *table);
        while (ip <= n - MFLIMIT) {
            uint32_t h = hash4(rd32(src + ip));
            size_t ref = table[h]; /* position + 1, 0 = empty */
            table[h] = (uint32_t)ip + 1;
            if (ref && ip - (ref - 1) <= MAXOFF && rd32(src + ref - 1) == rd32(src + ip)) {
                size_t r = ref - 1, mlen = MINMATCH;
                while (ip + mlen < n - LASTLIT && src[r + mlen] == src[ip + mlen]) mlen++;
                put_seq(out, src, anchor, ip, ip - r, mlen);
                ip += mlen;
                anchor = ip;
            } else {
                ip++;
            }
        }
        free(table);
    }
    put_seq(out, src, anchor, n, 0, 0);
}

int lz_decompress(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen) {
    size_t ip = 0, op = 0;
    for (;;) {
        size_t lit, mlen, off, k;
        int token;
        if (ip >= inlen) return 0;
        token = in[ip++];
        lit = (size_t)token >> 4;
        if (lit == 15) {
            int s;
            do {
                if (ip >= inlen) return 0;
                s = in[ip++];
                lit += (size_t)s;
                if (lit > outlen) return 0;
            } while (s == 255);
        }
        if (lit > inlen - ip || lit > outlen - op) return 0;
        memcpy(out + op, in + ip, lit);
        ip += lit;
        op += lit;
        if (ip == inlen) break; /* the last sequence has literals only */
        if (inlen - ip < 2) return 0;
        off = (size_t)in[ip] | (size_t)in[ip + 1] << 8;
        ip += 2;
        if (off == 0 || off > op) return 0;
        mlen = (size_t)(token & 15);
        if (mlen == 15) {
            int s;
            do {
                if (ip >= inlen) return 0;
                s = in[ip++];
                mlen += (size_t)s;
                if (mlen > outlen) return 0;
            } while (s == 255);
        }
        mlen += MINMATCH;
        if (mlen > outlen - op) return 0;
        for (k = 0; k < mlen; k++) out[op + k] = out[op + k - off]; /* may overlap */
        op += mlen;
    }
    return op == outlen;
}
