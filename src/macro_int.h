/* macro_int.h - internals shared by macro.c, macro_expr.c and macro_val.c. */
#ifndef ZA_MACRO_INT_H
#define ZA_MACRO_INT_H

#include "macro.h"

/* the largest string a macro value may hold (4 MB) */
#define MA_MAX_STR ((size_t)4 << 20)

/* ---- values ------------------------------------------------------------- */

typedef enum { V_INT, V_STR, V_LIST } VType;

/* A value is immutable; a list or string is shared, never modified in place. A string that
 * looks like "(a,b,(c,d))" is also a list (see v_items). */
typedef struct MVal {
    VType t;
    int64_t i;               /* V_INT */
    const char *s;           /* V_STR: NUL terminated, n bytes */
    size_t n;                /* V_STR: length, V_LIST: count */
    const struct MVal *items; /* V_LIST */
} MVal;

MVal v_int(int64_t i);
MVal v_str(const char *s); /* the string is not copied */
MVal v_strn(const char *s, size_t n); /* copies */
MVal v_list(const MVal *items, size_t n); /* the array is not copied */
const char *v_text(MVal v); /* the text of the value (a list as "(a,b)"); never NULL */
int v_truth(MVal v);
/* self-defining term in text: 123, -4, 0x1F, X'1F', B'101', C'A'; 1 when valid */
int v_parse_int(const char *s, int64_t *out);
/* the elements when the value is a list or the text of a list; a non-list non-empty value is
 * one element, "" has none */
size_t v_items(MVal v, const MVal **items);
int v_is_list(MVal v);

/* ---- evaluation context -------------------------------------------------- */

typedef struct Var {
    char *name; /* lower case */
    int depth;
    MVal *slot;
} Var;

typedef struct Macro Macro;
typedef struct MStmt MStmt;

typedef struct Frame {
    Vec(Var) vars;
    const Macro *mac; /* NULL: open code */
    const char *label;
    MVal args; /* sys.args */
    char ndx[8];
    int level;
    Span call;
    int open;
} Frame;

typedef struct {
    MacSys *ms;
    Frame *f;
    const MStmt *st;
} Ctx;

/* an error or warning at the statement being run (with the "in expansion of" note) */
Diag *ma_err(Ctx *c, const char *code, const char *fmt, ...);
Diag *ma_warn(Ctx *c, const char *code, const char *fmt, ...);

/* the next input record (aread), or NULL */
char *ma_aread(Ctx *c);

/* the MVal operators (arithmetic, comparison); op is a token of macro_expr.c */
MVal ex_binop(Ctx *c, int op, MVal a, MVal b);

/* ---- expressions --------------------------------------------------------- */

enum {
    TK_EOF = 256, TK_INT, TK_STR, TK_ID,
    TK_EQ, TK_NE, TK_LE, TK_GE, TK_SHL, TK_SHR, TK_LAND, TK_LOR, TK_INC, TK_DEC,
    TK_ADDEQ, TK_SUBEQ, TK_MULEQ, TK_DIVEQ, TK_MODEQ, TK_SHLEQ, TK_SHREQ, TK_ANDEQ, TK_OREQ,
    TK_XOREQ
};

typedef enum { X_INT, X_STR, X_VAR, X_SYS, X_UN, X_BIN, X_LAND, X_LOR, X_COND, X_INDEX, X_CALL,
               X_IN } XKind;

typedef struct MExpr MExpr;
struct MExpr {
    XKind k;
    int op;
    int64_t i;
    char *s;
    MExpr *a, *b, *c;
    MExpr **args;
    int nargs;
};

typedef struct {
    MacSys *ms;
    const char *file;
    int line, col0; /* position of src[0] */
    const char *src;
    size_t p;       /* scan position */
    int tk;         /* the current token: a character or a TK_ value */
    int64_t tv;
    char *ts;       /* TK_STR text, TK_ID lower case name */
    size_t tstart;  /* where the token starts */
    int err;
    int depth; /* nesting of the expression being parsed */
} Parser;

void ps_init(Parser *ps, MacSys *ms, const char *file, int line, int col0, const char *src);
void ps_next(Parser *ps);
Diag *ps_error(Parser *ps, const char *code, const char *fmt, ...);
MExpr *ps_expr(Parser *ps); /* a full expression, ternary included */
MExpr *ps_cond_paren(Parser *ps); /* '(' expr ')' */
MVal ex_eval(Ctx *c, const MExpr *e);
/* the line text of a variable reference (lookup order: frame variables, built-ins) */
MVal *var_find(Frame *f, const char *name);
MVal sys_value(Ctx *c, const char *name, int *ok);

Reporter *ms_rep(MacSys *ms);
int ma_defined(Ctx *c, const char *lname); /* a variable, #define or macro of that lower case name */

/* ---- functions ----------------------------------------------------------- */

typedef MVal (*FnImpl)(Ctx *c, MVal *a, int n);
typedef struct {
    const char *name;
    int min, max; /* argument count, max -1 = any */
    FnImpl fn;
} Fn;
const Fn *fn_find(const char *name);

/* the built-in names the macro language knows (macro.c) */
int is_builtin_name(const char *n);

#endif
