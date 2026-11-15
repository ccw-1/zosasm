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

#endif
