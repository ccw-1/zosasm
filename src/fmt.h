/* fmt.h - source formatter for .za files (the library behind zafmt). */
#ifndef ZA_FMT_H
#define ZA_FMT_H

#include <stddef.h>

typedef struct {
    int op_col;     /* column of the operation field, 1-based (default 10) */
    int remark_col; /* column of the remarks, 1-based; a longer operand field pushes them right (default 35) */
    int indent;     /* indentation per block level of the macro language (default 4) */
    int max_blank;  /* at most this many blank lines in a row (default 2) */
} FmtOptions;

void fmt_options_default(FmtOptions *o);

/* Format zosasm source text. The result is heap allocated and NUL terminated; it ends in one
 * newline (empty text stays empty). Only blanks and line ends change: no token is added,
 * removed or reordered, and text inside quotes is kept. Lines that cannot be parsed (a
 * quote or a parenthesis that is not closed) are only stripped of trailing blanks. */
char *za_format(const char *text, size_t len, const FmtOptions *o);

#endif
