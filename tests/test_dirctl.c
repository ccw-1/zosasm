/* test_dirctl.c - ACONTROL, PUSH/POP, CEJECT, EXITCTL, REPRO (src/dirctl.c). */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "testlib.h"

typedef struct {
    Module *m;
    char codes[256];
    char warns[256];
    char hex[256];
} Res;

static Res run(const char *body) {
    Res r;
    Reporter *rep = rep_new(0);
    char *src = xasprintf("M CSECT\n%s END\n", body);
    size_t i;
    memset(&r, 0, sizeof r);
    r.m = asm_assemble_text("t.za", src, strlen(src), rep);
    for (i = 0; i < rep_count(rep); i++) {
        const Diag *d = rep_get(rep, i);
        char *dst = d->sev == SEV_ERROR ? r.codes : d->sev == SEV_WARNING ? r.warns : NULL;
        if (!dst) continue;
        if (dst[0]) strcat(dst, " ");
        strncat(dst, d->code, 20);
    }
    if (r.m && r.m->sections.n) {
        const Class *c = r.m->sections.v[0]->classes.v[0];
        for (i = 0; i < (size_t)c->len && i < 100; i++) sprintf(r.hex + 2 * i, "%02X", c->data[i]);
    }
    return r;
}

static void hex(const char *src, const char *want) {
    Res r = run(src);
    CHECK_MSG(r.m != NULL && !r.codes[0], "%s: does not assemble (%s)", src, r.codes);
    if (r.m) CHECK_MSG(!strcmp(r.hex, want), "%s: %s, want %s", src, r.hex, want);
}

static void err(const char *src, const char *code) {
    Res r = run(src);
    CHECK_MSG(!strcmp(r.codes, code), "%s: codes \"%s\", want \"%s\"", src, r.codes, code);
}

static void test_afpr(void) {
    /* LER 1,2 is 3812 */
    hex(" LER 1,2\n", "3812");
    err(" ACONTROL NOAFPR\n LER 1,2\n", "ZA2071");
    err(" ACONTROL NOAFPR\n LER 0,9\n", "ZA2071");
    hex(" ACONTROL NOAFPR\n LER 0,6\n", "3806");
    hex(" ACONTROL NOAFPR\n ACONTROL AFPR\n LER 1,3\n", "3813");
    /* GPR operands are not affected */
    hex(" ACONTROL NOAFPR\n LR 1,3\n", "1813");
}

static void test_typecheck(void) {
    err(" ACONTROL TYPECHECK(REGISTER)\n LR 1,2\n", "ZA2067 ZA2067");
    hex(" ACONTROL TYPECHECK(REGISTER)\n LR %r1,%r2\n ACONTROL TYPECHECK(NOREGISTER)\n LR 3,4\n",
        "1812" "1834");
}

static void test_pushpop(void) {
    /* ACONTROL is restored */
    err(" PUSH ACONTROL\n ACONTROL NOAFPR\n POP ACONTROL\n LER 1,3\n", "");
    err(" ACONTROL NOAFPR\n PUSH ACONTROL\n ACONTROL AFPR\n POP ACONTROL\n LER 1,3\n", "ZA2071 ZA2071");
    /* USING is restored */
    hex(" USING *,12\nA DS 0H\n PUSH USING\n DROP 12\n POP USING\n L 1,A\n", "5810C000");
    err(" USING *,12\nA DS 0H\n PUSH USING\n DROP 12\n L 1,A\n", "ZA2030");
    err(" POP USING\n", "ZA2075");
    err(" POP ACONTROL\n", "ZA2075");
    err(" POP PRINT\n", "ZA2075");
    err(" PUSH\n", "ZA0010");
    err(" PUSH FOO\n", "ZA2074");
    err(" PUSH USING,USING\n", "ZA2074");
    err(" PUSH ,NOPRINT\n", "ZA2074");
}

static void test_others(void) {
    Res r;
    hex(" EXITCTL SOURCE,1,2\n DC X'01'\n", "01");
    err(" EXITCTL FOO,1\n", "ZA2076");
    err(" EXITCTL SOURCE\n", "ZA0010");
    hex(" CEJECT 3\n DC X'01'\n CEJECT\n", "01");
    err(" CEJECT 1,2\n", "ZA0010");
    /* REPRO skips the record that follows and warns */
    r = run(" REPRO\n THIS IS NOT A STATEMENT\n DC X'01'\n");
    CHECK(!r.codes[0]);
    CHECK(!strcmp(r.warns, "ZA2077"));
    CHECK(!strcmp(r.hex, "01"));
    /* ACONTROL options that are checked and ignored */
    hex(" ACONTROL COMPAT(CASE),FLAG(NOALIGN),RA2,ILMA,LIBMAC,CU(1200),MACHINE(ZSERIES),CA(819),CE(1047)\n DC X'01'\n", "01");
    err(" ACONTROL CE(37)\n", "ZA2073");
    err(" ACONTROL CA(923)\n", "ZA2073");
    err(" ACONTROL FOO\n", "ZA2072");
    err(" ACONTROL COMPAT(FOO)\n", "ZA2072");
}

int main(void) {
    test_afpr();
    test_typecheck();
    test_pushpop();
    test_others();
    TEST_MAIN_END();
}
