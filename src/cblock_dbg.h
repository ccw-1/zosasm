/* cblock_dbg.h - CBLOCK variable metadata for -g.  The CBLOCK compiler records
 * the assembler range and the ctemp slots of its locals; dbg.c turns this into
 * DWARF lexical blocks and variables. */
#ifndef ZA_CBLOCK_DBG_H
#define ZA_CBLOCK_DBG_H

#include <stddef.h>

typedef struct {
    char *name;
    int bits;  /* 32 or 64, 0 for a pointer */
    int unsig;
    int ptr;
    long off;  /* offset from the ctemp base register */
} CBlockVarDebug;

typedef struct {
    char *file;
    char *name;
    char *start_label;
    char *end_label;
    int line;
    int temp_reg;
    CBlockVarDebug *vars;
    size_t nvars;
} CBlockDebug;

typedef struct {
    CBlockDebug *v;
    size_t n, cap;
} CBlockDebugVec;

void cblock_debug_free(CBlockDebugVec *v);

#endif
