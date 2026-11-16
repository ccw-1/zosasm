/* test_disasm.c - GOFF -> source -> GOFF round trips, and decoding of every
 * instruction of the table. */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "../src/disasm.h"
#include "../src/ebcdic.h"
#include "../src/goff.h"
#include "testlib.h"

static int zlevel; /* ZSYM level of the objects assemble() writes */

/* assemble text; the object bytes, or NULL after printing the problem */
static uint8_t *assemble(const char *what, const char *src, size_t *n) {
    Reporter *rep = rep_new(0);
    Module *m = asm_assemble_text(what, src, strlen(src), rep);
    char err[256];
    uint8_t *obj;
    if (!m || rep_error_count(rep) > 0) {
        printf("--- %s does not assemble:\n", what);
        rep_print(rep, stdout, 0);
        return NULL;
    }
    m->zlevel = zlevel;
    obj = module_emit(m, n, err, sizeof err);
    if (!obj) printf("--- %s: emit: %s\n", what, err);
    return obj;
}

static int quiet;

static char *to_source(const uint8_t *obj, size_t n, int *nwarn) {
    const char *perr = NULL;
    char err[256];
    GoffModule *g = goff_parse(obj, n, &perr);
    char *src;
    if (!g) {
        printf("parse: %s\n", perr);
        return NULL;
    }
    src = disasm_module(g, quiet ? NULL : stdout, nwarn, err, sizeof err);
    if (!src) printf("disasm: %s\n", err);
    return src;
}

/* source -> object -> source -> object: the objects are the same and the
 * second source is the first one again */
static void round_trip(const char *name, const char *src) {
    size_t n1, n2;
    uint8_t *o1 = assemble(name, src, &n1), *o2;
    char *s1, *s2, *s3;
    int nw = 0;
    CHECK_MSG(o1 != NULL, "%s: assembly", name);
    if (!o1) return;
    s1 = to_source(o1, n1, &nw);
    CHECK_MSG(s1 != NULL, "%s: disassembly", name);
    if (!s1) return;
    CHECK_MSG(nw == 0, "%s: %d warnings", name, nw);
    o2 = assemble("disassembly", s1, &n2);
    if (!o2) {
        printf("%s\n", s1);
        CHECK_MSG(0, "%s: the disassembly does not assemble", name);
        return;
    }
    if (!t_hexeq(o1, n1, o2, n2)) {
        size_t i = 0;
        while (i < n1 && i < n2 && o1[i] == o2[i]) i++;
        printf("--- %s: objects differ at byte %zu (%zu/%zu bytes); source:\n%s\n", name, i, n1, n2, s1);
        CHECK_MSG(0, "%s: object differs", name);
    } else {
        CHECK(1);
    }
    s2 = to_source(o2, n2, &nw);
    s3 = s2 ? s2 : (char *)"";
    CHECK_MSG(s2 && !strcmp(s1, s3), "%s: the source is not a fixed point", name);
}

static const char *const programs[] = {
    /* hello: instructions, relative branches, A/V constants, data */
    "HELLO    CSECT\n"
    "HELLO    AMODE 31\n"
    "HELLO    RMODE ANY\n"
    "         STM   14,12,12(13)\n"
    "         LR    12,15\n"
    "         USING HELLO,12\n"
    "         LA    2,MSG\n"
    "         L     4,COUNT\n"
    "         MVC   BUF(8),MSG\n"
    "         BAS   14,SUB1\n"
    "         BRAS  14,SUB1\n"
    "         J     DONE\n"
    "SUB1     DS    0H\n"
    "         BR    14\n"
    "DONE     LM    14,12,12(13)\n"
    "         BR    14\n"
    "COUNT    DC    F'42'\n"
    "MSG      DC    CL8'HELLO'\n"
    "BUF      DS    CL8\n"
    "TAB      DC    A(SUB1,DONE),V(EXTFN)\n"
    "         DC    AD(TAB)\n"
    "         DC    P'-123',Z'45',X'F1F2',B'101'\n"
    "         END   HELLO\n",

    /* unnamed section, no END operand */
    "         CSECT\n"
    "         BR    14\n"
    "         END\n",

    /* several sections, relative relocations, entries, weak symbols */
    "MAIN     CSECT\n"
    "MAIN     AMODE 64\n"
    "MAIN     RMODE 31\n"
    "         ENTRY ALT,ALT2\n"
    "         EXTRN PUTS\n"
    "         WXTRN OPTL\n"
    "ALT      DS    0H\n"
    "ALT2     DS    0H\n"
    "         STMG  14,12,8(13)\n"
    "         BRASL 14,PUTS\n"
    "         BRASL 14,OTHER\n"
    "         BRCL  8,PUTS+8\n"
    "         BRC   7,SKIP\n"
    "         LGHI  1,-5\n"
    "SKIP     LG    2,TBL-ALT(0,15)\n"
    "         BR    14\n"
    "TBL      DC    A(MAIN),AD(OTHER),V(PUTS),AL3(MAIN+4),A(OPTL),A(PUTS+4),A(PUTS-4)\n"
    "OTHER    CSECT\n"
    "OTHER    AMODE 31\n"
    "         BRASL 14,MAIN\n"
    "         BR    14\n"
    "         DC    A(MAIN,ALT)\n"
    "         END   ALT\n",

    /* read-only and common sections, classes with attributes and parts */
    "ONE      RSECT\n"
    "ONE      AMODE 31\n"
    "ONE      RMODE 24\n"
    "         BR    14\n"
    "ONE      XATTR SCOPE(LIBRARY)\n"
    "D_DATA   CATTR ALIGN(2),NOTEXECUTABLE,FILL(0),RENT\n"
    "VAL      DC    F'1',A(ONE)\n"
    "C_PART   CATTR PART(PRT),PRIORITY(7),ALIGN(4),MERGE,READONLY\n"
    "         DC    CL16'part data'\n"
    "PRT      XATTR SCOPE(MODULE),LINKAGE(XPLINK)\n"
    "E_X      CATTR NOLOAD,RMODE(64),REUS\n"
    "         DC    A(PRT),A(VAL)\n"
    "COMMON   COM\n"
    "         DS    CL40\n"
    "         END\n",

    /* every kind of data: runs of zeros, text, odd lengths, labels between */
    "DATA     CSECT\n"
    "         DC    XL100'0'\n"
    "A        DC    C'text with spaces, and (parens)'\n"
    "         DC    X'0102030405060708090A0B0C0D0E0F1011121314'\n"
    "B        DC    X'FF'\n"
    "         DC    AL3(A),AL4(B),AD(A)\n"
    "         DS    0X\n"
    "         DC    H'1',F'-1',FD'5',E'1.5',D'2.5',L'3.5'\n"
    "         END\n",

    /* a label at the end of a class, an instruction jump to the end */
    "TAIL     CSECT\n"
    "         J     *+6\n"
    "         BR    14\n"
    "         J     *-2\n"
    "LAST     DS    0X\n"
    "         DC    A(LAST)\n"
    "         END\n",

    /* names that are no symbols: sections, entries, parts, externals */
    "\"sec.1\"  CSECT\n"
    "\"sec.1\"  AMODE 31\n"
    "         ENTRY \"ent-a\"\n"
    "         EXTRN \"ext b\"\n"
    "         WXTRN \"weak\\x22q\\\\\"\n"
    "\"ent-a\"  DS    0H\n"
    "         BRASL 14,\"ext b\"\n"
    "         BRASL 14,\"sec.2\"\n"
    "         BR    14\n"
    "TAB      DC    A(\"ext b\"),V(\"ext b\"),A(\"weak\\x22q\\\\\"),A(\"ent-a\")\n"
    "\"sec.2\"  CSECT\n"
    "         DC    A(\"sec.1\"),AD(\"sec.1\"+12)\n"
    "\"P-1\"    CATTR ALIGN(3)\n"
    "         DC    F'1'\n"
    "         END   \"ent-a\"\n",

    /* external dummy sections: DXD, DSECT used in Q/J, CXD, R, REFERENCE(INDIRECT) */
    "T        CSECT\n"
    "         ENTRY LAB\n"
    "LAB      DS    0H\n"
    "         DC    Q(WORK),QD(WORK),J(WORK)\n"
    "         CXD\n"
    "         DC    R(LAB),R(EXT1),J(DSA),Q(DSA),RD(T),QL3(WORK),JL2(WORK)\n"
    "         DC    QY(WORK),QY(DSA)\n"
    "         EXTRN EXT1\n"
    "WORK     DXD   CL100,F\n"
    "DSA      DSECT\n"
    "         DS    CL20,D\n"
    "EXT1     XATTR REFERENCE(DATA,INDIRECT)\n"
    "         END\n",

    /* classes and parts in J and Q; DXD of several sizes; only CXD */
    "T        CSECT\n"
    "BIG      DXD   0D,70000X\n"
    "NONE     DXD   0H\n"
    "ONE      DXD   X\n"
    "NONEL    DXD   XL0\n"
    "P1       CATTR PART(PT),ALIGN(3)\n"
    "         DC    F'1'\n"
    "         DC    J(PT),Q(BIG),J(C2),J(B_TEXT),JD(ONE)\n"
    "C2       CATTR ALIGN(3)\n"
    "         DC    F'2'\n"
    "         END\n",

    "T        CSECT\n"
    "         CXD\n"
    "         END\n",
    /* built-in functions, comparisons and ASSERT */
    "S        DSECT\n"
    "A        DS    F\n"
    "B        DS    4CL10\n"
    "SLEN     EQU   sizeof(S)\n"
    "M        CSECT\n"
    "         ASSERT sizeof(S)==44&&alignof(S)>=4,'S layout'\n"
    "         LHI   1,sizeof(S)\n"
    "         LA    2,offsetof(S,B)(3)\n"
    "         LHI   4,countof(B)\n"
    "SZ       DC    A(sizeof(M),SLEN)\n"
    "         END\n",

    /* relocatable differences: RLD items that subtract */
    "A        CSECT\n"
    "X        DC    A(A1-X)\n"
    "         DC    A(EXTA-*),A(EXTA-EXTB+4),A(B1-A1),AL3(-B1),AD(A1-B1+9),A(B-A,EXTB-A1)\n"
    "         EXTRN EXTA,EXTB\n"
    "A1       DC    X'01'\n"
    "B        CSECT\n"
    "B1       DC    X'02'\n"
    "         END\n",
};

static uint32_t zsym_esd(const GoffModule *g) {
    size_t i;
    for (i = 0; i < g->esds.n; i++) {
        char *nm = ebcdic_decode(g->esds.v[i]->name, g->esds.v[i]->name_len);
        int hit = g->esds.v[i]->type == GOFF_ED && !strcmp(nm, "C_ADATAE9C1");
        free(nm);
        if (hit) return (uint32_t)i + 1;
    }
    return 0;
}

static GoffModule *parse_obj(const uint8_t *o, size_t n) {
    const char *perr = NULL;
    GoffModule *g = goff_parse(o, n, &perr);
    if (!g) printf("parse: %s\n", perr);
    return g;
}

/* source -> object with ZSYM -> source (from the ZSYM statements) -> the same
 * object, and the second ZSYM source is the first one again */
static void smart_trip(const char *name, const char *src) {
    size_t n1, n2, n3;
    uint8_t *o1, *o2, *o3;
    GoffModule *g1, *g2, *g3, *g0;
    char *t1, *t2, err[256], *gen1, *gen0;
    int nw = 0, smart = 0;
    uint8_t *o0;
    size_t n0;
    zlevel = 1;
    o1 = assemble(name, src, &n1);
    zlevel = 0;
    o0 = assemble(name, src, &n0);
    if (!o1 || !o0) {
        CHECK_MSG(0, "%s: assembly", name);
        return;
    }
    g1 = parse_obj(o1, n1);
    g0 = parse_obj(o0, n0);
    if (!g1 || !g0) return;
    CHECK_MSG(zsym_esd(g1) != 0, "%s: no ZSYM class", name);
    t1 = disasm_ex(g1, 0, stdout, &nw, &smart, err, sizeof err);
    CHECK_MSG(t1 != NULL, "%s: smart disassembly: %s", name, err);
    if (!t1) return;
    CHECK_MSG(smart, "%s: ZSYM not used", name);
    CHECK_MSG(nw == 0, "%s: %d warnings", name, nw);
    /* without -generic ZSYM is only a better way to the same object */
    o2 = assemble("zsym source", t1, &n2);
    if (!o2) {
        printf("%s\n", t1);
        CHECK_MSG(0, "%s: the ZSYM source does not assemble", name);
        return;
    }
    g2 = parse_obj(o2, n2);
    if (!g2) return;
    if (!goff_equal(g1, zsym_esd(g1), 1, g2)) printf("--- %s: the ZSYM source gives another object:\n%s\n", name, t1);
    CHECK_MSG(goff_equal(g1, zsym_esd(g1), 1, g2), "%s: object differs", name);
    /* its own ZSYM gives the same source back */
    zlevel = 1;
    o3 = assemble("zsym source", t1, &n3);
    g3 = o3 ? parse_obj(o3, n3) : NULL;
    t2 = g3 ? disasm_ex(g3, 0, NULL, &nw, &smart, err, sizeof err) : NULL;
    CHECK_MSG(t2 && smart && !strcmp(t1, t2), "%s: the ZSYM source is not a fixed point", name);
    zlevel = 0;
    /* -generic ignores the information */
    gen1 = disasm_ex(g1, DISASM_GENERIC, NULL, &nw, &smart, err, sizeof err);
    gen0 = disasm_ex(g0, 0, NULL, &nw, &smart, err, sizeof err);
    CHECK_MSG(gen1 && gen0 && !strcmp(gen1, gen0), "%s: -generic differs from an object without ZSYM", name);
}

static void test_programs(void) {
    size_t i;
    char name[32];
    for (i = 0; i < sizeof programs / sizeof programs[0]; i++) {
        snprintf(name, sizeof name, "program %d", (int)i);
        round_trip(name, programs[i]);
        smart_trip(name, programs[i]);
    }
    {
        size_t n;
        char *text = read_file("testdata/hello.za", &n);
        CHECK(text != NULL);
        if (text) {
            round_trip("testdata/hello.za", text);
            smart_trip("testdata/hello.za", text);
        }
    }
}

/* ---- every instruction of the table ------------------------------------- */

static GoffESD mk(GoffESDType t, uint32_t parent, const char *name) {
    GoffESD e;
    memset(&e, 0, sizeof e);
    e.type = t;
    e.parent = parent;
    e.name = ebcdic_from_source(name, strlen(name), &e.name_len);
    return e;
}

/* an object whose B_TEXT class holds exactly the given bytes */
static uint8_t *object_of(const uint8_t *text, size_t len, size_t *n) {
    GoffWriter *w = goff_writer_new(1);
    GoffESD sd = mk(GOFF_SD, 0, "T"), ed, ld;
    GoffEnd end;
    const uint8_t *out;
    uint8_t *copy;
    goff_write_esd(w, &sd);
    ed = mk(GOFF_ED, sd.id, "B_TEXT");
    ed.name_space = GOFF_NS_NORMAL;
    ed.length = (uint32_t)len;
    ed.attr.alignment = GOFF_ALIGN_DBL;
    ed.attr.executable = GOFF_EXEC_CODE;
    goff_write_esd(w, &ed);
    ld = mk(GOFF_LD, ed.id, "T");
    ld.name_space = GOFF_NS_NORMAL;
    ld.attr.scope = GOFF_SCOPE_MODULE;
    ld.attr.executable = GOFF_EXEC_CODE;
    goff_write_esd(w, &ld);
    goff_write_text(w, ed.id, 0, GOFF_STYLE_BYTE, text, len);
    memset(&end, 0, sizeof end);
    end.kind = GOFF_ENTRY_BYID;
    end.esdid = ed.id;
    goff_write_end(w, &end, &out, n);
    copy = xmalloc(*n);
    memcpy(copy, out, *n);
    return copy;
}

/* class text of the first (only) class of an object */
static int text_of(const uint8_t *obj, size_t n, uint8_t *out, size_t *len) {
    const char *perr = NULL;
    GoffModule *g = goff_parse(obj, n, &perr);
    size_t i, k = 0;
    if (!g) return 0;
    for (i = 0; i < g->texts.n; i++) {
        memcpy(out + k, g->texts.v[i].data, g->texts.v[i].len);
        k += g->texts.v[i].len;
    }
    *len = k;
    return 1;
}

/* operand values: 0 = smallest, 1 = small, 2 = largest allowed */
static int64_t pick(const IsaOperand *op, int which) {
    int64_t hi, lo;
    unsigned bits = op->bits;
    if (op->flags & F_VR) return which == 0 ? 0 : which == 1 ? 3 : 31;
    if ((op->flags & F_SIGNED) && !(op->flags & F_DISP)) {
        lo = -((int64_t)1 << (bits - 1));
        hi = ((int64_t)1 << (bits - 1)) - 1;
    } else if (op->flags & (F_SIGNED | F_PCREL)) {
        lo = -((int64_t)1 << (bits - 1));
        hi = ((int64_t)1 << (bits - 1)) - 1;
    } else {
        lo = 0;
        hi = ((int64_t)1 << bits) - 1;
    }
    if (which == 0) return lo < 0 ? -1 : 0;
    if (which == 1) return hi < 5 ? hi : 5;
    return hi;
}

static void test_all_instructions(void) {
    size_t i, bad = 0, total = 0;
    int which;
    for (i = 0; i < isa_inst_count; i++) {
        const IsaInst *in = &isa_inst_tab[i];
        for (which = 0; which < 3; which++) {
            int64_t vals[6] = {0};
            size_t k, n = isa_nops(in), olen, tlen;
            uint8_t code[6], back[64], *obj;
            char err[100], *src;
            int nw = 0;
            for (k = 0; k < n; k++) vals[k] = pick(isa_op(in, k), which);
            if (isa_encode(in, vals, n, code, err, sizeof err) != 0) continue;
            total++;
            obj = object_of(code, in->length, &olen);
            {
                const char *perr = NULL;
                GoffModule *g = goff_parse(obj, olen, &perr);
                char e2[200];
                src = g ? disasm_module(g, NULL, &nw, e2, sizeof e2) : NULL;
            }
            if (src) {
                size_t n2;
                uint8_t *o2 = assemble("one instruction", src, &n2);
                if (o2 && text_of(o2, n2, back, &tlen) && tlen == in->length &&
                    memcmp(back, code, in->length) == 0) {
                    CHECK(1);
                    continue;
                }
            }
            bad++;
            if (bad < 15) {
                printf("--- %s variant %d:", in->mnemonic, which);
                for (k = 0; k < in->length; k++) printf(" %02X", code[k]);
                printf("\n%s\n", src ? src : "(no source)");
            }
            CHECK_MSG(0, "%s variant %d does not round trip", in->mnemonic, which);
        }
    }
    printf("test_disasm: %zu instruction variants, %zu failures\n", total, bad);
}

/* ---- random objects ---------------------------------------------------------- */

static uint32_t rng_state;
static uint32_t rnd(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* two classes (code, data), an external symbol, labels and random
 * relocations; text is random with plausible instructions mixed in */
static uint8_t *random_object(size_t *n, size_t *clen) {
    GoffWriter *w = goff_writer_new(1);
    GoffESD sd = mk(GOFF_SD, 0, "R"), ed1, ed2, ld1, ld2, er;
    GoffEnd end;
    GoffReloc rl[40];
    size_t nr = 0, i, len1 = 16 + rnd() % 150, len2 = 8 + rnd() % 60;
    uint8_t *t1 = xmalloc(len1), *t2 = xmalloc(len2);
    const uint8_t *out;
    uint8_t *copy;
    static const uint8_t seeds[][6] = {
        {0x07, 0xFE, 0, 0, 0, 0}, {0x18, 0x12, 0, 0, 0, 0}, {0x58, 0x10, 0xC0, 0x10, 0, 0},
        {0xA7, 0x74, 0x00, 0x04, 0, 0}, {0x47, 0xF0, 0xC0, 0x08, 0, 0}, {0xD2, 0x07, 0x10, 0, 0x20, 0},
        {0xC0, 0xE5, 0, 0, 0, 0}, {0xEB, 0xEC, 0xD0, 0x08, 0x00, 0x24}, {0x41, 0x10, 0xC0, 0x00, 0, 0}};
    for (i = 0; i < len1; i++) t1[i] = (uint8_t)rnd();
    for (i = 0; i < len2; i++) t2[i] = (rnd() % 3) ? 0 : (uint8_t)rnd();
    for (i = 0; i + 6 <= len1; i += 2 * (1 + rnd() % 6)) {
        const uint8_t *sd_ = seeds[rnd() % 9];
        size_t l = sd_[0] < 0x40 ? 2 : sd_[0] < 0xC0 ? 4 : 6;
        memcpy(t1 + i, sd_, l);
    }
    goff_write_esd(w, &sd);
    ed1 = mk(GOFF_ED, sd.id, "B_TEXT");
    ed1.name_space = GOFF_NS_NORMAL;
    ed1.length = (uint32_t)len1;
    ed1.attr.alignment = GOFF_ALIGN_DBL;
    ed1.attr.executable = GOFF_EXEC_CODE;
    goff_write_esd(w, &ed1);
    ed2 = mk(GOFF_ED, sd.id, "D_X");
    ed2.name_space = GOFF_NS_NORMAL;
    ed2.length = (uint32_t)len2;
    ed2.attr.alignment = GOFF_ALIGN_DBL;
    ed2.attr.executable = GOFF_EXEC_DATA;
    goff_write_esd(w, &ed2);
    ld1 = mk(GOFF_LD, ed1.id, "R");
    ld1.name_space = GOFF_NS_NORMAL;
    ld1.attr.scope = GOFF_SCOPE_MODULE;
    ld1.attr.executable = GOFF_EXEC_CODE;
    goff_write_esd(w, &ld1);
    ld2 = mk(GOFF_LD, ed1.id, "ENT");
    ld2.name_space = GOFF_NS_NORMAL;
    ld2.attr.scope = GOFF_SCOPE_MODULE;
    ld2.attr.executable = GOFF_EXEC_CODE;
    ld2.offset = (uint32_t)(2 * (rnd() % (len1 / 2)));
    goff_write_esd(w, &ld2);
    er = mk(GOFF_ER, sd.id, "EXT");
    er.name_space = GOFF_NS_NORMAL;
    er.attr.scope = GOFF_SCOPE_SECTION;
    goff_write_esd(w, &er);
    /* relocations at random places, not overlapping */
    {
        size_t k, o;
        for (k = 0; k < 12; k++) {
            int in1 = rnd() % 2;
            size_t len = in1 ? len1 : len2;
            GoffReloc r;
            int kind = rnd() % 3;
            uint8_t *t = in1 ? t1 : t2;
            memset(&r, 0, sizeof r);
            r.pid = in1 ? ed1.id : ed2.id;
            r.length = 4;
            if (len < 8) continue;
            o = rnd() % (len - 4);
            if (kind == 2) o &= ~(size_t)1;
            for (i = 0; i < nr; i++)
                if (rl[i].pid == r.pid && o < rl[i].offset + 6u && rl[i].offset < o + 6) break;
            if (i < nr) continue;
            r.offset = (uint32_t)o;
            r.action = GOFF_ACTION_ADD;
            if (kind == 0) { /* A-con to the external symbol */
                r.rid = er.id;
                r.referent = GOFF_REFERENT_LABEL;
                memset(t + o, 0, 4);
                t[o + 3] = (uint8_t)(rnd() % 3 * 4);
            } else if (kind == 1) { /* A-con to a class */
                int to1 = rnd() % 2;
                uint32_t v = (uint32_t)(rnd() % (to1 ? len1 : len2));
                r.rid = to1 ? ed1.id : ed2.id;
                r.referent = GOFF_REFERENT_CLASS;
                t[o] = t[o + 1] = 0;
                t[o + 2] = (uint8_t)(v >> 8);
                t[o + 3] = (uint8_t)v;
            } else { /* BRASL with a relative relocation */
                if (!in1 || o + 6 > len1) continue;
                t[o] = 0xC0;
                t[o + 1] = 0xE5;
                memset(t + o + 2, 0, 4);
                r.offset = (uint32_t)o + 2;
                r.reference = GOFF_REF_RELIMM;
                if (rnd() % 2) {
                    r.rid = er.id;
                    r.referent = GOFF_REFERENT_LABEL;
                } else {
                    /* the assembler resolves a relative reference inside
                     * a class itself, so the target is in the other one */
                    r.rid = ed2.id;
                    r.referent = GOFF_REFERENT_CLASS;
                    t[o + 5] = (uint8_t)(2 * (rnd() % (len2 / 2)));
                }
            }
            rl[nr++] = r;
        }
    }
    goff_write_text(w, ed1.id, 0, GOFF_STYLE_BYTE, t1, len1);
    goff_write_text(w, ed2.id, 0, GOFF_STYLE_BYTE, t2, len2);
    if (nr) goff_write_rld(w, rl, nr);
    memset(&end, 0, sizeof end);
    end.kind = GOFF_ENTRY_BYID;
    end.esdid = ed1.id;
    goff_write_end(w, &end, &out, n);
    copy = xmalloc(*n);
    memcpy(copy, out, *n);
    *clen = len1 + len2;
    return copy;
}

/* all class text, class after class */
static int texts_of(const uint8_t *obj, size_t n, uint8_t *out, size_t *len) { return text_of(obj, n, out, len); }

static void test_random(void) {
    int iter, bad = 0, lossless = 0;
    rng_state = 12345;
    quiet = 1;
    for (iter = 0; iter < 400; iter++) {
        size_t n, clen, n2, n3, l1, l2;
        uint8_t *o1 = random_object(&n, &clen), *o2, *o3, a[4096], b[4096];
        char *s1, *s2;
        int nw = 0;
        s1 = to_source(o1, n, &nw);
        if (!s1) {
            bad++;
            CHECK_MSG(0, "random %d: disassembly", iter);
            continue;
        }
        o2 = assemble("random", s1, &n2);
        if (!o2) {
            if (bad++ < 3) printf("%s\n", s1);
            CHECK_MSG(0, "random %d: the disassembly does not assemble", iter);
            continue;
        }
        CHECK_MSG(texts_of(o1, n, a, &l1) && texts_of(o2, n2, b, &l2) && l1 == l2 && !memcmp(a, b, l1),
                  "random %d: text differs", iter);
        /* an object with a warning could not have been written by the
         * assembler: only the text and a valid source are required */
        if (nw) continue;
        lossless++;
        s2 = to_source(o2, n2, &nw);
        if (!(s2 && !strcmp(s1, s2)) && bad++ < 2) printf("--- first:\n%s--- second:\n%s\n", s1, s2 ? s2 : "(none)");
        CHECK_MSG(s2 && !strcmp(s1, s2), "random %d: not a fixed point", iter);
        o3 = s2 ? assemble("random 2", s2, &n3) : NULL;
        CHECK_MSG(o3 && t_hexeq(o2, n2, o3, n3), "random %d: objects differ", iter);
    }
    CHECK_MSG(lossless > 50, "random: only %d objects without warnings", lossless);
}

int main(void) {
    test_programs();
    test_random();
    test_all_instructions();
    TEST_MAIN_END();
}
