/* mconv_int.h - internals shared by mconv.c (statement translation) and mflow.c (structure recovery). */
#ifndef ZA_MCONV_INT_H
#define ZA_MCONV_INT_H

#include "mconv.h"

/* The macro body as a sequence of nodes. Code lines are already in zosasm form. */
typedef enum { N_CODE, N_COND, N_GOTO, N_RETURN, N_LABEL } NKind;

typedef struct {
    NKind k;
    int line;
    char *text;   /* N_CODE: one or more lines separated by \n (no indentation) */
    char *cond;   /* N_COND: condition text (zosasm expression) */
    char *ncond;  /* N_COND: the negated condition, written readably */
    char *target; /* N_COND, N_GOTO, N_LABEL: sequence symbol (lower case, no dot) */
    char *remark; /* trailing macro comment or NULL */
} MNode;

typedef Vec(MNode) MNodeVec;

/* Write the body: flat form (#label/#goto) or, when possible, structured. Appends to out.
 * Returns 1 for structured, 0 for flat as asked, -1 when the flow could not be structured
 * and the flat form was written. */
int mflow_write(Buf *out, const MNodeVec *nodes, int mode, const char *pcvar);

/* readability passes over a structured body: else if chains, switch */
char *mflow_post(const char *body);

/* mflow_write modes */
enum { MF_STRUCT, MF_FLAT, MF_KEEPGOTO };

#endif
