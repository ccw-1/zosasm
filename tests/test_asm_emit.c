/* test_asm_emit.c - GOFF structure of emitted objects, literals, samples. */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "../src/ebcdic.h"
#include "../src/goff.h"
#include "testlib.h"

static Module *assemble(const char *src, Reporter **repp) {
    Reporter *rep = rep_new(0);
    Module *m = asm_assemble_text("t.za", src, strlen(src), rep);
    if (repp) *repp = rep;
    return m;
}

/* assemble, emit and parse; NULL (after printing) on failure */
static GoffModule *emit_src(const char *src) {
    Reporter *rep;
    Module *m = assemble(src, &rep);
    char err[256];
    size_t n;
    uint8_t *obj;
    const char *perr = NULL;
    GoffModule *g;
    if (!m) {
        rep_print(rep, stdout, 0);
        CHECK_MSG(0, "assembly failed");
        return NULL;
    }
    obj = module_emit(m, &n, err, sizeof err);
    CHECK_MSG(obj != NULL, "emit: %s", err);
    if (!obj) return NULL;
    g = goff_parse(obj, n, &perr);
    CHECK_MSG(g != NULL, "parse: %s", perr ? perr : "?");
    return g;
}

static const GoffESD *find_esd(const GoffModule *g, GoffESDType type, const char *name) {
    size_t i;
    for (i = 0; i < g->esds.n; i++) {
        const GoffESD *e = g->esds.v[i];
        char *nm;
        int ok;
        if (e->type != type) continue;
        nm = ebcdic_decode(e->name, e->name_len);
        ok = !strcmp(nm, name);
        free(nm);
        if (ok) return e;
    }
    return NULL;
}

/* all text bytes in record order */
static uint8_t *all_text(const GoffModule *g, size_t *n) {
    size_t i, total = 0;
    uint8_t *out;
    for (i = 0; i < g->texts.n; i++) total += g->texts.v[i].len;
    out = xmalloc(total + 1);
    *n = 0;
    for (i = 0; i < g->texts.n; i++) {
        memcpy(out + *n, g->texts.v[i].data, g->texts.v[i].len);
        *n += g->texts.v[i].len;
    }
    return out;
}

static void test_structure(void) {
    GoffModule *g = emit_src("PROG     CSECT\n"
                             "PROG     AMODE 31\n"
                             "PROG     RMODE ANY\n"
                             "         ENTRY ALT\n"
                             "START    BR    14\n"
                             "ALT      BR    14\n"
                             "TBL      DC    A(START),V(EXTF),A(EXTW)\n"
                             "         WXTRN EXTW\n"
                             "D_X      CATTR ALIGN(3),NOLOAD,PART(XP)\n"
                             "XP       XATTR LINKAGE(OS),SCOPE(SECTION)\n"
                             "         DC    X'0102'\n"
                             "         END   START\n");
    const GoffESD *sd, *ed, *ld, *er, *w, *dx, *pr;
    int got_ed = 0, got_pr = 0;
    size_t i;
    if (!g) return;
    sd = find_esd(g, GOFF_SD, "PROG");
    CHECK(sd != NULL);
    ed = find_esd(g, GOFF_ED, "B_TEXT");
    CHECK(ed && sd && ed->parent == sd->id && ed->attr.amode == GOFF_AMODE_31 &&
          ed->attr.rmode == GOFF_RMODE_31);
    if (!ed || !sd) return;
    ld = find_esd(g, GOFF_LD, "PROG");
    CHECK(ld && ld->parent == ed->id && ld->offset == 0);
    ld = find_esd(g, GOFF_LD, "ALT");
    CHECK(ld && ld->offset == 2);
    CHECK(find_esd(g, GOFF_LD, "START") == NULL); /* not an ENTRY */
    er = find_esd(g, GOFF_ER, "EXTF");
    CHECK(er && er->attr.bind_strength == GOFF_STRENGTH_STRONG);
    w = find_esd(g, GOFF_ER, "EXTW");
    CHECK(w && w->attr.bind_strength == GOFF_STRENGTH_WEAK);
    dx = find_esd(g, GOFF_ED, "D_X");
    pr = find_esd(g, GOFF_PR, "XP");
    CHECK(dx && pr && pr->parent == dx->id && pr->length == 2 && dx->attr.loading == GOFF_NOLOAD);
    CHECK(pr && pr->attr.scope == GOFF_SCOPE_SECTION);
    /* text goes to the ED, or to the PR for a class with a part */
    for (i = 0; i < g->texts.n; i++) {
        const GoffText *x = &g->texts.v[i];
        if (x->esdid == ed->id && x->offset == 0) got_ed = 1;
        if (pr && x->esdid == pr->id && x->len == 2 && x->data[0] == 1 && x->data[1] == 2)
            got_pr = 1;
    }
    CHECK(got_ed && got_pr);
    /* three relocations at 4, 8, 12 */
    CHECK(g->relocs.n == 3);
    if (g->relocs.n == 3 && er && w) {
        CHECK(g->relocs.v[0].offset == 4 && g->relocs.v[1].offset == 8 &&
              g->relocs.v[2].offset == 12);
        CHECK(g->relocs.v[0].referent == GOFF_REFERENT_CLASS && g->relocs.v[0].rid == ed->id);
        CHECK(g->relocs.v[1].rid == er->id && g->relocs.v[2].rid == w->id);
    }
    CHECK(g->end.kind == GOFF_ENTRY_BYID && g->end.esdid == ed->id && g->end.offset == 0);
}

static void test_dsect_not_emitted(void) {
    GoffModule *g = emit_src("MAIN     CSECT\n"
                             "         USING WORK,5\n"
                             "         L     1,FLD2\n"
                             "         BR    14\n"
                             "WORK     DSECT\n"
                             "FLD1     DS    F\n"
                             "FLD2     DS    F\n");
    size_t n;
    uint8_t *t;
    static const uint8_t want[6] = {0x58, 0x10, 0x50, 0x04, 0x07, 0xFE};
    if (!g) return;
    CHECK(find_esd(g, GOFF_SD, "WORK") == NULL);
    t = all_text(g, &n);
    CHECK(n >= 6 && memcmp(t, want, 6) == 0);
}

static void test_entry_external(void) {
    GoffModule *g = emit_src("M CSECT\n BR 14\n EXTRN FOO\n END FOO\n");
    char *nm;
    if (!g) return;
    CHECK(g->end.kind == GOFF_ENTRY_BYNAME);
    nm = ebcdic_decode(g->end.name, g->end.name_len);
    CHECK_STR(nm, "FOO");
}

static void test_quoted_names(void) {
    static const unsigned char dotted[] = {0x81, 0x4B, 0x82}; /* a.b */
    GoffModule *g = emit_src("\"a.b\" CSECT\n"
                             " ENTRY \"e\\x2D1\"\n"
                             " EXTRN \"ext sym\",\"w\\xE9\"\n"
                             "\"e\\x2D1\" DS 0H\n"
                             " DC A(\"ext sym\",\"w\\xE9\")\n"
                             " END \"ext sym\"\n");
    char *nm;
    const GoffESD *e;
    size_t i;
    if (!g) return;
    e = find_esd(g, GOFF_SD, "a.b");
    CHECK(e != NULL);
    if (e) CHECK(e->name_len == sizeof dotted && !memcmp(e->name, dotted, sizeof dotted));
    CHECK(find_esd(g, GOFF_LD, "e-1") != NULL);
    CHECK(find_esd(g, GOFF_ER, "ext sym") != NULL);
    CHECK(find_esd(g, GOFF_ER, "w\xE9") != NULL);
    CHECK(g->end.kind == GOFF_ENTRY_BYNAME);
    nm = ebcdic_decode(g->end.name, g->end.name_len);
    CHECK_STR(nm, "ext sym");
    for (i = 0; i < g->esds.n; i++) CHECK(g->esds.v[i]->name_len > 0);
}

static void test_samples(void) {
    static const char *const files[] = {"testdata/hello.za", "../hlasm-dwarf/dwemb.asm"};
    size_t i;
    for (i = 0; i < 2; i++) {
        size_t n, on;
        char *text = read_file(files[i], &n);
        Reporter *rep;
        Module *m;
        char err[256];
        uint8_t *obj;
        const char *perr;
        if (!text) {
            printf("skipping %s\n", files[i]);
            continue;
        }
        rep = rep_new(0);
        m = asm_assemble_text(files[i], text, n, rep);
        if (!m) {
            rep_print(rep, stdout, 0);
            CHECK_MSG(0, "%s: assembly failed", files[i]);
            continue;
        }
        obj = module_emit(m, &on, err, sizeof err);
        CHECK_MSG(obj != NULL, "%s: %s", files[i], err);
        if (obj) CHECK_MSG(goff_parse(obj, on, &perr) != NULL, "%s: does not parse", files[i]);
    }
}

/* ---- literals ---- */

static uint8_t *code_bytes(const char *src, size_t *n) {
    GoffModule *g = emit_src(src);
    if (!g) {
        *n = 0;
        return xcalloc(1);
    }
    return all_text(g, n);
}

static void test_literal_ltorg(void) {
    size_t n;
    uint8_t *t = code_bytes("M        CSECT\n"
                            "         BASR  12,0\n"
                            "         USING *,12\n"
                            "         L     1,=F'1'\n"
                            "         L     2,=F'1'\n"
                            "         LH    3,=H'7'\n"
                            "         LG    5,=FD'9'\n"
                            "         BR    14\n"
                            "         LTORG\n",
                            &n);
    /* code is 2+4+4+4+6+2 = 22 bytes, the pool starts at 24 (doubleword) */
    static const uint8_t want[14] = {0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 1, 0, 7};
    CHECK(n >= 24 + sizeof want && memcmp(t + 24, want, sizeof want) == 0);
    /* both loads share the constant at 24+8 = 32, base 12 = offset 2 */
    CHECK(n > 9 && t[2] == 0x58 && t[3] == 0x10 && t[4] == 0xC0 && t[5] == 30);
    CHECK(n > 9 && t[9] == 30);
}

static void test_literal_pool_at_end(void) {
    size_t n;
    uint8_t *t = code_bytes("M        CSECT\n"
                            "         BASR  12,0\n"
                            "         USING *,12\n"
                            "         MVC   B(3),=C'ABC'\n"
                            "         BR    14\n"
                            "B        DS    CL3\n"
                            "         END\n",
                            &n);
    CHECK(n >= 3 && t[n - 3] == 0xC1 && t[n - 2] == 0xC2 && t[n - 1] == 0xC3);
}

static int has_code(const char *src, const char *want) {
    Reporter *rep;
    size_t i;
    assemble(src, &rep);
    for (i = 0; i < rep_count(rep); i++)
        if (!strcmp(rep_get(rep, i)->code, want)) return 1;
    return 0;
}

static void test_literal_errors(void) {
    CHECK(has_code("M CSECT\n BASR 12,0\n USING *,12\n L 1,=S(1)\n END\n", "ZA9002"));
    CHECK(has_code("M CSECT\n BASR 12,0\n USING *,12\n L 1,=\n END\n", "ZA2051"));
    CHECK(has_code("D DSECT\n L 1,=F'1'\n", "ZA2052"));
    CHECK(has_code("M CSECT\n BASR 12,0\n USING *,12\n L 1,=F'1'\nD DSECT\n LTORG\n", "ZA3043"));
    CHECK(has_code("M CSECT\n BASR 12,0\n USING *,12\nD DSECT\n L 1,=F'1'\n LTORG\n", "ZA2052"));
}

/* A(X-Y) with X and Y in different places: one RLD item per term */
static void test_sub_relocs(void) {
    GoffModule *g = emit_src("A CSECT\n"
                             " DC A(A1-*)\n"
                             " DC A(EXTA-*)\n"
                             " DC A(EXTA-EXTB+4)\n"
                             " DC A(B1-A1)\n"
                             " DC AL3(-B1)\n"
                             " EXTRN EXTA,EXTB\n"
                             "A1 DC X'01'\n"
                             "B CSECT\n"
                             "B1 DC X'02'\n"
                             " END\n");
    size_t n;
    uint8_t *t;
    if (!g) return;
    CHECK(g->relocs.n == 7);
    if (g->relocs.n != 7) return;
    /* the first constant is absolute; then +EXTA -A, +EXTA -EXTB, +B -A, -B */
    CHECK(g->relocs.v[0].offset == 4 && g->relocs.v[0].action == GOFF_ACTION_ADD);
    CHECK(g->relocs.v[1].offset == 4 && g->relocs.v[1].action == GOFF_ACTION_SUB &&
          g->relocs.v[1].referent == GOFF_REFERENT_CLASS);
    CHECK(g->relocs.v[2].offset == 8 && g->relocs.v[2].action == GOFF_ACTION_ADD);
    CHECK(g->relocs.v[3].offset == 8 && g->relocs.v[3].action == GOFF_ACTION_SUB &&
          g->relocs.v[3].referent == GOFF_REFERENT_LABEL);
    CHECK(g->relocs.v[4].offset == 12 && g->relocs.v[4].action == GOFF_ACTION_ADD);
    CHECK(g->relocs.v[5].offset == 12 && g->relocs.v[5].action == GOFF_ACTION_SUB);
    CHECK(g->relocs.v[6].offset == 16 && g->relocs.v[6].action == GOFF_ACTION_SUB &&
          g->relocs.v[6].length == 3);
    CHECK(g->relocs.v[0].store == 0 && g->relocs.v[1].store == 0);
    t = all_text(g, &n);
    /* A1 is at 19: A(A1-*) = 19, A(EXTA-*) = -4 (the field is at 4), A(EXTA-EXTB+4) = 4,
     * A(B1-A1) = -19, AL3(-B1) = 0 */
    CHECK(n >= 19 && t[3] == 19 && t[4] == 0xFF && t[7] == 0xFC && t[11] == 4 && t[12] == 0xFF &&
          t[15] == 0xED);
}

int main(void) {
    test_sub_relocs();
    test_structure();
    test_dsect_not_emitted();
    test_entry_external();
    test_quoted_names();
    test_samples();
    test_literal_ltorg();
    test_literal_pool_at_end();
    test_literal_errors();
    TEST_MAIN_END();
}
