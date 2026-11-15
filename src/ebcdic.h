/* ebcdic.h - ISO-8859-1 <-> IBM-1047 conversion. */
#ifndef ZA_EBCDIC_H
#define ZA_EBCDIC_H

#include <stddef.h>

extern const unsigned char za_to_ebcdic[256];
extern const unsigned char za_to_latin1[256];

#define ZA_E(c) (za_to_ebcdic[(unsigned char)(c)])
#define ZA_A(c) (za_to_latin1[(unsigned char)(c)])

/* Convert a UTF-8/Latin-1 source string: code points above 255 become '?'.
 * Result is malloc'ed, *outlen set. */
unsigned char *ebcdic_from_source(const char *s, size_t n, size_t *outlen);
/* IBM-1047 bytes to a NUL terminated Latin-1 string (malloc'ed). */
char *ebcdic_decode(const unsigned char *b, size_t n);

#endif
