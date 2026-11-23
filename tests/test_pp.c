/* test_pp.c - the preprocessor. */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>

#include "../src/pp.h"
#include "../src/version.h"
#include "testlib.h"

typedef struct {
    char *text;  /* output lines joined with '|' */
    char *codes; /* diagnostic codes in report order, space separated */
    Reporter *rep;
} Out;

static void put_file(const char *path, const char *text);
static int g_repro, g_has_epoch;
static int64_t g_epoch;

static Out run_pp(const char *src, const char *const *defs, const char *const *incs) {
    PpOptions o;
    SrcVec lines = {0};
    Reporter *rep = rep_new(0);
    Buf t = {0}, c = {0};
    size_t i, n;
    Out out;
    memset(&o, 0, sizeof o);
    o.reproducible = g_repro;
    o.has_epoch = g_has_epoch;
    o.epoch = g_epoch;
    for (; defs && *defs; defs++) vec_push(&o.defines, *defs);
    for (; incs && *incs; incs++) vec_push(&o.include_dirs, *incs);
    rep_add_source(rep, "t.za", src, strlen(src));
    pp_run("t.za", src, strlen(src), &o, rep, &lines);
    n = lines.n;
    for (i = 0; i < n; i++) {
        if (i) buf_putc(&t, '|');
        buf_puts(&t, lines.v[i].text);
    }
    for (i = 0; i < rep_count(rep); i++) {
        if (i) buf_putc(&c, ' ');
        buf_puts(&c, rep_get(rep, i)->code);
    }
    out.text = xstrdup(buf_cstr(&t));
    out.codes = xstrdup(buf_cstr(&c));
    out.rep = rep;
    return out;
}

static void expect(const char *name, const char *src, const char *want_text,
                   const char *want_codes) {
    Out o = run_pp(src, NULL, NULL);
    CHECK_MSG(!strcmp(o.text, want_text), "%s: output \"%s\", want \"%s\"", name, o.text,
              want_text);
    CHECK_MSG(!strcmp(o.codes, want_codes), "%s: codes \"%s\", want \"%s\"", name, o.codes,
              want_codes);
}

static void test_macros(void) {
    expect("object macro", "#define N 10\n LHI 1,N\n", " LHI 1,10", "");
    expect("empty macro", "#define E\n LHI 1,E5\n DC E\n", " LHI 1,E5| DC ", "");
    expect("function macro", "#define SAVE(a,b) STM a,b,12(13)\n SAVE(14,12)\n",
           " STM 14,12,12(13)", "");
    expect("args are trimmed", "#define F(a,b) a+b\n X F( 1 , 2 )\n", " X 1+2", "");
    expect("nested parens in args", "#define F(a) [a]\n X F(1(2,3))\n", " X [1(2,3)]", "");
    expect("commas inside quotes", "#define F(a) [a]\n X F(C'A,B')\n", " X [C'A,B']", "");
    expect("function macro without call", "#define F(a) [a]\n X F\n", " X F", "");
    expect("macro in macro", "#define A 1\n#define B A+A\n X B\n", " X 1+1", "");
    expect("self reference", "#define A A+1\n X A\n", " X A+1", "");
    expect("mutual reference", "#define A B\n#define B A\n X A\n", " X A", "");
    expect("paste", "#define CAT(x,y) x##y\n X CAT(AB,CD),CAT( 1 , 2 )\n", " X ABCD,12", "");
    expect("argument expansion", "#define N 3\n#define F(a) (a)\n X F(N)\n", " X (3)", "");
    expect("redefinition is a warning", "#define N 1\n#define N 2\n X N\n", " X 2", "ZA4023");
    expect("identical redefinition is silent", "#define N 1\n#define N 1\n", "", "");
    expect("undef", "#define N 1\n#undef N\n X N\n", " X N", "");
    expect("define needs a name", "#define\n", "", "ZA4020");
    expect("bad params", "#define F(a,) x\n", "", "ZA4022");
    expect("line break in body", "#define P STM 14,12,12(13)\\n LR 12,15\\n USING M,12\nP\n",
           "STM 14,12,12(13)| LR 12,15| USING M,12", "");
    expect("line break keeps the label", "#define P NOP\\nL2 NOP2\n X P\n", " X NOP|L2 NOP2", "");
    expect("wrong argument count", "#define F(a,b) a b\n X F(1)\n", " X F(1)", "ZA4040");
    expect("unterminated call", "#define F(a) a\n X F(1\n", " X F(1", "ZA4041");
    expect("empty argument list for zero params", "#define F() 7\n X F()\n", " X 7", "");
}

static void test_no_expansion(void) {
    expect("quoted string", "#define N 10\n DC C'N',X'N'\n", " DC C'N',X'N'", "");
    expect("attribute reference", "#define L 9\n DC A(L'X)\n", " DC A(L'X)", "");
    expect("remark", "#define N 10\n LHI 1,N   N is here\n", " LHI 1,10   N is here", "");
    expect("comment line", "#define N 10\n* N\n; N\n", "* N|; N", "");
    expect("label with #", "#define COUNT 1\n#COUNT   DC F'1'\n", "#COUNT   DC F'1'", "");
    expect("name field expands", "#define N 5\nN EQU 1\n", "5 EQU 1", "");
    expect("number suffix", "#define E 1\n DC F'1E5',1E\n", " DC F'1E5',1E", "");
    expect("op field with blanks in parens", "#define S(a,b) STM a,b,0(13)\n S(1, 2)\n",
           " STM 1,2,0(13)", "");
    expect("quoted symbol", "#define N 10\n LHI 1,\"N\"\n\"N\" EQU 1\n", " LHI 1,\"N\"|\"N\" EQU 1", "");
    expect("quoted symbol in a macro body", "#define M DC A(\"a b\"),N\n#define N 3\nM\n",
           "DC A(\"a b\"),3", "");
    expect("quoted symbol with a quote", "#define N 1\n DC A(\"x\\\"N\")\n", " DC A(\"x\\\"N\")", "");
    expect("quoted symbol as macro argument", "#define F(a) [a]\n X F(\"a,b\")\n", " X [\"a,b\"]", "");
    expect("quote inside macro body", "#define M DC C'a b'\nM\n", "DC C'a b'", "");
    expect("__LINE__", "\n X __LINE__\n", "| X 2", "");
    expect("__FILE__", " X __FILE__\n", " X t.za", "");
    expect("line splice", "#define N \\\n  42\n X N\n", " X 42", "");
    expect("splice in an ordinary line", " DC A(1,\\\n2)\n", " DC A(1,2)", "");
}

static void test_anchor(void) {
    /* "^" starts the statement as if it stood in column 1 */
    expect("anchor expands", "#define N 10\n   ^L1  LHI 1,N   N is here\n", "   ^L1  LHI 1,10   N is here", "");
    expect("anchor comment", "#define N 10\n  ^* N\n", "  ^* N", "");
    expect("anchor macro call", "#define N 10\n#macro MM(n) {\n    ^L&n  DC  A(&n)\n    LB&n: DC  X'00'\n    ^     DC  X'01'\n#}\n"
           "    ^X1  MM N\n",
           "L10      DC    A(10)|LB10     DC    X'00'|         DC    X'01'", "");
    expect("anchor continuation", " ^A DC A(1),\n   A(2)\n LR 1,2\n", " ^A DC A(1),|   A(2)| LR 1,2", "");
}

static void test_block_comments(void) {
    /* the comment is blanked, the columns of the text around it stay */
    expect("one line", " LR 1,2 /* c */ remark\n", " LR 1,2         remark", "");
    expect("several lines", "/* a\n b */ LR 1,2\n X\n", "    |      LR 1,2| X", "");
    expect("in a string", " DC C'/* x */',\"/*\"\n", " DC C'/* x */',\"/*\"", "");
    expect("attribute quote", " DC A(L'X /* c */)\n", " DC A(L'X        )", "");
    expect("comment line", "* /* not a comment\n LR 1,2\n", "* /* not a comment| LR 1,2", "");
    expect("indented comment line", "    */* not a comment */\n LR 1,2\n", "    */* not a comment */| LR 1,2", "");
    expect("after //", " LR 1,2 // /* not\n LR 3,4\n", " LR 1,2 // /* not| LR 3,4", "");
    expect("define", "#define N 10 /* ten */\n LHI 1,N\n", " LHI 1,10", "");
    expect("macro body", "#macro MM(n) {\n/* a note\n   on two lines */\n    LR  1,&n /* r */\n#}\n    MM 2\n",
           "         LR    1,2", "");
    expect("not closed", " LR 1,2\n /* open\n LR 3,4\n", " LR 1,2|        |       ", "ZA4036");
}

static void test_block_comment_boundaries(void) {
    const char *incs[] = {"build/pp_inc", NULL};
    CHECK(system("rm -rf build/pp_inc && mkdir -p build/pp_inc") == 0);
    /* block comment in an #include file: ZA4036 names the include file */
    put_file("build/pp_inc/bc.inc", "/* open in include\n");
    {
        Out o = run_pp("#include \"bc.inc\"\n LR 1,2\n", NULL, incs);
        CHECK_MSG(strstr(o.codes, "ZA4036") != NULL, "ZA4036 for unclosed comment in include: %s", o.codes);
    }
    /* unclosed block comment at EOF */
    {
        Out o = run_pp(" LR 1,2\n /* open at eof\n", NULL, NULL);
        CHECK_MSG(strstr(o.codes, "ZA4036") != NULL, "ZA4036 for unclosed comment at EOF: %s", o.codes);
    }
    /* comment that ends exactly at the last line */
    {
        Out o = run_pp(" LR 1,2\n /* closed at eof */\n", NULL, NULL);
        CHECK_MSG(!strcmp(o.text, " LR 1,2|                    "), "comment closed at EOF: %s", o.text);
    }
    /* unclosed comment in an include is reported (ZA4036); it does not
     * carry over to the including file */
    put_file("build/pp_inc/bc2.inc", "/* open in include\n");
    {
        Out o = run_pp("#include \"bc2.inc\"\nmore */\n LR 1,2\n", NULL, incs);
        CHECK_MSG(strstr(o.codes, "ZA4036") != NULL, "ZA4036 for unclosed comment in include: %s", o.codes);
        /* the main-file text after the include is still processed */
        CHECK_MSG(strstr(o.text, " LR 1,2") != NULL, "main text after include processed: %s", o.text);
    }
}

static void test_conditionals(void) {
    expect("ifdef taken", "#define A\n#ifdef A\n yes\n#else\n no\n#endif\n", " yes", "");
    expect("ifdef not taken", "#ifdef A\n yes\n#else\n no\n#endif\n", " no", "");
    expect("ifndef", "#ifndef A\n yes\n#endif\n", " yes", "");
    expect("if number", "#if 1\n a\n#endif\n#if 0\n b\n#endif\n", " a", "");
    expect("elif", "#if 0\n a\n#elif 1\n b\n#elif 1\n c\n#else\n d\n#endif\n", " b", "");
    expect("else after elif", "#if 0\n a\n#elif 0\n b\n#else\n d\n#endif\n", " d", "");
    expect("nested skipped", "#if 0\n#if 1\n a\n#else\n b\n#endif\n#else\n c\n#endif\n", " c", "");
    expect("arithmetic", "#if (2+3)*4 == 20 && 7 % 4 == 3 && 9 / 2 == 4\n ok\n#endif\n", " ok",
           "");
    expect("comparison", "#if 3 > 2 && 2 >= 2 && 1 < 2 && 2 <= 2 && 1 != 2\n ok\n#endif\n", " ok",
           "");
    expect("logic", "#if !0 && (0 || 1)\n ok\n#endif\n", " ok", "");
    expect("unary", "#if -1 < 0 && ~0 == -1 && +2 == 2\n ok\n#endif\n", " ok", "");
    expect("hex", "#if 0x10 == 16\n ok\n#endif\n", " ok", "");
    expect("defined", "#define A\n#if defined(A) && !defined B && defined ( A )\n ok\n#endif\n",
           " ok", "");
    expect("macro value in if", "#define V 3\n#if V == 3\n ok\n#endif\n", " ok", "");
    expect("unknown name is 0", "#if FOO\n a\n#else\n b\n#endif\n", " b", "");
    expect("no error in skipped code", "#if 0\n#if 1/0\n#endif\n#error no\n#bogus\n#endif\n",
           "", "");
    expect("short circuit", "#if 0 && 1/0\n a\n#else\n b\n#endif\n", " b", "");
    expect("division by zero", "#if 1/0\n#endif\n", "", "ZA4034");
    expect("bad expression", "#if 1 +\n#endif\n", "", "ZA4030");
    expect("trailing text", "#if 1 2\n#endif\n", "", "ZA4030");
    expect("else without if", "#else\n", "", "ZA4031");
    expect("endif without if", "#endif\n", "", "ZA4031");
    expect("elif after else", "#if 1\n#else\n#elif 1\n#endif\n", "", "ZA4032");
    expect("double else", "#if 1\n#else\n#else\n#endif\n", "", "ZA4032");
    expect("unterminated if", "#if 1\n a\n", " a", "ZA4033");
    /* deeply nested parentheses must hit the depth limit, not the C stack
     * (a crash was here once) */
    {
        Buf b = {0};
        int i;
        Out o;
        buf_puts(&b, "#if ");
        for (i = 0; i < 300; i++) buf_putc(&b, '(');
        buf_puts(&b, "1");
        for (i = 0; i < 300; i++) buf_putc(&b, ')');
        buf_puts(&b, "\n a\n#endif\n");
        o = run_pp(buf_cstr(&b), NULL, NULL);
        CHECK_MSG(!strcmp(o.codes, "ZA4030"), "if depth: codes \"%s\"", o.codes);
    }
    /* a number that does not fit in 64 bits is an error, not a wrapped value */
    expect("number too large", "#if 99999999999999999999999\n#endif\n", "", "ZA4030");
    expect("defines in skipped code are ignored", "#if 0\n#define A\n#endif\n#ifdef A\n x\n#endif\n",
           "", "");
}

static void test_directives(void) {
    expect("error", "#error bad thing\n", "", "ZA4050");
    expect("warning", "#warning careful\n", "", "ZA4051");
    expect("pragmas",
           "#pragma title \"my program\"\n#pragma eject\n#pragma space 2\n#pragma print off\n", "",
           "");
    expect("bad pragma operand", "#pragma title nope\n#pragma print maybe\n#pragma space x\n", "",
           "ZA4061 ZA4061 ZA4061");
    expect("unknown pragma", "#pragma ttle \"x\"\n", "", "ZA4060");
    expect("not a directive", "#define2 DC X'1'\n#Define\n", "#define2 DC X'1'|#Define", "");
    expect("include syntax", "#include foo\n", "", "ZA4010");
    expect("include not found", "#include \"nope.za\"\n", "", "ZA4011");
}

static void test_comma(void) {
    expect("legit continuation", " DC A(1),\n    A(2)\n", " DC A(1),|    A(2)", "");
    expect("comma then column 1", " DC A(1),\nLBL DC A(2)\n", " DC A(1),|LBL DC A(2)", "ZA4070");
    expect("comma then comment", " DC A(1),\n* c\n    A(2)\n", " DC A(1),|* c|    A(2)", "");
    expect("comma in remark", " LR 1,2 remark,\nLBL DC A(2)\n", " LR 1,2 remark,|LBL DC A(2)", "");
}

static void test_builtins(void) {
    Out o;
    const char *incs[] = {"build/pp_inc", NULL};
    expect("__COUNTER__", " A __COUNTER__\n B __COUNTER__,__COUNTER__\n", " A 0| B 1,2", "");
    expect("__COUNTER__ in #if", "#if __COUNTER__ == 0 && __COUNTER__ == 1\n yes\n#endif\n", " yes",
           "");
    expect("__COUNTER__ in a macro", "#define N __COUNTER__\n X N,N\n", " X 0,1", "");
    expect("__COUNTER__ in a quote", " DC C'__COUNTER__'\n", " DC C'__COUNTER__'", "");
    {
        int v = ZASM_VERSION_NUM; /* keep this test in step with version.h */
        char want[80];
        snprintf(want, sizeof want, " X %d", v);
        expect("__ZASM_VERSION__", " X __ZASM_VERSION__\n", want, "");
        snprintf(want, sizeof want, "#if __ZASM_VERSION__ >= %d\n yes\n#else\n no\n#endif\n", v);
        expect("version in #if", want, " yes", "");
    }
    expect("__BASE_FILE__", " X __BASE_FILE__,__FILE__\n", " X t.za,t.za", "");
    expect("__INCLUDE_LEVEL__", " X __INCLUDE_LEVEL__\n", " X 0", "");
    expect("defined(builtin)",
           "#ifdef __LINE__\n a\n#endif\n#if defined(__COUNTER__)\n b\n#endif\n", " a| b", "");
    expect("builtin cannot be defined", "#define __COUNTER__ 5\n X __COUNTER__\n", " X 0",
           "ZA4021");
    expect("__AMODE__ and __RMODE__ start at 0", " X __AMODE__,__RMODE__\n", " X 0,0", "");
    expect("AMODE and RMODE set them",
           "S CSECT\n X __AMODE__,__RMODE__\nS AMODE 31\n X __AMODE__,__RMODE__\n"
           "S RMODE 64\n X __AMODE__,__RMODE__\n AMODE 24\n X __AMODE__,__RMODE__\n",
           "S CSECT| X 0,0|S AMODE 31| X 31,0|S RMODE 64| X 31,64| AMODE 24| X 24,64", "");
    expect("RMODE ANY is 31, AMODE ANY is unknown",
           " AMODE ANY\n X __AMODE__\n RMODE any\n X __RMODE__\n", " AMODE ANY| X 0| RMODE any| X 31",
           "");
    expect("a #define of the operand",
           "#define M 64\nS AMODE M\n#if __AMODE__ == 64\n yes\n#endif\n", "S AMODE 64| yes", "");
    expect("a skipped AMODE changes nothing",
           "#if 0\n AMODE 24\n#endif\n X __AMODE__\n", " X 0", "");
    expect("defined(__AMODE__)", "#ifdef __RMODE__\n a\n#endif\n", " a", "");
    expect("builtin cannot be defined 2", "#define __DATE__ 5\n", "", "ZA4021");
    CHECK(system("rm -rf build/pp_inc && mkdir -p build/pp_inc") == 0);
    put_file("build/pp_inc/l.inc", " IN __INCLUDE_LEVEL__,__BASE_FILE__,__FILE__\n"
                                   "#include \"l2.inc\"\n");
    put_file("build/pp_inc/l2.inc", " IN2 __INCLUDE_LEVEL__\n");
    o = run_pp(" TOP __INCLUDE_LEVEL__\n#include <l.inc>\n", NULL, incs);
    CHECK_MSG(!strcmp(o.text, " TOP 0| IN 1,t.za,build/pp_inc/l.inc| IN2 2"),
              "include level: \"%s\" (%s)", o.text, o.codes);
}

static void test_stringify(void) {
    expect("#(x)", "#define STR(x) C ## #(x)\n DC STR(abc)\n", " DC C'abc'", "");
    expect("#(x) with blanks", "#define S(x) #( x )\n X S(a b)\n", " X 'a b'", "");
    expect("#(x) doubles quotes", "#define S(x) #(x)\n X S(C'A')\n", " X 'C''A'''", "");
    expect("#(x) expands the argument", "#define N 10\n#define S(x) #(x)\n X S(N)\n", " X '10'",
           "");
    expect("#(x) of a built-in", "#define S(x) #(x)\n X S(__LINE__),S(__FILE__)\n",
           " X '2','t.za'", "");
    expect("#(x) second parameter", "#define S(a,b) a #(b) #(a)\n X S(1,two)\n", " X 1 'two' '1'",
           "");
    expect("#(x) pasted after", "#define S(x) #(x) ## Z\n X S(a)\n", " X 'a'Z", "");
    expect("#( not a parameter", "#define S(x) #(y) #(x+1)\n X S(a)\n", " X #(y) #(a+1)", "");
    expect("#( in an object macro", "#define S #(x)\n X S\n", " X #(x)", "");
    expect("# label is kept", "#define S(x) #X x\n X S(a)\n", " X #X a", "");
    expect("empty argument", "#define S(x) #(x)\n X S()\n", " X ''", "");
}

static void test_date(void) {
    Out o;
    g_has_epoch = 1;
    g_epoch = 1700000000; /* 2023-11-14 22:13:20 UTC */
    expect("__DATE__", " X __DATE__,__TIME__\n", " X 2023-11-14,22:13:20", "");
    g_epoch = 0;
    expect("epoch 0", " X __DATE__,__TIME__\n", " X 1970-01-01,00:00:00", "");
    g_epoch = 951782399; /* 2000-02-28 23:59:59 */
    expect("leap year", " X __DATE__\n", " X 2000-02-28", "");
    g_has_epoch = 0;
    setenv("SOURCE_DATE_EPOCH", "1700000000", 1);
    expect("SOURCE_DATE_EPOCH", " X __DATE__,__TIME__\n", " X 2023-11-14,22:13:20", "");
    g_repro = 1;
    expect("reproducible with SOURCE_DATE_EPOCH", " X __DATE__\n", " X 2023-11-14", "");
    setenv("SOURCE_DATE_EPOCH", "12x", 1);
    o = run_pp(" X __DATE__\n", NULL, NULL);
    CHECK_STR(o.codes, "ZA4081");
    setenv("SOURCE_DATE_EPOCH", "", 1);
    o = run_pp(" X __DATE__,__TIME__\n", NULL, NULL);
    CHECK_MSG(!strcmp(o.codes, "ZA4080"), "reproducible without a time: \"%s\"", o.codes);
    o = run_pp(" X __LINE__,__COUNTER__\n", NULL, NULL);
    CHECK_STR(o.codes, ""); /* the stable built-ins stay allowed */
    g_repro = 0;
    unsetenv("SOURCE_DATE_EPOCH");
    o = run_pp(" X __DATE__\n", NULL, NULL);
    CHECK_STR(o.codes, "");
    CHECK_MSG(strlen(o.text) == 13 && o.text[7] == '-' && o.text[10] == '-', "clock date \"%s\"",
              o.text);
}

static void test_command_line(void) {
    const char *defs[] = {"A", "B=7", "F(x)=x+x", NULL};
    Out o = run_pp(" DC A,B,F(2)\n#ifdef A\n yes\n#endif\n", defs, NULL);
    CHECK_STR(o.text, " DC 1,7,2+2| yes");
    CHECK_STR(o.codes, "");
}

static void put_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void test_include(void) {
    Out o;
    const char *incs[] = {"build/pp_inc", NULL};
    const char *src = "#include \"a.inc\"\n#include <b.inc>\n#include \"sub/c.inc\"\n X N\n";
    CHECK(system("rm -rf build/pp_inc && mkdir -p build/pp_inc/sub") == 0);
    put_file("build/pp_inc/b.inc", "#define N 5\n B_LINE\n");
    put_file("build/pp_inc/sub/c.inc", "#include \"d.inc\"\n C_LINE\n");
    put_file("build/pp_inc/sub/d.inc", " D_LINE N\n");
    o = run_pp(src, NULL, incs);
    CHECK_MSG(!strcmp(o.codes, "ZA4011"), "missing a.inc: codes \"%s\"", o.codes);
    put_file("build/pp_inc/a.inc", " A_LINE\n");
    o = run_pp(src, NULL, incs);
    CHECK_MSG(!strcmp(o.text, " A_LINE| B_LINE| D_LINE 5| C_LINE| X 5"), "include: \"%s\" (%s)",
              o.text, o.codes);
    CHECK_STR(o.codes, "");
    o = run_pp("#include <sub/c.inc>\n", NULL, incs);
    CHECK_MSG(!strcmp(o.text, " D_LINE N| C_LINE"), "nested include: \"%s\" (%s)", o.text, o.codes);
    put_file("build/pp_inc/r.inc", "#include \"r.inc\"\n");
    o = run_pp("#include <r.inc>\n", NULL, incs);
    CHECK_MSG(strstr(o.codes, "ZA4012") != NULL, "recursive include: \"%s\"", o.codes);
}

static void test_expansion_note(void) {
    const char *src = "#define BAD LA 1,NOPE\nM CSECT\n BAD\n";
    FILE *f = tmpfile();
    char buf[4096];
    size_t n;
    Module *m;
    SrcVec lines = {0};
    PpOptions po;
    Reporter *rep = rep_new(0);
    memset(&po, 0, sizeof po);
    rep_add_source(rep, "t.za", src, strlen(src));
    pp_run("t.za", src, strlen(src), &po, rep, &lines);
    m = asm_assemble(lines.v, lines.n, "t.za", rep);
    CHECK(m == NULL);
    rep_print(rep, f, 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(strstr(buf, "ZA1011") != NULL);
    CHECK(strstr(buf, "3 |  LA 1,NOPE") != NULL);
    CHECK(strstr(buf, "in expansion of macro \"BAD\"") != NULL);
    CHECK(strstr(buf, "3 |  BAD") != NULL);
    CHECK(strstr(buf, "1 | #define BAD") != NULL);
}

static void test_assemble(void) {
    /* the whole pipeline: macros produce a working program */
    const char *src = "#define LEN 8\n"
                      "#define ZERO(f) XC f(LEN),f\n"
                      "M        CSECT\n"
                      "         ZERO(BUF)\n"
                      "         BR    14\n"
                      "BUF      DS    CL(LEN)\n";
    Out o = run_pp(src, NULL, NULL);
    CHECK_STR(o.codes, "");
    CHECK(strstr(o.text, "XC BUF(8),BUF") != NULL);
}

int main(void) {
    test_macros();
    test_no_expansion();
    test_anchor();
    test_block_comments();
    test_block_comment_boundaries();
    test_conditionals();
    test_directives();
    test_comma();
    test_builtins();
    test_stringify();
    test_date();
    test_command_line();
    test_include();
    test_expansion_note();
    test_assemble();
    TEST_MAIN_END();
}
