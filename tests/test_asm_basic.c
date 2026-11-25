/* test_asm_basic.c - reader, DC constants and diagnostic codes. */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "testlib.h"

/* assemble source text silently; returns NULL on errors; *repp gets the reporter */
static Module *assemble(const char *src, Reporter **repp) {
    Reporter *rep = rep_new(0);
    Module *m = asm_assemble_text("t.za", src, strlen(src), rep);
    if (repp) *repp = rep;
    return m;
}

static char *hex_of(const uint8_t *b, int64_t n) {
    char *s = xmalloc((size_t)n * 2 + 1);
    int64_t i;
    for (i = 0; i < n; i++) sprintf(s + 2 * i, "%02x", b[i]);
    s[n * 2] = 0;
    return s;
}

static void test_read_statements(void) {
    const char *src = "* comment\n"
                      "MAIN     CSECT\n"
                      "         LGR   1,2        remark here\n"
                      "LOOP:    DC    C'A B',X'F0',     first part\n"
                      "               F'5'\n"
                      "\n"
                      "         DC    A(L'MAIN+4)\n";
    SrcVec lines = {0};
    StmtVec st = {0};
    Reporter *rep = rep_new(0);
    Span sp;
    int idx;
    src_split(&lines, "t.za", src, strlen(src));
    read_statements(&st, lines.v, lines.n, rep);
    CHECK(rep_error_count(rep) == 0);
    CHECK(st.n == 4);
    if (st.n != 4) return;
    CHECK_STR(st.v[0]->name, "MAIN");
    CHECK_STR(st.v[0]->op, "CSECT");
    CHECK_STR(st.v[0]->operands, "");
    CHECK_STR(st.v[1]->op, "LGR");
    CHECK_STR(st.v[1]->operands, "1,2");
    CHECK_STR(st.v[1]->remark, "remark here");
    CHECK_STR(st.v[2]->name, "LOOP");
    CHECK_STR(st.v[2]->operands, "C'A B',X'F0',F'5'");
    idx = (int)strlen("C'A B',X'F0',");
    sp = stmt_operand_span(st.v[2], idx, 4);
    CHECK(sp.pos.line == 5);
    CHECK(sp.pos.col == 16);
    CHECK_STR(st.v[3]->operands, "A(L'MAIN+4)");
}

static void test_anchor(void) {
    const char *src = "    ^LB1  DC  A(1),\n"
                      "            A(2)\n"
                      "  ^   DC  C'x'\n"
                      "    ^\n"
                      "    ^* a comment\n"
                      "^LB2 DC  C'x\n";
    SrcVec lines = {0};
    StmtVec st = {0};
    Reporter *rep = rep_new(0);
    src_split(&lines, "t.za", src, strlen(src));
    read_statements(&st, lines.v, lines.n, rep);
    CHECK(st.n == 3);
    if (st.n == 3) {
        CHECK_STR(st.v[0]->name, "LB1");
        CHECK_STR(st.v[0]->operands, "A(1),A(2)");
        CHECK(st.v[0]->name_span.pos.col == 6); /* columns count the blanks and the ^ */
        CHECK(st.v[0]->op_span.pos.col == 11);
        CHECK(stmt_operand_span(st.v[0], 0, 1).pos.col == 15);
        CHECK(stmt_operand_span(st.v[0], 5, 1).pos.col == 13 && stmt_operand_span(st.v[0], 5, 1).pos.line == 2);
        CHECK_STR(st.v[1]->name, "");
        CHECK_STR(st.v[1]->op, "DC");
        CHECK(st.v[1]->op_span.pos.col == 7);
        CHECK_STR(st.v[2]->name, "LB2");
    }
    CHECK(rep_error_count(rep) == 1);
    if (rep_count(rep) == 1) {
        CHECK_STR(rep_get(rep, 0)->code, "ZA0002");
        CHECK(rep_get(rep, 0)->span.pos.col == 10); /* "^LB2 DC  C'x": the operand */
    }
}

static void test_read_errors(void) {
    const char *src = "X  DC  C'abc\nLBL\n";
    SrcVec lines = {0};
    StmtVec st = {0};
    Reporter *rep = rep_new(0);
    src_split(&lines, "t.za", src, strlen(src));
    read_statements(&st, lines.v, lines.n, rep);
    CHECK(rep_error_count(rep) == 2);
    if (rep_count(rep) == 2) {
        CHECK_STR(rep_get(rep, 0)->code, "ZA0002");
        CHECK_STR(rep_get(rep, 1)->code, "ZA0001");
    }
}

static void test_split_operands(void) {
    static const struct {
        const char *in;
        const char *want[4];
        int n;
    } cases[] = {
        {"1,2", {"1", "2"}, 2},
        {"C'A,B',X'0F'", {"C'A,B'", "X'0F'"}, 2},
        {"0(8,1),4(2)", {"0(8,1)", "4(2)"}, 2},
        {",,", {"", "", ""}, 3},
        {"L'A,=C'x''y'", {"L'A", "=C'x''y'"}, 2},
        {"A(1,2),AL2(3)", {"A(1,2)", "AL2(3)"}, 2},
        {"\"a,b\",\"c)\"", {"\"a,b\"", "\"c)\""}, 2},
        {"A(\"x,\\\"y\"),2", {"A(\"x,\\\"y\")", "2"}, 2},
    };
    size_t c;
    int i;
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        OperandVec ops = {0};
        split_operands(&ops, cases[c].in);
        CHECK_MSG((int)ops.n == cases[c].n, "%s: %d operands", cases[c].in, (int)ops.n);
        if ((int)ops.n != cases[c].n) continue;
        for (i = 0; i < cases[c].n; i++) CHECK_STR(ops.v[i].text, cases[c].want[i]);
    }
}

static void test_quoted_symbols(void) {
    static const struct {
        const char *tok, *want; /* want NULL: invalid */
    } cases[] = {
        {"ABC", "ABC"},
        {"\"ABC\"", "ABC"},
        {"\"a.b\"", "a.b"},
        {"\"a b\"", "a b"},
        {"\"q\\\"q\"", "q\"q"},
        {"\"b\\\\s\"", "b\\s"},
        {"\"\\x41\\x2E\"", "A."},
        {"\"\\xE9\"", "\xC3\xA9"}, /* U+00E9 as UTF-8 */
        {"\"\"", NULL},
        {"\"abc", NULL},
        {"\"abc\"x", NULL},
        {"\"a\\q\"", NULL},
        {"\"\\x4\"", NULL},
        {"\"\\x00\"", NULL},
        {"\"a\tb\"", NULL},
        {"a.b", NULL},
        {"1ab", NULL},
        {"", NULL},
    };
    static const unsigned char eb[] = {0x81, 0x4B, 0x82}; /* a.b */
    static const unsigned char eb2[] = {0x81, 0x40, 0x82}; /* a b */
    static const unsigned char eb3[] = {0xC1, 0x7F, 0xE0, 0xC2}; /* A"\B */
    static const unsigned char eb4[] = {0xC1, 0x00};
    size_t c;
    char *g;
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        const char *why = NULL;
        char *got = sym_text(cases[c].tok, &why);
        if (cases[c].want) {
            CHECK_MSG(got && !strcmp(got, cases[c].want), "%s: got \"%s\"", cases[c].tok,
                      got ? got : "(null)");
        } else {
            CHECK_MSG(!got && why, "%s: should be invalid", cases[c].tok);
        }
    }
    g = sym_source(eb, sizeof eb);
    CHECK_STR(g, "\"a.b\"");
    g = sym_source(eb2, sizeof eb2);
    CHECK_STR(g, "\"a b\"");
    g = sym_source(eb3, sizeof eb3);
    CHECK_STR(g, "\"A\\\"\\\\B\"");
    g = sym_source((const unsigned char *)"\xC1\xC2", 2);
    CHECK_STR(g, "AB");
    CHECK(sym_source(eb4, sizeof eb4) == NULL);
    CHECK(sym_source(eb4, 0) == NULL);
    {
        const unsigned char hi[] = {0xC1, 0x51}; /* a byte that is no ASCII */
        g = sym_source(hi, sizeof hi);
        CHECK(g && g[0] == '"');
        if (g) {
            const char *why = NULL;
            char *back = sym_text(g, &why);
            CHECK(back != NULL);
        }
    }
}

static void test_dc(void) {
    static const struct {
        const char *src, *want;
    } cases[] = {
        {"DC X'F'", "0f"},
        {"DC X'ABC'", "0abc"},
        {"DC XL3'1'", "000001"},
        {"DC XL1'123'", "23"},
        {"DC X'01',X'02,03'", "010203"},
        {"DC C'AB'", "c1c2"},
        {"DC CL4'AB'", "c1c24040"},
        {"DC CL1'AB'", "c1"},
        {"DC CA'A'", "41"},
        {"DC CAL3'A'", "412020"},
        {"DC CE'A'", "c1"},
        {"DC C'IT''S'", "c9e37de2"},
        {"DC C'A&&B'", "c150c2"},
        {"DC B'101'", "05"},
        {"DC B'1111111111'", "03ff"},
        {"DC F'-1'", "ffffffff"},
        {"DC F'1,2'", "0000000100000002"},
        {"DC H'258'", "0102"},
        {"DC H'-2'", "fffe"},
        {"DC FL3'1'", "000001"},
        {"DC FD'1'", "0000000000000001"},
        {"DC X'01',F'1'", "0100000000000001"},
        {"DC X'01',FL4'1'", "0100000001"},
        {"DC X'01',H'1'", "01000001"},
        {"DC P'123'", "123c"},
        {"DC P'-123'", "123d"},
        {"DC P'1'", "1c"},
        {"DC P'12'", "012c"},
        {"DC PL3'1'", "00001c"},
        {"DC P'1.25'", "125c"},
        {"DC Z'123'", "f1f2c3"},
        {"DC Z'-12'", "f1d2"},
        {"DC ZL4'5'", "f0f0f0c5"},
        {"DC 2X'01'", "0101"},
        {"DC 2C'AB'", "c1c2c1c2"},
        {"DC AL1(5)", "05"},
        {"DC AL2(X'8001')", "8001"},
        {"DC AL4(-1)", "ffffffff"},
        {"DC Y(300)", "012c"},
        {"DC A(10,20)", "0000000a00000014"},
        {"DC AD(5)", "0000000000000005"},
        {"BUF      DS    CL8\n         DC A(L'BUF)", "000000000000000000000008"},
        {"B1      DS    F\n         DC AL1(T'B1,I'B1,S'B1,L'B1)", "00000000c61f0004"},
        {"DC AL1(T'NOPE,D'NOPE)", "e400"},
        {"LBL  DS H\n DC AL1(D'LBL,D'LATER)\nLATER DS H", "000001000000"},
        {"P1 DC PL3'123'\n DC AL1(I'P1,L'P1,T'P1)", "00123c0503d7"},
        {"Z1 DC ZL4'1'\n DC AL1(I'Z1)", "f0f0f0c104"},
        {"E1 DC E'1'\n DC AL1(I'E1)", "4110000006"},
        {"    MVC 0(L'*,1),0(2)", "d20510002000"},
        {"DC AL1(L'*)", "01"},
        {"X1 EQU 5,2,C'F'\n DC AL1(T'X1,L'X1)", "c602"},
        {"SRC DS CL8\nALIAS EQU SRC\n DC AL1(L'ALIAS)", "000000000000000008"},
        {"I1 LR 1,2\nJ1 EQU I1\n DC AL1(L'J1)", "181202"},
        {"X EQU 5,4,0\n DC AL1(T'X,L'X)", "0004"},
        {"X EQU 5,4,C'A',7,GR\n DC A(L'X)\n DC AL1(T'X)", "00000004c1"},
        {" DC 10EBP(7)L2'12'", "4140414041404140414041404140414041404140"},
        {" DC C'A'\n DC 0D\n DC 0F\n DC F'0'", "c10000000000000000000000"},
        {" DSECT\nF1 DS F\nF2 DS CL4", ""},
        {" ORG *,8,2\nL1 DC F'0'", "0000000000000000"},
        {"X EQU 5,0\n DC AL1(L'X)", "00"},
        {"X EQU Y+4\nY DC F'1'\n DC A(X)", "0000000100000004"},
        {" USING T,15\nALIAS EQU SRC\nSRC DS CL8\nDST DS CL8\n MVC ALIAS,DST",
         "00000000000000000000000000000000d207f000f008"},
        {"DC AL1(O'LR,O'DC,O'ZZZ)", "d6c1e4"},
        {"DC E'1'", "41100000"},
        {"DC E'-1'", "c1100000"},
        {"DC E'0.5'", "40800000"},
        {"DC E'100'", "42640000"},
        {"DC E'0'", "00000000"},
        {"DC D'1'", "4110000000000000"},
        {"DC D'0.1'", "401999999999999a"},
        {"DC EB'1'", "3f800000"},
        {"DC DB'1'", "3ff0000000000000"},
        {"DC DB'-2'", "c000000000000000"},
        {"DC LB'1'", "3fff0000000000000000000000000000"},
        {"DC L'1'", "41100000000000003300000000000000"},
        {"DS 0F", ""},
        {"DC X'01'\n         DS 0F\n         DC X'02'", "0100000002"},
        {"DC X'01'\n         DS CL3\n         DC X'02'", "0100000002"},
        {"DC X'01'\n         DS 3C\n         DC X'02'", "0100000002"},
        {"DC X'01'\n         DS 0H\n         DC X'02'", "010002"},
    };
    size_t c;
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        Reporter *rep;
        char *src = xasprintf("T        CSECT\n         %s\n", cases[c].src);
        Module *m;
        /* a case that starts with a label is already a complete statement */
        if (cases[c].src[0] != ' ' && strncmp(cases[c].src, "DC ", 3) && strncmp(cases[c].src, "DS ", 3)) {
            free(src);
            src = xasprintf("T        CSECT\n%s\n", cases[c].src);
        }
        m = assemble(src, &rep);
        if (!m) {
            printf("assembly of \"%s\" failed\n", cases[c].src);
            rep_print(rep, stdout, 0);
            CHECK(0);
            continue;
        }
        {
            Class *k = m->sections.v[0]->classes.v[0];
            char *got = hex_of(k->data, k->len);
            CHECK_MSG(!strcmp(got, cases[c].want), "%-30s = %s, want %s", cases[c].src, got,
                      cases[c].want);
        }
    }
}

/* GNU style registers (%r1) must encode like the numeric and EQU forms */
static void test_gnu_registers(void) {
    static const struct {
        const char *gnu, *plain;
    } cases[] = {
        {"LGR %r1,%r2", "LGR 1,2"},
        {"L %r3,4(%r4,%r5)", "L 3,4(4,5)"},
        {"L %r3,4(,%r5)", "L 3,4(,5)"},
        {"LER %f2,%f4", "LER 2,4"},
        {"LAM %a1,%a2,0(%r3)", "LAM 1,2,0(3)"},
        {"LCTL %c0,%c15,0(%r1)", "LCTL 0,15,0(1)"},
        {"VL %v31,0(%r1)", "VL 31,0(1)"},
        {"LGR %R1,%R2", "LGR 1,2"},
        {"LA %r1,0", "LA 1,0"},
        {"USING *,%r12\n         L %r1,*", "USING *,12\n         L 1,*"},
        {"MVC 0(8,%r1),0(%r2)", "MVC 0(8,1),0(2)"},
    };
    size_t c;
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        Reporter *rg, *rp;
        char *sg = xasprintf("T CSECT\n %s\n", cases[c].gnu);
        char *sp = xasprintf("T CSECT\n %s\n", cases[c].plain);
        Module *mg = assemble(sg, &rg), *mp = assemble(sp, &rp);
        CHECK_MSG(mg && mp, "assembly of \"%s\" failed", cases[c].gnu);
        if (mg && mp) {
            Class *kg = mg->sections.v[0]->classes.v[0], *kp = mp->sections.v[0]->classes.v[0];
            CHECK_MSG(kg->len == kp->len && !memcmp(kg->data, kp->data, (size_t)kg->len),
                      "\"%s\" differs from \"%s\"", cases[c].gnu, cases[c].plain);
        } else if (!mg) {
            rep_print(rg, stdout, 0);
        }
    }
}

static char *codes_of(const char *src) {
    Reporter *rep;
    size_t n, i;
    const Diag **d;
    Buf b = {0};
    assemble(src, &rep);
    d = rep_sorted(rep, &n);
    for (i = 0; i < n; i++) {
        if (i) buf_putc(&b, ' ');
        buf_puts(&b, d[i]->code);
    }
    return xstrdup(buf_cstr(&b));
}

static void test_diag_codes(void) {
    static const struct {
        const char *name, *src, *want;
    } cases[] = {
        {"undefined symbol", "T CSECT\n LA 1,UNDEF\n", "ZA1011"},
        {"unknown op with suggestion", "T CSECT\n LAAA 1,2\n", "ZA2001"},
        {"arity", "T CSECT\n AR 1\n", "ZA2031"},
        {"register range", "T CSECT\n LR 16,1\n", "ZA2041"},
        {"displacement range", "T CSECT\n L 1,4096(,2)\n", "ZA2036"},
        {"no using", "T CSECT\nF DS F\n L 1,F\n", "ZA2030"},
        {"duplicate symbol", "T CSECT\nF DS F\nF DS F\n", "ZA1020"},
        {"no section", " DC F'1'\n", "ZA3001"},
        {"no sections at all", "* nothing\n", "ZA3002"},
        {"bad hex", "T CSECT\n DC X'1G'\n", "ZA2021"},
        {"dc range", "T CSECT\n DC H'70000'\n", "ZA2023"},
        {"missing nominal", "T CSECT\n DC F\n", "ZA2018"},
        {"unterminated quote", "T CSECT\n DC C'AB\n", "ZA0002"},
        {"unknown dc type", "T CSECT\n DC K'1'\n", "ZA2013"},
        {"unsupported dc type", "T CSECT\n DC W(1)\n", "ZA2013"},
        {"literal without base", "T CSECT\n L 1,=F'1'\n", "ZA2030"},
        {"label with no op", "T CSECT\nFOO\n", "ZA0001"},
        {"odd branch", "T CSECT\n J *+3\n", "ZA2045"},
        {"branch range", "T CSECT\n J *+100000\n", "ZA2046"},
        {"bad amode", "T CSECT\nT AMODE 99\n", "ZA3041"},
        {"entry undefined", "T CSECT\n ENTRY NOPE\n", "ZA3010"},
        {"explicit base needs absolute", "T CSECT\nF DS F\n L 1,F(,2)\n", "ZA2037"},
        {"drop warning", "T CSECT\n DROP 5\n", "ZA3060"},
        {"K attribute", "T CSECT\n DC AL1(K'X)\n", "ZA1032"},
        {"L'* in DC C", "T CSECT\n DC CL(L'*)'A'\n", "ZA1031"},
        {"EQU type range", "T CSECT\nX EQU 1,1,300\n", "ZA1033"},
        {"EQU length negative", "T CSECT\nX EQU 1,-1\n", "ZA1034"},
        {"EQU length too large", "T CSECT\nX EQU 1,65536\n", "ZA1034"},
        {"circular EQU", "T CSECT\nX EQU Y\nY EQU X\n", "ZA1035"},
        {"unused EQU undefined", "T CSECT\nX EQU NOPE\n", "ZA1035"},
        {"psect undefined", "T CSECT\nE LR 1,2\n ENTRY E\nE XATTR PSECT(NOPE)\n", "ZA3038"},
        {"ORG boundary", "T CSECT\n ORG *,8,2\n", ""},
        {"float range", "T CSECT\n DC E'1E99'\n", "ZA2053"},
        {"bare R1 is not predefined", "T CSECT\n LA R1,0\n", "ZA1011"},
        {"bare F2 is not predefined", "T CSECT\n LER F2,%f4\n", "ZA1011"},
        {"bare R1 in USING", "T CSECT\n USING *,R12\n", "ZA1011"},
        {"R1 after its EQU", "T CSECT\nR1 EQU 1\n LA R1,0\n", ""},
        {"GNU register class", "T CSECT\n LGR %f1,2\n", "ZA2055"},
        {"GNU register name", "T CSECT\n LGR %r1,%x2\n", "ZA2054"},
        {"GNU register range", "T CSECT\n LR %r16,1\n", "ZA2041"},
        {"quoted symbol name", "\"a.b\" CSECT\n BR 14\n", ""},
        {"quoted equals plain", "T CSECT\nA DS F\n\"A\" DS F\n", "ZA1020"},
        {"quoted symbol use", "T CSECT\n USING *,12\n\"a b\" DS F\n L 1,\"a b\"\n", ""},
        {"quoted attribute", "T CSECT\n\"a b\" DS FL3\n DC AL1(L'\"a b\")\n", ""},
        {"bad escape in a name", "\"a\\q\" CSECT\n", "ZA0004"},
        {"empty quoted name", "\"\" CSECT\n", "ZA0004"},
        {"quoted name not closed", "\"abc CSECT\n", "ZA0004"},
        {"bad escape in an operand", "T CSECT\n LA 1,\"a\\q\"\n", "ZA0004"},
        {"unterminated quoted operand", "T CSECT\n LA 1,\"abc\n", "ZA0002"},
        {"bad symbol in ENTRY", "T CSECT\n ENTRY a.b\n", "ZA1021"},
        {"quoted ENTRY", "T CSECT\n ENTRY \"a.b\"\n\"a.b\" DS 0H\n", ""},
        {"quoted EXTRN", "T CSECT\n EXTRN \"a.b\",\"c d\"\n DC A(\"a.b\"),V(\"c d\")\n", ""},
        {"unquoted odd name", "T CSECT\na.b DS F\n", "ZA1021"},
        {"DS beyond the section limit", "T CSECT\n DS 2147483647CL65535\n", "ZA2087"},
        {"DC bit storage beyond the limit", "T CSECT\n DC 2000000000BL256'1'\n", "ZA2087"},
        {"ORG beyond the section limit", "T CSECT\n ORG 2000000000+2000000000\n", "ZA3053"},
        {"section grown past the limit", "T CSECT\n DS 2000000000C\n DS 2000000000C\n", "ZA3054"},
        {"END into a DSECT label", "MYD DSECT\nL1 DS F\nT CSECT\n BR 14\n END L1\n", "ZA3012"},
        {"END names a DSECT", "MYD DSECT\nT CSECT\n BR 14\n END MYD\n", "ZA3012"},
        {"ok", "T CSECT\n BR 14\n", ""},
    };
    size_t c;
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        char *got = codes_of(cases[c].src);
        CHECK_MSG(!strcmp(got, cases[c].want), "%s: codes \"%s\", want \"%s\"", cases[c].name, got,
                  cases[c].want);
    }
}

static void test_register_hint(void) {
    Reporter *rep;
    const Diag *d;
    int found = 0, i;
    assemble("T CSECT\n LA R1,0\n", &rep);
    d = rep_get(rep, 0);
    CHECK(d != NULL);
    for (i = 0; d && i < d->nhelp; i++)
        if (strstr(d->help[i], "%r1") && strstr(d->help[i], "R1 EQU 1")) found = 1;
    CHECK_MSG(found, "the hint for a bare register name");
}

/* -strict-registers: only %r1 and names made with EQU %r1 in register positions */
static void test_strict_registers(void) {
    static const struct {
        const char *name, *src, *want;
    } cases[] = {
        {"number rejected", "T CSECT\n LA 1,0\n", "ZA2067"},
        {"number in a base", "T CSECT\n LA %r1,0(,2)\n", "ZA2067"},
        {"EQU'd number name rejected", "T CSECT\nR1 EQU 1\n LR R1,%r2\n", "ZA2067"},
        {"USING number", "T CSECT\n USING *,12\n", "ZA2067"},
        {"DROP number", "T CSECT\n DROP 12\n", "ZA2067"},
        {"%r everywhere", "T CSECT\n LM %r14,%r12,12(%R13)\n L %r1,4(%r2,%r3)\n SLL %r1,2\n BR %r14\n", ""},
        {"mask and length stay numbers", "T CSECT\n BCR 15,%r14\n MVC 0(8,%r1),0(%r2)\n LHI %r1,5\n", ""},
        {"register names", "T CSECT\nBASE EQU %r12\nF2 EQU %f2\n USING T,BASE\n LER F2,%f4\n LA %r1,0(,BASE)\n DROP BASE\n", ""},
        {"name of the wrong kind", "T CSECT\nF2 EQU %f2\n LR F2,%r1\n", "ZA2055"},
        {"register name as a number", "T CSECT\nBASE EQU %r12\n LHI %r1,BASE\n", "ZA2068"},
        {"bad EQU register", "T CSECT\nX EQU %x1\n", "ZA2054"},
        {"EQU register range", "T CSECT\nX EQU %r16\n", "ZA2054"},
        {"vector range", "T CSECT\nX EQU %v31\nY EQU %v32\n", "ZA2054"},
    };
    size_t c;
    za_strict_registers = 1;
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        char *got = codes_of(cases[c].src);
        CHECK_MSG(!strcmp(got, cases[c].want), "strict %s: codes \"%s\", want \"%s\"", cases[c].name,
                  got, cases[c].want);
    }
    za_strict_registers = 0;
    /* not strict: numbers work, and register names are numbers too */
    {
        static const char *const ok[] = {
            "T CSECT\n LA 1,0\n",
            "T CSECT\nBASE EQU %r12\n LHI %r1,BASE\n LA 1,0(,BASE)\n",
            "T CSECT\nBASE EQU 12\n USING T,BASE\n",
        };
        for (c = 0; c < sizeof ok / sizeof ok[0]; c++) {
            char *got = codes_of(ok[c]);
            CHECK_MSG(!got[0], "relaxed: \"%s\" gave %s", ok[c], got);
        }
    }
    /* the same program, written strictly or not, gives the same bytes */
    {
        Module *m1, *m2;
        za_strict_registers = 1;
        m1 = assemble("T CSECT\nB EQU %r12\n USING T,B\n L %r1,4(%r2,%r3)\n STM %r14,%r12,12(B)\n", NULL);
        za_strict_registers = 0;
        m2 = assemble("T CSECT\n USING T,12\n L 1,4(2,3)\n STM 14,12,12(12)\n", NULL);
        CHECK(m1 && m2);
        if (m1 && m2) {
            Class *k1 = m1->sections.v[0]->classes.v[0], *k2 = m2->sections.v[0]->classes.v[0];
            CHECK(k1->len == k2->len && !memcmp(k1->data, k2->data, (size_t)k1->len));
        }
    }
}

static void test_diag_help(void) {
    Reporter *rep;
    size_t n, i;
    const Diag **d;
    int found = 0;
    assemble("T CSECT\n LAAA 1,2\n", &rep);
    d = rep_sorted(rep, &n);
    CHECK(n >= 1);
    if (n < 1) return;
    for (i = 0; i < (size_t)d[0]->nhelp; i++)
        if (strstr(d[0]->help[i], "did you mean")) found = 1;
    CHECK(found);
}

int main(void) {
    test_read_statements();
    test_read_errors();
    test_anchor();
    test_split_operands();
    test_quoted_symbols();
    test_dc();
    test_gnu_registers();
    test_diag_codes();
    test_diag_help();
    test_register_hint();
    test_strict_registers();
    TEST_MAIN_END();
}
