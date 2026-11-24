/* cblock.c - inline scalar C-like blocks. See PLAN-CBLOCK.md.
 *
 * M0: the preprocessor captures #pragma cblock through #pragma cend. The C
 * parser, type checker and code generator are not implemented yet. */
#include <string.h>

#include "cblock.h"
#include "util.h"

int cblock_compile(const char *file, int line, const char *head, const CBlockLine *body,
                   size_t n, Reporter *rep, SrcVec *out) {
    Span sp = span_make(file, line, 1, 1);
    (void)body;
    (void)n;
    (void)out;
    rep_errorf(rep, "ZA5002", sp, "CBLOCK \"%s\" is not implemented yet",
               head && *head ? head : "(unnamed)");
    return 1;
}
