/* test_listing.c - the assembly listing. */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>

#include "../src/listing.h"
#include "testlib.h"

/* assemble src and return the listing text */
static char *list_with(const char *src, ListOptions lo) {
    PpOptions o;
    PpEventVec ev = {0};
    SrcVec lines = {0};
    Reporter *rep = rep_new(0);
    Module *m;
    FILE *f = tmpfile();
    long n;
    char *out;
    memset(&o, 0, sizeof o);
    o.events = &ev;
    rep_add_source(rep, "t.za", src, strlen(src));
    pp_run("t.za", src, strlen(src), &o, rep, &lines);
    m = asm_assemble_all(lines.v, lines.n, "t.za", rep);
    listing_write(f, m, &lines, &ev, rep, &lo);
    n = ftell(f);
    rewind(f);
    out = xmalloc((size_t)n + 1);
    if (fread(out, 1, (size_t)n, f) != (size_t)n) n = 0;
    out[n] = 0;
    fclose(f);
    return out;
}

static char *list(const char *src) {
    ListOptions lo;
    memset(&lo, 0, sizeof lo);
    return list_with(src, lo);
}

/* the row of source line n (rows start with the location and end with the source) */
static int has(const char *text, const char *s) { return strstr(text, s) != NULL; }

/* is s before t in text? */
static int before(const char *text, const char *s, const char *t) {
    const char *a = strstr(text, s), *b = strstr(text, t);
    return a && b && a < b;
}

static void test_rows(void) {
    char *t = list("* comment\n"
                   "T        CSECT\n"
                   "         USING T,%r12\n"
                   "         LR    %r1,%r2\n"
                   "         L     %r3,X\n"
                   "X        DC    F'1'\n"
                   "         END\n");
    CHECK(has(t, "Loc     Object"));
    CHECK(has(t, "000000  1812 "));
    CHECK(has(t, "000002  5830C008"));
    CHECK(has(t, "000008  00000001"));
    CHECK(has(t, "     2   T        CSECT"));
    CHECK(has(t, "* comment"));
    CHECK(has(t, "Sections"));
    CHECK(has(t, "USING table"));
    CHECK(has(t, "Symbol cross reference"));
    CHECK(has(t, "7 source lines, 6 statements; 0 errors, 0 warnings"));
    /* X is defined on line 6 and used on line 5 */
    CHECK(has(t, "label    4       8         T          6        5"));
}

static void test_diag_inline(void) {
    char *t = list("T        CSECT\n"
                   "         LA    %r1,UNDEF\n"
                   "         BR    %r14\n"
                   "         END\n");
    CHECK(has(t, "** ZA1011 error: undefined symbol \"UNDEF\""));
    CHECK(before(t, "LA    %r1,UNDEF", "** ZA1011"));
    CHECK(before(t, "** ZA1011", "BR    %r14"));
    CHECK(has(t, "1 error"));
}

static void test_preprocessor_rows(void) {
    char *t = list("#define TWO(a) LR 1,a\n"
                   "#if 0\n"
                   "         DC    X'01'\n"
                   "#endif\n"
                   "T        CSECT\n"
                   "         TWO(2)\n"
                   "         END\n");
    CHECK(has(t, " # #define TWO(a) LR 1,a"));
    CHECK(has(t, " - "));
    CHECK(has(t, " +          LR 1,2"));
    CHECK(has(t, "000000  1812"));
    /* the line of the macro call has no object, only its expansion has */
    CHECK(before(t, "TWO(2)", "LR 1,2"));
}

static void test_block_comments(void) {
    /* the listing shows the lines as written, comments included */
    char *t = list("/* header\n"
                   "   comment */\n"
                   "T        CSECT  /* the section */\n"
                   "    ^L1  LR    %r1,%r2  /* r */  remark\n"
                   "         END\n");
    CHECK(has(t, "/* header"));
    CHECK(has(t, "   comment */"));
    CHECK(has(t, "T        CSECT  /* the section */"));
    CHECK(has(t, "000000  1812"));
    CHECK(has(t, "^L1  LR    %r1,%r2  /* r */  remark"));
    CHECK(has(t, "0 errors"));
}

static void test_literals(void) {
    char *t = list("T        CSECT\n"
                   "         USING T,%r12\n"
                   "         L     %r1,=F'7'\n"
                   "         LTORG\n"
                   "         END\n");
    CHECK(has(t, "=F'7'"));
    CHECK(has(t, "000008  00000007"));
    CHECK(before(t, "LTORG", "   =F'7'"));
}

static void test_literals_at_end(void) {
    char *t = list("T        CSECT\n"
                   "         USING T,%r12\n"
                   "         L     %r1,=F'7'\n"
                   "         END\n");
    CHECK(before(t, "         END", "   =F'7'"));
    CHECK(has(t, "000008  00000007"));
}

static void test_data(void) {
    const char *src = "T        CSECT\n"
                      "A        DC    X'000102030405060708090A0B'\n"
                      "         END\n";
    ListOptions lo;
    char *t = list(src);
    CHECK(has(t, "00010203 04050607+"));
    CHECK(!has(t, "000008  08090A0B"));
    memset(&lo, 0, sizeof lo);
    lo.data = 1;
    t = list_with(src, lo);
    CHECK(has(t, "00010203 04050607 "));
    CHECK(has(t, "000008  08090A0B"));
}

static void test_print(void) {
    char *t = list("T        CSECT\n"
                   "         PRINT OFF\n"
                   "         DC    X'11'\n"
                   "         PRINT ON\n"
                   "         DC    X'22'\n"
                   "#pragma print off\n"
                   "         DC    X'33'\n"
                   "#pragma print on\n"
                   "         DC    X'44'\n"
                   "         END\n");
    CHECK(!has(t, "X'11'"));
    CHECK(has(t, "PRINT OFF"));
    CHECK(has(t, "PRINT ON"));
    CHECK(has(t, "X'22'"));
    CHECK(!has(t, "X'33'"));
    CHECK(has(t, "X'44'"));
    /* an error under PRINT OFF is not hidden */
    t = list("T        CSECT\n"
             "         PRINT OFF\n"
             "         LA    %r1,UNDEF\n"
             "         PRINT ON\n"
             "         END\n");
    CHECK(has(t, "LA    %r1,UNDEF"));
    CHECK(has(t, "ZA1011"));
}

static void test_nogen(void) {
    char *t = list("#define TWO(a) LR 1,a\n"
                   "T        CSECT\n"
                   "         PRINT NOGEN\n"
                   "         TWO(2)\n"
                   "         PRINT GEN\n"
                   "         TWO(3)\n"
                   "         END\n");
    CHECK(!has(t, "LR 1,2"));
    CHECK(has(t, "LR 1,3"));
}

static void test_pages(void) {
    ListOptions lo;
    Buf b = {0};
    char *t;
    int i, ff = 0;
    memset(&lo, 0, sizeof lo);
    lo.page = 10;
    lo.brief = 1;
    buf_puts(&b, "         TITLE 'the title'\nT        CSECT\n");
    for (i = 0; i < 30; i++) buf_puts(&b, "         DC    X'00'\n");
    buf_puts(&b, "         END\n");
    t = list_with(buf_cstr(&b), lo);
    for (i = 0; t[i]; i++)
        if (t[i] == '\f') ff++;
    CHECK(ff >= 3);
    CHECK(has(t, "Page 1"));
    CHECK(has(t, "Page 4"));
    CHECK(has(t, "the title"));
    CHECK(!has(t, "Symbol cross reference"));
    /* EJECT starts a page */
    memset(&lo, 0, sizeof lo);
    lo.page = 50;
    lo.brief = 1;
    t = list_with("T        CSECT\n         DC    X'01'\n         EJECT\n         DC    X'02'\n         END\n", lo);
    CHECK(has(t, "Page 2"));
    CHECK(before(t, "EJECT", "Page 2"));
    CHECK(before(t, "Page 2", "X'02'"));
    /* SPACE 3 gives three blank lines */
    memset(&lo, 0, sizeof lo);
    lo.brief = 1;
    t = list_with("T        CSECT\n         SPACE 3\n         DC    X'02'\n         END\n", lo);
    CHECK(has(t, "SPACE 3\n\n\n\n"));
}

static void test_pragmas(void) {
    ListOptions lo;
    char *t;
    memset(&lo, 0, sizeof lo);
    lo.page = 50;
    lo.brief = 1;
    t = list_with("#pragma title \"from pragma\"\n"
                  "T        CSECT\n"
                  "         DC    X'01'\n"
                  "#pragma eject\n"
                  "         DC    X'02'\n"
                  "#pragma space 2\n"
                  "         DC    X'03'\n"
                  "         END\n",
                  lo);
    CHECK(has(t, "from pragma"));
    CHECK(before(t, "from pragma", "T        CSECT"));
    CHECK(has(t, "Page 2"));
    CHECK(before(t, "#pragma eject", "Page 2"));
    CHECK(has(t, "#pragma space 2\n\n\n"));
}

static void test_tables(void) {
    char *t = list("A        CSECT\n"
                   "         USING A,%r12\n"
                   "         LA    %r1,K\n"
                   "         DROP  %r12\n"
                   "K        EQU   5\n"
                   "B        CSECT\n"
                   "         EXTRN OUT\n"
                   "         DC    A(OUT)\n"
                   "         END\n");
    CHECK(has(t, "Section"));
    CHECK(has(t, "CSECT   B_TEXT"));
    /* the USING of line 2 was dropped on line 4 */
    CHECK(has(t, "r12  A"));
    CHECK(has(t, "4\n"));
    CHECK(has(t, "extrn"));
    CHECK(has(t, "equ      1       5"));
}

static void test_brief(void) {
    ListOptions lo;
    char *t;
    memset(&lo, 0, sizeof lo);
    lo.brief = 1;
    t = list_with("T        CSECT\n         BR    %r14\n         END\n", lo);
    CHECK(has(t, "BR    %r14"));
    CHECK(!has(t, "Sections"));
    CHECK(has(t, "0 errors"));
}

static void test_wide_loc(void) {
    /* classes longer than 16 MB get an 8 digit location column */
    char *t = list("T        CSECT\n         DS    70000XL256\nA        DC    X'01'\n         END\n");
    CHECK(has(t, "01117000  01"));
}

static void test_ctl2(void) {
    ListOptions lo;
    char *t;
    /* PUSH PRINT / POP PRINT restore the state */
    t = list("T        CSECT\n"
             "         PUSH  PRINT\n"
             "         PRINT OFF\n"
             "         DC    X'11'\n"
             "         POP   PRINT\n"
             "         DC    X'22'\n"
             "         END\n");
    CHECK(!has(t, "X'11'"));
    CHECK(has(t, "X'22'"));
    CHECK(has(t, "PUSH  PRINT"));
    CHECK(has(t, "POP   PRINT"));
    /* NOPRINT hides the statement itself */
    t = list("T        CSECT\n"
             "         PUSH  PRINT,NOPRINT\n"
             "         POP   PRINT,NOPRINT\n"
             "         DC    X'22'\n"
             "         END\n");
    CHECK(!has(t, "PUSH"));
    CHECK(!has(t, "POP "));
    CHECK(has(t, "X'22'"));
    /* CEJECT: only when too few lines remain */
    memset(&lo, 0, sizeof lo);
    lo.page = 10;
    lo.brief = 1;
    t = list_with("T        CSECT\n"
                  "         DC    X'01'\n"
                  "         CEJECT 2\n"
                  "         DC    X'02'\n"
                  "         END\n", lo);
    CHECK(!has(t, "CEJECT"));
    CHECK(!has(t, "Page 2"));
    t = list_with("T        CSECT\n"
                  "         DC    X'01'\n"
                  "         CEJECT 20\n"
                  "         DC    X'02'\n"
                  "         END\n", lo);
    CHECK(has(t, "Page 2"));
    CHECK(before(t, "Page 2", "X'02'"));
    t = list_with("T        CSECT\n"
                  "         DC    X'01'\n"
                  "         CEJECT\n"
                  "         DC    X'02'\n"
                  "         END\n", lo);
    CHECK(has(t, "Page 2"));
    t = list_with("T        CSECT\n"
                  "         DC    X'01'\n"
                  "#pragma ceject 20\n"
                  "         DC    X'02'\n"
                  "         END\n", lo);
    CHECK(has(t, "Page 2"));
}

int main(void) {
    test_ctl2();
    test_rows();
    test_diag_inline();
    test_preprocessor_rows();
    test_block_comments();
    test_literals();
    test_literals_at_end();
    test_data();
    test_print();
    test_nogen();
    test_pages();
    test_pragmas();
    test_tables();
    test_brief();
    test_wide_loc();
    TEST_MAIN_END();
}
