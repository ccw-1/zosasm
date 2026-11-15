/* pp.h - the preprocessor stage in front of the statement reader.
 *
 * A C style preprocessor (#include, #define, #if, ...) replaces HLASM's
 * COPY and macro language. It works on whole lines and keeps the HLASM
 * source format in mind: no expansion in comments, remarks or quoted
 * strings, and none for an identifier directly followed by a quote. */
#ifndef ZA_PP_H
#define ZA_PP_H

#include "asm.h"

typedef struct {
    Vec(const char *) include_dirs; /* -I */
    Vec(const char *) defines;      /* -D: "NAME", "NAME=value", "NAME(a,b)=text" */
    int reproducible;               /* -reproducible: no unstable built-ins (__DATE__ needs a fixed time) */
    int has_epoch;                  /* epoch is the time for __DATE__ and __TIME__ */
    int64_t epoch;                  /* seconds since 1970-01-01 UTC; else SOURCE_DATE_EPOCH, else the clock */
} PpOptions;

/* Preprocess text (the contents of file) and append the resulting lines to
 * out. Included files are registered with rep_add_source; problems go to
 * rep. Expanded lines keep the file name and line number of the source line
 * they came from. */
void pp_run(const char *file, const char *text, size_t len, const PpOptions *opt, Reporter *rep,
            SrcVec *out);

#endif
