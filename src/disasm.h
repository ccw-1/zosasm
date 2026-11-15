/* disasm.h - turn a GOFF object back into zosasm source text. */
#ifndef ZA_DISASM_H
#define ZA_DISASM_H

#include <stdio.h>

#include "goff.h"

/* Returns the source (malloc'ed, NUL terminated), or NULL with a message in
 * err. Things the source language cannot express are reported on warn (may
 * be NULL) and left out; the count is stored in *nwarn (may be NULL).
 *
 * Objects written by zosasm convert to source that assembles to the very
 * same bytes; for other objects the conversion is best effort. */
char *disasm_module(const GoffModule *m, FILE *warn, int *nwarn, char *err, size_t errsz);

/* The same with options. An object that carries a ZSYM block (zsym.h) is
 * converted from it: the statements of the original source come back, with
 * their labels and symbolic operands; a section whose bytes no longer match
 * the information is written by the generic converter. DISASM_GENERIC
 * ignores ZSYM. *smart (may be NULL) tells whether ZSYM was used. */
#define DISASM_GENERIC 1
/* DISASM_STRICT: the statements taken from ZSYM get their registers written as
 * %r1 (the notation of -strict-registers); the generic converter always does. */
#define DISASM_STRICT 2
char *disasm_ex(const GoffModule *m, int flags, FILE *warn, int *nwarn, int *smart, char *err, size_t errsz);

#endif
