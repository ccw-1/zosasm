/* test_asm_free.c - the library destructors: asm_free() and rep_free()
 * must release a Module built from ordinary source without corrupting the
 * heap (the test runs under ASan/UBSan with make test-free-asan). */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "../src/diag.h"
#include "../src/ebcdic.h"
#include "../src/goff.h"
#include "testlib.h"

static void free_source(const char *src, int keep_errors) {
    Reporter *rep = rep_new(20);
    SrcVec lines = {0};
    Module *m;
    rep_add_source(rep, "free.za", src, strlen(src));
    src_split(&lines, "free.za", src, strlen(src));
    m = asm_assemble_all(lines.v, lines.n, "free.za", rep);
    CHECK_MSG(m != NULL, "module was built");
    if (m && keep_errors) {
        size_t n;
        char err[256];
        uint8_t *obj = module_emit(m, &n, err, sizeof err);
        CHECK_MSG(obj != NULL, "emit: %s", err);
        free(obj);
    }
    asm_free(m);
    if (lines.n) free((char *)lines.v[0].text);
    free(lines.v);
    rep_free(rep);
}

int main(void) {
    /* the same symbol three times: the file name in the references is borrowed */
    free_source("MAIN CSECT\n  USING *,15\n  L 2,SYM\n  L 3,SYM\n  L 4,SYM\nSYM DS F\n END\n", 1);
    /* a statement with a name and no operation */
    free_source("MAIN CSECT\nORPHAN\n END\n", 0);
    /* literals (synthetic statements), LOCTR, OPSYN, XATTR, PUSH/POP and ZSYM data */
    free_source("MAIN CSECT\n"
                "  LOCTR\n"
                "  USING *,15\n"
                "  L 2,=A(1)\n"
                "  L 3,=C'abc'\n"
                "  OPSYN MYL L\n"
                "  MYL 4,=A(2)\n"
                "  XATTR LINKAGE(XPLINK),SCOPE(SECTION)\n"
                "  ENTRY MAIN\n"
                "  LTORG\n"
                " END\n", 1);
    /* numeric constants: their Big temporaries live in the Module arena */
    free_source("T CSECT\n  DC F'1',D'2',L'3',DD'4.5',ED'-2'\n END\n", 1);
    /* errors with a partially built module */
    free_source("T CSECT\n  L 1,NOSUCH\n  DS 0H\n END\n", 0);
    TEST_MAIN_END();
}
