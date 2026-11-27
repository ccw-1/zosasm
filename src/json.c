/* json.c - small JSON reader/writer for goffedit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

typedef struct { const char *p, *end, *err; int depth; } JP;
static void ws(JP *p) {
    while (p->p < p->end && (*p->p == ' ' || *p->p == '\t' || *p->p == '\n' || *p->p == '\r')) p->p++;
}
static int hex4(JP *p, unsigned *v) {
    int i; unsigned x = 0;
    if (p->end - p->p < 4) return 0;
    for (i = 0; i < 4; i++) {
        int c = (unsigned char)p->p[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        x = x * 16 + (unsigned)d;
    }
    p->p += 4; *v = x; return 1;
}
static void put8(Buf *b, unsigned cp) {
    if (cp < 0x80) buf_putc(b, (int)cp);
    else if (cp < 0x800) { buf_putc(b, 0xC0 | (int)(cp >> 6)); buf_putc(b, 0x80 | (int)(cp & 0x3F)); }
    else { buf_putc(b, 0xE0 | (int)(cp >> 12)); buf_putc(b, 0x80 | (int)((cp >> 6) & 0x3F)); buf_putc(b, 0x80 | (int)(cp & 0x3F)); }
}
static char *jstr(JP *p, size_t *outn) {
    Buf b = {0};
    while (p->p < p->end) {
        unsigned char c = (unsigned char)*p->p++;
        if (c == '"') { buf_putc(&b, 0); if (outn) *outn = b.len - 1; return b.p; }
        if (c < 0x20) { p->err = "control character in string"; free(b.p); return NULL; }
        if (c != '\\') { buf_putc(&b, c); continue; }
        if (p->p >= p->end) break;
        c = (unsigned char)*p->p++;
        switch (c) {
        case '"': buf_putc(&b, '"'); break; case '\\': buf_putc(&b, '\\'); break;
        case '/': buf_putc(&b, '/'); break; case 'b': buf_putc(&b, '\b'); break;
        case 'f': buf_putc(&b, '\f'); break; case 'n': buf_putc(&b, '\n'); break;
        case 'r': buf_putc(&b, '\r'); break; case 't': buf_putc(&b, '\t'); break;
        case 'u': { unsigned u, lo; if (!hex4(p, &u)) { p->err = "bad \\u"; free(b.p); return NULL; }
            if (u >= 0xD800 && u <= 0xDBFF && p->end - p->p >= 6 && p->p[0] == '\\' && p->p[1] == 'u') {
                p->p += 2; if (!hex4(p, &lo)) { p->err = "bad surrogate"; free(b.p); return NULL; }
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            }
            put8(&b, u); break; }
        default: p->err = "bad escape"; free(b.p); return NULL;
        }
    }
    p->err = "unterminated string"; free(b.p); return NULL;
}
static Json *jval(JP *p);
static Json *jnew(JsonType t) { Json *j = xcalloc(sizeof *j); j->type = t; return j; }
static Json *jarr(JP *p) {
    Json *j = jnew(JSON_ARR);
    ws(p);
    if (p->p < p->end && *p->p == ']') { p->p++; return j; }
    for (;;) {
        Json *v = jval(p); if (!v) { json_free(j); return NULL; }
        j->v = xrealloc(j->v, (j->n + 1) * sizeof *j->v); j->v[j->n++] = v;
        ws(p); if (p->p >= p->end) { p->err = "unterminated array"; json_free(j); return NULL; }
        if (*p->p == ',') { p->p++; continue; }
        if (*p->p == ']') { p->p++; return j; }
        p->err = "expected ',' or ']'"; json_free(j); return NULL;
    }
}
static Json *jobj(JP *p) {
    Json *j = jnew(JSON_OBJ);
    ws(p);
    if (p->p < p->end && *p->p == '}') { p->p++; return j; }
    for (;;) {
        char *key; size_t kn; Json *v;
        ws(p);
        if (p->p >= p->end || *p->p != '"') { p->err = "expected object key"; json_free(j); return NULL; }
        p->p++; key = jstr(p, &kn); if (!key) { json_free(j); return NULL; }
        ws(p); if (p->p >= p->end || *p->p != ':') { free(key); p->err = "expected ':'"; json_free(j); return NULL; }
        p->p++; v = jval(p); if (!v) { free(key); json_free(j); return NULL; }
        j->k = xrealloc(j->k, (j->n + 1) * sizeof *j->k); j->v = xrealloc(j->v, (j->n + 1) * sizeof *j->v);
        j->k[j->n] = key; j->v[j->n++] = v;
        ws(p); if (p->p >= p->end) { p->err = "unterminated object"; json_free(j); return NULL; }
        if (*p->p == ',') { p->p++; continue; }
        if (*p->p == '}') { p->p++; return j; }
        p->err = "expected ',' or '}'"; json_free(j); return NULL;
    }
}
static Json *jval(JP *p) {
    Json *j; ws(p);
    if (p->p >= p->end || p->depth++ > 200) { p->err = "bad JSON depth"; return NULL; }
    if (*p->p == '{') { p->p++; j = jobj(p); }
    else if (*p->p == '[') { p->p++; j = jarr(p); }
    else if (*p->p == '"') { p->p++; j = jnew(JSON_STR); j->str = jstr(p, &j->n); if (!j->str) { free(j); j = NULL; } }
    else if (!strncmp(p->p, "true", 4)) { p->p += 4; j = jnew(JSON_BOOL); j->b = 1; }
    else if (!strncmp(p->p, "false", 5)) { p->p += 5; j = jnew(JSON_BOOL); j->b = 0; }
    else if (!strncmp(p->p, "null", 4)) { p->p += 4; j = jnew(JSON_NULL); }
    else {
        char *end = NULL; double d = strtod(p->p, &end);
        if (end == p->p || end > p->end) { p->err = "bad JSON value"; j = NULL; }
        else { p->p = end; j = jnew(JSON_NUM); j->num = d; }
    }
    p->depth--; return j;
}
Json *json_parse(const char *s, size_t n, const char **err) {
    JP p; Json *j;
    p.p = s; p.end = s + n; p.err = NULL; p.depth = 0;
    j = jval(&p);
    if (j) { ws(&p); if (p.p != p.end) { p.err = "trailing JSON"; json_free(j); j = NULL; } }
    if (!j && err) *err = p.err ? p.err : "invalid JSON";
    return j;
}
void json_free(Json *j) {
    size_t i;
    if (!j) return;
    if (j->type == JSON_ARR || j->type == JSON_OBJ) {
        for (i = 0; i < j->n; i++) { json_free(j->v[i]); if (j->k) free(j->k[i]); }
    }
    free(j->v); free(j->k); free(j->str); free(j);
}
Json *json_get(const Json *o, const char *key) {
    size_t i; if (!o || o->type != JSON_OBJ) return NULL;
    for (i = 0; i < o->n; i++) if (!strcmp(o->k[i], key)) return o->v[i];
    return NULL;
}
const char *json_str(const Json *o, const char *key) { Json *v = json_get(o, key); return v && v->type == JSON_STR ? v->str : NULL; }
long json_int(const Json *o, const char *key, long d) { Json *v = json_get(o, key); return v && v->type == JSON_NUM ? (long)v->num : d; }
int json_bool(const Json *o, const char *key, int d) { Json *v = json_get(o, key); return v && v->type == JSON_BOOL ? v->b : d; }
static void wstr(Buf *b, const char *s, size_t n) {
    size_t i; buf_putc(b, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { buf_putc(b, '\\'); buf_putc(b, c); }
        else if (c == '\n') buf_puts(b, "\\n"); else if (c == '\r') buf_puts(b, "\\r"); else if (c == '\t') buf_puts(b, "\\t");
        else if (c < 0x20 || c == 0x7F) buf_printf(b, "\\u%04x", c);
        else if (c < 0x80) buf_putc(b, c);
        else { put8(b, c); }
    }
    buf_putc(b, '"');
}
void json_put_str(Buf *b, const char *s, size_t n) { wstr(b, s, n); }
void json_put_cstr(Buf *b, const char *s) { wstr(b, s, s ? strlen(s) : 0); }
