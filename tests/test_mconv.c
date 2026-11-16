/* test_mconv.c - HLASM MACRO..MEND -> zosasm macro (hlasm2za, MACRO.md section 10).
 * The macros here are written for the test. Each one is converted twice, flat (#goto/#label)
 * and structured, and both are expanded for several argument sets: the output must be equal. */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>

#include "../src/convert.h"
#include "../src/pp.h"
#include "../src/util.h"
#include "testlib.h"

static char *convert(const char *text, int flat, int *nwarn) {
    ConvOptions o;
    conv_options_default(&o);
    o.member = 1;
    o.flat = flat;
    return hlasm_convert(text, strlen(text), "t.asm", &o, NULL, nwarn);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* expand `src` (macro definitions followed by one call); text and the sorted diagnostic codes */
static char *expand(const char *src) {
    PpOptions o;
    SrcVec lines = {0};
    Reporter *rep = rep_new(0);
    Buf t = {0};
    char *codes[16];
    size_t i, nc = 0;
    memset(&o, 0, sizeof o);
    rep_add_source(rep, "t.za", src, strlen(src));
    pp_run("t.za", src, strlen(src), &o, rep, &lines);
    for (i = 0; i < lines.n; i++) {
        if (i) buf_putc(&t, '|');
        buf_puts(&t, lines.v[i].text);
    }
    for (i = 0; i < rep_count(rep) && nc < 16; i++) codes[nc++] = (char *)rep_get(rep, i)->code;
    qsort(codes, nc, sizeof codes[0], cmp_str);
    buf_puts(&t, " #");
    for (i = 0; i < nc; i++) buf_printf(&t, " %s", codes[i]);
    return xstrdup(buf_cstr(&t));
}

typedef struct {
    const char *name;
    const char *hlasm;
    const char *calls[8]; /* argument texts, NULL ends */
    int gotos;            /* the structured form must have at most this many #goto */
} Case;

static const Case cases[] = {
    {"guard clauses",
     "         MACRO\n&L       GUARD &A,&B\n         AIF   ('&A' EQ '').E1\n         AIF   ('&B' EQ '').E2\n"
     "&L       DC    F'&A,&B'\n         MEXIT\n.E1       MNOTE 8,'A missing'\n         MEXIT\n"
     ".E2       MNOTE 4,'B missing'\n         MEND\n",
     {"1,2", "1", ",2", "", NULL},
     0},
    {"if else",
     "         MACRO\n         IFE   &N\n         AIF   (&N GT 5).BIG\n         DC    C'SMALL'\n"
     "         AGO   .END\n.BIG     ANOP\n         DC    C'BIG'\n.END     ANOP\n         DC    C'DONE'\n         MEND\n",
     {"3", "5", "6", "9", NULL},
     0},
    {"while loop",
     "         MACRO\n         COUNT &N\n         LCLA  &I\n&I       SETA  0\n.LOOP    AIF   (&I GE &N).DONE\n"
     "&I       SETA  &I+1\n         DC    F'&I'\n         AGO   .LOOP\n.DONE    ANOP\n         DC    C'END'\n         MEND\n",
     {"0", "1", "4", NULL},
     0},
    {"loop with the test at the end",
     "         MACRO\n         DOW   &N\n         LCLA  &I\n&I       SETA  0\n.TOP     ANOP\n&I       SETA  &I+1\n"
     "         DC    F'&I'\n         AIF   (&I LT &N).TOP\n         MEND\n",
     {"1", "3", NULL},
     0},
    {"break and continue",
     "         MACRO\n         SKIP  &N\n         LCLA  &I\n&I       SETA  0\n.NEXT    ANOP\n&I       SETA  &I+1\n"
     "         AIF   (&I GT &N).OUT\n         AIF   (&I EQ 2).NEXT\n         AIF   (&I EQ 5).OUT\n"
     "         DC    F'&I'\n         AGO   .NEXT\n.OUT     DC    C'END'\n         MEND\n",
     {"1", "3", "8", NULL},
     0},
    {"nested loops",
     "         MACRO\n         GRID  &R,&C\n         LCLA  &I,&J\n&I       SETA  0\n.OUTER   AIF   (&I GE &R).ODONE\n"
     "&I       SETA  &I+1\n&J       SETA  0\n.INNER   AIF   (&J GE &C).IDONE\n&J       SETA  &J+1\n"
     "         DC    AL1(&I,&J)\n         AGO   .INNER\n.IDONE   ANOP\n         AGO   .OUTER\n.ODONE   ANOP\n         MEND\n",
     {"0,3", "2,2", "3,1", NULL},
     0},
    {"two entries into a block",
     "         MACRO\n         TWOENT &A\n         LCLA  &X\n         AIF   ('&A' NE '').HAVE\n&X       SETA  0\n"
     "         AGO   .JOIN\n.HAVE    AIF   (&A LE 255).OK\n         MNOTE 8,'BAD'\n&X       SETA  0\n         AGO   .JOIN\n"
     ".OK      ANOP\n&X       SETA  &A\n.JOIN    DC    AL1(&X)\n         MEND\n",
     {"", "7", "300", NULL},
     99},
    {"search loop with two exits",
     "         MACRO\n         FIND  &K\n         LCLA  &M\n&M       SETA  1\n.L       AIF   (&M GT 3).NF\n"
     "         AIF   (&M EQ &K).FOUND\n&M       SETA  &M+1\n         AGO   .L\n.NF      MNOTE 4,'not found'\n"
     ".FOUND   DC    F'&M'\n         MEND\n",
     {"1", "3", "9", NULL},
     99},
    {"branch order reversed",
     "         MACRO\n         REV   &A\n         AIF   ('&A' EQ 'X').ISX\n         AGO   .NOTX\n.ISX     DC    C'IS X'\n"
     "         AGO   .Z\n.NOTX    DC    C'NOT X'\n.Z       DC    C'Z'\n         MEND\n",
     {"X", "Y", NULL},
     0},
    {"counted loop with an error exit",
     "         MACRO\n         TAB   &N\n         LCLA  &I\n         AIF   (&N LE 8).OK\n         MNOTE 8,'too many'\n         MEXIT\n"
     ".OK      ANOP\n&I       SETA  0\n.LP      AIF   (&I EQ &N).END\n&I       SETA  &I+1\n         DC    F'&I'\n"
     "         AGO   .LP\n.END     ANOP\n         MEND\n",
     {"0", "3", "9", NULL},
     0},
};

static int count(const char *s, const char *what) {
    int n = 0;
    size_t l = strlen(what);
    for (; (s = strstr(s, what)) != NULL; s += l) n++;
    return n;
}

static void test_equivalence(void) {
    size_t c;
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        const Case *k = &cases[c];
        int nw = 0, nf = 0, a;
        char *st = convert(k->hlasm, 0, &nw);
        char *fl = convert(k->hlasm, 1, &nf);
        CHECK_MSG(strstr(st, "#macro ") != NULL, "%s: not converted:\n%s", k->name, st);
        CHECK_MSG(strstr(fl, "#macro ") != NULL, "%s: flat not converted", k->name);
        CHECK_MSG(count(st, "#goto") <= k->gotos, "%s: %d #goto in the structured form:\n%s", k->name,
                  count(st, "#goto"), st);
        CHECK_MSG(count(fl, "#goto") >= 1, "%s: the flat form has no #goto", k->name);
        CHECK_MSG(count(st, "#label") <= k->gotos, "%s: %d #label", k->name, count(st, "#label"));
        for (a = 0; k->calls[a]; a++) {
            char nm[16];
            size_t l = 0;
            char *s1, *s2;
            /* the macro name is the first word after the (optional) name field in the prototype */
            const char *p = strstr(k->hlasm, "MACRO\n") + 6;
            p += strcspn(p, " ");
            while (*p == ' ') p++;
            while (*p && *p != ' ' && l < sizeof nm - 1) nm[l++] = *p++;
            nm[l] = 0;
            s1 = expand(xasprintf("%s    %s %s\n", st, nm, k->calls[a]));
            s2 = expand(xasprintf("%s    %s %s\n", fl, nm, k->calls[a]));
            CHECK_MSG(!strcmp(s1, s2), "%s (%s): structured \"%s\" flat \"%s\"", k->name, k->calls[a], s1, s2);
            CHECK_MSG(strstr(s1, "#") && !strstr(s1, "ZA4113") && !strstr(s1, "ZA4100"), "%s (%s): %s", k->name,
                      k->calls[a], s1);
        }
    }
}

static void test_shapes(void) {
    int nw = 0;
    char *s = convert(cases[2].hlasm, 0, &nw); /* while loop */
    CHECK_MSG(strstr(s, "#while (i < n) {") != NULL, "while loop shape:\n%s", s);
    s = convert(cases[1].hlasm, 0, &nw); /* if else */
    CHECK_MSG(strstr(s, "#} else {") != NULL, "if/else shape:\n%s", s);
    s = convert(cases[0].hlasm, 0, &nw); /* guard clauses */
    CHECK_MSG(strstr(s, "#return") != NULL, "guard shape:\n%s", s);
    s = convert(cases[3].hlasm, 0, &nw); /* test at the end */
    CHECK_MSG(strstr(s, "#while (true) {") != NULL && strstr(s, "#break") != NULL, "do-while shape:\n%s", s);
}

int main(void) {
    test_equivalence();
    test_shapes();
    TEST_MAIN_END();
}
