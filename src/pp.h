/* pp.h - the preprocessor stage in front of the statement reader.
 *
 * A C style preprocessor (#include, #define, #if, ...) replaces HLASM's
 * COPY and macro language. It works on whole lines and keeps the HLASM
 * source format in mind: no expansion in comments, remarks or quoted
 * strings, and none for an identifier directly followed by a quote. */
#ifndef ZA_PP_H
#define ZA_PP_H

#include "asm.h"

/* What happened to each source line (for the listing). */
typedef enum { PE_LINE, PE_DIRECTIVE, PE_SKIPPED } PpKind;

typedef struct {
    const char *file;
    int line, line_end; /* the physical lines (more than one when joined by a backslash) */
    int depth;          /* #include nesting, 0 = the main file */
    PpKind kind;        /* ordinary line / preprocessor directive / inside a false #if */
    const char *text;   /* the line as written */
    size_t first, n;    /* PE_LINE: the lines it produced in the output array (0 when expansion
                         * produced nothing) */
} PpEvent;

typedef Vec(PpEvent) PpEventVec;

typedef struct {
    PpEventVec *events;          /* if not NULL: every source line is recorded here, in order */
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
