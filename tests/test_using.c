/* test_using.c - USING: labeled, dependent, ranges, DROP (DESIGN.md 4.3). */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "testlib.h"

static Module *assemble(const char *src, Reporter **repp) {
    Reporter *rep = rep_new(0);
    Module *m = asm_assemble_text("t.za", src, strlen(src), rep);
    if (repp) *repp = rep;
    return m;
}

/* the code of the first control section as hex, or "ERR codes" */
static char *code_hex(const char *src) {
    Reporter *rep;
    Module *m = assemble(src, &rep);
    Buf b = {0};
    size_t i, n;
    const Diag **d;
    if (!m) {
        d = rep_sorted(rep, &n);
        buf_puts(&b, "ERR");
        for (i = 0; i < n; i++) buf_printf(&b, " %s", d[i]->code);
        return xstrdup(buf_cstr(&b));
    }
    {
        Class *c = NULL;
        for (i = 0; i < m->sections.n && !c; i++)
            if (m->sections.v[i]->kind != K_DSECT) c = m->sections.v[i]->classes.v[0];
        for (i = 0; c &&  i < (size_t)c->len; i++) buf_printf(&b, "%02X", c->data[i]);
    }
    return xstrdup(buf_cstr(&b));
}

static void expect(const char *name, const char *src, const char *want) {
    char *got = code_hex(src);
    int ok = !strncmp(want, "ERR", 3) ? !strcmp(got, want) : !strncmp(got, want, strlen(want));
    CHECK_MSG(ok, "%s: got %s, want %s", name, got, want);
}

static void test_labeled(void) {
    expect("labeled qualifiers",
           "WORK     DSECT\nA DS F\nB DS F\n"
           "M CSECT\n"
           "W1 USING WORK,4\n"
           "W2 USING WORK,5\n"
           " L 1,W1.B\n"
           " MVC W1.A,W2.B\n"
           " L 1,W2.A\n",
           "58104004D2034000500458105000");
    /* an unqualified symbol does not use a labeled USING */
    expect("unqualified ignores labels",
           "WORK DSECT\nA DS F\nM CSECT\nW1 USING WORK,4\n L 1,A\n", "ERR ZA2030");
    expect("ordinary and labeled coexist",
           "WORK DSECT\nA DS F\nM CSECT\n USING WORK,3\nW1 USING WORK,4\n L 1,A\n L 1,W1.A\n",
           "5810300058104000");
    /* the label ends with DROP; a repeated label replaces the USING */
    expect("drop label", "WORK DSECT\nA DS F\nM CSECT\nW1 USING WORK,4\n DROP W1\n L 1,W1.A\n",
           "ERR ZA3068");
    expect("label reused",
           "WORK DSECT\nA DS F\nM CSECT\nW1 USING WORK,4\nW1 USING WORK,6\n L 1,W1.A\n",
           "58106000");
    expect("label not active", "WORK DSECT\nA DS F\nM CSECT\n L 1,W9.A\n", "ERR ZA3068");
    /* DROP with the register does not end a labeled USING */
    expect("drop register keeps label",
           "WORK DSECT\nA DS F\nM CSECT\nW1 USING WORK,4\n DROP 4\n L 1,W1.A\n", "58104000");
    /* several registers: one composite range */
    expect("labeled range of two registers",
           "WORK DSECT\nA DS F\n DS XL4092\nB DS F\nM CSECT\nW1 USING WORK,4,5\n L 1,W1.B\n",
           "58105000");
    expect("label equals a symbol",
           "M CSECT\nW1 DS F\nW1 USING M,4\n", "ERR ZA3067");
    expect("quoted label",
           "WORK DSECT\nA DS F\nM CSECT\n\"a b\" USING WORK,4\n L 1,\"a b\".A\n", "58104000");
    expect("qualified attribute",
           "WORK DSECT\nA DS FL3\nM CSECT\nW1 USING WORK,4\n DC AL1(L'W1.A)\n", "03");
}

static void test_dependent(void) {
    const char *map = "RECMAP   DSECT\nRF1 DS F\nRF2 DS F\n";
    char *s;
    s = xasprintf("M CSECT\n USING M,12\n USING RECMAP,INREC\n L 2,RF2\n"
                  "INREC DS CL16\n%s",
                  map);
    expect("dependent", s, "5820C008");
    /* the dependent USING goes with its parent */
    s = xasprintf("M CSECT\n USING M,12\n USING RECMAP,INREC\n DROP 12\n L 2,RF2\n"
                  "INREC DS CL16\n%s",
                  map);
    expect("dropped with parent", s, "ERR ZA2030");
    /* ... and DROP of an address in its range ends only itself */
    s = xasprintf("M CSECT\n USING M,12\n USING RECMAP,INREC\n DROP INREC\n L 2,RF2\n"
                  "INREC DS CL16\n%s",
                  map);
    expect("drop by address", s, "ERR ZA2030");
    /* labeled dependent USINGs */
    s = xasprintf("M CSECT\n USING M,12\nP USING RECMAP,INREC\nQ USING RECMAP,OUTREC\n"
                  " MVC Q.RF1,P.RF2\nINREC DS CL16\nOUTREC DS CL16\n%s",
                  map);
    expect("labeled dependent", s, "D203C016C00A");
    /* DROP of a dependent label leaves the parent */
    s = xasprintf("M CSECT\n USING M,12\nP USING RECMAP,INREC\n DROP P\n L 1,M\n L 2,P.RF1\n"
                  "INREC DS CL16\n%s",
                  map);
    expect("drop dependent label", s, "ERR ZA3068");
    /* a dependent USING with an address that no USING covers */
    s = xasprintf("M CSECT\n USING RECMAP,INREC\nINREC DS CL16\n%s", map);
    expect("dependent without parent", s, "ERR ZA2030");
    /* the supporting address qualified by a labeled USING, and a dependent
     * USING on a dependent: the base register comes from the root */
    s = xasprintf("M CSECT\nL1 USING M,12\n USING RECMAP,L1.INREC\n L 2,RF2\nINREC DS CL16\n%s",
                  map);
    expect("qualified support", s, "5820C008");
    s = xasprintf("M CSECT\n USING M,12\nINNER DSECT\nIA DS F\nIB DS F\nM CSECT\n"
                  " USING RECMAP,INREC\n USING INNER,RF2\n L 2,IB\nINREC DS CL16\n%s",
                  map);
    expect("dependent on dependent", s, "5820C00C");
}

static void test_ranges(void) {
    /* an end address limits the range for 12-bit displacements */
    expect("end limit inside",
           "M CSECT\n USING (M,M+8),12\n L 1,M+4\n DS XL10\n", "5810C004");
    expect("end limit outside", "M CSECT\n USING (M,M+8),12\n L 1,M+10\n DS XL20\n", "ERR ZA2030");
    expect("end limit does not apply to long displacements",
           "M CSECT\n USING (M,M+8),12\n LG 1,M+12\n DS XL20\n", "E310C00C0004");
    expect("lower and upper", "M CSECT\n USING (M,,M+4,M+12),12\n L 1,M+8\n DS XL20\n",
           "5810C008");
    expect("below lower", "M CSECT\n USING (M,,M+4,M+12),12\n L 1,M\n DS XL20\n", "ERR ZA2030");
    expect("upper is exclusive", "M CSECT\n USING (M,,M+4,M+12),12\n L 1,M+12\n DS XL20\n",
           "ERR ZA2030");
    expect("no limit", "M CSECT\n USING (M,,X'80000000',M+8),12\n L 1,M\n DS XL20\n",
           "5810C000");
}

static void test_register_replace(void) {
    /* a second USING of the same register ends the first */
    expect("replace",
           "M CSECT\n USING M,12\n USING M+4,12\n L 1,M+4\n", "5810C000");
    expect("abs base", "M CSECT\n USING 0,3\n LA 7,5\n", "41703005");
    expect("abs base none", "M CSECT\n LA 7,5\n", "41700005");
    expect("coinciding ranges use the higher register",
           "M CSECT\n USING M,5\n USING M,9\n L 1,M\n", "58109000");
    expect("overlap: smallest displacement",
           "M CSECT\n USING M,5\n USING M+4,9\n L 1,M+6\n", "58109002");
}

static void test_errors(void) {
    static const struct {
        const char *name, *src, *want;
    } cases[] = {
        {"end before base", "M CSECT\n USING (M,M),12\n", "ZA3065"},
        {"limits swapped", "M CSECT\n USING (M,M+8,M+4,M+2),12\n", "ZA3065"},
        {"limit in another section", "M CSECT\nN CSECT\n USING (M,N+8),12\n", "ZA3064"},
        {"too many range items", "M CSECT\n USING (M,M+8,M,M+8,M),12\n", "ZA3063"},
        {"address after a register", "M CSECT\n USING M,12,M\n", "ZA3066"},
        {"two dependent addresses", "M CSECT\n USING M,12\n USING M,M,M\n", "ZA3066"},
        {"register range", "M CSECT\n USING M,16\n", "ZA2041"},
        {"external base", "M CSECT\n EXTRN X\n USING X,12\n", "ZA3061"},
        {"bad label", "M CSECT\na.b USING M,12\n", "ZA1021"},
        {"drop unknown register", "M CSECT\n DROP 5\n", "ZA3060"},
        {"drop uncovered address", "M CSECT\n DROP M\n", "ZA3060"},
        {"ok", "M CSECT\n USING (M,M+4096,M-10,M+5000),12\n", ""},
    };
    size_t c;
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        Reporter *rep;
        size_t n, i;
        const Diag **d;
        Buf b = {0};
        assemble(cases[c].src, &rep);
        d = rep_sorted(rep, &n);
        for (i = 0; i < n; i++) {
            if (i) buf_putc(&b, ' ');
            buf_puts(&b, d[i]->code);
        }
        CHECK_MSG(!strcmp(buf_cstr(&b), cases[c].want), "%s: codes \"%s\", want \"%s\"",
                  cases[c].name, buf_cstr(&b), cases[c].want);
    }
}

int main(void) {
    test_labeled();
    test_dependent();
    test_ranges();
    test_register_replace();
    test_errors();
    TEST_MAIN_END();
}
