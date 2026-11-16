/* convert.h - HLASM source to zosasm source (the library behind hlasm2za). */
#ifndef ZA_CONVERT_H
#define ZA_CONVERT_H

#include <stddef.h>
#include <stdio.h>

typedef struct {
    int begin;          /* first source column (ICTL begin), 1-based; default 1 */
    int end;            /* last source column (ICTL end); default 71 */
    int cont;           /* continuation column (ICTL continue); default 16, 0 = none */
    int strict;         /* write registers as %r1 for -strict-registers (default 1) */
    int width;          /* break operand lists after a comma beyond this width; 0 = never */
    const char *inc_ext; /* extension for COPY members, default ".inc" */
    int flat;           /* macros: #goto/#label for every sequence symbol, no structure recovery */
    int keep_goto;      /* macros: where the flow cannot be structured, write #goto instead of a dispatch loop */
    int member;         /* a macro library member: write the macro definitions only, as .zmac text */
    const char *copy_dir; /* macros: COPY inside a macro reads DIR/MEMBER.mac (else it is not converted) */
    int comment_macros; /* macros: comment the definitions out (the old behaviour) */
} ConvOptions;

void conv_options_default(ConvOptions *o);

/* Convert HLASM source text. The result is heap allocated (NUL terminated).
 * Things that could not be converted, or that were guessed, are reported as
 * "file:line: warning: text" lines on warn (may be NULL); *nwarn (may be
 * NULL) receives their number. A leading ICTL statement changes the columns
 * for the rest of the text. */
char *hlasm_convert(const char *text, size_t len, const char *file, const ConvOptions *opt,
                    FILE *warn, int *nwarn);

#endif
