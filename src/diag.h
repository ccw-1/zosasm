/* diag.h - structured diagnostics: severity, stable code, span, notes, help. */
#ifndef ZA_DIAG_H
#define ZA_DIAG_H

#include <stdio.h>

#include "util.h"

typedef enum { SEV_NOTE, SEV_WARNING, SEV_ERROR, SEV_FATAL } Severity;

/* Position in a source file; line and col are 1-based, col counts bytes.
 * file is a name that outlives the reporter (usually interned). */
typedef struct {
    const char *file;
    int line, col;
} Pos;

/* Highlighted range on one line; len in bytes (0 = one caret). */
typedef struct {
    Pos pos;
    int len;
} Span;

typedef struct {
    Span span;
    char *msg;
} Label;

typedef struct Diag {
    Severity sev;
    char code[8];
    char *msg;
    Span span;
    char *label; /* shown under the caret */
    Label *notes;
    int nnotes;
    char **help;
    int nhelp;
} Diag;

typedef struct Reporter Reporter;

Reporter *rep_new(int max_errors);
void rep_set_warn_error(Reporter *r, int on);
/* Register the text of a file so excerpts can be printed (text is copied). */
void rep_add_source(Reporter *r, const char *name, const char *text, size_t len);
/* The registered sources in registration order (the first is the main file);
 * returns 0 past the end. text is not NUL-terminated at len. */
int rep_get_source(const Reporter *r, size_t i, const char **name, const char **text, size_t *len);

/* The source line (file, line) was produced by macro expansion. Diagnostics
 * on it show the expanded text, followed by a note that names the macro
 * (defined at def) and shows the original line. */
void rep_add_expansion(Reporter *r, const char *file, int line, const char *expanded,
                       const char *macro, Span def);

Diag *rep_errorf(Reporter *r, const char *code, Span sp, const char *fmt, ...);
Diag *rep_warnf(Reporter *r, const char *code, Span sp, const char *fmt, ...);
/* an informative message: not counted as a warning */
Diag *rep_notef(Reporter *r, const char *code, Span sp, const char *fmt, ...);
Diag *diag_label(Diag *d, const char *fmt, ...);
Diag *diag_note(Diag *d, Span sp, const char *fmt, ...);
Diag *diag_help(Diag *d, const char *fmt, ...);

int rep_error_count(const Reporter *r);
int rep_warning_count(const Reporter *r);
int rep_too_many(const Reporter *r);
size_t rep_count(const Reporter *r);
/* a position in the report; rep_rewind drops the diagnostics added since (used when an assembly is repeated) */
size_t rep_mark(const Reporter *r);
void rep_rewind(Reporter *r, size_t mark);
const Diag *rep_get(const Reporter *r, size_t i); /* in report order */
/* Diagnostics ordered by file, line, column (stable); caller frees array. */
const Diag **rep_sorted(const Reporter *r, size_t *n);

void rep_print(Reporter *r, FILE *w, int color);
/* the same as one JSON document: {"diagnostics":[...],"errors":n,"warnings":n}; see GUIDE */
void rep_print_json(Reporter *r, FILE *w);
char *rep_summary(const Reporter *r);
/* free the reporter and all diagnostics it holds (the caller still owns
 * the FILE and any strings passed to rep_add_source/rep_errorf etc.) */
void rep_free(Reporter *r);

/* Closest candidate to word (case-insensitive) or NULL when nothing is a
 * plausible typo. */
const char *diag_closest(const char *word, const char *const *cands, size_t n);

Span span_make(const char *file, int line, int col, int len);

#endif
