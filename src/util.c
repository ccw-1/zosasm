#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void die_oom(void) {
    fputs("zosasm: out of memory\n", stderr);
    exit(70);
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die_oom();
    return p;
}

void *xcalloc(size_t n) {
    void *p = calloc(n ? n : 1, 1);
    if (!p) die_oom();
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) die_oom();
    return p;
}

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

char *xvasprintf(const char *fmt, va_list ap) {
    va_list ap2;
    int n;
    char *p;
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) n = 0;
    p = xmalloc((size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    return p;
}

char *xasprintf(const char *fmt, ...) {
    va_list ap;
    char *p;
    va_start(ap, fmt);
    p = xvasprintf(fmt, ap);
    va_end(ap);
    return p;
}

int za_isdigit(int c) { return c >= '0' && c <= '9'; }
int za_isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int za_isalnum(int c) { return za_isdigit(c) || za_isalpha(c); }
int za_isxdigit(int c) { return za_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int za_isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int za_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int za_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

char *za_strupper(const char *s) {
    char *r = xstrdup(s);
    for (char *p = r; *p; p++) *p = (char)za_toupper((unsigned char)*p);
    return r;
}

char *za_strlower(const char *s) {
    char *r = xstrdup(s);
    for (char *p = r; *p; p++) *p = (char)za_tolower((unsigned char)*p);
    return r;
}

int za_strncmpi(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        int x = za_tolower((unsigned char)*a), y = za_tolower((unsigned char)*b);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}

int za_streqi(const char *a, const char *b) {
    return za_strncmpi(a, b, (size_t)-1) == 0;
}

void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 64;
        while (c < b->len + extra + 1) c *= 2;
        b->p = xrealloc(b->p, c);
        b->cap = c;
    }
}

void buf_putc(Buf *b, int c) {
    buf_reserve(b, 1);
    b->p[b->len++] = (char)c;
}

void buf_write(Buf *b, const void *p, size_t n) {
    buf_reserve(b, n);
    if (n) memcpy(b->p + b->len, p, n);
    b->len += n;
}

void buf_puts(Buf *b, const char *s) { buf_write(b, s, strlen(s)); }

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    char *s;
    va_start(ap, fmt);
    s = xvasprintf(fmt, ap);
    va_end(ap);
    buf_puts(b, s);
    free(s);
}

const char *buf_cstr(Buf *b) {
    buf_reserve(b, 0);
    b->p[b->len] = 0;
    return b->p;
}

void vec_grow_(void *vp, size_t *cap, size_t n, size_t esz) {
    void **pp = vp;
    size_t c = *cap ? *cap * 2 : 8;
    (void)n;
    *pp = xrealloc(*pp, c * esz);
    *cap = c;
}

static size_t hash_str(const char *s) {
    size_t h = 1469598103u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

void *strmap_get(const StrMap *m, const char *key) {
    size_t i;
    if (!m->cap) return NULL;
    for (i = hash_str(key) & (m->cap - 1); m->keys[i]; i = (i + 1) & (m->cap - 1))
        if (!strcmp(m->keys[i], key)) return m->vals[i];
    return NULL;
}

static void strmap_grow(StrMap *m) {
    StrMap o = *m;
    size_t i;
    m->cap = o.cap ? o.cap * 2 : 16;
    m->keys = xcalloc(m->cap * sizeof(char *));
    m->vals = xcalloc(m->cap * sizeof(void *));
    m->n = 0;
    for (i = 0; i < o.cap; i++)
        if (o.keys[i]) strmap_put(m, o.keys[i], o.vals[i]);
    free(o.keys);
    free(o.vals);
}

void *strmap_put(StrMap *m, const char *key, void *val) {
    size_t i;
    if (!m->cap || (m->n + 1) * 2 > m->cap) strmap_grow(m);
    for (i = hash_str(key) & (m->cap - 1); m->keys[i]; i = (i + 1) & (m->cap - 1))
        if (!strcmp(m->keys[i], key)) {
            void *old = m->vals[i];
            m->vals[i] = val;
            return old;
        }
    m->keys[i] = xstrdup(key);
    m->vals[i] = val;
    m->n++;
    return NULL;
}

size_t utf8_next(const char *s, size_t n, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)s;
    unsigned c = u[0];
    size_t need, i;
    uint32_t v;
    if (n == 0) {
        *cp = 0;
        return 0;
    }
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        need = 1;
        v = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        need = 2;
        v = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3;
        v = c & 0x07;
    } else {
        *cp = c;
        return 1;
    }
    if (n < need + 1) {
        *cp = c;
        return 1;
    }
    for (i = 1; i <= need; i++) {
        if ((u[i] & 0xC0) != 0x80) {
            *cp = c;
            return 1;
        }
        v = (v << 6) | (u[i] & 0x3F);
    }
    *cp = v;
    return need + 1;
}

int utf8_valid(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint32_t cp;
        size_t k = utf8_next(s + i, n - i, &cp);
        if (k == 1 && cp >= 0x80) return 0;
        if ((k == 3 && cp < 0x800) || (k == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
        i += k;
    }
    return 1;
}

size_t utf8_count(const char *s, size_t n) {
    size_t i = 0, cnt = 0;
    while (i < n) {
        uint32_t cp;
        i += utf8_next(s + i, n - i, &cp);
        cnt++;
    }
    return cnt;
}

char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    Buf b = {0};
    char tmp[8192];
    size_t n;
    if (!f) return NULL;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_write(&b, tmp, n);
    if (ferror(f)) {
        int e = errno;
        fclose(f);
        free(b.p);
        errno = e;
        return NULL;
    }
    fclose(f);
    buf_cstr(&b);
    *len = b.len;
    return b.p;
}

int edit_distance(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b), i, j;
    int *d = xmalloc((la + 1) * (lb + 1) * sizeof(int));
#define D(i, j) d[(i) * (lb + 1) + (j)]
    int r;
    for (i = 0; i <= la; i++) D(i, 0) = (int)i;
    for (j = 0; j <= lb; j++) D(0, j) = (int)j;
    for (i = 1; i <= la; i++)
        for (j = 1; j <= lb; j++) {
            int cost = a[i - 1] != b[j - 1];
            int v = D(i - 1, j) + 1;
            if (D(i, j - 1) + 1 < v) v = D(i, j - 1) + 1;
            if (D(i - 1, j - 1) + cost < v) v = D(i - 1, j - 1) + cost;
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] &&
                D(i - 2, j - 2) + 1 < v)
                v = D(i - 2, j - 2) + 1;
            D(i, j) = v;
        }
    r = D(la, lb);
#undef D
    free(d);
    return r;
}
