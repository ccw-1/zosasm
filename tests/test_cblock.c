/* test_cblock.c - capture and M1 compilation of #pragma cblock. */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "../src/pp.h"
#include "testlib.h"

typedef struct {
    char *text;
    char *codes;
    Reporter *rep;
} Out;

static Out run_pp(const char *src) {
    PpOptions o;
    SrcVec lines = {0};
    Reporter *rep = rep_new(0);
    Buf t = {0}, c = {0};
    size_t i;
    Out out;
    memset(&o, 0, sizeof o);
    rep_add_source(rep, "t.za", src, strlen(src));
    pp_run("t.za", src, strlen(src), &o, rep, &lines);
    for (i = 0; i < lines.n; i++) {
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

static void expect(const char *name, const char *src, const char *want_codes, const char *needle) {
    Out o = run_pp(src);
    CHECK_MSG(!strcmp(o.codes, want_codes), "%s: codes \"%s\", want \"%s\"", name, o.codes,
              want_codes);
    if (needle) CHECK_MSG(strstr(o.text, needle) != NULL, "%s: output lacks \"%s\": %s", name,
                         needle, o.text);
    free(o.text);
    free(o.codes);
    rep_free(o.rep);
}


static void test_arch_builtins(void) {
    const char *src =
        "#pragma cblock arch\n"
        "#pragma cin uint32_t a=%r2\n"
        "#pragma cin uint32_t *p=%r3\n"
        "#pragma cout uint32_t r=%r4\n"
        "#pragma cscratch %r0, %r1, %r5\n"
        "#pragma ctemp %r12, 128, down\n"
        "#pragma cbegin\n"
        "r = __popcnt(a);\n"
        "r = r + (int32_t)__lrv(p);\n"
        "__strv(r, p);\n"
        "r = r + (int32_t)__stck(p);\n"
        "#pragma cend\n"
        " END\n";
    Out o = run_pp(src);
    CHECK_STR(o.codes, "");
    CHECK_MSG(strstr(o.text, "POPCNT %r1,%r0") != NULL, "missing POPCNT: %s", o.text);
    CHECK_MSG(strstr(o.text, "LRV %r0,0(") != NULL, "missing LRV: %s", o.text);
    CHECK_MSG(strstr(o.text, "STRV %r0,0(") != NULL, "missing STRV: %s", o.text);
    CHECK_MSG(strstr(o.text, "STCK 0(") != NULL, "missing STCK: %s", o.text);
    CHECK_MSG(strstr(o.text, "IPM %r0") != NULL, "missing IPM: %s", o.text);
    free(o.text);
    free(o.codes);
    rep_free(o.rep);
}

static void test_atomic_builtins(void) {
    expect("lad builtin",
           "#pragma cblock a\n"
           "#pragma cin uint32_t *out=%r2\n"
           "#pragma cin uint32_t v=%r3\n"
           "#pragma cin uint32_t *mem=%r4\n"
           "#pragma cout int32_t cc=%r5\n"
           "#pragma cscratch %r0, %r1, %r6, %r7\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "cc = __lad(out, v, mem);\n"
           "#pragma cend\n"
           " END\n",
           "", "LAA %r1,%r0,0(");
    expect("lpd builtin",
           "#pragma cblock a\n"
           "#pragma cin uint32_t *a=%r2, uint32_t *b=%r3\n"
           "#pragma cin uint32_t *c=%r4, uint32_t *d=%r5\n"
           "#pragma cout int32_t cc=%r6\n"
           "#pragma cscratch %r0, %r1, %r7, %r8, %r9\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "cc = __lpd(a, b, c, d);\n"
           "#pragma cend\n"
           " END\n",
           "", "LPD %r8,0(");
    expect("cdsg builtin",
           "#pragma cblock a\n"
           "#pragma cin void *a=%r2, void *b=%r3, void *c=%r4\n"
           "#pragma cout int32_t cc=%r5\n"
           "#pragma cscratch %r0, %r1, %r6, %r7, %r8, %r9\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "cc = __cdsg(a, b, c);\n"
           "#pragma cend\n"
           " END\n",
           "", "CDSG %r6,%r8,0(");
    expect("csg builtin",
           "#pragma cblock a\n"
           "#pragma cin void *a=%r2, void *b=%r3, void *c=%r4\n"
           "#pragma cout int32_t cc=%r5\n"
           "#pragma cscratch %r0, %r1, %r6, %r7, %r8\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "cc = __csg(a, b, c);\n"
           "#pragma cend\n"
           " END\n",
           "", "CSG %r0,%r1,0(");
}

static void test_decimal_builtins(void) {
    expect("decimal and character builtins",
           "#pragma cblock a\n"
           "#pragma cin unsigned char *a=%r2, unsigned char *b=%r3\n"
           "#pragma cout int32_t cc=%r4\n"
           "#pragma cscratch %r0, %r1, %r5, %r6\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "cc = __ap(a, 1, b, 1);\n"
           "cc = __nc(a, b, 3);\n"
           "__tr(a, b, 3);\n"
           "cc = __cvb(a);\n"
           "__cvd(cc, a);\n"
           "cc = __ed(a, b, 5);\n"
           "cc = __tp(a, 5);\n"
           "cc = __srp(a, 5, 2, 15);\n"
           "__pack(a, 2, b, 3);\n"
           "#pragma cend\n"
           " END\n",
           "", "AP 0(1,%r1),0(1,%r5)");
    expect("IEEE float and double builtins",
           "#pragma cblock a\n"
           "#pragma cin double a=%f0, float b=%f2\n"
           "#pragma cout double c=%f4, float d=%f6\n"
           "#pragma cscratch %r2, %r3, %r4, %r5, %f8, %f10, %f12\n"
           "#pragma ctemp %r12, 256, down\n"
           "#pragma cbegin\n"
           "c = __sqdbr(a);\n"
           "d = __sqebr(b);\n"
           "c = __madbr(a, a, a);\n"
           "d = __fmadds(b, b, b);\n"
           "c = __msdbr(a, a, a);\n"
           "d = __fmsub(a, a, a);\n"
           "c = __fidbr(0, a);\n"
           "d = __fiebr(0, b);\n"
           "double x;\n"
           "float y;\n"
           "int32_t cc;\n"
           "cc = __lpdbr(&x, a);\n"
           "cc = __lndbr(&x, a);\n"
           "cc = __lpebr(&y, b);\n"
           "cc = __lnebr(&y, b);\n"
           "#pragma cend\n"
           " END\n",
           "", "SQDBR %f8,%f10");
    expect("IEEE conversions and FPC",
           "#pragma cblock a\n"
           "#pragma cin double a=%f0, float b=%f2\n"
           "#pragma cout int32_t cc=%r3\n"
           "#pragma cscratch %r2, %r4, %r5, %r6, %f8, %f10\n"
           "#pragma ctemp %r12, 256, down\n"
           "#pragma cbegin\n"
           "int32_t x;\n"
           "int64_t y;\n"
           "cc = __cfdbr(&x, 0, a);\n"
           "cc = __cfebr(&x, 0, b);\n"
           "cc = __cgdbr(&y, 0, a);\n"
           "cc = __cgebr(&y, 0, b);\n"
           "cc = __efpc();\n"
           "__sfpc(cc);\n"
           "#pragma cend\n"
           " END\n",
           "", "CFDBR %r0,0,%f8");
    expect("float literals",
           "#pragma cblock a\n"
           "#pragma cout double c=%f4, float d=%f6\n"
           "#pragma cscratch %r2, %r3, %f8\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "c = 1.5;\n"
           "d = 2.25f;\n"
           "#pragma cend\n"
           " END\n",
           "", "IIHF %r0,1073217536");
    expect("float casts",
           "#pragma cblock a\n"
           "#pragma cout float f=%f4, int32_t i=%r2\n"
           "#pragma cscratch %r3, %f8\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "f = 1.5;\n"
           "i = 1.5;\n"
           "#pragma cend\n"
           " END\n",
           "", "LEDBR %f8,%f8");
    expect("floating arithmetic and comparison",
           "#pragma cblock a\n"
           "#pragma cout double c=%f4, int32_t r=%r3\n"
           "#pragma cscratch %r2, %r5, %r6, %f8, %f10\n"
           "#pragma ctemp %r12, 256, down\n"
           "#pragma cbegin\n"
           "c = 1.5 + 2.25;\n"
           "c = c * 2.0;\n"
           "r = (c < 7.5) + (c >= 7.5) * 2;\n"
           "if (c != 0.0) r = r + 4;\n"
           "#pragma cend\n"
           " END\n",
           "", "ADBR %f8,%f10");
    expect("float binding needs an FPR",
           "#pragma cblock a\n"
           "#pragma cin double a=%r2\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "a = a;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5005", "");
    expect("packing and 64-bit decimal builtins",
           "#pragma cblock a\n"
           "#pragma cin char *a=%r2, char *b=%r3, uint16_t *u=%r4\n"
           "#pragma cout int64_t r=%r5\n"
           "#pragma cscratch %r0, %r1, %r6, %r7\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "r = __cvbg(a);\n"
           "__cvdg(r, b);\n"
           "__pka(a, b, 5);\n"
           "__pku(a, u, 5);\n"
           "__unpka(a, 5, b);\n"
           "__unpku(u, 5, b);\n"
           "__mvcrl(a, b, 5);\n"
           "#pragma cend\n"
           " END\n",
           "", "PKA 0(%r1),0(5,%r6)");
    expect("decimal length must be constant",
           "#pragma cblock a\n"
           "#pragma cin unsigned char *a=%r2, unsigned char *b=%r3\n"
           "#pragma cin int32_t n=%r4\n"
           "#pragma cscratch %r0, %r1, %r5\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "__nc(a, b, n);\n"
           "#pragma cend\n"
           " END\n",
           "ZA5005", NULL);
}

static void test_16bit_types(void) {
    expect("16-bit scalars and pointers",
           "#pragma cblock a\n"
           "#pragma cin uint16_t *p=%r2\n"
           "#pragma cout int32_t r=%r3\n"
           "#pragma cscratch %r0, %r1, %r4\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "int16_t x;\n"
           "uint16_t y;\n"
           "x = -1; y = 65535; x = x + 1; y = y + 1;\n"
           "r = x + y;\n"
           "r = r + (int32_t)__lrvh(p);\n"
           "__strvh(y, p);\n"
           "#pragma cend\n"
           " END\n",
           "", "LRVH %r0,0(");
}

static void test_debug_metadata(void) {
    const char *src =
        "#pragma cblock dbg\n"
        "#pragma cout int32_t c=%r4\n"
        "#pragma cscratch %r0, %r1, %r5\n"
        "#pragma ctemp %r12, 64, down\n"
        "#pragma cbegin\n"
        "int32_t x = 5; c = x;\n"
        "#pragma cend\n"
        " END\n";
    PpOptions o;
    SrcVec lines = {0};
    Reporter *rep = rep_new(0);
    CBlockDebugVec dbg = {0};
    memset(&o, 0, sizeof o);
    o.cblock_debug = &dbg;
    rep_add_source(rep, "t.za", src, strlen(src));
    pp_run("t.za", src, strlen(src), &o, rep, &lines);
    CHECK_MSG(dbg.n == 1, "debug: %zu cblocks", dbg.n);
    if (dbg.n == 1) {
        CHECK_MSG(dbg.v[0].nvars == 1, "debug: %zu vars", dbg.v[0].nvars);
        if (dbg.v[0].nvars == 1) {
            CHECK_STR(dbg.v[0].vars[0].name, "x");
            CHECK_MSG(dbg.v[0].vars[0].bits == 32, "debug: bits %d", dbg.v[0].vars[0].bits);
        }
        CHECK_MSG(dbg.v[0].temp_reg == 12, "debug: temp reg %d", dbg.v[0].temp_reg);
        CHECK_MSG(dbg.v[0].start_label != NULL && dbg.v[0].end_label != NULL,
                  "debug: missing range labels");
    }
    cblock_debug_free(&dbg);
    rep_free(rep);
    free(lines.v);
}

static void test_compiled_assembly(void) {
    const char *src =
        "P        DSECT\n"
        "P_A      DS    F\n"
        "P_B      DS    CL4\n"
        "MAIN CSECT\n"
        "         USING MAIN,15\n"
        "         LA    %r12,AREA\n"
        "         LHI   %r2,3\n"
        "         LHI   %r3,4\n"
        "#pragma cblock squares\n"
        "#pragma cin int32_t a=%r2, int32_t b=%r3\n"
        "#pragma cout int32_t c=%r4\n"
        "#pragma cscratch %r0, %r1, %r5\n"
        "#pragma ctemp %r12, 96, down\n"
        "#pragma cbegin\n"
        "c = a*a + b*b;\n"
        "#pragma cend\n"
        "         ST    %r4,RES\n"
        "#pragma cblock second\n"
        "#pragma cout int32_t d=%r5\n"
        "#pragma cscratch %r0, %r1, %r6\n"
        "#pragma ctemp %r12, 96, down\n"
        "#pragma cbegin\n"
        "int32_t i = 0; while (i < 2) { i++; } d = i;\n"
        "#pragma cend\n"
        "         ST    %r5,RES2\n"
        "#pragma cblock typed\n"
        "#pragma cout int32_t n=%r4, P *o=%r5\n"
        "#pragma cscratch %r0, %r1, %r6\n"
        "#pragma ctemp %r12, 96, down\n"
        "#pragma cbegin\n"
        "P *p; p = (P *)0; o = p; n = (int32_t)__sizeof(P) + (int32_t)__offsetof(P, P_B);\n"
        "#pragma cend\n"
        "         ST    %r4,RES3\n"
        "RES      DS    F\n"
        "RES2     DS    F\n"
        "RES3     DS    F\n"
        "AREA     DS    12F\n"
        "         END\n";
    Out o = run_pp(src);
    Buf b = {0};
    size_t i;
    Reporter *rep;
    Module *m;
    for (i = 0; o.text[i]; i++) buf_putc(&b, o.text[i] == '|' ? '\n' : o.text[i]);
    rep = rep_new(0);
    m = asm_assemble_text("t.za", buf_cstr(&b), b.len, rep);
    CHECK_MSG(!strcmp(o.codes, ""), "compiled block codes: %s", o.codes);
    CHECK_MSG(m != NULL, "compiled block does not assemble");
    if (m) asm_free(m);
    rep_free(rep);
    free(b.p);
    free(o.text);
    free(o.codes);
    rep_free(o.rep);
}

int main(void) {
    expect("compiled block",
           "#pragma cblock demo\n"
           "#pragma cin int32_t a=%r2\n"
           "#pragma cout int32_t b=%r3\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "b = a + 1;\n"
           "#pragma cend\n"
           " END\n",
           "", "AR %r0,%r1");
    expect("no cbegin",
           "#pragma cblock demo\n"
           " END\n",
           "ZA5001", NULL);
    expect("unterminated",
           "#pragma cblock demo\n"
           "#pragma cbegin\n"
           "return;\n"
           " END\n",
           "ZA5001", NULL);
    expect("return unsupported",
           "#pragma cblock demo\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma cbegin\n"
           "return;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5008", NULL);
    expect("temp too small",
           "#pragma cblock demo\n"
           "#pragma cin int32_t a=%r2\n"
           "#pragma cout int32_t b=%r3\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 8, down\n"
           "#pragma cbegin\n"
           "b = a;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5006", NULL);
    expect("string literal table",
           "#pragma cblock lit\n"
           "#pragma cout uint32_t n=%r2\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "const char *p = __align(8) \"A\\x00B\";\n"
           "n = __strlen(p);\n"
           "#pragma cend\n"
           " END\n",
           "", "DC X'41004200'");
    expect("string literal strlen and byte",
           "#pragma cblock lit\n"
           "#pragma cin char *p=%r2\n"
           "#pragma cout uint32_t n=%r3\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "n = __strlen(\"hello\");\n"
           "n = p[1];\n"
           "#pragma cend\n"
           " END\n",
           "", "LGFI %r0,5");
    expect("memcmp and strcmp compile",
           "#pragma cblock cmp\n"
           "#pragma cin char *a=%r2, char *b=%r3\n"
           "#pragma cout int32_t n=%r4\n"
           "#pragma cscratch %r0, %r1, %r5\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "n = __memcmp(a, b, 4);\n"
           "n = n + __strcmp(a, b);\n"
           "#pragma cend\n"
           " END\n",
           "", "CLC 0(1,%r1),0(%r5)");
    expect("strcpy compile",
           "#pragma cblock cp\n"
           "#pragma cin char *d=%r2, char *s=%r3\n"
           "#pragma cout char *r=%r4\n"
           "#pragma cscratch %r0, %r1, %r5\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "r = __strcpy(d, s);\n"
           "#pragma cend\n"
           " END\n",
           "", "MVC 0(1,");
    expect("bad string alignment",
           "#pragma cblock lit\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "char *p = __align(3) \"x\";\n"
           "#pragma cend\n"
           " END\n",
           "ZA5001 ZA5001", NULL);
    expect("ctemp r1 is internal scratch",
           "#pragma cblock demo\n"
           "#pragma ctemp %r1, 64, down\n"
           "#pragma cbegin\n"
           "int32_t x;\n"
           "x = 1;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5005", NULL);
    expect("read before assign",
           "#pragma cblock demo\n"
           "#pragma cout int32_t b=%r3\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "b = b + 1;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5003", NULL);
    expect("cend outside",
           "#pragma cend\n"
           " END\n",
           "ZA5001", NULL);
    expect("if and while compile",
           "#pragma cblock demo\n"
           "#pragma cout int32_t c=%r4\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "int32_t i = 0; while (i < 2) { i++; } c = i;\n"
           "#pragma cend\n"
           " END\n",
           "", "BRCL");
    expect("switch and goto compile",
           "#pragma cblock demo\n"
           "#pragma cout int32_t c=%r4\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "int32_t t = 0; switch (t) { case 0: t = 1; break; default: t = 2; } goto done; done: c = t;\n"
           "#pragma cend\n"
           " END\n",
           "", "BRCL");
    expect("memcpy compiles",
           "#pragma cblock demo\n"
           "#pragma cin uint8_t *d=%r2, uint8_t *s=%r3\n"
           "#pragma cout uint8_t *o=%r4\n"
           "#pragma cscratch %r0, %r1, %r5\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "__memcpy(d, s, 4); o = d;\n"
           "#pragma cend\n"
           " END\n",
           "", "MVC");
    expect("memcpy needs pointers",
           "#pragma cblock demo\n"
           "#pragma cscratch %r0, %r1, %r5\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "__memcpy(1, 2, 4);\n"
           "#pragma cend\n"
           " END\n",
           "ZA5003", NULL);
    expect("undefined goto",
           "#pragma cblock demo\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "goto nowhere;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5004", NULL);
    expect("break outside loop",
           "#pragma cblock demo\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "break;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5008", NULL);
    expect("cbegin outside",
           "#pragma cbegin\n"
           " END\n",
           "ZA5001", NULL);
    test_arch_builtins();
    test_atomic_builtins();
    test_decimal_builtins();
    test_16bit_types();
    test_debug_metadata();
    test_compiled_assembly();
    TEST_MAIN_END();
}
