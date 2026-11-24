/* util.h - small helpers shared by all modules (portable C99). */
#ifndef ZA_UTIL_H
#define ZA_UTIL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Memory: all allocation aborts on failure. The assembler is a short lived
 * process, so most objects are never freed. */
void *xmalloc(size_t n);
void *xcalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...);
char *xvasprintf(const char *fmt, va_list ap);

/* ASCII-only character classes and case conversion (independent of the
 * C locale). */
int za_isdigit(int c);
int za_isalpha(int c);
int za_isalnum(int c);
int za_is_idc(int c); /* an identifier character: alnum or _ */
int za_isxdigit(int c);
int za_isspace(int c);
int za_toupper(int c);
int za_tolower(int c);
char *za_strupper(const char *s); /* new string */
char *za_strlower(const char *s);
int za_streqi(const char *a, const char *b);
int za_strncmpi(const char *a, const char *b, size_t n);
/* a whole non-negative decimal option value that fits in an int */
int za_parse_int(const char *s, int *out);

/* Growable byte buffer. buf_cstr() always returns a NUL terminated string. */
typedef struct {
    char *p;
    size_t len, cap;
} Buf;

void buf_reserve(Buf *b, size_t extra);
void buf_putc(Buf *b, int c);
void buf_write(Buf *b, const void *p, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);
const char *buf_cstr(Buf *b);

/* Growable arrays: Vec(T) v = {0}; vec_push(&v, x); v.v[i], v.n */
#define Vec(T) \
    struct {   \
        T *v;  \
        size_t n, cap; \
    }
void vec_grow_(void *vp, size_t *cap, size_t n, size_t esz);
#define vec_push(V, X)                                                        \
    do {                                                                      \
        if ((V)->n == (V)->cap) vec_grow_(&(V)->v, &(V)->cap, (V)->n, sizeof(*(V)->v)); \
        (V)->v[(V)->n++] = (X);                                               \
    } while (0)

/* String keyed hash map (keys are copied, values are borrowed). */
typedef struct {
    char **keys;
    void **vals;
    size_t cap, n;
} StrMap;

void *strmap_get(const StrMap *m, const char *key);
/* returns the previous value (or NULL) */
void *strmap_put(StrMap *m, const char *key, void *val);

/* UTF-8: decode one code point. Invalid bytes are returned as the byte
 * value itself (Latin-1 fallback). Returns the number of bytes used. */
size_t utf8_next(const char *s, size_t n, uint32_t *cp);
size_t utf8_count(const char *s, size_t n); /* number of characters */
int utf8_valid(const char *s, size_t n);    /* strictly valid UTF-8? */

/* Read a whole file; returns NULL and sets errno on failure. */
char *read_file(const char *path, size_t *len);

/* Damerau-Levenshtein (optimal string alignment) distance on bytes. */
int edit_distance(const char *a, const char *b);

#endif
