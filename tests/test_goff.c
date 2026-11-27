#include <stdlib.h>

#include "../src/ebcdic.h"
#include "../src/goff.h"
#include "testlib.h"

static uint8_t *E(const char *s, size_t *n) { return ebcdic_from_source(s, strlen(s), n); }

static GoffESD mk(GoffESDType t, uint32_t parent, const char *name) {
    GoffESD e;
    memset(&e, 0, sizeof e);
    e.type = t;
    e.parent = parent;
    e.name_space = GOFF_NS_NORMAL;
    e.name = E(name, &e.name_len);
    return e;
}

static const char *LONGNAME =
    "A_VERY_LONG_SYMBOL_NAME_THAT_NEEDS_CONTINUATION_RECORDS_0123456789_0123456789_0123456789";

static void build(const uint8_t **out, size_t *len, uint32_t *ed_id, uint32_t *er_id) {
    GoffWriter *w = goff_writer_new(1);
    GoffESD sd = mk(GOFF_SD, 0, "MYCSECT"), ed, ld, er;
    uint8_t data[200];
    GoffReloc r[3];
    GoffDeferredLen dl;
    GoffEnd end;
    size_t i;
    CHECK(!goff_write_esd(w, &sd));
    ed = mk(GOFF_ED, sd.id, "B_TEXT");
    ed.length = 0x40;
    ed.attr.rmode = GOFF_RMODE_64;
    ed.attr.alignment = GOFF_ALIGN_DBL;
    CHECK(!goff_write_esd(w, &ed));
    ld = mk(GOFF_LD, ed.id, LONGNAME);
    ld.attr.amode = GOFF_AMODE_64;
    ld.attr.scope = GOFF_SCOPE_MODULE;
    CHECK(!goff_write_esd(w, &ld));
    er = mk(GOFF_ER, sd.id, "EXTFUNC");
    CHECK(!goff_write_esd(w, &er));
    for (i = 0; i < sizeof data; i++) data[i] = (uint8_t)i;
    CHECK(!goff_write_text(w, ed.id, 0, GOFF_STYLE_BYTE, data, sizeof data));
    memset(r, 0, sizeof r);
    r[0].rid = er.id; r[0].pid = ed.id; r[0].offset = 8; r[0].store = 1; r[0].length = 4;
    r[1].rid = er.id; r[1].pid = ed.id; r[1].offset = 16; r[1].store = 1; r[1].length = 8;
    r[2].rid = ld.id; r[2].pid = ed.id; r[2].offset = 24; r[2].store = 1; r[2].length = 8; r[2].amode_sens = 1;
    CHECK(!goff_write_rld(w, r, 3));
    dl.esdid = ed.id;
    dl.length = 200;
    CHECK(!goff_write_len(w, &dl, 1));
    memset(&end, 0, sizeof end);
    end.kind = GOFF_ENTRY_BYNAME;
    end.amode = GOFF_AMODE_64;
    end.name = E("MYCSECT", &end.name_len);
    CHECK(!goff_write_end(w, &end, out, len));
    *ed_id = ed.id;
    *er_id = er.id;
}

static void test_behavior(void) {
    GoffBehavior b, u;
    uint8_t p[10], want[10] = {0x04, 0x04, 0x00, 0x6A, 0x01, 0x82, 0x23, 0, 0, 0};
    memset(&b, 0, sizeof b);
    b.amode = GOFF_AMODE_64;
    b.rmode = GOFF_RMODE_64;
    b.tasking = GOFF_TASK_RENT;
    b.read_only = 1;
    b.executable = GOFF_EXEC_CODE;
    b.bind_strength = GOFF_STRENGTH_WEAK;
    b.loading = GOFF_NOLOAD;
    b.scope = GOFF_SCOPE_MODULE;
    b.linkage = GOFF_LINKAGE_XPLINK;
    b.alignment = GOFF_ALIGN_DBL;
    goff_behavior_pack(&b, p);
    CHECK(t_hexeq(p, 10, want, 10));
    goff_behavior_unpack(p, &u);
    CHECK(!memcmp(&u, &b, sizeof b));
}

static void test_roundtrip(void) {
    const uint8_t *out;
    size_t len, i;
    uint32_t ed, er;
    const char *err = NULL;
    GoffModule *m;
    char *nm;
    build(&out, &len, &ed, &er);
    CHECK(len % GOFF_RECORD_LEN == 0);
    CHECK(out[0] == 0x03 && out[1] == 0xF0 && out[2] == 0x00);
    m = goff_parse(out, len, &err);
    CHECK_MSG(m != NULL, "parse: %s", err ? err : "?");
    if (!m) return;
    CHECK(m->esds.n == 4);
    nm = ebcdic_decode(m->esds.v[2]->name, m->esds.v[2]->name_len);
    CHECK(strlen(nm) == 88 && !strcmp(nm, LONGNAME));
    CHECK(m->esds.v[1]->length == 0x40 && m->esds.v[1]->parent == 1 && m->esds.v[1]->type == GOFF_ED);
    CHECK(m->texts.n == 1 && m->texts.v[0].len == 200 && m->texts.v[0].esdid == ed);
    for (i = 0; i < 200; i++) CHECK(m->texts.v[0].data[i] == (uint8_t)i);
    CHECK(m->relocs.n == 3);
    CHECK(m->relocs.v[1].rid == er && m->relocs.v[1].pid == ed && m->relocs.v[1].offset == 16 &&
          m->relocs.v[1].length == 8 && m->relocs.v[1].store);
    CHECK(m->relocs.v[2].amode_sens && m->relocs.v[2].rid == 3);
    CHECK(m->lens.n == 1 && m->lens.v[0].length == 200);
    nm = ebcdic_decode(m->end.name, m->end.name_len);
    CHECK(m->end.kind == GOFF_ENTRY_BYNAME && !strcmp(nm, "MYCSECT"));
    CHECK((int)m->end_count == m->logical);
}

static void test_large_text(void) {
    GoffWriter *w = goff_writer_new(1);
    GoffESD sd = mk(GOFF_SD, 0, "S"), ed;
    size_t n = GOFF_MAX_TEXT * 2 + 17, len;
    uint8_t *big = xmalloc(n);
    const uint8_t *out;
    GoffEnd end;
    const char *err;
    GoffModule *m;
    memset(big, 0xAA, n);
    goff_write_esd(w, &sd);
    ed = mk(GOFF_ED, sd.id, "B_TEXT");
    goff_write_esd(w, &ed);
    goff_write_text(w, ed.id, 0, GOFF_STYLE_BYTE, big, n);
    memset(&end, 0, sizeof end);
    goff_write_end(w, &end, &out, &len);
    m = goff_parse(out, len, &err);
    CHECK(m != NULL);
    if (m)
        CHECK(m->texts.n == 3 && m->texts.v[2].offset == 2 * GOFF_MAX_TEXT && m->texts.v[2].len == 17);
}

static char *dump_all(const GoffModule *m, size_t *len) {
    FILE *f = tmpfile();
    char *b;
    long n;
    if (!f) return NULL;
    goff_dump(m, f);
    fflush(f);
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    b = xmalloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    b[n] = 0;
    fclose(f);
    *len = (size_t)n;
    return b;
}

static void test_hdr_idrl(void) {
    GoffWriter *w = goff_writer_new(1);
    GoffESD sd = mk(GOFF_SD, 0, "S"), ed;
    GoffEnd end;
    const uint8_t *out;
    size_t len;
    const char *err = NULL;
    GoffModule *m;
    static const uint8_t idr[] = {
        0x00, 0x03, 0x00, 0x1E,
        0xD3, 0xD3, 0xE5, 0xD4, 0x40, 0x40, 0xF1, 0xF4, 0xF0, 0xF0,
        0xF1, 0xF4, 0xF0, 0xF0, 0xF2, 0xF0, 0xF2, 0xF5, 0xF0, 0xF9,
        0xF0, 0xF0, 0xF3, 0xF2, 0xF1, 0xF1, 0xF1, 0xF0, 0xF0, 0xF0
    };
    goff_write_esd(w, &sd);
    ed = mk(GOFF_ED, sd.id, "B_IDRL");
    goff_write_esd(w, &ed);
    goff_write_text(w, ed.id, 0, GOFF_STYLE_STRUCTURED, idr, sizeof idr);
    memset(&end, 0, sizeof end);
    goff_write_end(w, &end, &out, &len);
    /* the writer emits a minimal HDR; add the SYSTEM_LE module property */
    ((uint8_t *)out)[53] = 4;
    ((uint8_t *)out)[62] = 0x80;
    m = goff_parse(out, len, &err);
    CHECK_MSG(m != NULL, "parse: %s", err ? err : "?");
    if (m) {
        char *d;
        size_t dn;
        CHECK(m->props_len == 4 && m->props[0] == 0 && m->props[1] == 0 &&
              m->props[2] == 0x80 && m->props[3] == 0);
        CHECK(m->idrs.n == 1);
        if (m->idrs.n == 1) {
            const GoffIdr *x = &m->idrs.v[0];
            CHECK(x->format == 3 && x->length == 30);
            CHECK_STR(x->translator, "LLVM  1400");
            CHECK_STR(x->version, "14");
            CHECK_STR(x->release, "00");
            CHECK_STR(x->date, "2025090");
            CHECK_STR(x->time, "032111000");
        }
        d = dump_all(m, &dn);
        CHECK(d != NULL);
        if (d) {
            CHECK(strstr(d, "props=00008000 property=SYSTEM_LE") != NULL);
            CHECK(strstr(d, "IDRL esd=2 format=3") != NULL);
            CHECK(strstr(d, "translator=\"LLVM  1400\"") != NULL);
            free(d);
        }
        goff_free(m);
    }
    goff_writer_free(w);
}

static void test_errors(void) {
    uint8_t zero[79] = {0};
    const uint8_t *out;
    size_t len;
    uint32_t a, b;
    const char *err;
    uint8_t *bad;
    CHECK(goff_parse(zero, sizeof zero, &err) == NULL);
    build(&out, &len, &a, &b);
    CHECK(goff_parse(out, len - GOFF_RECORD_LEN, &err) == NULL); /* no END */
    bad = xmalloc(len);
    memcpy(bad, out, len);
    bad[0] = 0x02;
    CHECK(goff_parse(bad, len, &err) == NULL);
}

static void test_rld_sequencing(void) {
    /* items sharing R and P: the second one is 8 bytes shorter */
    GoffWriter *w1 = goff_writer_new(1), *w2 = goff_writer_new(1);
    GoffReloc a, b;
    size_t l1, l2;
    memset(&a, 0, sizeof a);
    a.rid = 5; a.pid = 2; a.offset = 0; a.length = 4; a.store = 1;
    b = a;
    b.offset = 4;
    goff_write_rld(w1, &a, 1);
    {
        GoffReloc two[2];
        two[0] = a;
        two[1] = b;
        goff_write_rld(w2, two, 2);
    }
    l1 = w1->buf.len;
    l2 = w2->buf.len;
    CHECK(l1 == l2); /* both fit one record: header(6)+20 vs 20+12 data bytes */
    /* data length field of the logical RLD record (bytes 4..5) */
    CHECK(((unsigned char)w1->buf.p[GOFF_RECORD_LEN + 5]) == 20);
    CHECK(((unsigned char)w2->buf.p[GOFF_RECORD_LEN + 5]) == 32);
    CHECK(((unsigned char)w2->buf.p[GOFF_RECORD_LEN + 6 + 20] & 0xC0) == 0xC0);
}

int main(void) {
    test_behavior();
    test_roundtrip();
    test_large_text();
    test_hdr_idrl();
    test_errors();
    test_rld_sequencing();
    TEST_MAIN_END();
}
