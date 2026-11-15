/* lz.h - a small LZ compressor and its decoder.
 *
 * The format is the LZ4 block format (sequences of token, literals, 16-bit
 * little-endian offset, match length), written to the rules of the reference
 * encoder (the last match starts at least 12 bytes before the end, the last
 * 5 bytes are literals), so any LZ4 block decoder reads it. It is used for the
 * original sources in the object (zsrc.h); no external library is needed. */
#ifndef ZA_LZ_H
#define ZA_LZ_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

/* Append the compressed form of src[0..n) to out. */
void lz_compress(const uint8_t *src, size_t n, Buf *out);

/* Decode in[0..inlen) into out[0..outlen). Returns 1 when the block is valid
 * and produces exactly outlen bytes, else 0 (no read or write outside the
 * buffers whatever the input). */
int lz_decompress(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen);

#endif
