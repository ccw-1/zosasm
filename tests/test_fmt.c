/* test_fmt.c - the .za source formatter (zafmt). */
#include <stdlib.h>
#include <string.h>

#include "../src/fmt.h"
#include "../src/util.h"
#include "testlib.h"

static char *fmt(const char *text) {
    FmtOptions o;
    fmt_options_default(&o);
    return za_format(text, strlen(text), &o);
}

/* the result must stay the same when it is formatted again */
static char *fmt_idem(const char *text) {
    char *a = fmt(text), *b = fmt(a);
    CHECK_MSG(!strcmp(a, b), "not idempotent:\n%s--\n%s", a, b);
    return a;
}

static void test_fields(void) {
    CHECK_STR(fmt_idem("loop lr 1,2\n"), "loop     lr    1,2\n");
    CHECK_STR(fmt_idem("\tLR\t1,2\tremark\n"), "         LR    1,2                remark\n");
    CHECK_STR(fmt_idem("X DS 0H\n"), "X        DS    0H\n");
    /* a long name pushes the operation right, the operands stay in their column */
    CHECK_STR(fmt_idem("END_TABLE DS 0F\n"), "END_TABLE DS   0F\n");
    CHECK_STR(fmt_idem("A_VERY_LONG_NAME_HERE DC F'1'\n"), "A_VERY_LONG_NAME_HERE DC F'1'\n");
    /* a long operation */
    CHECK_STR(fmt_idem(" CSECT\n"), "         CSECT\n");
    CHECK_STR(fmt_idem(" DROP %r1\n"), "         DROP  %r1\n");
    /* blanks inside quotes and parentheses are not field separators */
    CHECK_STR(fmt_idem(" DC C'a   b',A(1 + 2) rem   ark\n"), "         DC    C'a   b',A(1 + 2)  rem   ark\n");
    CHECK_STR(fmt_idem("\"a b\" DS 0H\n"), "\"a b\"    DS    0H\n");
    /* the line is not touched when a quote is open */
    CHECK_STR(fmt("  DC C'abc  \n"), "  DC C'abc\n");
}

static void test_anchor(void) {
    /* a line that starts with ^ is kept as written, with its continuation lines */
    CHECK_STR(fmt_idem("  ^LB1 dc  a(1),\n     a(2)   r\n lr 1,2\n"), "  ^LB1 dc  a(1),\n     a(2)   r\n         lr    1,2\n");
    CHECK_STR(fmt_idem("#macro M(n) {\n  ^L&n   DC  A(&n)\n  LB:  DC X'0'\n#}\n"),
              "#macro M(n) {\n  ^L&n   DC  A(&n)\n    LB: DC    X'0'\n#}\n");
}

static void test_block_comments(void) {
    /* lines that touch a block comment are kept, and so are the continuation lines of a statement */
    CHECK_STR(fmt_idem("/* a  comment\n   over  lines */\n lr 1,2 /* r */\n lr 3,4\n"),
              "/* a  comment\n   over  lines */\n lr 1,2 /* r */\n         lr    3,4\n");
    CHECK_STR(fmt_idem(" dc a(1), /* c */\n   a(2)   r\n lr 1,2\n"), " dc a(1), /* c */\n   a(2)   r\n         lr    1,2\n");
    CHECK_STR(fmt_idem(" dc a(1),\n /* c */ a(2)\n lr 1,2\n"), "         dc    a(1),\n /* c */ a(2)\n         lr    1,2\n");
}

static void test_remarks(void) {
    /* remarks of neighbours that reach the remark column are aligned with each other */
    CHECK_STR(fmt_idem(" LA 1,A_LONG_OPERAND_FIELD_HERE r1\n LA 1,X r2\n LA 2,A_LONG_OPERAND_FIELD_HERE_2 r3\n"),
              "         LA    1,A_LONG_OPERAND_FIELD_HERE   r1\n"
              "         LA    1,X                r2\n"
              "         LA    2,A_LONG_OPERAND_FIELD_HERE_2 r3\n");
    /* text from the remark column on is a remark, also after an operation without operands */
    CHECK_STR(fmt_idem("         LTORG                    the pool\n"), "         LTORG                    the pool\n");
}

static void test_continuation(void) {
    CHECK_STR(fmt_idem(" MVC 0(8,1),\n   0(2)   more\n LR 1,2\n"),
              "         MVC   0(8,1),\n               0(2)               more\n         LR    1,2\n");
}

static void test_comments_blank(void) {
    CHECK_STR(fmt_idem("* a  comment   \n; semi\n// x\n\n\n\n\n LR 1,2\n\n\n"), "* a  comment\n; semi\n// x\n\n\n         LR    1,2\n");
    CHECK_STR(fmt("\r\n LR 1,2\r\n"), "\n         LR    1,2\n");
    CHECK_STR(fmt(""), "");
    /* the data record after REPRO is kept */
    CHECK_STR(fmt_idem(" REPRO\n  raw   data  \n LR 1,2\n"), "         REPRO\n  raw   data  \n         LR    1,2\n");
}

static void test_blocks(void) {
    const char *src =
        "#macro M(a,\n   b) {\n// c\n#if (a) {\nL: LR 1,2 r\n#} else {\n   LR 3,4\n#}\n#switch (a) {\n#case \"A\":\n"
        "DC C'a'\n#default:\n DC A(&{a + 1})\n#}\n#}\n#define X 1\n LR 1,2\n";
    const char *want =
        "#macro M(a,\n   b) {\n    // c\n    #if (a) {\n        L: LR    1,2              r\n    #} else {\n"
        "        LR    3,4\n    #}\n    #switch (a) {\n    #case \"A\":\n        DC    C'a'\n    #default:\n"
        "        DC    A(&{a + 1})\n    #}\n#}\n#define X 1\n         LR    1,2\n";
    CHECK_STR(fmt_idem(src), want);
    /* a classic #if does not indent anything */
    CHECK_STR(fmt_idem("#if X\n LR 1,2\n#else\n LR 3,4\n#endif\n"), "#if X\n         LR    1,2\n#else\n         LR    3,4\n#endif\n");
    /* open code blocks */
    CHECK_STR(fmt_idem("#for (let i = 0; i < 3; i++) {\n DC A(&i)\n#}\n"), "#for (let i = 0; i < 3; i++) {\n    DC    A(&i)\n#}\n");
    /* a multi line # statement keeps its shape */
    CHECK_STR(fmt_idem("#macro M() {\n  #let x = f(1,\n          2)\n#}\n"), "#macro M() {\n    #let x = f(1,\n            2)\n#}\n");
}

static void test_options(void) {
    FmtOptions o;
    char *r;
    fmt_options_default(&o);
    o.op_col = 12;
    o.remark_col = 40;
    o.indent = 2;
    {
        const char *in = "L LR 1,2 r\n#macro M() {\n LR 1,2\n#}\n";
        r = za_format(in, strlen(in), &o);
    }
    CHECK_STR(r, "L          LR    1,2                   r\n#macro M() {\n  LR    1,2\n#}\n");
}

int main(void) {
    test_fields();
    test_remarks();
    test_block_comments();
    test_anchor();
    test_continuation();
    test_comments_blank();
    test_blocks();
    test_options();
    printf("tests/test_fmt.c: %d checks, %d failed\n", t_count, t_fail);
    return t_fail != 0;
}
