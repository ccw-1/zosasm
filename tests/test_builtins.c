/* test_builtins.c - sizeof, offsetof, alignof, countof, comparisons and
 * ASSERT (DESIGN.md 12). The preprocessor built-ins are in test_pp.c. */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "testlib.h"

typedef struct {
    Module *m;
    char codes[256];
    const uint8_t *data; /* the first class of the first CSECT */
    int64_t len;
} Res;

static Res run(const char *src) {
    Res r;
    Reporter *rep = rep_new(0);
    size_t i;
    memset(&r, 0, sizeof r);
    r.m = asm_assemble_text("t.za", src, strlen(src), rep);
    for (i = 0; i < rep_count(rep); i++) {
        if (rep_get(rep, i)->sev != SEV_ERROR) continue;
        if (r.codes[0]) strcat(r.codes, " ");
        strncat(r.codes, rep_get(rep, i)->code, 20);
    }
    if (r.m) {
        for (i = 0; i < r.m->sections.n; i++)
            if (r.m->sections.v[i]->kind == K_CSECT) {
                r.data = r.m->sections.v[i]->classes.v[0]->data;
                r.len = r.m->sections.v[i]->classes.v[0]->len;
                break;
            }
    }
    return r;
}

static uint32_t word(const Res *r, int at) {
    return ((uint32_t)r->data[at] << 24) | ((uint32_t)r->data[at + 1] << 16) |
           ((uint32_t)r->data[at + 2] << 8) | r->data[at + 3];
}

static const char *const hdr = "S        DSECT\n"
                               "A        DS    F\n"
                               "B        DS    4CL10\n"
                               "C        DS    H\n"
                               "D        DS    0D\n"
                               "E        DS    F,H\n"
                               "T        DC    3H'1',F'2'\n"
                               "W        DS    CL7\n"
                               "X        DS    2D\n";

/* a CSECT with one A-type constant per expression; returns the values */
static Res consts(const char *exprs) {
    char *src = xasprintf("%sM        CSECT\n         DC    A(%s)\n         END\n", hdr, exprs);
    return run(src);
}

static void expect_words(const char *name, const char *exprs, const uint32_t *want, int n) {
    Res r = consts(exprs);
    int i;
    CHECK_MSG(r.m != NULL, "%s: does not assemble (%s)", name, r.codes);
    if (!r.m) return;
    CHECK_MSG(r.len >= 4 * n, "%s: length %lld", name, (long long)r.len);
    for (i = 0; i < n && r.len >= 4 * (i + 1); i++)
        CHECK_MSG(word(&r, 4 * i) == want[i], "%s: value %d is %u, want %u", name, i,
                  (unsigned)word(&r, 4 * i), (unsigned)want[i]);
}

static void test_functions(void) {
    /* S: A 0, B 4-44, C 44, D 48 (0D), E 48 (F,H), T 54 (3H,F), W 64, X 72 */
    uint32_t w1[] = {4, 10, 40, 4, 44, 48};
    uint32_t w2[] = {4, 4, 2, 3, 1, 6};
    uint32_t w3[] = {7, 1, 16, 2, 8};
    expect_words("sizeof and offsetof",
                 "sizeof(A),L'B,sizeof(B),sizeof(E),offsetof(S,C),offsetof(S,E)", w1, 6);
    expect_words("alignof and countof",
                 "alignof(A),alignof(E),alignof(C),countof(T),alignof(B),sizeof(T)", w2, 6);
    expect_words("arrays and byte items",
                 "sizeof(W),countof(W),sizeof(X),countof(X),alignof(X)", w3, 5);
}

static void test_sections(void) {
    Res r = run("S        DSECT\n"
                "A        DS    F\n"
                "B        DS    CL3\n"
                "SLEN     EQU   sizeof(S)\n" /* a DSECT is known when it is complete */
                "M        CSECT\n"
                "         DC    A(SLEN,sizeof(S),sizeof(M),alignof(S),alignof(M))\n"
                "         DC    H'0'\n"
                "L        LR    1,1\n"
                "         DC    A(offsetof(M,L),offsetof(S,B))\n"
                "BUF      DS    CL(sizeof(S))\n"
                "         END\n");
    uint32_t want[] = {7, 7, 39, 4, 4};
    int i;
    CHECK_MSG(r.m != NULL, "sections: %s", r.codes);
    if (!r.m) return;
    CHECK_MSG(r.len == 39, "length %lld", (long long)r.len);
    for (i = 0; i < 5; i++)
        CHECK_MSG(word(&r, 4 * i) == want[i], "sections %d: %u, want %u", i,
                  (unsigned)word(&r, 4 * i), (unsigned)want[i]);
    CHECK_MSG(word(&r, 24) == 22, "offsetof(M,L) is %u", (unsigned)word(&r, 24));
    CHECK_MSG(word(&r, 28) == 4, "offsetof(S,B) is %u", (unsigned)word(&r, 28));
}

static void expect_codes(const char *name, const char *src, const char *want) {
    Res r = run(src);
    CHECK_MSG(!strcmp(r.codes, want), "%s: codes \"%s\", want \"%s\"", name, r.codes, want);
}

static void test_errors(void) {
    const char *pre = "S DSECT\nA DS F\nM CSECT\nL LR 1,1\nQ EQU 5\n";
    char *s;
    s = xasprintf("%s DC A(sizeof(NOPE))\n END\n", pre);
    expect_codes("undefined", s, "ZA1011");
    s = xasprintf("%s DC A(sizeof(Q))\n END\n", pre);
    expect_codes("equ has no size", s, "ZA1041");
    s = xasprintf("%s DC A(countof(S))\n END\n", pre);
    expect_codes("countof of a section", s, "ZA1041");
    s = xasprintf("%s DC A(offsetof(A,A))\n END\n", pre);
    expect_codes("offsetof of a label", s, "ZA1041");
    s = xasprintf("%s DC A(offsetof(S,L))\n END\n", pre);
    expect_codes("not a field", s, "ZA1043");
    s = xasprintf("%s DC A(offsetof(S))\n END\n", pre);
    expect_codes("offsetof needs two", s, "ZA1040");
    s = xasprintf("%s DC A(sizeof(S,A))\n END\n", pre);
    expect_codes("sizeof takes one", s, "ZA1040");
    s = xasprintf("%s DC A(sizeof(1))\n END\n", pre);
    expect_codes("sizeof of a number", s, "ZA1040");
    /* the size of a CSECT cannot size storage */
    s = xasprintf("%sB DS CL(sizeof(M))\n END\n", pre);
    expect_codes("CSECT size sizes storage", s, "ZA1042");
    s = xasprintf("%sQQ EQU sizeof(M)\n END\n", pre);
    expect_codes("CSECT size in EQU", s, "ZA1042");
    /* a DSECT that grows after sizeof */
    expect_codes("DSECT grew",
                 "S DSECT\nA DS F\nSL EQU sizeof(S)\nS DSECT\nB DS F\nM CSECT\n DC A(SL)\n END\n",
                 "ZA1044");
    expect_codes("DSECT did not grow", "S DSECT\nA DS F\nSL EQU sizeof(S)\nM CSECT\n DC A(SL)\n END\n",
                 "");
}

static void test_operators(void) {
    uint32_t want[] = {1, 0, 1, 0, 1, 1, 0, 1, 1, 0, 1, 7, 0, 1};
    Res r = run("M CSECT\n"
                " DC A(1==1,1==2,1!=2,2<2,2<=2,3>2,2>=3,!0,!(1-1),!5,1&&2,"
                "(1<2)*7,0||0,0||3)\n"
                " END\n");
    int i;
    CHECK_MSG(r.m != NULL, "operators: %s", r.codes);
    if (!r.m) return;
    for (i = 0; i < 14; i++)
        CHECK_MSG(word(&r, 4 * i) == want[i], "operator %d: %u, want %u", i,
                  (unsigned)word(&r, 4 * i), (unsigned)want[i]);
    expect_codes("compare relocatable", "M CSECT\nA DS F\n DC A(A<1)\n END\n", "ZA1012");
    expect_codes("precedence", "M CSECT\n DC A(1+1==2&&3-1==2)\n END\n", "");
    expect_codes("single &", "M CSECT\n DC A(1&1)\n END\n", "ZA1002");
}

static void test_assert(void) {
    Reporter *rep;
    Module *m;
    char buf[2048];
    FILE *f = tmpfile();
    size_t n;
    const char *src = "S DSECT\nA DS F\nM CSECT\n"
                      " ASSERT sizeof(S)==4\n"
                      " ASSERT sizeof(S)<=2,'S is ''big'''\n"
                      " ASSERT sizeof(S)<=2\n"
                      " ASSERT alignof(S)==4,'fine'\n"
                      " END\n";
    expect_codes("ASSERT ok", "S DSECT\nA DS F\nM CSECT\n ASSERT sizeof(S)==4,'S is 4'\n LR 1,1\n END\n",
                 "");
    expect_codes("ASSERT forward", "M CSECT\n ASSERT sizeof(M)==2\n LR 1,1\n END\n", "");
    expect_codes("ASSERT forward label", "M CSECT\n ASSERT FWD-M>0\n LR 1,1\nFWD LR 1,1\n END\n", "");
    expect_codes("ASSERT fails", "M CSECT\n ASSERT 1==2\n END\n", "ZA1052");
    expect_codes("ASSERT undefined", "M CSECT\n ASSERT NOPE\n END\n", "ZA1011");
    expect_codes("ASSERT needs an operand", "M CSECT\n ASSERT\n END\n", "ZA0010");
    expect_codes("ASSERT message", "M CSECT\n ASSERT 1,nomsg\n END\n", "ZA1051");
    expect_codes("ASSERT message 2", "M CSECT\n ASSERT 1,'a'b'\n END\n", "ZA0002");
    expect_codes("ASSERT message 3", "M CSECT\n ASSERT 1,'a''b'\n END\n", "");
    expect_codes("ASSERT name", "M CSECT\nL ASSERT 1\n END\n", "ZA1053");
    expect_codes("ASSERT relocatable", "M CSECT\nL LR 1,1\n ASSERT L\n END\n", "ZA1013");
    expect_codes("ASSERT comma in message", "M CSECT\n ASSERT 0,'a,b'\n END\n", "ZA1052");
    rep = rep_new(0);
    m = asm_assemble_text("t.za", src, strlen(src), rep);
    CHECK(m == NULL);
    rep_print(rep, f, 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(strstr(buf, "assertion failed: S is 'big'") != NULL);
    CHECK(strstr(buf, "the condition was: sizeof(S)<=2") != NULL);
    CHECK(strstr(buf, "assertion failed: sizeof(S)<=2\n") != NULL);
    CHECK(strstr(buf, "fine") == NULL);
}

int main(void) {
    test_functions();
    test_sections();
    test_errors();
    test_operators();
    test_assert();
    TEST_MAIN_END();
}
