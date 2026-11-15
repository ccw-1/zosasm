/* test_zsym.c - ZSYM, the symbolic information in the object (ZSYM.md). */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "../src/disasm.h"
#include "../src/ebcdic.h"
#include "../src/goff.h"
#include "../src/zsym.h"
#include "testlib.h"

typedef struct {
    Module *m;
    uint8_t *obj;
    size_t objlen;
    GoffModule *g;
    uint8_t *z; /* the ZSYM bytes */
    size_t zlen;
    Zsym *zs;
} Built;

static Built build(const char *src, int zlevel) {
    Built b;
    Reporter *rep = rep_new(0);
    char err[256];
    const char *perr = NULL;
    memset(&b, 0, sizeof b);
    b.m = asm_assemble_text("t.za", src, strlen(src), rep);
    if (!b.m) {
        rep_print(rep, stdout, 0);
        CHECK_MSG(0, "assembly failed");
        return b;
    }
    b.m->zlevel = zlevel;
    b.obj = module_emit(b.m, &b.objlen, err, sizeof err);
    CHECK_MSG(b.obj != NULL, "emit: %s", err);
    if (!b.obj) return b;
    b.g = goff_parse(b.obj, b.objlen, &perr);
    CHECK_MSG(b.g != NULL, "parse: %s", perr ? perr : "?");
    if (!b.g) return b;
    b.z = zsym_extract(b.g, &b.zlen);
    if (b.z) b.zs = zsym_parse(b.z, b.zlen);
    return b;
}

static uint32_t esd_id(const GoffModule *g, const char *name) {
    size_t i;
    for (i = 0; i < g->esds.n; i++) {
        char *nm = ebcdic_decode(g->esds.v[i]->name, g->esds.v[i]->name_len);
        int ok = !strcmp(nm, name);
        free(nm);
        if (ok) return (uint32_t)i + 1;
    }
    return 0;
}

static long sym_index(const ZBlock *b, const char *name) {
    size_t i;
    for (i = 0; i < b->syms.n; i++)
        if (!strcmp(b->syms.v[i].name, name)) return (long)i;
    return -1;
}

static const char *SRC = "X        EQU   5\n"
                         "T        CSECT\n"
                         "         USING T,12\n"
                         "         LR    12,15\n"
                         "         MVC   B(L'A),A\n"
                         "         LA    1,\"q.s\"\n"
                         "A        DC    CL4'ABCD'\n"
                         "B        DC    CL4'WXYZ'\n"
                         "\"q.s\"    DS    0F\n"
                         "D        DSECT\n"
                         "F1       DS    F\n"
                         "F2       DS    CL8\n"
                         "         END\n";

static void test_block(void) {
    Built b = build(SRC, 1);
    const ZBlock *z;
    size_t i;
    CHECK(b.zs != NULL);
    if (!b.zs) return;
    CHECK(b.zs->blocks.n == 1);
    CHECK(b.zs->bad_chunks == 0);
    if (b.zs->blocks.n != 1) return;
    z = b.zs->blocks.v[0];
    CHECK(z->complete);
    CHECK(z->level == 1);
    CHECK(z->files.n == 1);
    CHECK(z->files.n == 1 && !strcmp(z->files.v[0], "t.za"));
    CHECK(z->scopes.n == 2);
    CHECK(z->scopes.n == 2 && z->scopes.v[0].kind == K_CSECT && !strcmp(z->scopes.v[0].sect, "T") &&
          !strcmp(z->scopes.v[0].cls, "B_TEXT"));
    CHECK(z->scopes.n == 2 && z->scopes.v[1].kind == K_DSECT && !strcmp(z->scopes.v[1].sect, "D"));
    CHECK(z->stmts.n == 13);
    for (i = 0; i < z->stmts.n && i < 13; i++) CHECK(z->stmts.v[i].line == i + 1);

    /* symbols */
    {
        long a = sym_index(z, "A"), q = sym_index(z, "q.s"), x = sym_index(z, "X"), f2 = sym_index(z, "F2");
        CHECK(a >= 0 && z->syms.v[a].kind == ZY_LABEL && z->syms.v[a].scope == 0 && z->syms.v[a].val == 12 &&
              z->syms.v[a].len == 4 && z->syms.v[a].type == 'C');
        CHECK(q >= 0 && z->syms.v[q].kind == ZY_LABEL && z->syms.v[q].val == 20);
        CHECK(x >= 0 && z->syms.v[x].kind == ZY_ABS && z->syms.v[x].val == 5);
        CHECK(f2 >= 0 && z->syms.v[f2].scope == 1 && z->syms.v[f2].val == 4 && z->syms.v[f2].len == 8);
        CHECK(sym_index(z, "T") >= 0 && z->syms.v[sym_index(z, "T")].kind == ZY_SECT);
    }

    /* statements */
    CHECK(z->stmts.v[0].kind == ZS_EQU && z->stmts.v[0].scope == -1);
    CHECK(z->stmts.v[1].kind == ZS_SECTION && z->stmts.v[1].scope == 0);
    CHECK(z->stmts.v[2].kind == ZS_USING);
    CHECK(z->stmts.v[3].kind == ZS_INSN && z->stmts.v[3].off == 0 && z->stmts.v[3].len == 2);
    CHECK(z->stmts.v[4].kind == ZS_INSN && z->stmts.v[4].off == 2 && z->stmts.v[4].len == 6);
    CHECK(z->stmts.v[6].kind == ZS_DC && z->stmts.v[6].off == 12 && z->stmts.v[6].len == 4);
    CHECK(z->stmts.v[10].kind == ZS_DS && z->stmts.v[10].scope == 1 && z->stmts.v[10].len == 4 &&
          z->stmts.v[10].crc == 0);
    CHECK(z->stmts.v[12].kind == ZS_END);
    {
        char *t = zsym_render(z, &z->stmts.v[4].operands);
        CHECK_STR(t, "B(L'A),A");
        t = zsym_render(z, &z->stmts.v[5].operands);
        CHECK_STR(t, "1,\"q.s\"");
        t = zsym_render(z, &z->stmts.v[8].label);
        CHECK_STR(t, "\"q.s\"");
        t = zsym_render(z, &z->stmts.v[6].operands);
        CHECK_STR(t, "CL4'ABCD'");
        /* the template of MVC: symbol, text, attribute, text, symbol */
        {
            const ZTmpl *o = &z->stmts.v[4].operands;
            CHECK(o->n == 5);
            CHECK(o->n == 5 && o->v[0].tag == 's' && o->v[1].tag == 't' && o->v[2].tag == 'a' &&
                  o->v[2].attr == 'L' && o->v[3].tag == 't' && o->v[4].tag == 's');
            CHECK(o->n == 5 && !strcmp(z->syms.v[o->v[0].id].name, "B") && !strcmp(z->syms.v[o->v[2].id].name, "A"));
        }
        /* a register number or an undefined name stays text */
        CHECK(z->stmts.v[3].operands.n == 1 && z->stmts.v[3].operands.v[0].tag == 't');
    }

    /* the class has the layout of the specification */
    {
        const GoffESD *e = goff_module_esd(b.g, esd_id(b.g, ZSYM_CLASS));
        CHECK(e != NULL);
        if (e) {
            CHECK(e->type == GOFF_ED && e->attr.loading == GOFF_NOLOAD && e->attr.text_style == GOFF_STYLE_UNSTRUCTURED &&
                  e->attr.bind_algorithm == GOFF_BIND_CAT);
            CHECK(e->parent == esd_id(b.g, "T"));
            CHECK(e->length == b.zlen);
            CHECK(esd_id(b.g, ZSYM_CLASS) == b.g->esds.n); /* the last ESD */
        }
    }
}

static void test_crc(void) {
    Built b = build(SRC, 1);
    const ZBlock *z;
    uint32_t cls;
    uint8_t *data;
    size_t i, hits = 0;
    if (!b.zs || b.zs->blocks.n != 1) {
        CHECK(0);
        return;
    }
    z = b.zs->blocks.v[0];
    cls = esd_id(b.g, "B_TEXT");
    CHECK(cls != 0);
    for (i = 0; i < b.g->texts.n; i++)
        if (b.g->texts.v[i].esdid == cls) break;
    CHECK(i < b.g->texts.n);
    if (i >= b.g->texts.n) return;
    data = b.g->texts.v[i].data;
    for (i = 0; i < z->stmts.n; i++) {
        const ZStmt *s = &z->stmts.v[i];
        if (s->scope != 0 || s->len == 0) continue;
        CHECK_MSG(zsym_crc(data, b.g->texts.v[0].len, NULL, s->off, s->len) == s->crc, "crc of statement %zu", i);
    }
    /* a changed byte is found in the statement that holds it */
    data[9] ^= 0x40;
    for (i = 0; i < z->stmts.n; i++) {
        const ZStmt *s = &z->stmts.v[i];
        if (s->scope != 0 || s->len == 0) continue;
        if (zsym_crc(data, b.g->texts.v[0].len, NULL, s->off, s->len) != s->crc) {
            hits++;
            CHECK(s->off <= 9 && 9 < s->off + s->len);
        }
    }
    CHECK(hits == 1);
    CHECK(zsym_crc(data, 4, NULL, 0, 8) == 0); /* out of range */
}

static void test_reloc_mask(void) {
    Built b = build("T CSECT\n"
                    " DC F'1'\n"
                    "P DC A(E)\n"
                    " DC A(T)\n"
                    " EXTRN E\n"
                    " END\n",
                    1);
    const ZBlock *z;
    uint8_t *data, *mask;
    size_t i, len;
    uint32_t cls;
    if (!b.zs || b.zs->blocks.n != 1) {
        CHECK(0);
        return;
    }
    z = b.zs->blocks.v[0];
    cls = esd_id(b.g, "B_TEXT");
    data = b.g->texts.v[0].data;
    len = b.g->texts.v[0].len;
    CHECK(b.g->texts.v[0].esdid == cls);
    mask = xcalloc(len + 1);
    CHECK(b.g->relocs.n == 2);
    for (i = 0; i < b.g->relocs.n; i++)
        memset(mask + b.g->relocs.v[i].offset, 1, b.g->relocs.v[i].length);
    /* the binder fills in the addresses: the checksums, taken with the
     * relocated bytes masked, still agree */
    data[4] = 0x12;
    data[5] = 0x34;
    data[8] = 0x56;
    data[11] = 0x78;
    for (i = 0; i < z->stmts.n; i++) {
        const ZStmt *s = &z->stmts.v[i];
        if (s->scope != 0 || s->len == 0) continue;
        CHECK_MSG(zsym_crc(data, len, mask, s->off, s->len) == s->crc, "masked crc of statement %zu", i);
    }
    data[0] = 0x01; /* but not other bytes */
    CHECK(zsym_crc(data, len, mask, 0, 4) != z->stmts.v[1].crc);
    CHECK(zsym_crc(data, len, NULL, 4, 4) != z->stmts.v[2].crc);
}

static void test_same_object(void) {
    Built z0 = build(SRC, 0), z1 = build(SRC, 1);
    CHECK(z0.g && z1.g);
    if (!z0.g || !z1.g) return;
    CHECK(z0.z == NULL);
    CHECK(esd_id(z0.g, ZSYM_CLASS) == 0);
    CHECK(z1.z != NULL);
    CHECK(z1.g->esds.n == z0.g->esds.n + 1);
    CHECK(goff_equal(z1.g, esd_id(z1.g, ZSYM_CLASS), z0.g));
    CHECK(!goff_equal(z1.g, 0, z0.g));
    CHECK(goff_equal(z0.g, 0, z0.g));
    /* the module library default is no ZSYM */
    {
        Reporter *rep = rep_new(0);
        Module *m = asm_assemble_text("t.za", SRC, strlen(SRC), rep);
        CHECK(m && m->zlevel == 0);
    }
}

static void test_scopes(void) {
    Built b = build("A CSECT\n"
                    " DC F'1'\n"
                    "L1 LOCTR\n"
                    " DC F'2'\n"
                    "A CSECT\n"
                    " DC F'3'\n"
                    "B CSECT\n"
                    "Q CATTR PART(PP)\n"
                    " DC F'4'\n"
                    "S RSECT\n"
                    " DC F'5'\n"
                    " END\n",
                    1);
    const ZBlock *z;
    size_t i;
    if (!b.zs || b.zs->blocks.n != 1) {
        CHECK(0);
        return;
    }
    z = b.zs->blocks.v[0];
    CHECK(z->complete);
    CHECK(z->scopes.n == 4);
    CHECK(z->stmts.n == 12);
    /* every statement range lies in its scope */
    for (i = 0; i < z->stmts.n; i++) CHECK(z->stmts.v[i].scope < (int64_t)z->scopes.n);
    /* the LOCTR does not make a scope of its own, its bytes follow the first */
    CHECK(z->stmts.v[3].scope == 0 && z->stmts.v[3].off == 8 && z->stmts.v[3].len == 4);
    /* re-entering the section resumes the last location counter */
    CHECK(z->stmts.v[5].scope == 0 && z->stmts.v[5].off == 12 && z->stmts.v[5].len == 4);
    CHECK(z->scopes.v[3].kind == K_RSECT);
}

static void test_many(void) {
    Buf s = {0};
    int i;
    Built b;
    buf_puts(&s, "T CSECT\n");
    for (i = 0; i < 1000; i++) buf_printf(&s, "L%d DC F'%d'\n", i, i);
    buf_puts(&s, " END\n");
    b = build(buf_cstr(&s), 1);
    CHECK(b.zs && b.zs->blocks.n == 1);
    if (b.zs && b.zs->blocks.n == 1) {
        const ZBlock *z = b.zs->blocks.v[0];
        CHECK(z->complete);
        CHECK(z->stmts.n == 1002);
        CHECK(z->syms.n >= 1001);
        CHECK(z->stmts.n == 1002 && z->stmts.v[1000].off == 4 * 999);
        CHECK(z->stmts.n == 1002 && z->stmts.v[500].line == 501);
        CHECK(b.zlen > 10000);
    }
    /* a module of this size needs several text records */
    CHECK(b.g && esd_id(b.g, ZSYM_CLASS));
}

static void test_damage(void) {
    Built b = build(SRC, 1);
    Zsym *z;
    uint8_t *c;
    size_t n, i;
    if (!b.z) {
        CHECK(0);
        return;
    }
    n = b.zlen;
    /* empty and garbage */
    z = zsym_parse(b.z, 0);
    CHECK(z->blocks.n == 0 && z->bad_chunks == 0);
    c = xcalloc(100);
    z = zsym_parse(c, 100);
    CHECK(z->blocks.n == 0 && z->bad_chunks == 0); /* zero padding is not damage */
    memset(c, 0x55, 100);
    z = zsym_parse(c, 100);
    CHECK(z->blocks.n == 0 && z->bad_chunks == 1);
    /* truncation */
    z = zsym_parse(b.z, n - 5);
    CHECK(z->blocks.n == 1 && !z->blocks.v[0]->complete && z->bad_chunks >= 1);
    z = zsym_parse(b.z, 20);
    CHECK(z->blocks.n == 0 || !z->blocks.v[0]->complete);
    /* a flipped bit anywhere makes the block incomplete, never a crash */
    c = xmalloc(n);
    for (i = 0; i < n; i++) {
        memcpy(c, b.z, n);
        c[i] ^= 0x10;
        z = zsym_parse(c, n);
        CHECK_MSG(z->blocks.n == 0 || !z->blocks.v[0]->complete, "flip at %zu left the block complete", i);
    }
    /* two modules bound together, with padding between them */
    {
        uint8_t *two = xcalloc(2 * n + 8);
        memcpy(two, b.z, n);
        memcpy(two + n + 5, b.z, n);
        z = zsym_parse(two, 2 * n + 8);
        CHECK(z->blocks.n == 2 && z->bad_chunks == 0);
        CHECK(z->blocks.n == 2 && z->blocks.v[0]->complete && z->blocks.v[1]->complete);
        /* damage in the first leaves the second */
        two[n / 2] ^= 0xFF;
        z = zsym_parse(two, 2 * n + 8);
        CHECK(z->blocks.n == 2 && !z->blocks.v[0]->complete && z->blocks.v[1]->complete);
    }
}

static uint32_t seed = 12345;
static uint32_t rnd(void) {
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

static void test_fuzz(void) {
    Built b = build(SRC, 1);
    FILE *sink = fopen("/dev/null", "w");
    uint8_t *c;
    int it;
    if (!b.z || !sink) {
        CHECK(0);
        return;
    }
    c = xmalloc(b.zlen);
    for (it = 0; it < 4000; it++) {
        size_t n = b.zlen, k, nm = 1 + rnd() % 4;
        Zsym *z;
        size_t bi, i;
        memcpy(c, b.z, n);
        for (k = 0; k < nm; k++) c[rnd() % n] = (uint8_t)rnd();
        if (rnd() % 4 == 0) n = rnd() % n;
        z = zsym_parse(c, n);
        zsym_dump(z, sink);
        for (bi = 0; bi < z->blocks.n; bi++)
            for (i = 0; i < z->blocks.v[bi]->stmts.n; i++) {
                zsym_render(z->blocks.v[bi], &z->blocks.v[bi]->stmts.v[i].operands);
                zsym_render(z->blocks.v[bi], &z->blocks.v[bi]->stmts.v[i].label);
            }
    }
    fclose(sink);
    CHECK(1); /* no crash, no sanitizer report */
}

static void test_quote(void) {
    CHECK_STR(zsym_quote("ABC"), "ABC");
    CHECK_STR(zsym_quote("a.b"), "\"a.b\"");
    CHECK_STR(zsym_quote("a\"b"), "\"a\\\"b\"");
}

/* the text of the class with the given ESD id */
static GoffText *text_of(GoffModule *g, uint32_t id) {
    size_t i;
    for (i = 0; i < g->texts.n; i++)
        if (g->texts.v[i].esdid == id) return &g->texts.v[i];
    return NULL;
}

static GoffModule *reassemble(const char *text, int zl, Module **mod_out) {
    Reporter *rep = rep_new(0);
    Module *m = asm_assemble_text("re.za", text, strlen(text), rep);
    char err[256];
    size_t n;
    uint8_t *o;
    const char *perr = NULL;
    if (!m) {
        printf("--- does not assemble:\n%s\n", text);
        rep_print(rep, stdout, 0);
        return NULL;
    }
    m->zlevel = zl;
    if (mod_out) *mod_out = m;
    o = module_emit(m, &n, err, sizeof err);
    return o ? goff_parse(o, n, &perr) : NULL;
}

static const char *TWO = "S1       CSECT\n"
                         "         USING S1,12\n"
                         "         LR    12,15\n"
                         "         LA    2,FIELD1\n"
                         "         BR    14\n"
                         "FIELD1   DC    F'1'\n"
                         "S2       CSECT\n"
                         "         USING S2,12\n"
                         "         LR    12,15\n"
                         "         LA    2,FIELD2\n"
                         "         BR    14\n"
                         "FIELD2   DC    F'2'\n"
                         "         END\n";

static void test_smart_degrade(void) {
    Built b = build(TWO, 1);
    char err[256], *t;
    int nw = 0, smart = 0;
    uint32_t id;
    GoffText *tx;
    GoffModule *g2;
    if (!b.g) return;
    /* untouched: all of it from ZSYM, without warnings */
    t = disasm_ex(b.g, 0, NULL, &nw, &smart, err, sizeof err);
    CHECK(t && smart && nw == 0);
    CHECK(t && strstr(t, "LA    2,FIELD1") && strstr(t, "LA    2,FIELD2") && strstr(t, "USING S2,12"));
    /* a patched byte in S2 degrades S2 only */
    id = esd_id(b.g, "B_TEXT");
    {
        size_t i;
        uint32_t s2 = 0;
        for (i = 0; i < b.g->esds.n; i++) {
            char *nm = ebcdic_decode(b.g->esds.v[i]->name, b.g->esds.v[i]->name_len);
            if (b.g->esds.v[i]->type == GOFF_ED && !strcmp(nm, "B_TEXT") && (uint32_t)i + 1 > id) s2 = (uint32_t)i + 1;
            free(nm);
        }
        CHECK(s2 > id);
        tx = text_of(b.g, s2);
        CHECK(tx && tx->len >= 12);
        if (!tx || tx->len < 12) return;
        tx->data[8] ^= 0x01; /* FIELD2 */
    }
    nw = 0;
    t = disasm_ex(b.g, 0, NULL, &nw, &smart, err, sizeof err);
    CHECK(t && smart && nw == 1);
    CHECK(t && strstr(t, "LA    2,FIELD1") && strstr(t, "USING S1,12"));
    CHECK(t && !strstr(t, "USING S2,12") && !strstr(t, "LA    2,FIELD2"));
    CHECK(t && strstr(t, "FIELD2")); /* the label survives in the generic text */
    g2 = t ? reassemble(t, 0, NULL) : NULL;
    CHECK(g2 && goff_equal(b.g, esd_id(b.g, ZSYM_CLASS), g2));

    /* a patched byte in S1 degrades S1 only */
    tx->data[8] ^= 0x01;
    tx = text_of(b.g, id);
    tx->data[10] ^= 0x80;
    nw = 0;
    t = disasm_ex(b.g, 0, NULL, &nw, &smart, err, sizeof err);
    CHECK(t && smart && nw == 1 && strstr(t, "LA    2,FIELD2") && !strstr(t, "LA    2,FIELD1"));
    g2 = t ? reassemble(t, 0, NULL) : NULL;
    CHECK(g2 && goff_equal(b.g, esd_id(b.g, ZSYM_CLASS), g2));
}

static void test_smart_pool(void) {
    Built b = build("T CSECT\n USING T,12\n LR 12,15\n L 1,=F'5'\n END\n", 1);
    char err[256], *t;
    int nw = 0, smart = 0;
    GoffText *tx;
    GoffModule *g2;
    if (!b.g) return;
    t = disasm_ex(b.g, 0, NULL, &nw, &smart, err, sizeof err);
    CHECK(t && smart && nw == 0 && strstr(t, "=F'5'"));
    tx = text_of(b.g, esd_id(b.g, "B_TEXT"));
    CHECK(tx && tx->len == 12);
    if (!tx || tx->len != 12) return;
    tx->data[11] = 7; /* the literal */
    t = disasm_ex(b.g, 0, NULL, &nw, &smart, err, sizeof err);
    CHECK(t && smart && nw == 1 && !strstr(t, "=F'5'"));
    g2 = t ? reassemble(t, 0, NULL) : NULL;
    CHECK(g2 && goff_equal(b.g, esd_id(b.g, ZSYM_CLASS), g2));
    /* a byte nobody wrote (the padding behind the code) */
    tx->data[11] = 5;
    tx->data[7] = 1;
    nw = 0;
    t = disasm_ex(b.g, 0, NULL, &nw, &smart, err, sizeof err);
    CHECK(t && smart && nw == 1);
}

static void test_smart_fallback(void) {
    Built b = build(TWO, 1);
    GoffText *z;
    char err[256], *t, *g;
    int nw = 0, smart = 0;
    if (!b.g) return;
    g = disasm_ex(b.g, DISASM_GENERIC, NULL, &nw, &smart, err, sizeof err);
    CHECK(g && !smart && nw == 0);
    z = text_of(b.g, esd_id(b.g, ZSYM_CLASS));
    CHECK(z != NULL);
    if (!z) return;
    z->data[z->len / 2] ^= 0x04;
    t = disasm_ex(b.g, 0, NULL, &nw, &smart, err, sizeof err);
    CHECK(t && !smart && nw >= 1 && g && !strcmp(t, g));
    /* an object without ZSYM: no information, no warning */
    {
        Built n = build(TWO, 0);
        nw = 0;
        t = disasm_ex(n.g, 0, NULL, &nw, &smart, err, sizeof err);
        CHECK(t && !smart && nw == 0 && !strcmp(t, g));
    }
}

/* the source can be edited: symbolic operands make the offsets follow */
static void test_smart_edit(void) {
    Built b = build(SRC, 1);
    char err[256], *t, *p, *edited;
    int nw = 0, smart = 0;
    Module *m;
    GoffModule *g2;
    const ZBlock *z;
    if (!b.g) return;
    t = disasm_ex(b.g, 0, NULL, &nw, &smart, err, sizeof err);
    CHECK(t && smart);
    if (!t) return;
    p = strstr(t, "F1       DS    F\n");
    CHECK(p != NULL);
    if (!p) return;
    edited = xasprintf("%.*sF1       DS    2F\n%s", (int)(p - t), t, p + strlen("F1       DS    F\n"));
    g2 = reassemble(edited, 1, &m);
    CHECK(g2 != NULL);
    if (g2) {
        size_t zl;
        uint8_t *zb = zsym_extract(g2, &zl);
        Zsym *zs = zb ? zsym_parse(zb, zl) : NULL;
        CHECK(zs && zs->blocks.n == 1);
        if (zs && zs->blocks.n == 1) {
            z = zs->blocks.v[0];
            CHECK(sym_index(z, "F2") >= 0 && z->syms.v[sym_index(z, "F2")].val == 8);
        }
    }
    /* the generic text would have fixed the offsets in numbers */
    {
        Module *mm = NULL;
        GoffModule *gg = reassemble(t, 0, &mm);
        CHECK(gg != NULL);
    }
}

int main(void) {
    test_block();
    test_crc();
    test_reloc_mask();
    test_same_object();
    test_scopes();
    test_many();
    test_damage();
    test_fuzz();
    test_quote();
    test_smart_degrade();
    test_smart_pool();
    test_smart_fallback();
    test_smart_edit();
    TEST_MAIN_END();
}
