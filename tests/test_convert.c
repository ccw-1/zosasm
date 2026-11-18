/* test_convert.c - HLASM to zosasm source conversion (hlasm2za). */
#include <stdlib.h>
#include <string.h>

#include "../src/convert.h"
#include "../src/util.h"
#include "testlib.h"

static char *conv_opt(const char *text, const ConvOptions *o, int *nw) {
    return hlasm_convert(text, strlen(text), "t.asm", o, NULL, nw);
}

static char *conv(const char *text) {
    ConvOptions o;
    int nw;
    conv_options_default(&o);
    return conv_opt(text, &o, &nw);
}

static int warns(const char *text) {
    ConvOptions o;
    int nw;
    conv_options_default(&o);
    conv_opt(text, &o, &nw);
    return nw;
}

/* a card: the text padded to column 71 and a continuation mark in column 72 */
static char *card(const char *text, int cont) {
    char *s = xcalloc(100);
    memset(s, ' ', 71);
    memcpy(s, text, strlen(text) < 71 ? strlen(text) : 71);
    s[71] = cont ? 'X' : ' ';
    s[72] = 0;
    return s;
}

static void test_layout(void) {
    CHECK_STR(conv("         lr    1,2\n"), "         LR    %r1,%r2\n");
    CHECK_STR(conv("loop  lr 1,2 remark\n"), "LOOP     LR    %r1,%r2             remark\n");
    CHECK_STR(conv("*  a Comment in lower case\n\n.* macro comment\n"), "*  a Comment in lower case\n\n* macro comment\n");
    CHECK_STR(conv("A DC C'abc'\n"), "A        DC    C'abc'\n");
    CHECK_STR(conv("A DC X'ab12',AL2(l'x)\n"), "A        DC    X'AB12',AL2(L'X)\n");
    CHECK_STR(conv("A DC C'L'\n"), "A        DC    C'L'\n");
    CHECK_STR(conv("A DC C'it''s a ''b'''\n"), "A        DC    C'it''s a ''b'''\n");
    CHECK_STR(conv("VERYLONGNAME1 DS    0H\n"), "VERYLONGNAME1 DS    0H\n");
    CHECK_STR(conv("A\tDC\tF'1'\n"), "A        DC    F'1'\n");
    CHECK_STR(conv("A DC F'1'\r\nB DC F'2'\r\n"), "A        DC    F'1'\nB        DC    F'2'\n");
    CHECK_STR(conv("         LA 1,0 remark here\n"), "         LA    %r1,0               remark here\n");
}

static void test_columns(void) {
    char *a, *b, *src;
    /* sequence numbers in columns 73-80 are dropped */
    CHECK_STR(conv("         LR    1,2                                                      00000010\n"), "         LR    %r1,%r2\n");
    /* continuation of an operand field after a comma */
    a = card("         MVC   T(8),", 1);
    b = xasprintf("%s\n%s\n", a, "               S");
    CHECK_STR(conv(b), "         MVC   T(8),S\n");
    /* continuation with the text broken inside a string: blanks to column 71 belong to it */
    a = card("         DC    C'ABC", 1);
    b = xasprintf("%s\n%s\n", a, "               DEF'");
    src = conv(b);
    CHECK(strstr(src, "C'ABC") != NULL && strstr(src, "DEF'") != NULL);
    CHECK_MSG(strstr(src, "C'ABC      ") != NULL, "blanks inside the string kept: %s", src);
    /* continued after the last column of a symbol */
    a = card("         DC    A(ABCDEFGH", 0);
    memcpy(a + 71 - 4, "WXYZ", 4);
    a[71] = 'X';
    b = xasprintf("%s\n%s\n", a, "               IJK)");
    CHECK(strstr(conv(b), "WXYZIJK)") != NULL);
    /* a remark that continues */
    a = card("         LR    1,2   REMARK", 1);
    b = xasprintf("%s\n%s\n", a, "               MORE REMARK");
    CHECK(strstr(conv(b), "REMARK MORE REMARK") != NULL);
    /* ICTL: new columns for the following statements */
    CHECK_STR(conv(" ICTL 10,60,20\n          lr 1,2\n"), "         LR    %r1,%r2\n");
    CHECK_STR(conv(" ICTL 10,80\n          lr 1,2   remark\n"), "         LR    %r1,%r2             remark\n");
    CHECK_STR(conv("* a comment\n         ICTL 1,71,16\n lr 1,2\n"), "* a comment\n         LR    %r1,%r2\n");
    /* the continuation column must lie between begin and end: a continuation
     * mark outside the range used to send the padding loop near 2^64 (a hang) */
    CHECK(warns(" ICTL 1,71,80\n lr 1,2\n") == 1);
    CHECK(warns(" ICTL 40,71\n lr 1,2\n") == 1); /* default continuation 16 < 40 */
    CHECK(warns(" ICTL 1,71,16\n lr 1,2\n") == 0);
}

static void test_remarks(void) {
    /* operands of an operand-less operation are a remark */
    CHECK_STR(conv("X CSECT   the first section\n"), "* the first section\nX        CSECT\n");
    CHECK_STR(conv("   LTORG   pool\n"), "* pool\n         LTORG\n");
    CHECK_STR(conv("   PR    return\n"), "* return\n         PR\n");
    CHECK_STR(conv("X CSECT\n"), "X        CSECT\n");
}

static void test_registers(void) {
    const char *r;
    /* a name used only as a register becomes %r */
    r = conv("R1 EQU 1\nR2 EQU 2\n LR R1,R2\n L R1,0(R2)\n");
    CHECK(strstr(r, "R1       EQU   %r1\n") != NULL);
    CHECK(strstr(r, "R2       EQU   %r2\n") != NULL);
    CHECK(strstr(r, "LR    R1,R2\n") != NULL);
    CHECK(strstr(r, "L     R1,0(R2)\n") != NULL);
    /* used for something else too: stays a number, uses become %r */
    r = conv("LEN EQU 4\n LA 1,LEN\n LR LEN,2\n");
    CHECK(strstr(r, "LEN      EQU   4\n") != NULL);
    CHECK(strstr(r, "LA    %r1,LEN\n") != NULL);
    CHECK(strstr(r, "LR    %r4,%r2\n") != NULL);
    /* two classes */
    r = conv("X EQU 2\n LR X,1\n LER X,4\n");
    CHECK(strstr(r, "X        EQU   2\n") != NULL);
    CHECK(strstr(r, "LR    %r2,%r1\n") != NULL);
    CHECK(strstr(r, "LER   %f2,%f4\n") != NULL);
    /* floating point, access and control registers */
    r = conv("F0 EQU 0\n LD F0,=D'1'\n SAR 3,1\n LCTL 1,2,X\n");
    CHECK(strstr(r, "F0       EQU   %f0\n") != NULL);
    CHECK(strstr(r, "SAR   %a3,%r1\n") != NULL);
    CHECK(strstr(r, "LCTL  %c1,%c2,X\n") != NULL);
    /* aliases */
    r = conv("R12 EQU 12\nBASE EQU R12\n USING *,BASE\n LA BASE,0(,R12)\n DROP BASE\n");
    CHECK(strstr(r, "R12      EQU   %r12\n") != NULL);
    CHECK(strstr(r, "BASE     EQU   %r12\n") != NULL);
    CHECK(strstr(r, "USING *,BASE\n") != NULL);
    CHECK(strstr(r, "LA    BASE,0(,R12)\n") != NULL);
    /* undefined R-names are taken as registers, with a warning */
    r = conv(" LR R3,R4\n");
    CHECK_STR(r, "         LR    %r3,%r4\n");
    CHECK_MSG(warns(" LR R3,R4\n") == 2, "two names: %d", warns(" LR R3,R4\n"));
    /* USING and DROP with numbers */
    r = conv(" USING *,12\n DROP 12\n");
    CHECK(strstr(r, "USING *,%r12\n") != NULL);
    CHECK(strstr(r, "DROP  %r12\n") != NULL);
    /* index and base in an address, an empty index, a length */
    r = conv(" L 1,4(2,3)\n MVC 0(8,5),0(6)\n L 1,4(,3)\n");
    CHECK(strstr(r, "L     %r1,4(%r2,%r3)\n") != NULL);
    CHECK(strstr(r, "MVC   0(8,%r5),0(%r6)\n") != NULL);
    CHECK(strstr(r, "L     %r1,4(,%r3)\n") != NULL);
    /* literals are left alone */
    CHECK(strstr(conv(" L 1,=A(5)\n"), "L     %r1,=A(5)\n") != NULL);
    /* an operand that is not a register is reported and kept */
    r = conv(" LR 1,X+1\n");
    CHECK(strstr(r, "LR    %r1,X+1\n") != NULL);
    CHECK_MSG(warns(" LR 1,X+1\n") == 1, "one warning");
    /* a number out of range */
    CHECK(warns(" LR 1,17\n") == 1);
    /* YREGS */
    r = conv(" YREGS\n LR R1,R2\n");
    CHECK(strstr(r, "R0       EQU   %r0\n") != NULL);
    CHECK(strstr(r, "R15      EQU   %r15\n") != NULL);
    CHECK(strstr(r, "LR    R1,R2\n") != NULL);
}

static void test_relaxed(void) {
    ConvOptions o;
    int nw;
    char *r;
    conv_options_default(&o);
    o.strict = 0;
    r = conv_opt("R1 EQU 1\n lr r1,2\n", &o, &nw);
    CHECK_STR(r, "R1       EQU   1\n         LR    R1,2\n");
    CHECK(nw == 0);
}

static void test_width(void) {
    ConvOptions o;
    int nw;
    char *r;
    conv_options_default(&o);
    o.width = 40;
    r = conv_opt(" DC A(AAAAAAAA),A(BBBBBBBB),A(CCCCCCCC),A(DDDDDDDD)\n", &o, &nw);
    CHECK_STR(r, "         DC    A(AAAAAAAA),A(BBBBBBBB),\n               A(CCCCCCCC),A(DDDDDDDD)\n");
    o.width = 0;
    r = conv_opt(" DC A(AAAAAAAA),A(BBBBBBBB),A(CCCCCCCC),A(DDDDDDDD)\n", &o, &nw);
    CHECK_STR(r, "         DC    A(AAAAAAAA),A(BBBBBBBB),A(CCCCCCCC),A(DDDDDDDD)\n");
}

static void test_unconverted(void) {
    char *r;
    /* COPY and ISEQ */
    CHECK_STR(conv(" COPY MyRegs\n"), "#include \"myregs.inc\"\n");
    CHECK_STR(conv(" ISEQ 73,80\n LR 1,2\n"), "         LR    %r1,%r2\n");
    /* a macro is converted and its call needs no warning */
    r = conv("         MACRO\n&L       MYM &A\n         MEND\n         MYM 1\n");
    CHECK(strstr(r, "#macro MYM(a) {\n#}\n") != NULL);
    CHECK(strstr(r, "         MYM   1\n") != NULL);
    CHECK_MSG(warns("         MACRO\n&L       MYM &A\n         MEND\n         MYM 1\n") == 0, "converted macro warnings");
    /* -comment-macros: commented out and reported */
    {
        ConvOptions o;
        int nw;
        conv_options_default(&o);
        o.comment_macros = 1;
        r = conv_opt("         MACRO\n&L       MYM &A\n         MEND\n         MYM 1\n", &o, &nw);
        CHECK(strstr(r, "*          MACRO\n") != NULL);
        CHECK(strstr(r, "* &L       MYM &A\n") != NULL);
        CHECK(strstr(r, "         MYM   1\n") != NULL);
        CHECK_MSG(nw == 2, "macro warnings %d", nw);
    }
    /* a macro that cannot be converted is commented out with the reason */
    r = conv("         MACRO\n         PUN\n         LCLA &A\n&A       SETAF 'X'\n         MEND\n");
    CHECK(strstr(r, "*          MACRO\n") != NULL);
    /* open code conditional assembly becomes a macro OPENCODE that is called in place */
    r = conv("         LCLA  &I\n&I       SETA  1\n&N       SETC  'wr'\nL&N      DS    0H\n         AIF   (&I EQ 2).X\n"
             "         LR    1,2\n.X       ANOP\n         END\n");
    CHECK_MSG(strstr(r, "#macro OPENCODE() {") != NULL && strstr(r, "\n         OPENCODE\n") != NULL, "open code:\n%s", r);
    CHECK_MSG(strstr(r, "upper(") != NULL, "open code: the SETC value is folded to upper case:\n%s", r);
    CHECK(warns("         LCLA  &I\n&I       SETA  1\n") == 0);
    CHECK(warns("         AIF (&X).A\n") == 2); /* &X is not declared, .A is not defined */
    CHECK(warns("         SETAF 'X'\n") >= 1);
    /* open code that cannot be converted is commented out, but a MACRO..MEND
     * inside it was already emitted and must not appear a second time */
    {
        char *r = conv("         LCLA  &A\n&A       SETA  1\n         SETAF 'X'\n         AGO   .D\n"
                       ".D       ANOP\n         MACRO\n         M1    &P\n         DC    C'&P'\n"
                       "         MEND\n         END\n");
        const char *m = strstr(r, "#macro M1(p) {");
        CHECK_MSG(m != NULL, "macro in open code: converted:\n%s", r);
        CHECK_MSG(m == NULL || strstr(m + 1, "#macro M1") == NULL, "macro in open code: not repeated:\n%s", r);
    }
    CHECK(warns(" OPSYN X\n") == 1);
    /* an unknown operation (a macro call) */
    CHECK_STR(conv(" WTO 'x'\n"), "         WTO   'x'\n");
    CHECK(warns(" WTO 'x'\n") == 1);
    /* no warning for plain code */
    CHECK(warns("T CSECT\n LR 1,2\n DC F'1'\n END\n") == 0);
}

int main(void) {
    test_layout();
    test_columns();
    test_remarks();
    test_registers();
    test_relaxed();
    test_width();
    test_unconverted();
    TEST_MAIN_END();
}
