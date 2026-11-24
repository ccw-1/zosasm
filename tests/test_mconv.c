/* test_mconv.c - HLASM MACRO..MEND -> zosasm macro (hlasm2za, MACRO.md section 10).
 * The macros here are written for the test. Each one is converted twice, flat (#goto/#label)
 * and structured, and both are expanded for several argument sets: the output must be equal. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    {"fork and join over four arms",
     "         MACRO\n&L       SW1   &T\n         LCLC  &X\n         AIF   ('&T' EQ 'A').LA\n         AIF   ('&T' EQ 'B').LB\n"
     "         AIF   ('&T' EQ 'C').LC\n         AGO   .LD\n.LA       ANOP\n&X       SETC  'one'\n         AGO   .DONE\n"
     ".LB       ANOP\n&X       SETC  'two'\n         AGO   .DONE\n.LC       ANOP\n&X       SETC  'three'\n         AGO   .DONE\n"
     ".LD       ANOP\n&X       SETC  'other'\n.DONE     ANOP\n&L       DC    C'&X'\n         MEND\n",
     {"A", "B", "C", "Z", "", NULL},
     0},
    {"arms that all leave",
     "         MACRO\n         EL1   &T\n         AIF   ('&T' EQ 'A').LA\n         AIF   ('&T' EQ 'B').LB\n"
     "         DC    C'N'\n         MEXIT\n.LA       ANOP\n         DC    C'A'\n         MEXIT\n.LB       ANOP\n"
     "         DC    C'B'\n         MEND\n",
     {"A", "B", "Q", NULL},
     0},
    {"a jump into the middle of the code before it",
     "         MACRO\n         DSP   &N\n         LCLA  &I\n&I       SETA  &N\n         AIF   (&I GT 5).BIG\n"
     ".TOP     ANOP\n         DC    A(1)\n         AIF   (&I EQ 1).MID\n         DC    A(2)\n.BIG     ANOP\n"
     "         DC    A(3)\n&I       SETA  1\n         AGO   .TOP\n.MID     ANOP\n         DC    A(4)\n         MEND\n",
     {"1", "3", "9", NULL},
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

static char *convert_copy(const char *text, const char *dir) {
    ConvOptions o;
    int nw = 0;
    conv_options_default(&o);
    o.member = 1;
    o.copy_dir = dir;
    return hlasm_convert(text, strlen(text), "t.asm", &o, NULL, &nw);
}

/* AREAD, AINSERT, OPSYN, PUNCH, nested MACRO, COPY and the system variables inside macros */
static void test_features(void) {
    int nw = 0;
    char *c, *x;
    char dir[512];
    FILE *f;
    /* AREAD reads the next source record; AINSERT queues records after the outermost macro */
    c = convert("         MACRO\n         RD\n         LCLC  &A\n&A       SETC  AREAD\n"
                "         DC    C'&A'\n         AINSERT '         DC    C''ins'''\n"
                "         AINSERT '         DC    C''first''',FRONT\n         MEND\n",
                0, &nw);
    CHECK_MSG(strstr(c, "aread()") && strstr(c, "#insert") && strstr(c, ", front"), "aread/ainsert:\n%s", c);
    x = expand(xasprintf("%s    RD\ntaken\n         DC    C'next'\n", c));
    CHECK_MSG(strstr(x, "C'taken ") != NULL, "aread expansion: %s", x);
    CHECK_MSG(strstr(x, "C'first'") && strstr(x, "C'ins'") && strstr(x, "C'first'") < strstr(x, "C'ins'") &&
                  strstr(x, "C'ins'") < strstr(x, "C'next'") && !strstr(x, "|taken"),
              "ainsert order: %s", x);
    /* OPSYN of a macro; PUNCH stays a statement */
    c = convert("         MACRO\n         M1    &X\n         DC    C'&X'\n         PUNCH '//JOB'\n"
                "_B_      OPSYN B\n         MEND\n",
                0, &nw);
    CHECK_MSG(strstr(c, "PUNCH") && strstr(c, "OPSYN") && strstr(c, "#macro M1("), "punch/opsyn:\n%s", c);
    x = expand(xasprintf("%sMM       OPSYN M1\n         MM    ab\n", c));
    CHECK_MSG(strstr(x, "C'ab'") != NULL && !strstr(x, "ZA4"), "macro opsyn: %s", x);
    /* a macro inside a macro becomes a macro of its own */
    c = convert("         MACRO\n         OUTER &A\n         MACRO\n         INNER &B\n         DC    C'in&B'\n"
                "         MEND\n         INNER &A\n         MEND\n",
                0, &nw);
    CHECK_MSG(count(c, "#macro ") == 2 && strstr(c, "#macro INNER(") < strstr(c, "#macro OUTER("), "nested:\n%s", c);
    x = expand(xasprintf("%s    OUTER zz\n", c));
    CHECK_MSG(strstr(x, "C'inzz'") != NULL && !strstr(x, "ZA4"), "nested expansion: %s", x);
    /* the system variables */
    c = convert("         MACRO\n         SV\n         DC    C'&SYSDATE|&SYSPARM|&SYSLOC'\n         MEND\n", 0, &nw);
    CHECK_MSG(strstr(c, "sys.date") && strstr(c, "sys.parm") && strstr(c, "sys.loc"), "sys vars:\n%s", c);
    x = expand(xasprintf("T CSECT\n%s    SV\n", c));
    CHECK_MSG(strstr(x, "C'") && !strstr(x, "ZA41"), "sys var expansion: %s", x);
    /* COPY inside a macro reads the member when a directory is given */
    snprintf(dir, sizeof dir, "%s/zmcXXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    CHECK(mkdtemp(dir) != NULL);
    if (!*dir) return;
    f = fopen(xasprintf("%s/CPYM.mac", dir), "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("         DC    C'from copy &Q'\n", f);
        fclose(f);
    }
    c = convert_copy("         MACRO\n         CP    &Q\n         COPY  CPYM\n         MEND\n", dir);
    CHECK_MSG(strstr(c, "#macro CP(") && strstr(c, "from copy &q"), "copy:\n%s", c);
    c = convert("         MACRO\n         CP    &Q\n         COPY  CPYM\n         MEND\n", 0, &nw);
    CHECK_MSG(!strstr(c, "#macro CP("), "COPY without a directory is not converted:\n%s", c);
    {
        char *m = xasprintf("%s/CPYM.mac", dir);
        remove(m);
        free(m);
        rmdir(dir);
    }
}

/* else if, #switch, the dispatch loop and -keep-goto (MACRO.md section 10) */
static void test_post(void) {
    int nw = 0;
    char *c;
    ConvOptions o;
    /* a chain of tests of one variable: #switch */
    c = convert(cases[sizeof cases / sizeof cases[0] - 3].hlasm, 0, &nw);
    CHECK_MSG(strstr(c, "#switch (t) {") != NULL && strstr(c, "#case \"A\":") != NULL &&
                  strstr(c, "#default:") != NULL && count(c, "#goto") == 0,
              "switch shape:\n%s", c);
    /* two arms stay an if / else if */
    c = convert("         MACRO\n         TWO   &T\n         AIF   ('&T' EQ 'A').A\n         AIF   ('&T' EQ 'B').B\n"
                "         DC    C'N'\n         AGO   .Z\n.A        DC    C'A'\n         AGO   .Z\n.B        DC    C'B'\n"
                ".Z        DC    C'Z'\n         MEND\n",
                0, &nw);
    CHECK_MSG(strstr(c, "#} else if (t == \"B\") {") != NULL && !strstr(c, "#switch"), "else if shape:\n%s", c);
    /* a flow with two entries into a block: the dispatch loop, no #goto */
    c = convert(cases[sizeof cases / sizeof cases[0] - 1].hlasm, 0, &nw);
    CHECK_MSG(strstr(c, "#switch (pc) {") != NULL && count(c, "#goto") == 0 && count(c, "#label") == 0,
              "dispatch shape:\n%s", c);
    /* the variable name does not collide with a parameter */
    c = convert("         MACRO\n         DSP   &PC\n         LCLA  &I\n&I       SETA  &PC\n         AIF   (&I GT 5).BIG\n"
                ".TOP     ANOP\n         DC    A(1)\n         AIF   (&I EQ 1).MID\n         DC    A(2)\n.BIG     ANOP\n"
                "         DC    A(3)\n&I       SETA  1\n         AGO   .TOP\n.MID     ANOP\n         DC    A(4)\n         MEND\n",
                0, &nw);
    CHECK_MSG(strstr(c, "#let pc_ = ") != NULL && strstr(c, "#switch (pc_) {") != NULL, "pc name:\n%s", c);
    /* -keep-goto keeps #goto where the flow cannot be structured */
    conv_options_default(&o);
    o.member = 1;
    o.keep_goto = 1;
    c = hlasm_convert(cases[sizeof cases / sizeof cases[0] - 1].hlasm, strlen(cases[sizeof cases / sizeof cases[0] - 1].hlasm), "t.asm", &o, NULL, &nw);
    CHECK_MSG(count(c, "#goto") >= 1 && !strstr(c, "#switch (pc)"), "keep-goto shape:\n%s", c);
}

/* the SYS1.MACLIB members that used to fail (PLAN.md Phase 4); each form was probed with HLASM */
static char *card(const char *text, int mark) { /* a record of 71 columns + column 72 */
    char *r = xasprintf("%-71s%c\n", text, mark ? 'X' : ' ');
    return r;
}

static void test_maclib_forms(void) {
    int nw = 0;
    char *c, *x, *src;
    /* a comment line with a mark in column 72 continues on the next record (IXGCON) */
    src = xasprintf("         MACRO\n         CC\n%s%s%s%s         DC    C'CC'\n         MEND\n",
                    card("* A COMMENT", 1), card("         MACRO Reference\". Do one of the following", 1),
                    card(".* MACRO COMMENT", 1), card("               MNOTE 0,'IN COMMENT'", 0));
    c = convert(src, 0, &nw);
    CHECK_MSG(strstr(c, "#macro CC(") != NULL && strstr(c, "#macro Reference") == NULL, "comment continuation:\n%s", c);
    x = expand(xasprintf("%s    CC\n", c));
    CHECK_MSG(strstr(x, "|         DC    C'CC'") != NULL && !strstr(x, "ZA4"), "comment continuation expansion: %s", x);
    /* the same in open code: the continued record is not a statement */
    c = convert(xasprintf("%s         DC    C'A'\n", card("* TEXT", 1)), 0, &nw);
    CHECK_MSG(count(c, "DC") == 1, "open code comment continuation:\n%s", c);
    /* MNOTE: a blank after the comma ends the operand field; an unquoted message (CPF, IHBRDWRA) */
    c = convert("         MACRO\n         MN    &A\n         MNOTE 4, 'TRAILING COMMA'\n"
                "         MNOTE 4,SEQNO=&A INVALID - IGNORED'\n         MNOTE 0,'AFTER &A'\n         MEND\n",
                0, &nw);
    CHECK_MSG(strstr(c, "#macro MN(") != NULL && nw >= 2, "mnote forms converted (%d warnings):\n%s", nw, c);
    x = expand(xasprintf("%s    MN    7\n", c));
    CHECK_MSG(strstr(x, "MNOTE 4,SEQNO=7") != NULL && strstr(x, "ZA4052") != NULL, "mnote expansion: %s", x);
    /* LCLB with a bare name starting with @ (IHBRDWRA) */
    c = convert("         MACRO\n         LB\n         LCLB  @IDS\n&IDS     SETB  1\n"
                "         AIF   (NOT &IDS).X\n         DC    C'SET'\n.X       ANOP\n         MEND\n",
                0, &nw);
    CHECK_MSG(strstr(c, "#macro LB(") != NULL, "bare @ declaration:\n%s", c);
    /* a prototype operand that is not a symbolic parameter is ignored (IGWRMRCD) */
    c = convert("         MACRO\n         PR                             !                          @T4C\n"
                "         DC    C'PR'\n         MEND\n",
                0, &nw);
    CHECK_MSG(strstr(c, "#macro PR(") != NULL && nw >= 1, "bad prototype operand:\n%s", c);
    x = expand(xasprintf("%s    PR\n", c));
    CHECK_MSG(strstr(x, "C'PR'") != NULL, "prototype expansion: %s", x);
    /* text after the last MEND of a member that holds a MACRO word is not a failure (IRRPICRX) */
    c = convert("         MACRO\n         TR\n         DC    C'TR'\n         MEND\n"
                "* PL/X TEXT\n                                       macro IRRPIDID                */\n"
                "      3 FIELD FIXED(8);\n",
                0, &nw);
    CHECK_MSG(strstr(c, "#macro TR(") != NULL && !strstr(c, "commented out"), "trailer after MEND:\n%s", c);
    /* a PL/X macro member is not an assembler macro (ERBFITEM) */
    c = convert("%ERBFITEM:\n     MACRO KEYS(FLOAT               /* Floating point precision      */\n"
                "               );\n DCL X FIXED;\n %END ERBFITEM;\n",
                0, &nw);
    CHECK_MSG(strstr(c, "not an assembler macro") != NULL || strstr(c, "PL/X") != NULL, "PL/X member:\n%s", c);
    /* a remark that opens a PL/X block comment must not swallow the rest of the macro (IHAFPC):
     * the converted text can be read back */
    c = convert("         MACRO\n         BCMT\n         DC    F'1'          3 * BIT(6), /* RESERVED\n"
                "         DC    F'2'\n         MEND\n",
                0, &nw);
    CHECK_MSG(strstr(c, "#macro BCMT(") != NULL && strstr(c, "/*") == NULL, "block comment in a remark:\n%s", c);
    x = expand(xasprintf("%s    BCMT\n", c));
    CHECK_MSG(strstr(x, "F'1'") != NULL && strstr(x, "F'2'") != NULL && !strstr(x, "ZA4"), "block comment expansion: %s", x);
}

int main(void) {
    test_equivalence();
    test_shapes();
    test_features();
    test_post();
    test_maclib_forms();
    TEST_MAIN_END();
}
