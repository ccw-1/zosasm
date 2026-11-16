/* mconv.h - HLASM macro definition -> zosasm macro (the engine of hlasm2za for MACRO..MEND). */
#ifndef ZA_MCONV_H
#define ZA_MCONV_H

#include <stdio.h>

#include "util.h"

enum { MH_STMT, MH_ASMCOMMENT, MH_MACCOMMENT, MH_BLANK };

/* one logical statement of the macro text, as written (not case folded) */
typedef struct {
    int line;  /* first physical line, 1-based */
    int kind;  /* MH_* */
    char *name, *op, *operands, *remark; /* MH_STMT */
    char *text; /* comments: the text after "*" or ".*" */
} MHStmt;

typedef Vec(MHStmt) MHStmtVec;

typedef struct {
    int flat; /* no structure recovery: #label / #goto for every sequence symbol */
    int keep_goto; /* leave #goto where the structure cannot show a jump (default: a dispatch loop) */
    void (*warn)(void *user, int line, const char *msg);
    void *user;
} MConvOptions;

/* Convert one macro. st[0] is the prototype statement; the last statement is MEND.
 * Returns the text "#macro ... #}\n", or NULL when the macro cannot be converted;
 * *why then says what stopped it (heap allocated, may be NULL). */
char *mconv_macro(const MHStmtVec *st, const MConvOptions *o, char **why);

/* upper case outside quoted strings (the folding of hlasm2za) */
char *conv_fold(const char *s);

#endif
