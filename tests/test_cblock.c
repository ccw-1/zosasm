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

static void test_compiled_assembly(void) {
    const char *src =
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
        "RES      DS    F\n"
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
    test_compiled_assembly();
    TEST_MAIN_END();
}
