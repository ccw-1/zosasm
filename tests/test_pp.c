/* test_pp.c - the preprocessor. */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>

#include "../src/pp.h"
#include "testlib.h"

typedef struct {
    char *text;  /* output lines joined with '|' */
    char *codes; /* diagnostic codes in report order, space separated */
    Reporter *rep;
} Out;

static Out run_pp(const char *src, const char *const *defs, const char *const *incs) {
    PpOptions o;
    SrcVec lines = {0};
    Reporter *rep = rep_new(0);
    Buf t = {0}, c = {0};
    size_t i, n;
    Out out;
    memset(&o, 0, sizeof o);
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
    expect("quote inside macro body", "#define M DC C'a b'\nM\n", "DC C'a b'", "");
    expect("__LINE__", "\n X __LINE__\n", "| X 2", "");
    expect("__FILE__", " X __FILE__\n", " X t.za", "");
    expect("line splice", "#define N \\\n  42\n X N\n", " X 42", "");
    expect("splice in an ordinary line", " DC A(1,\\\n2)\n", " DC A(1,2)", "");
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
    test_conditionals();
    test_directives();
    test_comma();
    test_command_line();
    test_include();
    test_expansion_note();
    test_assemble();
    TEST_MAIN_END();
}
