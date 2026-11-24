/* cblock.h - inline scalar C-like blocks (#pragma cblock, CBLOCK-DESIGN.md). */
#ifndef ZA_CBLOCK_H
#define ZA_CBLOCK_H

#include <stddef.h>

#include "asm.h"
#include "cblock_dbg.h"

/* One raw line of a captured block, with its physical line number. */
typedef struct {
    const char *text;
    int line;
} CBlockLine;

typedef Vec(CBlockLine) CBlockLineVec;

/* Compile the captured lines of a #pragma cblock block. head is the text
 * after "cblock" (the optional block name). Generated assembler lines are
 * appended to out. Returns 0 on success, nonzero after reporting an error. */
int cblock_compile(const char *file, int line, const char *head, const CBlockLine *body,
                   size_t n, Reporter *rep, SrcVec *out, int *label_counter,
                   CBlockDebugVec *debug);

#endif
