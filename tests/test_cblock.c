/* test_cblock.c - capture of #pragma cblock (PLAN-CBLOCK.md, M0). */
#include <stdlib.h>
#include <string.h>

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

static void expect(const char *name, const char *src, const char *want_text,
                   const char *want_codes) {
    Out o = run_pp(src);
    CHECK_MSG(!strcmp(o.text, want_text), "%s: output \"%s\", want \"%s\"", name, o.text,
              want_text);
    CHECK_MSG(!strcmp(o.codes, want_codes), "%s: codes \"%s\", want \"%s\"", name, o.codes,
              want_codes);
    free(o.text);
    free(o.codes);
    rep_free(o.rep);
}

int main(void) {
    expect("captured block",
           "#pragma cblock demo\n"
           "#pragma cbegin\n"
           "int x = 1;\n"
           "#pragma cend\n"
           " END\n",
           " END", "ZA5002");
    expect("unterminated",
           "#pragma cblock demo\n"
           "#pragma cbegin\n"
           "int x = 1;\n"
           " END\n",
           "", "ZA5001");
    expect("nested",
           "#pragma cblock a\n"
           "#pragma cblock b\n"
           "#pragma cend\n"
           "#pragma cend\n"
           " END\n",
           " END", "ZA5001 ZA5002 ZA5001");
    expect("cend outside",
           "#pragma cend\n"
           " END\n",
           " END", "ZA5001");
    expect("cbegin outside",
           "#pragma cbegin\n"
           " END\n",
           " END", "ZA5001");
    TEST_MAIN_END();
}
