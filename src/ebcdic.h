/* ebcdic.h - ISO-8859-1 <-> IBM-1047 conversion. */
#ifndef ZA_EBCDIC_H
#define ZA_EBCDIC_H

#include <stddef.h>

extern const unsigned char za_to_ebcdic[256];
extern const unsigned char za_to_latin1[256];

/* Character set of C'..' constants and of the type attribute: IBM-1047
 * (default) or ISO-8859-1 (-ascii, and implied by -elf).  Names in the GOFF
 * object and the EBCDIC DC variants (CE) always use IBM-1047. */
extern int za_ascii;

#define ZA_E(c) (za_ascii ? (unsigned char)(c) : za_to_ebcdic[(unsigned char)(c)])
#define ZA_A(c) (za_to_latin1[(unsigned char)(c)])

/* Convert a UTF-8/Latin-1 source string: code points above 255 become '?'.
 * Result is malloc'ed, *outlen set. */
unsigned char *ebcdic_from_source(const char *s, size_t n, size_t *outlen);
/* Like ebcdic_from_source, but ISO-8859-1 when za_ascii is set. */
unsigned char *charset_from_source(const char *s, size_t n, size_t *outlen);
/* IBM-1047 bytes to a NUL terminated Latin-1 string (malloc'ed). */
char *ebcdic_decode(const unsigned char *b, size_t n);

#endif
