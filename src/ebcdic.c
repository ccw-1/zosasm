#include "ebcdic.h"

#include "util.h"

unsigned char *ebcdic_from_source(const char *s, size_t n, size_t *outlen) {
    unsigned char *out = xmalloc(n + 1);
    size_t i = 0, k = 0;
    while (i < n) {
        uint32_t cp;
        i += utf8_next(s + i, n - i, &cp);
        if (cp > 255) cp = '?';
        out[k++] = za_to_ebcdic[cp];
    }
    *outlen = k;
    return out;
}

char *ebcdic_decode(const unsigned char *b, size_t n) {
    char *out = xmalloc(n + 1);
    size_t i;
    for (i = 0; i < n; i++) out[i] = (char)za_to_latin1[b[i]];
    out[n] = 0;
    return out;
}
