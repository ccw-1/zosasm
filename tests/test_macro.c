/* test_macro.c - the C-like macro language (MACRO.md). */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>

#include "../src/pp.h"
#include "testlib.h"

typedef struct {
    char *text;  /* output lines joined with '|', blanks collapsed */
    char *codes; /* diagnostic codes in report order, space separated */
} Out;

/* collapse runs of blanks to one and drop leading blanks, so expected strings stay readable */
static char *norm(const char *s) {
    Buf b = {0};
    int bol = 1, blank = 0;
    for (; *s; s++) {
        if (*s == '|') {
            buf_putc(&b, '|');
            bol = 1;
            blank = 0;
        } else if (*s == ' ') {
            blank = !bol;
        } else {
            if (blank) buf_putc(&b, ' ');
            blank = 0;
            bol = 0;
            buf_putc(&b, *s);
        }
    }
    return xstrdup(buf_cstr(&b));
}

static int g_keep_hide; /* keep ZA4106 "hides an instruction" in the codes */

static Out run(const char *src, const char *const *mdirs, long steps) {
    PpOptions o;
    SrcVec lines = {0};
    Reporter *rep = rep_new(0);
    Buf t = {0}, c = {0};
    size_t i;
    Out out;
    memset(&o, 0, sizeof o);
    for (; mdirs && *mdirs; mdirs++) vec_push(&o.macro_dirs, *mdirs);
    o.macro_steps = steps;
    rep_add_source(rep, "t.za", src, strlen(src));
    pp_run("t.za", src, strlen(src), &o, rep, &lines);
    for (i = 0; i < lines.n; i++) {
        if (i) buf_putc(&t, '|');
        buf_puts(&t, lines.v[i].text);
    }
    for (i = 0; i < rep_count(rep); i++) {
        const char *code = rep_get(rep, i)->code;
        if (!g_keep_hide && !strcmp(code, "ZA4106") && strstr(rep_get(rep, i)->msg, "hides")) continue;
        if (c.len) buf_putc(&c, ' ');
        buf_puts(&c, code);
    }
    out.text = norm(buf_cstr(&t));
    out.codes = xstrdup(buf_cstr(&c));
    return out;
}

static void expect(const char *name, const char *src, const char *want_text, const char *want_codes) {
    Out o = run(src, NULL, 0);
    CHECK_MSG(!strcmp(o.text, want_text), "%s: output \"%s\", want \"%s\"", name, o.text, want_text);
    CHECK_MSG(!strcmp(o.codes, want_codes), "%s: codes \"%s\", want \"%s\"", name, o.codes, want_codes);
}

static void put_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void test_basic(void) {
    expect("simple macro", "#macro INC(r) {\n    AHI   &r,1\n#}\n    INC 5\n", "AHI 5,1", "");
    expect("two calls", "#macro Z(r) {\n    XR    &r,&r\n#}\n    Z 1\n    Z 2\n", "XR 1,1|XR 2,2", "");
    expect("name field", "#macro L() {\n    &sys.label:  DS 0H\n#}\nHERE L\n", "HERE DS 0H", "");
    expect("default parameter", "#macro D(a, b = 7) {\n    DC F'&a,&b'\n#}\n    D 1\n    D 1,2\n",
           "DC F'1,7'|DC F'1,2'", "");
    expect("keyword argument", "#macro K(a, b = 7) {\n    DC F'&a,&b'\n#}\n    K 1,b=3\n    K b=4,a=9\n",
           "DC F'1,3'|DC F'9,4'", "");
    expect("empty argument uses the default", "#macro D(a, b = 7, c = 8) {\n    DC F'&a,&b,&c'\n#}\n    D 1,,3\n",
           "DC F'1,7,3'", "");
    expect("too many arguments", "#macro D(a) {\n    DC F'&a'\n#}\n    D 1,2\n", "", "ZA4102");
    expect("macro comment", "#macro C() {\n    // never emitted\n    NOP   0\n#}\n    C\n",
           "NOP 0", "");
    expect("asm comment is emitted", "#macro C() {\n    * hello\n#}\n    C\n", "* hello", "");
    expect("&& is one &", "#macro C() {\n    DC    C'a&&b'\n#}\n    C\n", "DC C'a&b'", "");
    expect("&{expr}", "#macro C(n) {\n    DC    A(&{n * 2 + 1})\n#}\n    C 4\n", "DC A(9)", "");
    expect("redefinition is replaced", "#macro MYA() {\n    X1\n#}\n#macro MYA() {\n    X2\n#}\n    MYA\n", "X2", "ZA4106");
}

static void test_flow(void) {
    expect("if else",
           "#macro C(n) {\n    #if (n > 1) {\n        BIG\n    #} else if (n == 1) {\n        ONE\n"
           "    #} else {\n        ZERO\n    #}\n#}\n    C 2\n    C 1\n    C 0\n",
           "BIG|ONE|ZERO", "");
    expect("while break continue",
           "#macro W() {\n    #let i = 0\n    #while (i < 10) {\n        #i++\n        #if (i == 2) {\n"
           "            #continue\n        #}\n        #if (i > 4) {\n            #break\n        #}\n"
           "        DC    F'&i'\n    #}\n#}\n    W\n",
           "DC F'1'|DC F'3'|DC F'4'", "");
    expect("for with counter",
           "#macro F() {\n    #for (let i = 0; i < 3; i++) {\n        DC    A(&i)\n    #}\n"
           "    #for (let i = 5; i > 3; i--) {\n        DC    A(&i)\n    #}\n#}\n    F\n",
           "DC A(0)|DC A(1)|DC A(2)|DC A(5)|DC A(4)", "");
    expect("for over a list",
           "#macro F(regs) {\n    #for (let r : regs) {\n        STG   &r,0(,13)\n    #}\n#}\n    F (2,3,4)\n",
           "STG 2,0(,13)|STG 3,0(,13)|STG 4,0(,13)", "");
    expect("two loops in a row",
           "#macro F(a, b) {\n    #for (let x : a) {\n        A &x\n    #}\n    #for (let x : b) {\n"
           "        B &x\n    #}\n#}\n    F (1,2),(3)\n",
           "A 1|A 2|B 3", "");
    expect("switch",
           "#macro S(x) {\n    #switch (x) {\n    #case 1, 2:\n        LOW\n    #case \"A\":\n        ALPHA\n"
           "    #default:\n        OTHER\n    #}\n#}\n    S 1\n    S 2\n    S A\n    S Z\n",
           "LOW|LOW|ALPHA|OTHER", "");
    expect("goto and label",
           "#macro L(n) {\n    #let k = 0\n    #label top\n    #k++\n    #if (k < n) {\n        #goto top\n"
           "    #}\n    DC    F'&k'\n#}\n    L 3\n",
           "DC F'3'", "");
    expect("return", "#macro R(a) {\n    #if (a == 1) {\n        #return\n    #}\n    NOP\n#}\n    R 1\n    R 2\n",
           "NOP", "");
    expect("break outside a loop", "#macro R() {\n    #break\n#}\n", "", "ZA4100");
}

static void test_values(void) {
    expect("int plus numeric string adds",
           "#macro P(s) {\n    #let off = 20\n    #off += s\n    DC F'&off'\n#}\n    P 8\n", "DC F'28'", "");
    expect("string plus string joins",
           "#macro P(a, b) {\n    DC C'&{a + b}'\n#}\n    P 1,2\n", "DC C'12'", "");
    expect("string plus int joins when not numeric",
           "#macro P(a) {\n    DC C'&{a + 1}'\n#}\n    P X\n", "DC C'X1'", "");
    expect("list index and count",
           "#macro P(l) {\n    DC F'&{count(l)}',F'&l[1]'\n#}\n    P (5,6,7)\n", "DC F'3',F'6'", "");
    expect("nested list",
           "#macro P(l) {\n    DC F'&l[2][1]'\n#}\n    P (1,2,(3,4))\n", "DC F'4'", "");
    expect("string functions",
           "#macro P(s) {\n    DC C'&{upper(s)}',F'&{len(s)}',C'&{substr(s, 1, 2)}'\n#}\n    P abcd\n",
           "DC C'ABCD',F'4',C'bc'", "");
    expect("shorter string is lesser",
           "#macro P() {\n    #if (\"9\" < \"15\") {\n        YES\n    #} else {\n        NO\n    #}\n#}\n    P\n",
           "YES", "");
    expect("division by zero", "#macro P() {\n    #let x = 1 / 0\n#}\n    P\n", "", "ZA4112");
    expect("unknown identifier", "#macro P() {\n    #let x = nope + 1\n#}\n    P\n", "", "ZA4111");
    expect("unknown &name", "#macro P() {\n    DC &nope\n#}\n    P\n", "DC", "ZA4110");
    expect("redeclared", "#macro P() {\n    #let x = 1\n    #let x = 2\n#}\n    P\n", "", "ZA4100");
    expect("assign to undeclared", "#macro P() {\n    #y = 3\n#}\n    P\n", "", "ZA4111");
    expect("hidden variable warns",
           "#macro P() {\n    #let x = 1\n    #if (x) {\n        #let x = 2\n    #}\n#}\n    P\n", "", "ZA4107");
}

static void test_sys(void) {
    expect("sys.ndx is unique",
           "#macro N() {\n    DC C'&sys.ndx'\n#}\n    N\n    N\n", "DC C'0001'|DC C'0002'", "");
    expect("rest arguments",
           "#macro R(a, rest...) {\n    DC F'&{count(rest)}'\n    #for (let q : rest) {\n        DC F'&q'\n    #}\n#}\n"
           "    R 1,2,3,4\n",
           "DC F'3'|DC F'2'|DC F'3'|DC F'4'", "");
    expect("nested macros",
           "#macro IN(r) {\n    XR &r,&r\n#}\n#macro OUT(r) {\n    IN &r\n    IN &{r + 1}\n#}\n    OUT 3\n",
           "XR 3,3|XR 4,4", "");
    expect("global is shared",
           "#macro A() {\n    #global g = 0\n    #g += 1\n#}\n#macro B() {\n    #global g = 0\n    DC F'&g'\n#}\n"
           "    A\n    A\n    B\n",
           "DC F'2'", "");
    expect("recursion limit", "#macro R(n) {\n    R &n\n#}\n    R 1\n", "", "ZA4120");
    expect("call continued over lines",
           "#macro C(a, b, c) {\n    DC C'&a&b&c'\n#}\n    C A,\n         B,\n         C\n", "DC C'ABC'", "");
    expect("generated continuation",
           "#macro C(a) {\n    DC    C'&a',\n          C'x'\n#}\n    C A\n", "DC C'A',|C'x'", "");
    g_keep_hide = 1;
    expect("hidden instruction", "#macro LR(a) {\n    NOP\n#}\n    LR 1\n", "NOP", "ZA4106");
    g_keep_hide = 0;
}

static void test_messages(void) {
    expect("#error", "#macro E() {\n    #error \"bad\"\n#}\n    E\n", "", "ZA4050");
    expect("#warning", "#macro E() {\n    #warning \"hmm\"\n#}\n    E\n", "", "ZA4051");
    expect("#note", "#macro E() {\n    #note \"fyi\"\n#}\n    E\n", "", "ZA4052");
    expect("model in column 1", "#macro E() {\nNOP\n#}\n", "", "ZA4104");
    expect("unclosed block", "#macro E() {\n    #if (1) {\n#}\n", "", "ZA4100 ZA4100");
    expect("macro inside macro", "#macro E() {\n    #macro F() {\n    #}\n#}\n", "", "ZA4100 ZA4100");
}

static void test_open_code(void) {
    expect("open code for",
           "#for (let i = 0; i < 3; i++) {\n    DC    A(&i)\n#}\n", "DC A(0)|DC A(1)|DC A(2)", "");
    expect("open code variable",
           "#let n = 2\n#if (n > 1) {\n    BIG\n#} else {\n    SMALL\n#}\n", "BIG", "");
    expect("preprocessor #if is unchanged", "#if 1\n    A\n#else\n    B\n#endif\n", "A", "");
}

static void test_limits(void) {
    Out o = run("#macro L() {\n    #while (1) {\n    #}\n#}\n    L\n", NULL, 1000);
    CHECK_MSG(!strcmp(o.codes, "ZA4121"), "step limit: codes \"%s\"", o.codes);
}

static void test_library(void) {
    const char *dirs[] = {"build/mac_lib", NULL};
    Out o;
    CHECK(system("rm -rf build/mac_lib && mkdir -p build/mac_lib") == 0);
    put_file("build/mac_lib/mylib.zmac", "#macro MYLIB(r) {\n    LR    &r,&r\n#}\n#macro HELP2(r) {\n    XR    &r,&r\n#}\n");
    put_file("build/mac_lib/old.mac", "#macro OLD() {\n    NOPR  0\n#}\n");
    o = run("    MYLIB 1\n    HELP2 2\n    OLD\n    NOSUCH 1\n", dirs, 0);
    CHECK_MSG(!strcmp(o.text, "LR 1,1|XR 2,2|NOPR 0|NOSUCH 1"), "library: \"%s\" (%s)", o.text, o.codes);
    CHECK_MSG(!strcmp(o.codes, ""), "library codes \"%s\"", o.codes);
}

static void test_assemble(void) {
    const char *src = "#macro ZERO(f, n) {\n    XC    &f(&n),&f\n#}\n"
                      "M        CSECT\n"
                      "         ZERO  BUF,8\n"
                      "         BR    14\n"
                      "BUF      DS    CL8\n";
    Out o = run(src, NULL, 0);
    CHECK_STR(o.codes, "");
    CHECK(strstr(o.text, "XC BUF(8),BUF") != NULL);
}

int main(void) {
    test_basic();
    test_flow();
    test_values();
    test_sys();
    test_messages();
    test_open_code();
    test_limits();
    test_library();
    test_assemble();
    TEST_MAIN_END();
}
