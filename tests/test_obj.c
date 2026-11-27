#include <stdlib.h>
#include <string.h>

#include "../src/ebcdic.h"
#include "../src/goff.h"
#include "../src/obj.h"
#include "testlib.h"

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put24(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }
static void fill(uint8_t *r) { memset(r, 0x40, 80); }
static void rec(uint8_t *r, const char *type) {
    fill(r);
    r[0] = 0x02;
    r[1] = (uint8_t)type[0]; r[2] = (uint8_t)type[1]; r[3] = (uint8_t)type[2];
}
static void name8(uint8_t *p, const char *s) {
    size_t n = 0;
    uint8_t *e = ebcdic_from_source(s, strlen(s), &n);
    memset(p, 0x40, 8);
    memcpy(p, e, n < 8 ? n : 8);
    free(e);
}

static void build_obj(uint8_t **out, size_t *len, int with_sym) {
    uint8_t *b = xcalloc(80 * 5);
    uint8_t *r;
    size_t n = 0;
    if (with_sym) {
        r = b + (n++) * 80;
        rec(r, "\xE2\xE8\xD4"); /* SYM */
        put16(r + 10, 8);
        memset(r + 16, 0x11, 8);
    }
    r = b + (n++) * 80;
    rec(r, "\xC5\xE2\xC4"); /* ESD */
    put16(r + 10, 16);
    put16(r + 14, 1);
    name8(r + 16, "SIMPLE");
    r[16 + 8] = 0; /* SD */
    put24(r + 17, 0);
    r[20] = 0;
    put24(r + 21, 4);
    r = b + (n++) * 80;
    rec(r, "\xE3\xE7\xE3"); /* TXT */
    put24(r + 5, 0);
    put16(r + 10, 4);
    put16(r + 14, 1);
    r[16] = 0x47; r[17] = 0xF0; r[18] = 0x00; r[19] = 0x04;
    r = b + (n++) * 80;
    rec(r, "\xC5\xD5\xC4"); /* END */
    put16(r + 14, 1);
    put24(r + 5, 0);
    *out = b;
    *len = n * 80;
}

static void test_parse_convert(void) {
    uint8_t *b;
    size_t n;
    ObjFile f;
    Buf out = {0};
    char err[256];
    const char *e;
    GoffModule *g;
    const char *perr = NULL;
    build_obj(&b, &n, 0);
    e = obj_parse(b, n, &f);
    CHECK_MSG(!e, "parse: %s", e ? e : "");
    if (!e) {
        CHECK(f.modules.n == 1 && f.modules.v[0]->esds.n == 1 && f.modules.v[0]->texts.n == 1);
        e = obj_to_goff(&f, &out, 0, 0, err, sizeof err);
        CHECK_MSG(!e, "convert: %s", e ? e : "");
        if (!e) {
            g = goff_parse((const uint8_t *)out.p, out.len, &perr);
            CHECK_MSG(g != NULL, "GOFF parse: %s", perr ? perr : "");
            if (g) {
                CHECK(g->esds.n >= 3);
                CHECK(g->texts.n == 1 && g->texts.v[0].len == 4);
                CHECK(g->end.kind == GOFF_ENTRY_BYID && g->end.esdid == 2);
                goff_free(g);
            }
        }
        obj_free(&f);
    }
    free(out.p);
    free(b);
}

static void test_xsd(void) {
    uint8_t *b = xcalloc(80 * 3), *r;
    ObjFile f;
    Buf out = {0};
    char err[256];
    const char *e, *perr = NULL;
    GoffModule *g;
    r = b;
    rec(r, "\xE7\xE2\xC4"); /* XSD */
    put16(r + 10, 0x18);
    put16(r + 14, 1);
    put16(r + 18, 6);
    r[24] = 0; /* SD */
    r[28] = 0;
    put24(r + 29, 4);
    name8(r + 32, "SIMPLE");
    r = b + 80;
    rec(r, "\xE3\xE7\xE3"); /* TXT */
    put16(r + 14, 1);
    put16(r + 10, 4);
    r[16] = 0x47; r[17] = 0xF0; r[18] = 0; r[19] = 4;
    r = b + 160;
    rec(r, "\xC5\xD5\xC4"); /* END */
    e = obj_parse(b, 240, &f);
    CHECK_MSG(!e, "XSD parse: %s", e ? e : "");
    if (!e) {
        e = obj_to_goff(&f, &out, 0, 0, err, sizeof err);
        CHECK_MSG(!e, "XSD convert: %s", e ? e : "");
        if (!e) {
            g = goff_parse((const uint8_t *)out.p, out.len, &perr);
            CHECK_MSG(g != NULL, "XSD GOFF parse: %s", perr ? perr : "");
            if (g) {
                char *nm = ebcdic_decode(g->esds.v[0]->name, g->esds.v[0]->name_len);
                CHECK_STR(nm, "SIMPLE");
                free(nm);
                goff_free(g);
            }
        }
        obj_free(&f);
    }
    free(out.p);
    free(b);
}

static void test_sym_preserved(void) {
    uint8_t *b;
    size_t n;
    ObjFile f;
    Buf out = {0};
    char err[256];
    const char *e;
    GoffModule *g;
    const char *perr = NULL;
    build_obj(&b, &n, 1);
    e = obj_parse(b, n, &f);
    CHECK_MSG(!e, "parse: %s", e ? e : "");
    if (!e) {
        e = obj_to_goff(&f, &out, 1, 0, err, sizeof err);
        CHECK_MSG(!e, "convert: %s", e ? e : "");
        if (!e) {
            g = goff_parse((const uint8_t *)out.p, out.len, &perr);
            CHECK_MSG(g != NULL, "GOFF parse: %s", perr ? perr : "");
            if (g) {
                size_t i;
                int found = 0;
                for (i = 0; i < g->esds.n; i++) {
                    char *nm = ebcdic_decode(g->esds.v[i]->name, g->esds.v[i]->name_len);
                    if (!strcmp(nm, "C_OBJSYM")) found = 1;
                    free(nm);
                }
                CHECK(found);
                goff_free(g);
            }
        }
        obj_free(&f);
    }
    free(out.p);
    free(b);
}

int main(void) {
    test_parse_convert();
    test_xsd();
    test_sym_preserved();
    TEST_MAIN_END();
}
