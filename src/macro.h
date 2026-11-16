/* macro.h - the C-like macro language (MACRO.md).
 *
 * Macro code lines start with '#'; every other line of a macro body is a
 * model statement that is emitted with &name substituted. The preprocessor
 * (pp.c) hands the lines of a macro definition or of an open code block to
 * this module and receives the generated lines through MacHost.emit; it
 * rescans them as source, so a generated line may call another macro.
 *
 * Diagnostic codes ZA4100-ZA4130, see MACRO.md section 11. */
#ifndef ZA_MACRO_H
#define ZA_MACRO_H

#include "asm.h"

typedef struct MacSys MacSys;

typedef struct {
    int line;   /* first physical line */
    char *text; /* the logical line: continuation lines are joined, in a model statement
                 * with '\n' between the physical lines */
} MacLine;

typedef struct {
    const char *file;
    int is_def; /* a #macro definition (else an open code block) */
    Vec(MacLine) lines;
} MacText;

typedef struct {
    void *user;
    /* one generated line (a label, operation and operands laid out in columns, or a comment):
     * the host rescans it as source. srcline is the line of the model statement for open code,
     * else 0 (the line of the call). */
    void (*emit)(void *user, const char *text, int srcline);
    /* current control section and its kind ("CSECT", "DSECT", ...), or NULL */
    const char *(*sect)(void *user, const char **type);
    /* is there a #define of that name? */
    int (*is_defined)(void *user, const char *name);
} MacHost;

/* step_limit: statements per expansion (0 = default 1,000,000). elf selects __ELF__. */
MacSys *mac_new(Reporter *rep, const MacHost *host, long step_limit, int elf);

/* Does the source line t start a macro statement at the top level of a file?
 * 0: no (an ordinary line or a preprocessor directive), 1: a #macro definition,
 * 2: another macro statement (#let, #while, ...). */
int mac_top_kind(const char *t);

/* Collect the statement that starts at lines[i] (a block runs to the matching '#}').
 * Returns the number of physical lines used (at least 1). */
size_t mac_collect(MacSys *ms, const SrcLine *lines, size_t n, size_t i, int kind, MacText *out);

/* Compile a collected #macro definition (a mistake is reported and the macro is not defined). */
void mac_define(MacSys *ms, const MacText *t);
/* Run a collected open code statement or block. */
void mac_exec(MacSys *ms, const MacText *t);

/* Is name an instruction or directive of the assembler? (library macros never hide those) */
int mac_known_op(const char *name);
/* where the macro was defined (empty span if unknown) */
Span mac_def_span(const MacSys *ms, const char *op);
/* Is op the name of a defined macro? */
int mac_defined(const MacSys *ms, const char *op);
/* Define every #macro of a library file. */
void mac_load_library(MacSys *ms, const char *file, const char *text, size_t len);

/* Expand a call. args are the operand texts as written ("" = omitted); callsite is the position
 * of the call line. Lines go to MacHost.emit. */
void mac_call(MacSys *ms, const char *op, const char *label, Span callsite, char **args,
              size_t nargs);

/* The section name and kind for sys.sect / sys.sectype are asked from the host. */
/* A diagnostic of the host's counting: how many macro calls were expanded (for tests). */
long mac_expansions(const MacSys *ms);

#endif
