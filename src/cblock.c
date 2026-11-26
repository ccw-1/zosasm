/* cblock.c - inline scalar C-like blocks. See docs/CBLOCK.md.
 *
 * The implementation covers expressions, bound registers, scalar locals,
 * assignments, integer operators, control flow including switch and goto,
 * byte pointers, and the memory and scan intrinsics. The parser is
 * deliberately small and reports ZA5xxx diagnostics. */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "cblock.h"
#include "util.h"

typedef struct {
    int bits;  /* 32 or 64 */
    int unsig; /* unsigned */
    int ptr;   /* a byte/void pointer value */
    int target; /* named opaque pointer target: 0 byte/void, else CBlock->ptypenames index + 1 */
    int elem_bits;  /* typed pointer element width: 0 byte/void/named, else 32 or 64 */
    int elem_unsig;
} CType;

typedef enum { V_CIN, V_COUT, V_CINOUT, V_LOCAL } VKind;

typedef struct CBVar {
    char *name;
    CType type;
    VKind kind;
    int reg;       /* bound register, -1 otherwise */
    int slot;      /* temp-area slot index */
    int initialized;
    char *lit;     /* string literal the variable was initialized with, or NULL */
    size_t lit_len;
} CBVar;

typedef Vec(CBVar *) CBVarVec;

typedef enum { E_NUM, E_STR, E_VAR, E_UN, E_BIN, E_CAST, E_COND, E_INC, E_CALL, E_DEREF, E_ADDR } CBEKind;

typedef struct CBExpr CBExpr;
static void cblock_expr_free(CBExpr *e);
struct CBExpr {
    CBEKind kind;
    CType type;
    int op; /* operator character or OP_* */
    int64_t num;
    CBVar *var;
    CBExpr *a, *b, *c;
    char *name; /* E_CALL intrinsic */
    char *str;  /* E_STR bytes, not necessarily NUL terminated */
    size_t slen; /* E_STR length */
    int align;  /* E_STR table alignment, 0/1 = none */
    char *sym, *field; /* __sizeof/__offsetof symbol arguments */
    int read_before;
    int line, col;
};

typedef enum { S_EXPR, S_ASSIGN, S_DECL, S_IF, S_WHILE, S_DO, S_FOR, S_BREAK, S_CONTINUE,
               S_LABEL, S_GOTO, S_SWITCH, S_CASE, S_DEFAULT,
               S_MEMCPY, S_MEMMOVE, S_MEMSET } CBSKind;
typedef struct CBStmt CBStmt;
typedef Vec(CBStmt) CBStmtVec;
struct CBStmt {
    CBSKind kind;
    CBVar *var; /* S_ASSIGN to a variable, NULL for an lvalue expression */
    CBExpr *lval; /* S_ASSIGN to *p or p[i] */
    int op; /* assignment operator */
    CBExpr *expr;
    CBExpr *cond;
    CBExpr *step;
    char *label; /* S_LABEL internal name, S_GOTO target, S_CASE/S_DEFAULT label */
    int64_t num; /* S_CASE value */
    CBStmtVec body;
    CBStmtVec other;
    CBStmtVec init;
    int line;
};

typedef struct CBString {
    char *data;
    size_t len;
    int align;
    char *label;
} CBString;

typedef struct {
    const char *file;
    int line;
    char *name;
    CBVarVec vars;
    StrMap byname;
    int scratch[16];
    int nscratch;
    Vec(char *) ptypenames; /* opaque pointer target names, index + 1 in CType.target */
    struct CBString *lits; /* string literal tables, emitted after the block */
    size_t nlits, lits_cap;
    char *lit_end; /* branch target after the literal tables */
    int temp_reg;
    long temp_size;
    int temp_down;
    int save_r0, save_r1;
    int base_slots;
    int label_no;
    int label_seq;
    StrMap labels; /* C label name -> internal assembler label */
    Reporter *rep;
    SrcVec *out;
    const CBlockLine *body;
    size_t body_n;
    CBlockDebugVec *debug;
    char *start_label, *end_label;
} CBlock;

/* multi-character operators */
enum { OP_SHL = 256, OP_SHR, OP_LE, OP_GE, OP_EQ, OP_NE,
       OP_ADDEQ, OP_SUBEQ, OP_MULEQ, OP_DIVEQ, OP_MODEQ,
       OP_ANDEQ, OP_OREQ, OP_XOREQ, OP_SHLEQ, OP_SHREQ,
       OP_LAND, OP_LOR, OP_INC, OP_DEC };

/* ---- small helpers ------------------------------------------------------ */

static CType type_of(int bits, int unsig) {
    CType t;
    t.bits = bits;
    t.unsig = unsig;
    t.ptr = 0;
    t.target = 0;
    t.elem_bits = 0;
    t.elem_unsig = 0;
    return t;
}
static CType type_ptr(void) {
    CType t;
    t.bits = 64;
    t.unsig = 1;
    t.ptr = 1;
    t.target = 0;
    t.elem_bits = 8; /* byte pointer: pointer arithmetic in bytes, *p is one byte */
    t.elem_unsig = 1;
    return t;
}
static CType type_ptr_scalar(int bits, int unsig) {
    CType t = type_ptr();
    t.elem_bits = bits;
    t.elem_unsig = unsig;
    return t;
}

static CType type_ptr_named(CBlock *b, const char *name) {
    CType t = type_ptr();
    size_t i;
    t.elem_bits = 0;
    t.elem_unsig = 0;
    for (i = 0; i < b->ptypenames.n; i++)
        if (!strcmp(b->ptypenames.v[i], name)) {
            t.target = (int)i + 1;
            return t;
        }
    vec_push(&b->ptypenames, xstrdup(name));
    t.target = (int)b->ptypenames.n;
    return t;
}

static int type_same(CType a, CType b) {
    return a.bits == b.bits && a.unsig == b.unsig && a.ptr == b.ptr && a.target == b.target &&
           a.elem_bits == b.elem_bits && a.elem_unsig == b.elem_unsig;
}

static int type_parse(const char *s, CType *t) {
    char *p = xstrdup(s);
    char *q = str_trim(p);
    int r = 0;
    size_t n = strlen(q);
    while (n > 0 && (q[n - 1] == ' ' || q[n - 1] == '\t')) q[--n] = 0;
    if (n > 5 && !za_strncmpi(q, "const ", 6)) { q += 6; n -= 6; }
    while (n > 0 && (q[n - 1] == ' ' || q[n - 1] == '\t')) q[--n] = 0;
    if (n > 0 && q[n - 1] == '*') {
        q[--n] = 0;
        while (n > 0 && (q[n - 1] == ' ' || q[n - 1] == '\t')) q[--n] = 0;
        if (za_streqi(q, "uint8_t") || za_streqi(q, "void") || za_streqi(q, "char")) {
            *t = type_ptr();
            free(p);
            return 1;
        }
    }
    if (za_streqi(q, "int32_t")) { *t = type_of(32, 0); r = 1; }
    else if (za_streqi(q, "uint32_t")) { *t = type_of(32, 1); r = 1; }
    else if (za_streqi(q, "int64_t")) { *t = type_of(64, 0); r = 1; }
    else if (za_streqi(q, "uint64_t")) { *t = type_of(64, 1); r = 1; }
    else if (za_streqi(q, "int")) { *t = type_of(32, 0); r = 1; }
    else if (za_streqi(q, "unsigned")) { *t = type_of(32, 1); r = 1; }
    free(p);
    return r;
}
static int type_text_parse(CBlock *b, const char *text, CType *t) {
    char *p = xstrdup(text), *q = str_trim(p);
    size_t n = strlen(q);
    int r = 0;
    while (n > 0 && (q[n - 1] == ' ' || q[n - 1] == '\t')) q[--n] = 0;
    if (n > 5 && !za_strncmpi(q, "const ", 6)) { q += 6; n -= 6; }
    while (n > 0 && (q[n - 1] == ' ' || q[n - 1] == '\t')) q[--n] = 0;
    if (n > 0 && q[n - 1] == '*') {
        q[--n] = 0;
        while (n > 0 && (q[n - 1] == ' ' || q[n - 1] == '\t')) q[--n] = 0;
        if (za_streqi(q, "uint8_t") || za_streqi(q, "void") || za_streqi(q, "char")) {
            *t = type_ptr();
            r = 1;
        } else {
            CType base;
            if (type_parse(q, &base)) {
                *t = type_ptr_scalar(base.bits, base.unsig);
                r = 1;
            } else {
                *t = type_ptr_named(b, q);
                r = 1;
            }
        }
    } else {
        r = type_parse(q, t);
    }
    free(p);
    return r;
}

#if 0
static int type_parse_old(const char *s, CType *t) {
    if (za_streqi(s, "int32_t")) { *t = type_of(32, 0); return 1; }
    if (za_streqi(s, "uint32_t")) { *t = type_of(32, 1); return 1; }
    if (za_streqi(s, "int64_t")) { *t = type_of(64, 0); return 1; }
    if (za_streqi(s, "uint64_t")) { *t = type_of(64, 1); return 1; }
    if (za_streqi(s, "int")) { *t = type_of(32, 0); return 1; }
    if (za_streqi(s, "unsigned")) { *t = type_of(32, 1); return 1; }
    return 0;
}
#endif

static CType type_usual(CType a, CType b) {
    if (a.bits == b.bits) return type_of(a.bits, a.unsig || b.unsig);
    if (a.bits > b.bits) return type_of(a.bits, a.unsig);
    return type_of(b.bits, b.unsig);
}

static char *trim(const char *s) { return str_trim(s); }

static int reg_parse(const char *s, int *out) {
    const char *p = s;
    char kind;
    int n;
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != '%') return 0;
    kind = (char)za_tolower((unsigned char)*p++);
    if (kind != 'r' || !za_isdigit((unsigned char)*p)) return 0;
    n = 0;
    while (za_isdigit((unsigned char)*p)) {
        n = n * 10 + (*p - '0');
        p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p || n > 15) return 0;
    *out = n;
    return 1;
}

/* ---- diagnostics -------------------------------------------------------- */

static void cb_error(CBlock *b, int line, int col, const char *code, const char *fmt, ...) {
    va_list ap;
    char *msg;
    va_start(ap, fmt);
    msg = xvasprintf(fmt, ap);
    va_end(ap);
    rep_errorf(b->rep, code, span_make(b->file, line, col, 1), "%s", msg);
    free(msg);
}

/* ---- captured lines and the C lexer ------------------------------------ */

typedef enum { TK_EOF, TK_ID, TK_NUM, TK_STR, TK_OP } TokKind;
typedef struct {
    TokKind kind;
    char *s; /* TK_ID, TK_STR */
    size_t slen; /* TK_STR */
    int64_t num;
    int op;
    int line, col;
} Tok;

typedef struct {
    CBlock *b;
    const char *src;
    int *line_at;
    size_t n, pos;
    Tok tok;
    Vec(char *) ids;
} Lex;

static int is_id_start(int c) { return za_isalpha(c) || c == '_'; }
static int is_id_char(int c) { return is_id_start(c) || za_isdigit(c); }

static void lex_skip(Lex *l) {
    for (;;) {
        while (l->pos < l->n && (l->src[l->pos] == ' ' || l->src[l->pos] == '\t' ||
                                 l->src[l->pos] == '\n' || l->src[l->pos] == '\r'))
            l->pos++;
        if (l->pos + 1 < l->n && l->src[l->pos] == '/' && l->src[l->pos + 1] == '/') {
            while (l->pos < l->n && l->src[l->pos] != '\n') l->pos++;
            continue;
        }
        if (l->pos + 1 < l->n && l->src[l->pos] == '/' && l->src[l->pos + 1] == '*') {
            int line = l->line_at[l->pos];
            l->pos += 2;
            while (l->pos + 1 < l->n && !(l->src[l->pos] == '*' && l->src[l->pos + 1] == '/')) l->pos++;
            if (l->pos + 1 < l->n) l->pos += 2;
            else cb_error(l->b, line, 1, "ZA5001", "unterminated C comment");
            continue;
        }
        break;
    }
}

static void lex_next(Lex *l) {
    size_t start;
    int line, col;
    if (l->tok.s) { /* strings stay owned by ids until the block is freed */ }
    l->tok.s = NULL;
    lex_skip(l);
    start = l->pos;
    line = l->pos < l->n ? l->line_at[l->pos] : l->b->line;
    col = 1;
    if (l->pos < l->n) {
        size_t p = l->pos;
        while (p > 0 && l->src[p - 1] != '\n') { p--; col++; }
    }
    if (l->pos >= l->n) {
        l->tok.kind = TK_EOF;
        l->tok.line = line;
        l->tok.col = col;
        return;
    }
    if (l->src[l->pos] == '"') {
        Buf b = {0};
        char *str;
        size_t n = 0;
        for (;;) {
            size_t j = l->pos + 1;
            while (j < l->n && l->src[j] != '"') {
                int c = (unsigned char)l->src[j++];
                if (c == '\\' && j < l->n) {
                    c = (unsigned char)l->src[j++];
                    switch (c) {
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'v': c = '\v'; break;
                    case 'a': c = '\a'; break;
                    case '\\': break;
                    case '\'': break;
                    case '"': break;
                    case '?': break;
                    case 'x': {
                        int v = 0, k = 0;
                        while (j < l->n && k < 2) {
                            int h = (unsigned char)l->src[j], d;
                            if (h >= '0' && h <= '9') d = h - '0';
                            else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                            else break;
                            v = v * 16 + d;
                            j++;
                            k++;
                        }
                        c = v;
                        break;
                    }
                    default:
                        if (c >= '0' && c <= '7') {
                            int v = c - '0', k = 1;
                            while (j < l->n && k < 3 && l->src[j] >= '0' && l->src[j] <= '7') {
                                v = v * 8 + (l->src[j] - '0');
                                j++;
                                k++;
                            }
                            c = v & 0xFF;
                        }
                        break;
                    }
                }
                buf_putc(&b, (char)c);
                n++;
            }
            if (j >= l->n) {
                cb_error(l->b, line, col, "ZA5001", "unterminated string literal");
                break;
            }
            l->pos = j + 1;
            lex_skip(l);
            if (!(l->pos < l->n && l->src[l->pos] == '"')) break;
            /* adjacent string literals are concatenated, as in C */
        }
        str = xmalloc(n + 1);
        memcpy(str, b.p ? b.p : "", n);
        str[n] = 0;
        free(b.p);
        l->tok.kind = TK_STR;
        l->tok.s = str;
        l->tok.slen = n;
        vec_push(&l->ids, str);
        return;
    } else if (is_id_start((unsigned char)l->src[l->pos])) {
        size_t j = l->pos + 1;
        while (j < l->n && is_id_char((unsigned char)l->src[j])) j++;
        l->tok.kind = TK_ID;
        l->tok.s = xstrndup(l->src + l->pos, j - l->pos);
        vec_push(&l->ids, l->tok.s);
        l->pos = j;
    } else if (za_isdigit((unsigned char)l->src[l->pos])) {
        uint64_t v = 0;
        int base = 10, suffix = 0, digits = 0;
        size_t j = l->pos;
        if (l->src[j] == '0' && j + 1 < l->n && (l->src[j + 1] == 'x' || l->src[j + 1] == 'X')) {
            base = 16; j += 2;
        } else if (l->src[j] == '0' && j + 1 < l->n && (l->src[j + 1] == 'b' || l->src[j + 1] == 'B')) {
            base = 2; j += 2;
        }
        for (; j < l->n; j++) {
            int c = (unsigned char)l->src[j], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            if (d >= base || v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) {
                cb_error(l->b, line, col, "ZA5001", "integer literal is too large");
                break;
            }
            v = v * (uint64_t)base + (uint64_t)d;
            digits++;
        }
        if (!digits) cb_error(l->b, line, col, "ZA5001", "invalid integer literal");
        while (j < l->n && (l->src[j] == 'u' || l->src[j] == 'U' || l->src[j] == 'l' || l->src[j] == 'L')) {
            suffix |= (l->src[j] == 'u' || l->src[j] == 'U') ? 1 : 2;
            j++;
        }
        l->tok.kind = TK_NUM;
        l->tok.num = (int64_t)v;
        /* the literal type is inferred in the parser from the value and suffix */
        l->tok.line = line;
        l->tok.col = col;
        l->pos = j;
        if (suffix == 2 && v <= INT64_MAX) { /* long/long long: make it 64 bit */ l->tok.op = 1; }
        else l->tok.op = suffix;
    } else {
        int c = (unsigned char)l->src[l->pos++];
        int op = c;
        if (c == '&' && l->pos < l->n && l->src[l->pos] == '&') { op = OP_LAND; l->pos++; }
        else if (c == '|' && l->pos < l->n && l->src[l->pos] == '|') { op = OP_LOR; l->pos++; }
        else if (c == '+' && l->pos < l->n && l->src[l->pos] == '+') { op = OP_INC; l->pos++; }
        else if (c == '-' && l->pos < l->n && l->src[l->pos] == '-') { op = OP_DEC; l->pos++; }
        else if (c == '+' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_ADDEQ; l->pos++; }
        else if (c == '-' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_SUBEQ; l->pos++; }
        else if (c == '*' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_MULEQ; l->pos++; }
        else if (c == '/' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_DIVEQ; l->pos++; }
        else if (c == '%' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_MODEQ; l->pos++; }
        else if (c == '&' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_ANDEQ; l->pos++; }
        else if (c == '|' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_OREQ; l->pos++; }
        else if (c == '^' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_XOREQ; l->pos++; }
        else if (c == '<' && l->pos + 1 < l->n && l->src[l->pos] == '<' && l->src[l->pos + 1] == '=') { op = OP_SHLEQ; l->pos += 2; }
        else if (c == '>' && l->pos + 1 < l->n && l->src[l->pos] == '>' && l->src[l->pos + 1] == '=') { op = OP_SHREQ; l->pos += 2; }
        else if (c == '<' && l->pos < l->n && l->src[l->pos] == '<') { op = OP_SHL; l->pos++; }
        else if (c == '>' && l->pos < l->n && l->src[l->pos] == '>') { op = OP_SHR; l->pos++; }
        else if (c == '<' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_LE; l->pos++; }
        else if (c == '>' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_GE; l->pos++; }
        else if (c == '=' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_EQ; l->pos++; }
        else if (c == '!' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_NE; l->pos++; }
        l->tok.kind = TK_OP;
        l->tok.op = op;
    }
    l->tok.line = line;
    l->tok.col = col;
    if (l->tok.kind == TK_OP && l->tok.op == 0) l->tok.op = start;
}

static int tok_is(const Lex *l, int op) {
    return l->tok.kind == TK_OP && l->tok.op == op;
}
static int tok_id(const Lex *l, const char *s) {
    return l->tok.kind == TK_ID && za_streqi(l->tok.s, s);
}

static int peek_is_star(Lex *l) {
    Lex t = *l;
    int r;
    t.ids.v = NULL;
    t.ids.n = t.ids.cap = 0;
    t.tok.s = NULL;
    lex_next(&t);
    r = tok_is(&t, '*');
    {
        size_t i;
        for (i = 0; i < t.ids.n; i++) free(t.ids.v[i]);
        free(t.ids.v);
    }
    return r;
}

static int after_tok_colon(const Lex *l) {
    const char *s = l->src + l->pos;
    while (*s == ' ' || *s == '\t') s++;
    return *s == ':' && s[1] != ':';
}

/* ---- parser ------------------------------------------------------------- */

typedef struct {
    CBlock *b;
    Lex l;
    int err;
    int loop_depth;
    int switch_depth;
    int switch_default[16];
} Parser;

static CBExpr *parse_cond(Parser *p);
static CBExpr *parse_assignment(Parser *p);
static CBExpr *parse_unary(Parser *p);
static void parse_stmt(Parser *p, CBStmtVec *st);

static CBExpr *mk_expr(CBEKind k, CType t, int line, int col) {
    CBExpr *e = xcalloc(sizeof *e);
    e->kind = k;
    e->type = t;
    e->line = line;
    e->col = col;
    return e;
}

static CBExpr *mk_cast(CBExpr *e, CType t) {
    CBExpr *c;
    if (!e || type_same(e->type, t)) return e;
    c = mk_expr(E_CAST, t, e->line, e->col);
    c->a = e;
    return c;
}

static CBExpr *mk_bin(Parser *p, int op, CBExpr *a, CBExpr *b) {
    CType t;
    (void)p;
    if ((op == '+' || op == '-') && b->type.ptr && !a->type.ptr) {
        if (op == '+') {
            CBExpr *sw = a;
            a = b;
            b = sw;
        } else {
            cb_error(p->b, a->line, a->col, "ZA5002", "pointer subtraction needs a pointer on the left");
            p->err = 1;
            return a;
        }
    }
    if (op == OP_EQ || op == OP_NE || op == OP_LE || op == OP_GE || op == '<' || op == '>' ||
        op == OP_LAND || op == OP_LOR) {
        t = type_of(32, 0);
        if (op != OP_LAND && op != OP_LOR && a->type.ptr && b->type.ptr) {
            /* pointer comparison: the operands stay pointers */
        }
    } else if (op == '+' && (a->type.ptr || b->type.ptr)) {
        if (a->type.ptr && b->type.ptr) {
            cb_error(p->b, a->line, a->col, "ZA5002", "cannot add two pointers");
            p->err = 1;
            return a;
        }
        t = a->type.ptr ? a->type : b->type;
    } else if (op == '-' && a->type.ptr && b->type.ptr) {
        t = type_of(64, 0); /* byte pointer difference */
    } else if (op == '-' && a->type.ptr) {
        t = type_ptr();
    } else if (a->type.ptr || b->type.ptr) {
        cb_error(p->b, a->line, a->col, "ZA5002", "unsupported pointer operation");
        p->err = 1;
        return a;
    } else {
        t = type_usual(a->type, b->type);
        a = mk_cast(a, t);
        b = mk_cast(b, t);
    }
    {
        CBExpr *e = mk_expr(E_BIN, t, a->line, a->col);
        e->op = op;
        e->a = a;
        e->b = b;
        return e;
    }
}

static CBExpr *parse_primary(Parser *p) {
    Lex *l = &p->l;
    CBExpr *e;
    int line = l->tok.line, col = l->tok.col;
    if (l->tok.kind == TK_STR) {
        e = mk_expr(E_STR, type_ptr(), line, col);
        e->str = xmalloc(l->tok.slen + 1);
        memcpy(e->str, l->tok.s, l->tok.slen);
        e->str[l->tok.slen] = 0;
        e->slen = l->tok.slen;
        lex_next(l);
        return e;
    }
    if (l->tok.kind == TK_NUM) {
        uint64_t v = (uint64_t)l->tok.num;
        CType t;
        int suffix = l->tok.op;
        if (suffix & 2) {
            t = (suffix & 1) || v > INT64_MAX ? type_of(64, 1) : type_of(64, 0);
        } else if (suffix & 1) {
            t = v <= UINT32_MAX ? type_of(32, 1) : type_of(64, 1);
        } else if (v <= INT32_MAX) t = type_of(32, 0);
        else if (v <= UINT32_MAX) t = type_of(32, 1);
        else if (v <= INT64_MAX) t = type_of(64, 0);
        else t = type_of(64, 1);
        e = mk_expr(E_NUM, t, line, col);
        e->num = (int64_t)v;
        lex_next(l);
        return e;
    }
    if (l->tok.kind == TK_ID) {
        char *name = xstrdup(l->tok.s);
        if (!strcmp(name, "__sizeof") || !strcmp(name, "__offsetof")) {
            int is_off = !strcmp(name, "__offsetof");
            char *sym = NULL, *field = NULL, *iname = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(') || (lex_next(l), l->tok.kind != TK_ID)) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001",
                         "expected a section name in %s", iname);
                free(iname); free(sym); free(field);
                p->err = 1;
                return NULL;
            }
            sym = xstrdup(l->tok.s);
            lex_next(l);
            if (is_off) {
                if (!tok_is(l, ',')) {
                    cb_error(p->b, l->tok.line, l->tok.col, "ZA5001",
                             "expected ',' in __offsetof");
                    free(iname); free(sym);
                    p->err = 1;
                    return NULL;
                }
                lex_next(l);
                if (l->tok.kind != TK_ID) {
                    cb_error(p->b, l->tok.line, l->tok.col, "ZA5001",
                             "expected a field name in __offsetof");
                    free(iname); free(sym);
                    p->err = 1;
                    return NULL;
                }
                field = xstrdup(l->tok.s);
                lex_next(l);
            }
            if (!tok_is(l, ')')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ')' after %s", iname);
                free(iname); free(sym); free(field);
                p->err = 1;
                return NULL;
            }
            lex_next(l);
            {
                CBExpr *e = mk_expr(E_CALL, type_of(64, 1), line, col);
                e->name = iname;
                e->sym = sym;
                e->field = field;
                return e;
            }
        }
        if (!strcmp(name, "__strlen") || !strcmp(name, "__memchr") ||
            !strcmp(name, "__memcmp") || !strcmp(name, "__strcmp")) {
            CBExpr *e = mk_expr(E_CALL, type_of(64, 1), line, col);
            int want = !strcmp(name, "__strlen") ? 1 : !strcmp(name, "__strcmp") ? 2 : 3;
            int is_memchr = !strcmp(name, "__memchr");
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after intrinsic");
                free(e->name); free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            e->a = parse_assignment(p);
            if (want >= 2) {
                if (!tok_is(l, ',')) {
                    cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ',' in intrinsic");
                    free(e->name); free(e); p->err = 1; return NULL;
                }
                lex_next(l);
                e->b = parse_assignment(p);
            }
            if (want == 3) {
                if (!tok_is(l, ',')) {
                    cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ',' in intrinsic");
                    free(e->name); free(e); p->err = 1; return NULL;
                }
                lex_next(l);
                e->c = parse_assignment(p);
            }
            if (!tok_is(l, ')')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ')' in intrinsic");
                free(e->name); free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!e->a || (want >= 2 && !e->b) || (want == 3 && !e->c)) {
                cblock_expr_free(e);
                p->err = 1;
                return NULL;
            }
            if (!e->a->type.ptr) {
                cb_error(p->b, line, col, "ZA5003", "%s needs a pointer", e->name);
                p->err = 1;
            }
            if (!is_memchr && want >= 2 && e->b && !e->b->type.ptr) {
                cb_error(p->b, line, col, "ZA5003", "%s needs pointer arguments", e->name);
                p->err = 1;
            }
            if (want == 3 && e->c && e->c->type.ptr) {
                cb_error(p->b, line, col, "ZA5003", "%s needs an integer length", e->name);
                p->err = 1;
            }
            if (!strcmp(e->name, "__strlen")) e->type = type_of(64, 1);
            else if (is_memchr) e->type = type_ptr();
            else e->type = type_of(32, 0);
            return e;
        }
        CBVar *v = strmap_get(&p->b->byname, name);
        CType t;
        if (type_parse(name, &t)) {
            cb_error(p->b, line, col, "ZA5002", "type name \"%s\" is not an expression", name);
            free(name);
            lex_next(l);
            return NULL;
        }
        if (!v) {
            cb_error(p->b, line, col, "ZA5004", "undeclared variable \"%s\"", name);
            free(name);
            lex_next(l);
            return NULL;
        }
        free(name);
        e = mk_expr(E_VAR, v->type, line, col);
        e->var = v;
        e->read_before = !v->initialized;
        lex_next(l);
        return e;
    }
    if (tok_is(l, '(')) {
        lex_next(l);
        if (l->tok.kind == TK_ID) {
            CType ct;
            char *tn = xstrdup(l->tok.s);
            if (type_parse(tn, &ct)) {
                lex_next(l);
                free(tn);
                if (tok_is(l, '*')) {
                    ct = type_ptr_scalar(ct.bits, ct.unsig);
                    lex_next(l);
                    if (!tok_is(l, ')')) {
                        cb_error(p->b, l->tok.line, l->tok.col, "ZA5001",
                                 "expected ')' after a cast type");
                        p->err = 1;
                        return NULL;
                    }
                    lex_next(l);
                    e = parse_unary(p);
                    if (!e) return NULL;
                    return mk_cast(e, ct);
                }
                if (!tok_is(l, ')')) {
                    cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ')' after a cast type");
                    p->err = 1;
                    return NULL;
                }
                lex_next(l);
                e = parse_unary(p);
                if (!e) return NULL;
                return mk_cast(e, ct);
            }
            if (peek_is_star(l)) {
                lex_next(l); /* move to * */
                lex_next(l); /* past * */
                ct = type_ptr_named(p->b, tn);
                free(tn);
                if (!tok_is(l, ')')) {
                    cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ')' after a cast type");
                    p->err = 1;
                    return NULL;
                }
                lex_next(l);
                e = parse_unary(p);
                if (!e) return NULL;
                return mk_cast(e, ct);
            }
            free(tn);
        }
        e = parse_cond(p);
        if (!tok_is(l, ')')) {
            cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ')'");
            p->err = 1;
            return e;
        }
        lex_next(l);
        return e;
    }
    cb_error(p->b, line, col, "ZA5001", "expected an expression");
    p->err = 1;
    return NULL;
}

static CBExpr *parse_postfix(Parser *p) {
    CBExpr *e = parse_primary(p);
    while (e && tok_is(&p->l, '[')) {
        CBExpr *idx, *base, *d;
        int line = e->line, col = e->col;
        lex_next(&p->l);
        idx = parse_assignment(p);
        if (!tok_is(&p->l, ']')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ']'");
            p->err = 1;
            return e;
        }
        lex_next(&p->l);
        if (!idx) { p->err = 1; return e; }
        if (!e->type.ptr) {
            cb_error(p->b, line, col, "ZA5003", "p[i] needs a pointer on the left");
            p->err = 1;
            return e;
        }
        if (e->type.target) {
            cb_error(p->b, line, col, "ZA5002",
                     "cannot index a named DSECT pointer; use __offsetof and a byte pointer");
            p->err = 1;
            return e;
        }
        base = mk_bin(p, '+', e, idx);
        if (!base) { p->err = 1; return e; }
        d = mk_expr(E_DEREF, e->type.elem_bits ? type_of(e->type.elem_bits, e->type.elem_unsig)
                                               : type_of(8, 1), line, col);
        d->a = base;
        e = d;
    }
    while (e && (tok_is(&p->l, OP_INC) || tok_is(&p->l, OP_DEC))) {
        CBExpr *n;
        if (e->kind != E_VAR) {
            cb_error(p->b, e->line, e->col, "ZA5003", "++/-- needs a variable");
            p->err = 1;
            return e;
        }
        n = mk_expr(E_INC, e->type, e->line, e->col);
        n->op = p->l.tok.op;
        n->var = e->var;
        n->num = 1; /* postfix */
        n->read_before = e->read_before;
        lex_next(&p->l);
        e = n;
    }
    return e;
}

static CBExpr *parse_unary(Parser *p) {
    Lex *l = &p->l;
    int line = l->tok.line, col = l->tok.col;
    if (tok_id(l, "__align")) {
        CBExpr *e;
        int64_t n;
        int align = 1;
        lex_next(l);
        if (!tok_is(l, '(')) {
            cb_error(p->b, line, col, "ZA5001", "__align needs a power-of-two alignment");
            p->err = 1;
            return NULL;
        }
        lex_next(l);
        if (l->tok.kind != TK_NUM || l->tok.num < 1 || l->tok.num > 4096 ||
            (l->tok.num & (l->tok.num - 1)) != 0) {
            cb_error(p->b, l->tok.line, l->tok.col, "ZA5001",
                     "__align needs a power-of-two number from 1 to 4096");
            p->err = 1;
            return NULL;
        }
        n = l->tok.num;
        align = (int)n;
        lex_next(l);
        if (!tok_is(l, ')')) {
            cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "__align needs one number");
            p->err = 1;
            return NULL;
        }
        lex_next(l);
        e = parse_unary(p);
        if (!e) return NULL;
        if (e->kind != E_STR) {
            cb_error(p->b, e->line, e->col, "ZA5001", "__align applies only to a string literal");
            p->err = 1;
            return e;
        }
        e->align = align;
        return e;
    }
    if (tok_is(l, '*')) {
        CBExpr *a, *e;
        lex_next(l);
        a = parse_unary(p);
        if (!a) return NULL;
        if (!a->type.ptr) {
            cb_error(p->b, a->line, a->col, "ZA5003", "cannot dereference a non-pointer");
            p->err = 1;
            return a;
        }
        if (a->type.target) {
            cb_error(p->b, a->line, a->col, "ZA5002",
                     "cannot dereference a named DSECT pointer; use __offsetof and a byte pointer");
            p->err = 1;
            return a;
        }
        e = mk_expr(E_DEREF, a->type.elem_bits ? type_of(a->type.elem_bits, a->type.elem_unsig)
                                               : type_of(8, 1), line, col);
        e->a = a;
        return e;
    }
    if (tok_is(l, '&')) {
        CBExpr *a;
        lex_next(l);
        a = parse_unary(p);
        if (!a) return NULL;
        if (a->kind == E_DEREF) return a->a;
        if (a->kind != E_VAR) {
            cb_error(p->b, a->line, a->col, "ZA5003", "& needs a variable with storage");
            p->err = 1;
            return a;
        }
        if (a->var->kind != V_LOCAL) {
            cb_error(p->b, a->line, a->col, "ZA5003", "& needs a local variable, not a bound register");
            p->err = 1;
            return a;
        }
        {
            CBExpr *e = mk_expr(E_ADDR, type_ptr(), line, col);
            e->var = a->var;
            cblock_expr_free(a);
            return e;
        }
    }
    if (tok_is(l, OP_INC) || tok_is(l, OP_DEC)) {
        int op = l->tok.op;
        CBExpr *a, *e;
        lex_next(l);
        a = parse_unary(p);
        if (!a) return NULL;
        if (a->kind != E_VAR) {
            cb_error(p->b, a->line, a->col, "ZA5003", "++/-- needs a variable");
            p->err = 1;
            return a;
        }
        e = mk_expr(E_INC, a->type, line, col);
        e->op = op;
        e->var = a->var;
        e->num = 0; /* prefix */
        e->read_before = a->read_before;
        return e;
    }
    if (tok_is(l, '+') || tok_is(l, '-') || tok_is(l, '!') || tok_is(l, '~')) {
        int op = l->tok.op;
        CBExpr *a;
        CBExpr *e;
        lex_next(l);
        a = parse_unary(p);
        if (!a) return NULL;
        e = mk_expr(E_UN, op == '!' ? type_of(32, 0) : a->type, line, col);
        e->op = op;
        e->a = a;
        return e;
    }
    return parse_postfix(p);
}

static int prec_of(int op) {
    switch (op) {
    case OP_LOR: return 1;
    case OP_LAND: return 2;
    case '*': case '/': case '%': return 10;
    case '+': case '-': return 9;
    case OP_SHL: case OP_SHR: return 8;
    case '<': case '>': case OP_LE: case OP_GE: return 7;
    case OP_EQ: case OP_NE: return 6;
    case '&': return 5;
    case '^': return 4;
    case '|': return 3;
    default: return 0;
    }
}

static CBExpr *parse_binary(Parser *p, int minprec) {
    CBExpr *a = parse_unary(p);
    if (!a) return NULL;
    for (;;) {
        int op = p->l.tok.kind == TK_OP ? p->l.tok.op : 0;
        int prec = prec_of(op);
        CBExpr *b;
        if (!prec || prec < minprec) break;
        lex_next(&p->l);
        b = parse_binary(p, prec + 1);
        if (!b) return a;
        a = mk_bin(p, op, a, b);
    }
    return a;
}

static CBExpr *parse_cond(Parser *p) {
    CBExpr *c = parse_binary(p, 1);
    if (c && tok_is(&p->l, '?')) {
        CBExpr *a, *b, *e;
        CType t;
        int line = c->line, col = c->col;
        lex_next(&p->l);
        a = parse_cond(p);
        if (!tok_is(&p->l, ':')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ':' in ?:");
            p->err = 1;
            return c;
        }
        lex_next(&p->l);
        b = parse_cond(p);
        if (!a || !b) return c;
        t = type_usual(a->type, b->type);
        e = mk_expr(E_COND, t, line, col);
        e->a = c;
        e->b = mk_cast(a, t);
        e->c = mk_cast(b, t);
        return e;
    }
    return c;
}

static CBExpr *parse_assignment(Parser *p) { return parse_cond(p); }

static CBVar *var_new(CBlock *b, const char *name, CType t, VKind k, int reg) {
    CBVar *v = xcalloc(sizeof *v);
    v->name = xstrdup(name);
    v->type = t;
    v->kind = k;
    v->reg = reg;
    v->slot = (int)b->vars.n;
    v->initialized = k != V_COUT;
    vec_push(&b->vars, v);
    strmap_put(&b->byname, v->name, v);
    return v;
}

static int parse_type(Parser *p, CType *t) {
    if (p->l.tok.kind == TK_ID && tok_id(&p->l, "const")) {
        lex_next(&p->l);
        if (p->l.tok.kind != TK_ID) return 0;
    }
    if (p->l.tok.kind != TK_ID) return 0;
    if (za_streqi(p->l.tok.s, "uint8_t") || za_streqi(p->l.tok.s, "void") ||
        za_streqi(p->l.tok.s, "char")) {
        lex_next(&p->l);
        if (!tok_is(&p->l, '*')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5002",
                     "only pointers to uint8_t/void/char are implemented");
            p->err = 1;
            return 1; /* the caller reports no declaration */
        }
        lex_next(&p->l);
        *t = type_ptr();
        return 1;
    }
    if (peek_is_star(&p->l)) {
        char *name = xstrdup(p->l.tok.s);
        CType base;
        if (type_parse(name, &base) && !base.ptr) {
            *t = type_ptr_scalar(base.bits, base.unsig);
        } else {
            *t = type_ptr_named(p->b, name);
        }
        lex_next(&p->l);
        lex_next(&p->l);
        free(name);
        return 1;
    }
    if (!type_parse(p->l.tok.s, t)) return 0;
    lex_next(&p->l);
    return 1;
}

static void parse_decl(Parser *p, CBStmtVec *st, CType t) {
    char *name;
    CBVar *v;
    CBExpr *init = NULL;
    int line = p->l.tok.line;
    if (p->l.tok.kind != TK_ID) {
        cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected a variable name");
        p->err = 1;
        return;
    }
    name = xstrdup(p->l.tok.s);
    lex_next(&p->l);
    if (strmap_get(&p->b->byname, name)) {
        cb_error(p->b, line, 1, "ZA5004", "\"%s\" is already defined", name);
        free(name);
        p->err = 1;
        return;
    }
    v = var_new(p->b, name, t, V_LOCAL, -1);
    free(name);
    if (tok_is(&p->l, '=')) {
        lex_next(&p->l);
        init = parse_assignment(p);
        if (init && init->kind == E_STR) {
            v->lit = xmalloc(init->slen + 1);
            memcpy(v->lit, init->str, init->slen);
            v->lit[init->slen] = 0;
            v->lit_len = init->slen;
        }
    }
    if (!tok_is(&p->l, ';')) {
        cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ';' after declaration");
        p->err = 1;
    } else lex_next(&p->l);
    {
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = S_DECL;
        s.var = v;
        s.expr = init ? mk_cast(init, v->type) : NULL;
        s.line = line;
        vec_push(st, s);
    }
}

static int is_assign_op(int op) {
    return op == '=' || op == OP_ADDEQ || op == OP_SUBEQ || op == OP_MULEQ ||
           op == OP_DIVEQ || op == OP_MODEQ || op == OP_ANDEQ || op == OP_OREQ ||
           op == OP_XOREQ || op == OP_SHLEQ || op == OP_SHREQ;
}

static int assign_base(int op) {
    switch (op) {
    case OP_ADDEQ: return '+';
    case OP_SUBEQ: return '-';
    case OP_MULEQ: return '*';
    case OP_DIVEQ: return '/';
    case OP_MODEQ: return '%';
    case OP_ANDEQ: return '&';
    case OP_OREQ: return '|';
    case OP_XOREQ: return '^';
    case OP_SHLEQ: return OP_SHL;
    case OP_SHREQ: return OP_SHR;
    default: return op;
    }
}

static int const_eval(CBExpr *e, int64_t *out) {
    int64_t a, b;
    if (!e) return 0;
    switch (e->kind) {
    case E_NUM: *out = e->num; return 1;
    case E_UN:
        if (!const_eval(e->a, &a)) return 0;
        if (e->op == '+') *out = a;
        else if (e->op == '-') *out = (int64_t)(0 - (uint64_t)a);
        else if (e->op == '~') *out = ~a;
        else if (e->op == '!') *out = !a;
        else return 0;
        return 1;
    case E_BIN:
        if (!const_eval(e->a, &a) || !const_eval(e->b, &b)) return 0;
        switch (e->op) {
        case '+': *out = (int64_t)((uint64_t)a + (uint64_t)b); return 1;
        case '-': *out = (int64_t)((uint64_t)a - (uint64_t)b); return 1;
        case '*': *out = (int64_t)((uint64_t)a * (uint64_t)b); return 1;
        case '/': if (!b) return 0; *out = a / b; return 1;
        case '%': if (!b) return 0; *out = a % b; return 1;
        case '&': *out = a & b; return 1;
        case '|': *out = a | b; return 1;
        case '^': *out = a ^ b; return 1;
        case OP_SHL: *out = (int64_t)((uint64_t)a << (b & 63)); return 1;
        case OP_SHR: *out = a >> (b & 63); return 1;
        default: return 0;
        }
    default:
        return 0;
    }
}

static void parse_stmt(Parser *p, CBStmtVec *st) {
    CType t;
    CBExpr *e;
    int line = p->l.tok.line;
    if (p->l.tok.kind == TK_ID &&
        (tok_id(&p->l, "__memcpy") || tok_id(&p->l, "__memmove") || tok_id(&p->l, "__memset"))) {
        int kind = tok_id(&p->l, "__memcpy") ? S_MEMCPY
                 : tok_id(&p->l, "__memmove") ? S_MEMMOVE : S_MEMSET;
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = (CBSKind)kind;
        s.line = line;
        lex_next(&p->l);
        if (!tok_is(&p->l, '(')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '(' after intrinsic");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        s.expr = parse_assignment(p);
        if (!tok_is(&p->l, ',')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ',' in intrinsic");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        s.step = parse_assignment(p);
        if (!tok_is(&p->l, ',')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ',' in intrinsic");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        s.cond = parse_assignment(p);
        if (!tok_is(&p->l, ')')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ')' in intrinsic");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (!tok_is(&p->l, ';')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ';' after intrinsic");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (!s.expr || !s.step || !s.cond) { p->err = 1; return; }
        if (!s.expr->type.ptr) {
            cb_error(p->b, line, 1, "ZA5003", "the first argument must be a pointer");
            p->err = 1;
            return;
        }
        if (kind != S_MEMSET && !s.step->type.ptr) {
            cb_error(p->b, line, 1, "ZA5003", "the source argument must be a pointer");
            p->err = 1;
            return;
        }
        if (kind == S_MEMSET && s.step->type.ptr) {
            cb_error(p->b, line, 1, "ZA5003", "the fill value must be an integer");
            p->err = 1;
            return;
        }
        if (s.cond->type.ptr) {
            cb_error(p->b, line, 1, "ZA5003", "the length must be an integer");
            p->err = 1;
            return;
        }
        vec_push(st, s);
        return;
    }
    if (p->switch_depth > 0 && p->l.tok.kind == TK_ID && tok_id(&p->l, "case")) {
        CBStmt s;
        CBExpr *v;
        int64_t cv;
        lex_next(&p->l);
        v = parse_assignment(p);
        if (!v || !const_eval(v, &cv)) {
            cb_error(p->b, line, 1, "ZA5003", "case value must be an integer constant");
            p->err = 1;
            return;
        }
        if (!tok_is(&p->l, ':')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ':' after case");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        memset(&s, 0, sizeof s);
        s.kind = S_CASE;
        s.num = cv;
        s.label = xasprintf("$CB_C%d", ++p->b->label_seq);
        s.line = line;
        vec_push(st, s);
        return;
    }
    if (p->switch_depth > 0 && p->l.tok.kind == TK_ID && tok_id(&p->l, "default")) {
        CBStmt s;
        if (p->switch_default[p->switch_depth - 1]) {
            cb_error(p->b, line, p->l.tok.col, "ZA5004", "duplicate default in switch");
            p->err = 1;
            return;
        }
        p->switch_default[p->switch_depth - 1] = 1;
        lex_next(&p->l);
        if (!tok_is(&p->l, ':')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ':' after default");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        memset(&s, 0, sizeof s);
        s.kind = S_DEFAULT;
        s.label = xasprintf("$CB_D%d", ++p->b->label_seq);
        s.line = line;
        vec_push(st, s);
        return;
    }
    if (p->l.tok.kind == TK_ID && after_tok_colon(&p->l)) {
        char *name = xstrdup(p->l.tok.s);
        char *lab;
        CBStmt s;
        if (strmap_get(&p->b->labels, name)) {
            cb_error(p->b, line, p->l.tok.col, "ZA5004", "label \"%s\" is already defined", name);
            free(name);
            p->err = 1;
            return;
        }
        lab = xasprintf("$CB_L%d", ++p->b->label_seq);
        strmap_put(&p->b->labels, name, lab);
        free(name);
        lex_next(&p->l); /* name */
        lex_next(&p->l); /* : */
        memset(&s, 0, sizeof s);
        s.kind = S_LABEL;
        s.label = xstrdup(lab);
        s.line = line;
        vec_push(st, s);
        if (!tok_is(&p->l, '}') && p->l.tok.kind != TK_EOF) parse_stmt(p, st);
        return;
    }
    if (tok_is(&p->l, ';')) { lex_next(&p->l); return; }
    if (tok_is(&p->l, '{')) {
        lex_next(&p->l);
        while (!tok_is(&p->l, '}') && p->l.tok.kind != TK_EOF && !p->err) parse_stmt(p, st);
        if (tok_is(&p->l, '}')) lex_next(&p->l);
        else cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '}'");
        return;
    }
    if (p->l.tok.kind == TK_ID && tok_id(&p->l, "switch")) {
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = S_SWITCH;
        s.line = line;
        lex_next(&p->l);
        if (!tok_is(&p->l, '(')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '(' after switch");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        s.cond = parse_assignment(p);
        if (!tok_is(&p->l, ')')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ')' after switch expression");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (!tok_is(&p->l, '{')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '{' after switch");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (p->switch_depth >= 16) {
            cb_error(p->b, line, 1, "ZA5008", "switch nested too deeply");
            p->err = 1;
            return;
        }
        p->switch_depth++;
        p->switch_default[p->switch_depth - 1] = 0;
        while (!tok_is(&p->l, '}') && p->l.tok.kind != TK_EOF && !p->err)
            parse_stmt(p, &s.body);
        p->switch_depth--;
        if (!tok_is(&p->l, '}')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '}' after switch");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        vec_push(st, s);
        return;
    }
    if (p->l.tok.kind == TK_ID && tok_id(&p->l, "if")) {
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = S_IF;
        s.line = line;
        lex_next(&p->l);
        if (!tok_is(&p->l, '(')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '(' after if");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        s.cond = parse_assignment(p);
        if (!tok_is(&p->l, ')')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ')' after if condition");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        parse_stmt(p, &s.body);
        if (p->l.tok.kind == TK_ID && tok_id(&p->l, "else")) {
            lex_next(&p->l);
            parse_stmt(p, &s.other);
        }
        vec_push(st, s);
        return;
    }
    if (p->l.tok.kind == TK_ID && tok_id(&p->l, "while")) {
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = S_WHILE;
        s.line = line;
        lex_next(&p->l);
        if (!tok_is(&p->l, '(')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '(' after while");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        s.cond = parse_assignment(p);
        if (!tok_is(&p->l, ')')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ')' after while condition");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        p->loop_depth++;
        parse_stmt(p, &s.body);
        p->loop_depth--;
        vec_push(st, s);
        return;
    }
    if (p->l.tok.kind == TK_ID && tok_id(&p->l, "do")) {
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = S_DO;
        s.line = line;
        lex_next(&p->l);
        p->loop_depth++;
        parse_stmt(p, &s.body);
        p->loop_depth--;
        if (!(p->l.tok.kind == TK_ID && tok_id(&p->l, "while"))) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected while after do body");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (!tok_is(&p->l, '(')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '(' after while");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        s.cond = parse_assignment(p);
        if (!tok_is(&p->l, ')')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ')' after do-while condition");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (!tok_is(&p->l, ';')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ';' after do-while");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        vec_push(st, s);
        return;
    }
    if (p->l.tok.kind == TK_ID && tok_id(&p->l, "for")) {
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = S_FOR;
        s.line = line;
        lex_next(&p->l);
        if (!tok_is(&p->l, '(')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '(' after for");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (tok_is(&p->l, ';')) lex_next(&p->l);
        else parse_stmt(p, &s.init);
        if (!tok_is(&p->l, ';')) {
            s.cond = parse_assignment(p);
            if (!tok_is(&p->l, ';')) {
                cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ';' in for");
                p->err = 1;
                return;
            }
        }
        lex_next(&p->l);
        if (!tok_is(&p->l, ')')) s.step = parse_assignment(p);
        if (!tok_is(&p->l, ')')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ')' in for");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        p->loop_depth++;
        parse_stmt(p, &s.body);
        p->loop_depth--;
        vec_push(st, s);
        return;
    }
    if (p->l.tok.kind == TK_ID && tok_id(&p->l, "goto")) {
        CBStmt s;
        lex_next(&p->l);
        if (p->l.tok.kind != TK_ID) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "goto needs a label");
            p->err = 1;
            return;
        }
        memset(&s, 0, sizeof s);
        s.kind = S_GOTO;
        s.label = xstrdup(p->l.tok.s);
        s.line = line;
        lex_next(&p->l);
        if (!tok_is(&p->l, ';')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ';' after goto");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        vec_push(st, s);
        return;
    }
    if (p->l.tok.kind == TK_ID && (tok_id(&p->l, "break") || tok_id(&p->l, "continue"))) {
        int brk = tok_id(&p->l, "break");
        if (p->loop_depth == 0 && (!brk || p->switch_depth == 0)) {
            cb_error(p->b, line, p->l.tok.col, "ZA5008", "%s is only valid in a loop%s",
                     brk ? "break" : "continue", brk ? " or switch" : "");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (!tok_is(&p->l, ';')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ';'");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        {
            CBStmt s;
            memset(&s, 0, sizeof s);
            s.kind = brk ? S_BREAK : S_CONTINUE;
            s.line = line;
            vec_push(st, s);
        }
        return;
    }
    if (p->l.tok.kind == TK_ID &&
        (tok_id(&p->l, "return") || tok_id(&p->l, "switch") || tok_id(&p->l, "goto"))) {
        cb_error(p->b, line, p->l.tok.col, "ZA5008", "\"%s\" is not implemented", p->l.tok.s);
        p->err = 1;
        return;
    }
    if (parse_type(p, &t)) {
        parse_decl(p, st, t);
        return;
    }
    e = parse_assignment(p);
    if (!e) { p->err = 1; return; }
    if (p->l.tok.kind == TK_OP && is_assign_op(p->l.tok.op)) {
        int op = p->l.tok.op;
        CBVar *v;
        CBExpr *rhs;
        CBStmt s;
        if (e->kind != E_VAR && e->kind != E_DEREF) {
            cb_error(p->b, e->line, e->col, "ZA5003",
                     "the left side of an assignment must be a variable or *p");
            p->err = 1;
            return;
        }
        v = e->kind == E_VAR ? e->var : NULL;
        if (v) {
            if (op != '=' && e->read_before) {
                cb_error(p->b, e->line, e->col, "ZA5003", "\"%s\" is read before it is assigned",
                         v->name);
                p->err = 1;
                return;
            }
            if (v->kind == V_CIN) {
                cb_error(p->b, e->line, e->col, "ZA5003",
                         "\"%s\" is a cin variable and cannot be assigned", v->name);
                p->err = 1;
                return;
            }
        }
        lex_next(&p->l);
        rhs = parse_assignment(p);
        if (!rhs) { p->err = 1; return; }
        if (v) {
            free(v->lit);
            v->lit = NULL;
            v->lit_len = 0;
            if (op == '=' && rhs->kind == E_STR) {
                v->lit = xmalloc(rhs->slen + 1);
                memcpy(v->lit, rhs->str, rhs->slen);
                v->lit[rhs->slen] = 0;
                v->lit_len = rhs->slen;
            }
        }
        memset(&s, 0, sizeof s);
        s.kind = S_ASSIGN;
        s.var = v;
        s.lval = v ? NULL : e;
        s.op = assign_base(op);
        if (v && v->type.ptr) {
            if (op != '=' && s.op != '+' && s.op != '-') {
                cb_error(p->b, line, 1, "ZA5003", "a pointer supports only =, += and -=");
                p->err = 1;
                return;
            }
            s.expr = op == '=' ? mk_cast(rhs, v->type) : rhs;
        } else {
            s.expr = mk_cast(rhs, e->type);
        }
        s.line = line;
        vec_push(st, s);
        if (v) v->initialized = 1;
    } else {
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = S_EXPR;
        s.expr = e;
        s.line = line;
        vec_push(st, s);
    }
    if (!tok_is(&p->l, ';')) {
        cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ';' after statement");
        p->err = 1;
    } else lex_next(&p->l);
}

static int check_gotos(CBlock *b, CBStmtVec *v) {
    size_t i;
    for (i = 0; i < v->n; i++) {
        CBStmt *s = &v->v[i];
        if (s->kind == S_GOTO && !strmap_get(&b->labels, s->label)) {
            cb_error(b, s->line, 1, "ZA5004", "goto target \"%s\" is not defined", s->label);
            return 0;
        }
        if (!check_gotos(b, &s->body) || !check_gotos(b, &s->other) || !check_gotos(b, &s->init))
            return 0;
    }
    return 1;
}

/* ---- metadata parsing --------------------------------------------------- */

static int pragma_split(const char *line, char **word, char **rest) {
    const char *p = line;
    size_t n = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '#') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (za_strncmpi(p, "pragma", 6) != 0) return 0;
    p += 6;
    while (*p == ' ' || *p == '\t') p++;
    while (p[n] && p[n] != ' ' && p[n] != '\t') n++;
    if (!n) return 0;
    *word = za_strlower_own(xstrndup(p, n));
    *rest = trim(p + n);
    return 1;
}

static void add_scratch(CBlock *b, int reg, int line) {
    int i;
    for (i = 0; i < b->nscratch; i++)
        if (b->scratch[i] == reg) {
            cb_error(b, line, 1, "ZA5005", "register %%r%d is listed twice in cscratch", reg);
            return;
        }
    if (b->nscratch >= 16) {
        cb_error(b, line, 1, "ZA5005", "too many cscratch registers");
        return;
    }
    b->scratch[b->nscratch++] = reg;
}

static int binding_parse(CBlock *b, const char *rest, int line, VKind kind) {
    char *s = xstrdup(rest);
    char *p = s;
    for (;;) {
        char *comma = strchr(p, ',');
        char *decl = NULL, *eq, *vn = NULL, *rn = NULL;
        CType t;
        int reg, i, bad = 0;
        if (comma) *comma = 0;
        decl = trim(p);
        if (*decl) {
            size_t n;
            char *name_start;
            eq = strchr(decl, '=');
            if (!eq) {
                cb_error(b, line, 1, "ZA5005", "expected \"type name=%%rN\" in cblock metadata");
                bad = 1;
            } else {
                *eq = 0;
                decl = trim(decl);
                n = strlen(decl);
                while (n > 0 && (decl[n - 1] == ' ' || decl[n - 1] == '\t')) decl[--n] = 0;
                while (n > 0 && is_id_char((unsigned char)decl[n - 1])) n--;
                name_start = decl + n;
                if (!*name_start) {
                    cb_error(b, line, 1, "ZA5005", "expected a variable name in \"%s\"", decl);
                    bad = 1;
                } else {
                    char *type_text = xstrndup(decl, n);
                    vn = xstrdup(name_start);
                    rn = trim(eq + 1);
                    if (!type_text_parse(b, type_text, &t)) {
                        cb_error(b, line, 1, "ZA5002", "unsupported cblock type \"%s\"", type_text);
                        bad = 1;
                    } else if (!reg_parse(rn, &reg)) {
                        cb_error(b, line, 1, "ZA5005", "\"%s\" is not a %%r0-%%r15 register binding", rn);
                        bad = 1;
                    } else {
                        for (i = 0; i < (int)b->vars.n; i++)
                            if (b->vars.v[i]->reg == reg) {
                                cb_error(b, line, 1, "ZA5005", "register %%r%d is bound more than once", reg);
                                bad = 1;
                                break;
                            }
                        if (!bad && strmap_get(&b->byname, vn)) {
                            cb_error(b, line, 1, "ZA5004", "\"%s\" is already defined", vn);
                            bad = 1;
                        }
                        if (!bad) var_new(b, vn, t, kind, reg);
                    }
                    free(type_text);
                }
            }
        }
        free(vn);
        free(rn);
        free(decl);
        if (bad) { free(s); return 0; }
        if (!comma) break;
        p = comma + 1;
    }
    free(s);
    return 1;
}

static int ctemp_parse(CBlock *b, const char *rest, int line) {
    char *s = xstrdup(rest), *a, *c, *d;
    char *comma;
    int reg;
    long size;
    char *end = NULL;
    a = s;
    comma = strchr(a, ',');
    if (!comma) goto bad;
    *comma = 0;
    c = comma + 1;
    comma = strchr(c, ',');
    if (!comma) goto bad;
    *comma = 0;
    d = comma + 1;
    {
        char *ta = trim(a), *tc = trim(c), *td = trim(d);
        int ok;
        if (!reg_parse(ta, &reg)) { free(ta); free(tc); free(td); goto bad; }
        if (reg == 0 || reg == 1) {
            cb_error(b, line, 1, "ZA5005",
                     "ctemp cannot use %%r0 or %%r1 as its base register (internal scratch)");
            free(ta); free(tc); free(td); free(s); return 0;
        }
        errno = 0;
        size = strtol(tc, &end, 10);
        if (errno || !end || *end || size <= 0 || size % 8 != 0) {
            free(ta); free(tc); free(td); goto bad;
        }
        if (b->temp_reg >= 0) {
            cb_error(b, line, 1, "ZA5005", "ctemp is given more than once");
            free(ta); free(tc); free(td); free(s); return 0;
        }
        b->temp_reg = reg;
        b->temp_size = size;
        b->temp_down = za_streqi(td, "down");
        ok = b->temp_down || za_streqi(td, "up");
        free(ta); free(tc); free(td);
        if (!ok) goto bad;
    }
    free(s);
    return 1;
bad:
    cb_error(b, line, 1, "ZA5005", "ctemp takes \"%%rN,SIZE,up|down\" with SIZE a multiple of 8");
    free(s);
    return 0;
}

static int has_scratch(const CBlock *b, int reg) {
    int i;
    for (i = 0; i < b->nscratch; i++) if (b->scratch[i] == reg) return 1;
    return 0;
}

static void cblock_expr_free(CBExpr *e) {
    if (!e) return;
    cblock_expr_free(e->a);
    cblock_expr_free(e->b);
    cblock_expr_free(e->c);
    free(e->name);
    free(e->str);
    free(e->sym);
    free(e->field);
    free(e);
}

static void cblock_stmt_vec_free(CBStmtVec *v);

static void cblock_stmt_free(CBStmt *s) {
    cblock_expr_free(s->expr);
    cblock_expr_free(s->cond);
    cblock_expr_free(s->step);
    free(s->label);
    cblock_stmt_vec_free(&s->body);
    cblock_stmt_vec_free(&s->other);
    cblock_stmt_vec_free(&s->init);
}

static void cblock_stmt_vec_free(CBStmtVec *v) {
    size_t i;
    for (i = 0; i < v->n; i++) cblock_stmt_free(&v->v[i]);
    free(v->v);
    memset(v, 0, sizeof *v);
}

/* ---- code generation ---------------------------------------------------- */

typedef struct {
    CBlock *b;
    int base_slots;
    int slot_top;
    int max_slot;
    int ctrl_depth;
    int ctrl_loop[16];
    const char *ctrl_break[16];
    const char *ctrl_continue[16];
} Cg;

static int alloc_slot(Cg *cg) {
    int s = cg->slot_top++;
    if (s + 1 > cg->max_slot) cg->max_slot = s + 1;
    return s;
}

static long slot_off(CBlock *b, int slot) {
    if (b->temp_down) return b->temp_size - (long)(slot + 1) * 8;
    return (long)slot * 8;
}

static const char *csrc_for(const CBlock *b, int line) {
    size_t i;
    for (i = 0; i < b->body_n; i++)
        if (b->body[i].line == line) return b->body[i].text;
    return NULL;
}

static void asm_raw(CBlock *b, int line, char *s) {
    SrcLine sl;
    memset(&sl, 0, sizeof sl);
    sl.file = b->file;
    sl.line = line;
    sl.text = s;
    {
        const char *cs = csrc_for(b, line);
        if (cs) sl.orig = xstrdup(cs);
    }
    vec_push(b->out, sl);
}

static void asm_line(CBlock *b, int line, const char *fmt, ...) {
    va_list ap;
    char *s, *p;
    va_start(ap, fmt);
    s = xvasprintf(fmt, ap);
    va_end(ap);
    if (s[0] == '*') {
        asm_raw(b, line, s);
        return;
    }
    p = xasprintf("         %s", s);
    free(s);
    asm_raw(b, line, p);
}

static void emit_label(CBlock *b, int line, const char *label) {
    asm_raw(b, line, xasprintf("%s DS 0H", label));
}

static void load_slot(CBlock *b, int line, int slot, CType t, int reg) {
    long off = slot_off(b, slot);
    if (t.bits == 64) asm_line(b, line, "LG %%r%d,%ld(%%r%d)", reg, off, b->temp_reg);
    else if (t.unsig) asm_line(b, line, "LLGF %%r%d,%ld(%%r%d)", reg, off, b->temp_reg);
    else asm_line(b, line, "LGF %%r%d,%ld(%%r%d)", reg, off, b->temp_reg);
}

static void store_slot(CBlock *b, int line, int slot, CType t, int reg) {
    long off = slot_off(b, slot);
    if (t.bits == 64) asm_line(b, line, "STG %%r%d,%ld(%%r%d)", reg, off, b->temp_reg);
    else asm_line(b, line, "ST %%r%d,%ld(%%r%d)", reg, off, b->temp_reg);
}

static void load_imm(CBlock *b, int line, int64_t v, CType t, int reg) {
    uint64_t u = (uint64_t)v;
    if (t.bits == 32) {
        if (t.unsig && u > INT32_MAX) asm_line(b, line, "LLILF %%r%d,%llu", reg, (unsigned long long)u);
        else asm_line(b, line, "LGFI %%r%d,%lld", reg, (long long)v);
    } else if (v >= INT32_MIN && v <= INT32_MAX) {
        asm_line(b, line, "LGFI %%r%d,%lld", reg, (long long)v);
    } else if (!t.unsig && v >= 0 && u <= UINT32_MAX) {
        asm_line(b, line, "LLILF %%r%d,%llu", reg, (unsigned long long)u);
    } else {
        asm_line(b, line, "IIHF %%r%d,%lu", reg, (unsigned long)(u >> 32));
        asm_line(b, line, "IILF %%r%d,%lu", reg, (unsigned long)(u & 0xFFFFFFFFu));
    }
}

static void emit_compare(CBlock *b, int line, CType t, int op, int r1, int r2, int slot) {
    int mask;
    const char *lab = xasprintf("$CB%d", ++b->label_no);
    switch (op) {
    case '<': mask = 4; break;
    case '>': mask = 2; break;
    case OP_LE: mask = 12; break;
    case OP_GE: mask = 10; break;
    case OP_EQ: mask = 8; break;
    default: mask = 14; break; /* != */
    }
    if (t.bits == 64) asm_line(b, line, t.unsig ? "CLGR %%r%d,%%r%d" : "CGR %%r%d,%%r%d", r1, r2);
    else asm_line(b, line, t.unsig ? "CLR %%r%d,%%r%d" : "CR %%r%d,%%r%d", r1, r2);
    asm_line(b, line, "LHI %%r0,1");
    asm_line(b, line, "BRCL %d,%s", mask, lab);
    asm_line(b, line, "LHI %%r0,0");
    emit_label(b, line, lab);
    store_slot(b, line, slot, type_of(32, 0), 0);
    free((char *)lab);
}

static int find_div_pair(CBlock *b, int *even, int *divisor) {
    int i, j, k;
    for (i = 0; i < b->nscratch; i++) {
        int e = b->scratch[i];
        if (e % 2) continue;
        for (j = 0; j < b->nscratch; j++)
            if (b->scratch[j] == e + 1) {
                for (k = 0; k < b->nscratch; k++)
                    if (b->scratch[k] != e && b->scratch[k] != e + 1) {
                        *even = e;
                        *divisor = b->scratch[k];
                        return 1;
                    }
            }
    }
    return 0;
}

static char *new_label(CBlock *b) { return xasprintf("$CB%d", ++b->label_no); }

static char *literal_add(CBlock *b, const char *data, size_t len, int align) {
    CBString *l;
    if (b->nlits == b->lits_cap) {
        b->lits_cap = b->lits_cap ? b->lits_cap * 2 : 4;
        b->lits = xrealloc(b->lits, b->lits_cap * sizeof *b->lits);
    }
    l = &b->lits[b->nlits++];
    l->data = xmalloc(len + 1);
    memcpy(l->data, data, len);
    l->data[len] = 0;
    l->len = len;
    l->align = align;
    l->label = new_label(b);
    return l->label;
}

static void emit_literal_data(CBlock *b, int line) {
    size_t i;
    for (i = 0; i < b->nlits; i++) {
        CBString *l = &b->lits[i];
        size_t at = 0, total = l->len + 1;
        if (l->align == 2) asm_line(b, line, "DS 0H");
        else if (l->align == 4) asm_line(b, line, "DS 0F");
        else if (l->align == 8) asm_line(b, line, "DS 0D");
        else if (l->align > 8) asm_line(b, line, "CNOP 0,%d", l->align);
        emit_label(b, line, l->label);
        while (at < total) {
            Buf h = {0};
            size_t k = total - at > 16 ? 16 : total - at;
            size_t j;
            for (j = 0; j < k; j++) {
                unsigned char c = at + j == l->len ? 0 : (unsigned char)l->data[at + j];
                buf_printf(&h, "%02X", c);
            }
            asm_line(b, line, "DC X'%s'", buf_cstr(&h));
            free(h.p);
            at += k;
        }
    }
    emit_label(b, line, b->lit_end);
}

static void emit_test(CBlock *b, int line, CType t) {
    asm_line(b, line, t.bits == 64 ? "LTGR %%r0,%%r0" : "LTR %%r0,%%r0");
}

static int pick_addr(CBlock *b, int used1, int used2);
static int eval(Cg *cg, CBExpr *e, int *slotp);

static int eval_bin(Cg *cg, CBExpr *e, int *slotp) {
    CBlock *b = cg->b;
    int s0, s1, res;
    CType t = e->type;
    CType ot = type_usual(e->a->type, e->b->type);
    if (e->op == OP_LAND || e->op == OP_LOR) {
        char *lab = new_label(b), *end = new_label(b);
        res = alloc_slot(cg);
        if (!eval(cg, e->a, &s0)) { free(lab); free(end); return 0; }
        load_slot(b, e->line, s0, e->a->type, 0);
        emit_test(b, e->line, e->a->type);
        asm_line(b, e->line, "BRCL %d,%s", e->op == OP_LAND ? 8 : 7, lab);
        if (!eval(cg, e->b, &s1)) { free(lab); free(end); return 0; }
        load_slot(b, e->line, s1, e->b->type, 0);
        emit_test(b, e->line, e->b->type);
        asm_line(b, e->line, "BRCL 8,%s", lab);
        asm_line(b, e->line, "LHI %%r0,1");
        asm_line(b, e->line, "BRCL 15,%s", end);
        emit_label(b, e->line, lab);
        asm_line(b, e->line, "LHI %%r0,0");
        emit_label(b, e->line, end);
        store_slot(b, e->line, res, type_of(32, 0), 0);
        free(lab); free(end);
        *slotp = res;
        return 1;
    }
    if (!eval(cg, e->a, &s0) || !eval(cg, e->b, &s1)) return 0;
    if (e->op == OP_EQ || e->op == OP_NE || e->op == '<' || e->op == '>' || e->op == OP_LE ||
        e->op == OP_GE) {
        load_slot(b, e->line, s0, e->a->type, 0);
        load_slot(b, e->line, s1, e->b->type, 1);
        res = alloc_slot(cg);
        emit_compare(b, e->line, (e->a->type.ptr || e->b->type.ptr) ? type_ptr() : ot, e->op, 0, 1, res);
        *slotp = res;
        return 1;
    }
    if (e->a->type.ptr || e->b->type.ptr) {
        CType pt = e->a->type.ptr ? e->a->type : e->b->type;
        int shift = pt.elem_bits == 64 ? 3 : pt.elem_bits == 32 ? 2 : 0;
        load_slot(b, e->line, s0, e->a->type, 0);
        load_slot(b, e->line, s1, e->b->type, 1);
        if (e->a->type.ptr && e->b->type.ptr) {
            asm_line(b, e->line, "SGR %%r0,%%r1");
            if (shift) asm_line(b, e->line, "SRLG %%r0,%%r0,%d", shift);
        } else {
            if (shift) asm_line(b, e->line, "SLLG %%r1,%%r1,%d", shift);
            asm_line(b, e->line, e->op == '-' ? "SGR %%r0,%%r1" : "AGR %%r0,%%r1");
        }
        res = alloc_slot(cg);
        store_slot(b, e->line, res, e->type, 0);
        *slotp = res;
        return 1;
    }
    load_slot(b, e->line, s0, e->a->type, 0);
    load_slot(b, e->line, s1, e->b->type, 1);
    if (e->op == '/' || e->op == '%') {
        int even, divisor;
        if (!find_div_pair(b, &even, &divisor)) {
            cb_error(b, e->line, e->col, "ZA5005",
                     "division needs an even/odd scratch pair plus a third scratch register");
            return 0;
        }
        if (ot.unsig) asm_line(b, e->line, "LGHI %%r%d,0", even);
        load_slot(b, e->line, s0, e->a->type, even + 1);
        load_slot(b, e->line, s1, e->b->type, divisor);
        asm_line(b, e->line, ot.unsig ? "DLGR %%r%d,%%r%d" : "DSGR %%r%d,%%r%d", even, divisor);
        res = alloc_slot(cg);
        /* DSGR: remainder=R1, quotient=R1+1.  DLGR keeps the 128-bit
         * quotient in R1/R1+1; for values that fit 64 bits the low half is
         * R1+1 and the remainder is not used. */
        store_slot(b, e->line, res, t, e->op == '%' ? even : even + 1);
        *slotp = res;
        return 1;
    }
    switch (e->op) {
    case '+': asm_line(b, e->line, t.bits == 64 ? "AGR %%r0,%%r1" : "AR %%r0,%%r1"); break;
    case '-': asm_line(b, e->line, t.bits == 64 ? "SGR %%r0,%%r1" : "SR %%r0,%%r1"); break;
    case '*': asm_line(b, e->line, t.bits == 64 ? "MSGR %%r0,%%r1" : "MSR %%r0,%%r1"); break;
    case '&': asm_line(b, e->line, t.bits == 64 ? "NGR %%r0,%%r1" : "NR %%r0,%%r1"); break;
    case '|': asm_line(b, e->line, t.bits == 64 ? "OGR %%r0,%%r1" : "OR %%r0,%%r1"); break;
    case '^': asm_line(b, e->line, t.bits == 64 ? "XGR %%r0,%%r1" : "XR %%r0,%%r1"); break;
    case OP_SHL: asm_line(b, e->line, t.bits == 64 ? "SLLG %%r0,%%r0,0(%%r1)" : "SLL %%r0,0(%%r1)"); break;
    case OP_SHR:
        asm_line(b, e->line, t.unsig ? (t.bits == 64 ? "SRLG %%r0,%%r0,0(%%r1)" : "SRL %%r0,0(%%r1)")
                                     : (t.bits == 64 ? "SRAG %%r0,%%r0,0(%%r1)" : "SRA %%r0,0(%%r1)"));
        break;
    default:
        cb_error(b, e->line, e->col, "ZA5002", "unsupported cblock operator");
        return 0;
    }
    res = alloc_slot(cg);
    store_slot(b, e->line, res, t, 0);
    *slotp = res;
    return 1;
}

static int eval(Cg *cg, CBExpr *e, int *slotp) {
    CBlock *b = cg->b;
    int s, s0, res;
    switch (e->kind) {
    case E_NUM:
        load_imm(b, e->line, e->num, e->type, 0);
        s = alloc_slot(cg);
        store_slot(b, e->line, s, e->type, 0);
        *slotp = s;
        return 1;
    case E_STR: {
        char *label = literal_add(b, e->str, e->slen, e->align);
        asm_line(b, e->line, "LARL %%r1,%s", label);
        res = alloc_slot(cg);
        store_slot(b, e->line, res, type_ptr(), 1);
        *slotp = res;
        return 1;
    }
    case E_VAR:
        if (e->read_before) {
            cb_error(b, e->line, e->col, "ZA5003", "\"%s\" is read before it is assigned",
                     e->var->name);
            return 0;
        }
        s = alloc_slot(cg);
        load_slot(b, e->line, e->var->slot, e->var->type, 0);
        store_slot(b, e->line, s, e->var->type, 0);
        *slotp = s;
        return 1;
    case E_DEREF: {
        int sa, addr;
        if (!eval(cg, e->a, &sa)) return 0;
        addr = pick_addr(b, 0, 0);
        if (addr < 0) {
            cb_error(b, e->line, e->col, "ZA5005", "a dereference needs a nonzero scratch register");
            return 0;
        }
        load_slot(b, e->line, sa, type_ptr(), addr);
        if (e->type.bits == 64) asm_line(b, e->line, "LG %%r0,0(%%r%d)", addr);
        else if (e->type.bits == 32) asm_line(b, e->line, "L %%r0,0(%%r%d)", addr);
        else asm_line(b, e->line, "LLGC %%r0,0(%%r%d)", addr);
        res = alloc_slot(cg);
        store_slot(b, e->line, res, e->type, 0);
        *slotp = res;
        return 1;
    }
    case E_ADDR: {
        int addr;
        if (!e->var || e->var->kind != V_LOCAL) {
            cb_error(b, e->line, e->col, "ZA5003", "& needs a local variable with storage");
            return 0;
        }
        addr = pick_addr(b, 0, 0);
        if (addr < 0) {
            cb_error(b, e->line, e->col, "ZA5005", "& needs a nonzero scratch register");
            return 0;
        }
        asm_line(b, e->line, "LA %%r%d,%ld(%%r%d)", addr, slot_off(b, e->var->slot), b->temp_reg);
        res = alloc_slot(cg);
        store_slot(b, e->line, res, type_ptr(), addr);
        *slotp = res;
        return 1;
    }
    case E_CAST:
        if (!eval(cg, e->a, &s0)) return 0;
        load_slot(b, e->line, s0, e->a->type, 0);
        if (e->type.bits == 64 && e->a->type.bits == 32)
            asm_line(b, e->line, e->a->type.unsig ? "LLGFR %%r0,%%r0" : "LGFR %%r0,%%r0");
        res = alloc_slot(cg);
        store_slot(b, e->line, res, e->type, 0);
        *slotp = res;
        return 1;
    case E_UN:
        if (!eval(cg, e->a, &s0)) return 0;
        load_slot(b, e->line, s0, e->a->type, 0);
        if (e->op == '!') {
            const char *lab = xasprintf("$CB%d", ++b->label_no);
            asm_line(b, e->line, e->a->type.bits == 64 ? "LTGR %%r0,%%r0" : "LTR %%r0,%%r0");
            asm_line(b, e->line, "LHI %%r0,1");
            asm_line(b, e->line, "BRCL 8,%s", lab);
            asm_line(b, e->line, "LHI %%r0,0");
            emit_label(b, e->line, lab);
            free((char *)lab);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (e->op == '-') asm_line(b, e->line, e->a->type.bits == 64 ? "LNGR %%r0,%%r0" : "LNR %%r0,%%r0");
        else if (e->op == '~') {
            asm_line(b, e->line, e->a->type.bits == 64 ? "LGHI %%r1,-1" : "LHI %%r1,-1");
            asm_line(b, e->line, e->a->type.bits == 64 ? "XGR %%r0,%%r1" : "XR %%r0,%%r1");
        }
        res = alloc_slot(cg);
        store_slot(b, e->line, res, e->type, 0);
        *slotp = res;
        return 1;
    case E_COND: {
        char *els = new_label(b), *end = new_label(b);
        res = alloc_slot(cg);
        if (!eval(cg, e->a, &s0)) { free(els); free(end); return 0; }
        load_slot(b, e->line, s0, e->a->type, 0);
        emit_test(b, e->line, e->a->type);
        asm_line(b, e->line, "BRCL 8,%s", els);
        if (!eval(cg, e->b, &s0)) { free(els); free(end); return 0; }
        load_slot(b, e->line, s0, e->b->type, 0);
        store_slot(b, e->line, res, e->type, 0);
        asm_line(b, e->line, "BRCL 15,%s", end);
        emit_label(b, e->line, els);
        if (!eval(cg, e->c, &s0)) { free(els); free(end); return 0; }
        load_slot(b, e->line, s0, e->c->type, 0);
        store_slot(b, e->line, res, e->type, 0);
        emit_label(b, e->line, end);
        free(els); free(end);
        *slotp = res;
        return 1;
    }
    case E_INC:
        if (e->var->kind == V_CIN) {
            cb_error(b, e->line, e->col, "ZA5003", "\"%s\" is a cin variable", e->var->name);
            return 0;
        }
        if (e->read_before) {
            cb_error(b, e->line, e->col, "ZA5003", "\"%s\" is read before it is assigned",
                     e->var->name);
            return 0;
        }
        s = alloc_slot(cg);
        load_slot(b, e->line, e->var->slot, e->var->type, 0);
        if (e->num) {
            /* postfix: keep the old value in r1 */
            asm_line(b, e->line, e->var->type.bits == 64 ? "LGR %%r1,%%r0" : "LR %%r1,%%r0");
        }
        asm_line(b, e->line, e->var->type.bits == 64
                                ? (e->op == OP_INC ? "AGHI %%r0,1" : "AGHI %%r0,-1")
                                : (e->op == OP_INC ? "AHI %%r0,1" : "AHI %%r0,-1"));
        store_slot(b, e->line, e->var->slot, e->var->type, 0);
        if (e->num) store_slot(b, e->line, s, e->var->type, 1);
        else store_slot(b, e->line, s, e->var->type, 0);
        *slotp = s;
        return 1;
    case E_CALL: {
        int sa, sp, sl = -1;
        if (!strcmp(e->name, "__sizeof") || !strcmp(e->name, "__offsetof")) {
            if (e->field)
                asm_line(b, e->line, "LGFI %%r0,offsetof(%s,%s)", e->sym, e->field);
            else
                asm_line(b, e->line, "LGFI %%r0,sizeof(%s)", e->sym);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(64, 1), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__strlen")) {
            if (e->a->kind == E_STR || (e->a->kind == E_VAR && e->a->var->lit)) {
                size_t len = e->a->kind == E_STR ? e->a->slen : e->a->var->lit_len;
                asm_line(b, e->line, "LGFI %%r0,%lu", (unsigned long)len);
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(64, 1), 0);
                *slotp = res;
                return 1;
            }
            int addr, chr;
            if (!eval(cg, e->a, &sa)) return 0;
            addr = pick_addr(b, 0, 0);
            chr = pick_addr(b, addr, 0);
            if (addr < 0 || chr < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "__strlen needs two nonzero scratch registers");
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), addr);
            asm_line(b, e->line, "LGHI %%r0,0");
            {
                char *top = new_label(b), *end = new_label(b);
                emit_label(b, e->line, top);
                asm_line(b, e->line, "LLGC %%r%d,0(%%r%d)", chr, addr);
                asm_line(b, e->line, "LTR %%r%d,%%r%d", chr, chr);
                asm_line(b, e->line, "BRCL 8,%s", end);
                asm_line(b, e->line, "AGHI %%r%d,1", addr);
                asm_line(b, e->line, "AGHI %%r0,1");
                asm_line(b, e->line, "BRCL 15,%s", top);
                emit_label(b, e->line, end);
                free(top); free(end);
            }
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(64, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__memcmp")) {
            int ra, rb;
            char *top, *eq, *less, *greater, *end;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sl))
                return 0;
            ra = pick_addr(b, 0, 0);
            rb = pick_addr(b, ra, 0);
            if (ra < 0 || rb < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "__memcmp needs two nonzero scratch registers");
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), ra);
            load_slot(b, e->line, sp, type_ptr(), rb);
            load_slot(b, e->line, sl, e->c->type, 0);
            top = new_label(b); eq = new_label(b); less = new_label(b);
            greater = new_label(b); end = new_label(b);
            asm_line(b, e->line, "LTGR %%r0,%%r0");
            asm_line(b, e->line, "BRCL 8,%s", eq);
            emit_label(b, e->line, top);
            asm_line(b, e->line, "CLC 0(1,%%r%d),0(%%r%d)", ra, rb);
            asm_line(b, e->line, "BRCL 4,%s", less);
            asm_line(b, e->line, "BRCL 2,%s", greater);
            asm_line(b, e->line, "AGHI %%r%d,1", ra);
            asm_line(b, e->line, "AGHI %%r%d,1", rb);
            asm_line(b, e->line, "AGHI %%r0,-1");
            asm_line(b, e->line, "BRCL 15,%s", top);
            emit_label(b, e->line, eq);
            asm_line(b, e->line, "LGHI %%r0,0");
            asm_line(b, e->line, "BRCL 15,%s", end);
            emit_label(b, e->line, less);
            asm_line(b, e->line, "LGHI %%r0,-1");
            asm_line(b, e->line, "BRCL 15,%s", end);
            emit_label(b, e->line, greater);
            asm_line(b, e->line, "LGHI %%r0,1");
            emit_label(b, e->line, end);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            free(top); free(eq); free(less); free(greater); free(end);
            return 1;
        }
        if (!strcmp(e->name, "__strcmp")) {
            int ra, rb;
            char *top, *eq, *less, *greater, *end;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            ra = pick_addr(b, 0, 0);
            rb = pick_addr(b, ra, 0);
            if (ra < 0 || rb < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "__strcmp needs two nonzero scratch registers");
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), ra);
            load_slot(b, e->line, sp, type_ptr(), rb);
            top = new_label(b); eq = new_label(b); less = new_label(b);
            greater = new_label(b); end = new_label(b);
            emit_label(b, e->line, top);
            asm_line(b, e->line, "CLC 0(1,%%r%d),0(%%r%d)", ra, rb);
            asm_line(b, e->line, "BRCL 4,%s", less);
            asm_line(b, e->line, "BRCL 2,%s", greater);
            asm_line(b, e->line, "LLGC %%r0,0(%%r%d)", ra);
            asm_line(b, e->line, "LTR %%r0,%%r0");
            asm_line(b, e->line, "BRCL 8,%s", eq);
            asm_line(b, e->line, "AGHI %%r%d,1", ra);
            asm_line(b, e->line, "AGHI %%r%d,1", rb);
            asm_line(b, e->line, "BRCL 15,%s", top);
            emit_label(b, e->line, eq);
            asm_line(b, e->line, "LGHI %%r0,0");
            asm_line(b, e->line, "BRCL 15,%s", end);
            emit_label(b, e->line, less);
            asm_line(b, e->line, "LGHI %%r0,-1");
            asm_line(b, e->line, "BRCL 15,%s", end);
            emit_label(b, e->line, greater);
            asm_line(b, e->line, "LGHI %%r0,1");
            emit_label(b, e->line, end);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            free(top); free(eq); free(less); free(greater); free(end);
            return 1;
        }
        if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sl)) return 0;
        {
            int addr = pick_addr(b, 0, 0), pat = pick_addr(b, addr, 0), chr = pick_addr(b, addr, pat);
            char *top, *found, *notf, *end;
            if (addr < 0 || pat < 0 || chr < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "__memchr needs three nonzero scratch registers");
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), addr);
            load_slot(b, e->line, sp, e->b->type, pat);
            asm_line(b, e->line, "NILF %%r%d,255", pat);
            load_slot(b, e->line, sl, e->c->type, 0);
            top = new_label(b); found = new_label(b); notf = new_label(b); end = new_label(b);
            asm_line(b, e->line, "LTGR %%r0,%%r0");
            asm_line(b, e->line, "BRCL 8,%s", notf);
            emit_label(b, e->line, top);
            asm_line(b, e->line, "LLGC %%r%d,0(%%r%d)", chr, addr);
            asm_line(b, e->line, "CLR %%r%d,%%r%d", chr, pat);
            asm_line(b, e->line, "BRCL 8,%s", found);
            asm_line(b, e->line, "AGHI %%r%d,1", addr);
            asm_line(b, e->line, "AGHI %%r0,-1");
            asm_line(b, e->line, "BRCL 7,%s", top);
            emit_label(b, e->line, notf);
            asm_line(b, e->line, "LGHI %%r0,0");
            asm_line(b, e->line, "BRCL 15,%s", end);
            emit_label(b, e->line, found);
            asm_line(b, e->line, "LGR %%r0,%%r%d", addr);
            emit_label(b, e->line, end);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_ptr(), 0);
            *slotp = res;
            free(top); free(found); free(notf); free(end);
            return 1;
        }
    }
    case E_BIN:
        return eval_bin(cg, e, slotp);
    }
    return 0;
}

static int codegen_stmt(Cg *cg, CBStmt *st);
static int codegen_block(Cg *cg, CBStmtVec *v);

/* Compare loops used when __memcmp/__strcmp is the condition itself. They
 * branch directly on the condition code and do not materialize -1/0/1. */
static int emit_memcmp_cond(Cg *cg, CBExpr *e, const char *false_label) {
    CBlock *b = cg->b;
    int sa, sp, sl, ra, rb;
    char *top, *true_label;
    if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sl))
        return 0;
    ra = pick_addr(b, 0, 0);
    rb = pick_addr(b, ra, 0);
    if (ra < 0 || rb < 0) {
        cb_error(b, e->line, e->col, "ZA5005", "__memcmp needs two nonzero scratch registers");
        return 0;
    }
    load_slot(b, e->line, sa, type_ptr(), ra);
    load_slot(b, e->line, sp, type_ptr(), rb);
    load_slot(b, e->line, sl, e->c->type, 0);
    top = new_label(b);
    true_label = new_label(b);
    asm_line(b, e->line, "LTGR %%r0,%%r0");
    asm_line(b, e->line, "BRCL 8,%s", false_label);
    emit_label(b, e->line, top);
    asm_line(b, e->line, "CLC 0(1,%%r%d),0(%%r%d)", ra, rb);
    asm_line(b, e->line, "BRCL 4,%s", true_label);
    asm_line(b, e->line, "BRCL 2,%s", true_label);
    asm_line(b, e->line, "AGHI %%r%d,1", ra);
    asm_line(b, e->line, "AGHI %%r%d,1", rb);
    asm_line(b, e->line, "AGHI %%r0,-1");
    asm_line(b, e->line, "BRCL 15,%s", top);
    emit_label(b, e->line, true_label);
    free(top);
    free(true_label);
    return 1;
}

static int emit_strcmp_cond(Cg *cg, CBExpr *e, const char *false_label) {
    CBlock *b = cg->b;
    int sa, sp, ra, rb;
    char *top, *true_label;
    if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
    ra = pick_addr(b, 0, 0);
    rb = pick_addr(b, ra, 0);
    if (ra < 0 || rb < 0) {
        cb_error(b, e->line, e->col, "ZA5005", "__strcmp needs two nonzero scratch registers");
        return 0;
    }
    load_slot(b, e->line, sa, type_ptr(), ra);
    load_slot(b, e->line, sp, type_ptr(), rb);
    top = new_label(b);
    true_label = new_label(b);
    emit_label(b, e->line, top);
    asm_line(b, e->line, "CLC 0(1,%%r%d),0(%%r%d)", ra, rb);
    asm_line(b, e->line, "BRCL 4,%s", true_label);
    asm_line(b, e->line, "BRCL 2,%s", true_label);
    asm_line(b, e->line, "LLGC %%r0,0(%%r%d)", ra);
    asm_line(b, e->line, "LTR %%r0,%%r0");
    asm_line(b, e->line, "BRCL 8,%s", false_label);
    asm_line(b, e->line, "AGHI %%r%d,1", ra);
    asm_line(b, e->line, "AGHI %%r%d,1", rb);
    asm_line(b, e->line, "BRCL 15,%s", top);
    emit_label(b, e->line, true_label);
    free(top);
    free(true_label);
    return 1;
}

static int emit_cond_zero(Cg *cg, CBExpr *e, const char *label) {
    CBlock *b = cg->b;
    int s;
    if (e->kind == E_CALL) {
        if (!strcmp(e->name, "__memcmp")) return emit_memcmp_cond(cg, e, label);
        if (!strcmp(e->name, "__strcmp")) return emit_strcmp_cond(cg, e, label);
    }
    if (!eval(cg, e, &s)) return 0;
    load_slot(b, e->line, s, e->type, 0);
    emit_test(b, e->line, e->type);
    asm_line(b, e->line, "BRCL 8,%s", label);
    return 1;
}

static int codegen_block(Cg *cg, CBStmtVec *v) {
    size_t i;
    for (i = 0; i < v->n; i++)
        if (!codegen_stmt(cg, &v->v[i])) return 0;
    return 1;
}

/* pick a nonzero register for an address operand; avoids r0 and used */
static int pick_addr(CBlock *b, int used1, int used2) {
    int i;
    for (i = 0; i < b->nscratch; i++)
        if (b->scratch[i] != 0 && b->scratch[i] != used1 && b->scratch[i] != used2)
            return b->scratch[i];
    if (used1 != 1 && used2 != 1) return 1;
    return -1;
}

static int codegen_intrinsic(Cg *cg, CBStmt *st) {
    CBlock *b = cg->b;
    int sd, ss, sl = -1, addr_d, addr_s = -1;
    int64_t len_cv = 0, pat_cv = 0;
    int have_len = const_eval(st->cond, &len_cv);
    int have_pat = st->kind == S_MEMSET ? const_eval(st->step, &pat_cv) : 0;
    int64_t pat = pat_cv;
    if (!eval(cg, st->expr, &sd)) return 0;
    if (st->kind != S_MEMSET && !eval(cg, st->step, &ss)) return 0;
    if (st->kind == S_MEMSET && !eval(cg, st->step, &ss)) return 0;
    if (!have_len && !eval(cg, st->cond, &sl)) return 0;
    if (have_len && len_cv < 0) {
        cb_error(b, st->line, 1, "ZA5003", "the length of a memory intrinsic is negative");
        return 0;
    }
    if (st->kind == S_MEMSET) {
        addr_d = pick_addr(b, 0, 1);
        if (addr_d < 0) {
            cb_error(b, st->line, 1, "ZA5005", "memset needs a nonzero scratch address register");
            return 0;
        }
        load_slot(b, st->line, sd, type_ptr(), addr_d);
        if (!have_pat) {
            load_slot(b, st->line, ss, st->step->type, 0);
        } else {
            asm_line(b, st->line, "LHI %%r0,%d", (int)(pat & 0xFF));
        }
        if (!have_len) load_slot(b, st->line, sl, st->cond->type, 1);
        else asm_line(b, st->line, "LGHI %%r1,%lld", (long long)len_cv);
        if (have_len && len_cv == 0) return 1;
        if (have_len && have_pat && (pat & 0xFF) == 0 && len_cv <= 4096) {
            int64_t off;
            for (off = 0; off < len_cv; off += 256) {
                int64_t n = len_cv - off > 256 ? 256 : len_cv - off;
                asm_line(b, st->line, "XC %lld(%lld,%%r%d),%lld(%%r%d)", (long long)off,
                         (long long)n, addr_d, (long long)off, addr_d);
            }
            return 1;
        }
        {
            char *top = new_label(b), *end = new_label(b);
            asm_line(b, st->line, "LTGR %%r1,%%r1");
            asm_line(b, st->line, "BRCL 8,%s", end);
            emit_label(b, st->line, top);
            asm_line(b, st->line, "STC %%r0,0(%%r%d)", addr_d);
            asm_line(b, st->line, "AGHI %%r%d,1", addr_d);
            asm_line(b, st->line, "AGHI %%r1,-1");
            asm_line(b, st->line, "BRCL 7,%s", top);
            emit_label(b, st->line, end);
            free(top); free(end);
        }
        return 1;
    }
    addr_d = pick_addr(b, 0, 1);
    addr_s = pick_addr(b, addr_d, 0);
    if (addr_d < 0 || addr_s < 0) {
        cb_error(b, st->line, 1, "ZA5005",
                 "%s needs two nonzero scratch address registers", st->kind == S_MEMCPY ? "__memcpy" : "__memmove");
        return 0;
    }
    load_slot(b, st->line, sd, type_ptr(), addr_d);
    load_slot(b, st->line, ss, type_ptr(), addr_s);
    if (!have_len) load_slot(b, st->line, sl, st->cond->type, 0);
    else asm_line(b, st->line, "LGHI %%r0,%lld", (long long)len_cv);
    if (have_len && len_cv == 0) return 1;
    if (st->kind == S_MEMCPY && have_len && len_cv <= 4096) {
        int64_t off;
        for (off = 0; off < len_cv; off += 256) {
            int64_t n = len_cv - off > 256 ? 256 : len_cv - off;
            asm_line(b, st->line, "MVC %lld(%lld,%%r%d),%lld(%%r%d)", (long long)off,
                     (long long)n, addr_d, (long long)off, addr_s);
        }
        return 1;
    }
    {
        char *top = new_label(b), *end = new_label(b);
        if (st->kind == S_MEMMOVE) {
            char *back = new_label(b), *btop = new_label(b);
            asm_line(b, st->line, "LTGR %%r0,%%r0");
            asm_line(b, st->line, "BRCL 8,%s", end);
            asm_line(b, st->line, "CLGR %%r%d,%%r%d", addr_d, addr_s);
            asm_line(b, st->line, "BRCL 2,%s", back);
            emit_label(b, st->line, top);
            asm_line(b, st->line, "MVC 0(1,%%r%d),0(%%r%d)", addr_d, addr_s);
            asm_line(b, st->line, "AGHI %%r%d,1", addr_d);
            asm_line(b, st->line, "AGHI %%r%d,1", addr_s);
            asm_line(b, st->line, "AGHI %%r0,-1");
            asm_line(b, st->line, "BRCL 7,%s", top);
            asm_line(b, st->line, "BRCL 15,%s", end);
            emit_label(b, st->line, back);
            asm_line(b, st->line, "AGR %%r%d,%%r0", addr_d);
            asm_line(b, st->line, "AGR %%r%d,%%r0", addr_s);
            emit_label(b, st->line, btop);
            asm_line(b, st->line, "AGHI %%r%d,-1", addr_d);
            asm_line(b, st->line, "AGHI %%r%d,-1", addr_s);
            asm_line(b, st->line, "MVC 0(1,%%r%d),0(%%r%d)", addr_d, addr_s);
            asm_line(b, st->line, "AGHI %%r0,-1");
            asm_line(b, st->line, "BRCL 7,%s", btop);
            free(back); free(btop);
        } else {
            char *chunk = new_label(b), *after = new_label(b);
            asm_line(b, st->line, "CGHI %%r0,256");
            asm_line(b, st->line, "BRCL 4,%s", after);
            emit_label(b, st->line, chunk);
            asm_line(b, st->line, "MVC 0(256,%%r%d),0(%%r%d)", addr_d, addr_s);
            asm_line(b, st->line, "AGHI %%r%d,256", addr_d);
            asm_line(b, st->line, "AGHI %%r%d,256", addr_s);
            asm_line(b, st->line, "AGHI %%r0,-256");
            asm_line(b, st->line, "CGHI %%r0,256");
            asm_line(b, st->line, "BRCL 10,%s", chunk);
            emit_label(b, st->line, after);
            asm_line(b, st->line, "LTGR %%r0,%%r0");
            asm_line(b, st->line, "BRCL 8,%s", end);
            {
                int treg = pick_addr(b, addr_d, addr_s);
                if (treg < 0) {
                    /* no third scratch register: copy the tail one byte at a time */
                    emit_label(b, st->line, top);
                    asm_line(b, st->line, "MVC 0(1,%%r%d),0(%%r%d)", addr_d, addr_s);
                    asm_line(b, st->line, "AGHI %%r%d,1", addr_d);
                    asm_line(b, st->line, "AGHI %%r%d,1", addr_s);
                    asm_line(b, st->line, "AGHI %%r0,-1");
                    asm_line(b, st->line, "BRCL 7,%s", top);
                } else {
                    char *tmpl = new_label(b), *ex = new_label(b);
                    /* EX ORs the low byte of a nonzero register into the MVC
                       length byte. R0 cannot be used: EX with an R1 field of
                       zero performs no ORing at all. */
                    asm_line(b, st->line, "AGHI %%r0,-1");
                    asm_line(b, st->line, "BRCL 15,%s", ex);
                    emit_label(b, st->line, tmpl);
                    asm_line(b, st->line, "MVC 0(0,%%r%d),0(%%r%d)", addr_d, addr_s);
                    emit_label(b, st->line, ex);
                    asm_line(b, st->line, "LARL %%r%d,%s", treg, tmpl);
                    asm_line(b, st->line, "EX %%r%d,0(%%r%d)", treg, treg);
                    free(tmpl); free(ex);
                }
            }
            free(chunk); free(after);
        }
        emit_label(b, st->line, end);
        free(top); free(end);
    }
    return 1;
}

static int codegen_stmt(Cg *cg, CBStmt *st) {
    CBlock *b = cg->b;
    int s;
    cg->slot_top = cg->base_slots;
    if (st->kind == S_IF) {
        char *els = new_label(b), *end = NULL;
        if (!emit_cond_zero(cg, st->cond, els)) { free(els); return 0; }
        if (!codegen_block(cg, &st->body)) { free(els); return 0; }
        if (st->other.n) {
            end = new_label(b);
            asm_line(b, st->line, "BRCL 15,%s", end);
            emit_label(b, st->line, els);
            if (!codegen_block(cg, &st->other)) { free(els); free(end); return 0; }
            emit_label(b, st->line, end);
            free(end);
        } else {
            emit_label(b, st->line, els);
        }
        free(els);
        return 1;
    }
    if (st->kind == S_SWITCH) {
        char *end = new_label(b);
        const char *def = NULL;
        size_t i;
        int s;
        for (i = 0; i < st->body.n; i++)
            if (st->body.v[i].kind == S_DEFAULT) def = st->body.v[i].label;
        if (!eval(cg, st->cond, &s)) { free(end); return 0; }
        for (i = 0; i < st->body.n; i++) {
            CBStmt *c = &st->body.v[i];
            if (c->kind != S_CASE) continue;
            load_slot(b, c->line, s, st->cond->type, 0);
            load_imm(b, c->line, c->num, st->cond->type, 1);
            asm_line(b, c->line, st->cond->type.bits == 64
                                    ? (st->cond->type.unsig ? "CLGR %%r0,%%r1" : "CGR %%r0,%%r1")
                                    : (st->cond->type.unsig ? "CLR %%r0,%%r1" : "CR %%r0,%%r1"));
            asm_line(b, c->line, "BRCL 8,%s", c->label);
        }
        asm_line(b, st->line, "BRCL 15,%s", def ? def : end);
        if (cg->ctrl_depth >= 16) {
            cb_error(b, st->line, 1, "ZA5008", "control flow nested too deeply");
            free(end);
            return 0;
        }
        cg->ctrl_break[cg->ctrl_depth] = end;
        cg->ctrl_continue[cg->ctrl_depth] = "";
        cg->ctrl_loop[cg->ctrl_depth] = 0;
        cg->ctrl_depth++;
        for (i = 0; i < st->body.n; i++) {
            CBStmt *c = &st->body.v[i];
            if (c->kind == S_CASE || c->kind == S_DEFAULT) emit_label(b, c->line, c->label);
            else if (!codegen_stmt(cg, c)) {
                cg->ctrl_depth--;
                free(end);
                return 0;
            }
        }
        cg->ctrl_depth--;
        emit_label(b, st->line, end);
        free(end);
        return 1;
    }
    if (st->kind == S_WHILE) {
        char *top = new_label(b), *end = new_label(b);
        if (cg->ctrl_depth >= 16) {
            cb_error(b, st->line, 1, "ZA5008", "loops nested too deeply");
            free(top); free(end);
            return 0;
        }
        cg->ctrl_break[cg->ctrl_depth] = end;
        cg->ctrl_continue[cg->ctrl_depth] = top;
        cg->ctrl_loop[cg->ctrl_depth] = 1;
        cg->ctrl_depth++;
        emit_label(b, st->line, top);
        if (!emit_cond_zero(cg, st->cond, end)) goto while_fail;
        if (!codegen_block(cg, &st->body)) goto while_fail;
        asm_line(b, st->line, "BRCL 15,%s", top);
        emit_label(b, st->line, end);
        cg->ctrl_depth--;
        free(top); free(end);
        return 1;
while_fail:
        cg->ctrl_depth--;
        free(top); free(end);
        return 0;
    }
    if (st->kind == S_DO) {
        char *top = new_label(b), *cont = new_label(b), *end = new_label(b);
        if (cg->ctrl_depth >= 16) {
            cb_error(b, st->line, 1, "ZA5008", "loops nested too deeply");
            free(top); free(cont); free(end);
            return 0;
        }
        cg->ctrl_break[cg->ctrl_depth] = end;
        cg->ctrl_continue[cg->ctrl_depth] = cont;
        cg->ctrl_loop[cg->ctrl_depth] = 1;
        cg->ctrl_depth++;
        emit_label(b, st->line, top);
        if (!codegen_block(cg, &st->body)) goto do_fail;
        emit_label(b, st->line, cont);
        {
            int s;
            if (!eval(cg, st->cond, &s)) goto do_fail;
            load_slot(b, st->cond->line, s, st->cond->type, 0);
            emit_test(b, st->cond->line, st->cond->type);
            asm_line(b, st->line, "BRCL 7,%s", top);
        }
        emit_label(b, st->line, end);
        cg->ctrl_depth--;
        free(top); free(cont); free(end);
        return 1;
do_fail:
        cg->ctrl_depth--;
        free(top); free(cont); free(end);
        return 0;
    }
    if (st->kind == S_FOR) {
        char *top = new_label(b), *step = new_label(b), *end = new_label(b);
        if (!codegen_block(cg, &st->init)) { free(top); free(step); free(end); return 0; }
        if (cg->ctrl_depth >= 16) {
            cb_error(b, st->line, 1, "ZA5008", "loops nested too deeply");
            free(top); free(step); free(end);
            return 0;
        }
        cg->ctrl_break[cg->ctrl_depth] = end;
        cg->ctrl_continue[cg->ctrl_depth] = step;
        cg->ctrl_loop[cg->ctrl_depth] = 1;
        cg->ctrl_depth++;
        emit_label(b, st->line, top);
        if (st->cond && !emit_cond_zero(cg, st->cond, end)) goto for_fail;
        if (!codegen_block(cg, &st->body)) goto for_fail;
        emit_label(b, st->line, step);
        if (st->step) {
            int s;
            if (!eval(cg, st->step, &s)) goto for_fail;
        }
        asm_line(b, st->line, "BRCL 15,%s", top);
        emit_label(b, st->line, end);
        cg->ctrl_depth--;
        free(top); free(step); free(end);
        return 1;
for_fail:
        cg->ctrl_depth--;
        free(top); free(step); free(end);
        return 0;
    }
    if (st->kind == S_BREAK || st->kind == S_CONTINUE) {
        if (cg->ctrl_depth <= 0) {
            cb_error(b, st->line, 1, "ZA5008", "break/continue outside a loop");
            return 0;
        }
        {
            int k = cg->ctrl_depth - 1;
            if (st->kind == S_CONTINUE) {
                while (k >= 0 && !cg->ctrl_loop[k]) k--;
                if (k < 0) {
                    cb_error(b, st->line, 1, "ZA5008", "continue outside a loop");
                    return 0;
                }
                asm_line(b, st->line, "BRCL 15,%s", cg->ctrl_continue[k]);
            } else {
                asm_line(b, st->line, "BRCL 15,%s", cg->ctrl_break[k]);
            }
        }
        return 1;
    }
    if (st->kind == S_MEMCPY || st->kind == S_MEMMOVE || st->kind == S_MEMSET)
        return codegen_intrinsic(cg, st);
    if (st->kind == S_LABEL) {
        emit_label(b, st->line, st->label);
        return 1;
    }
    if (st->kind == S_GOTO) {
        const char *lab = strmap_get(&b->labels, st->label);
        if (!lab) {
            cb_error(b, st->line, 1, "ZA5004", "goto target \"%s\" is not defined", st->label);
            return 0;
        }
        asm_line(b, st->line, "BRCL 15,%s", lab);
        return 1;
    }
    if (st->kind == S_DECL) {
        if (st->expr) {
            if (!eval(cg, st->expr, &s)) return 0;
            load_slot(b, st->line, s, st->var->type, 0);
            store_slot(b, st->line, st->var->slot, st->var->type, 0);
        } else {
            asm_line(b, st->line, "LGHI %%r0,0");
            store_slot(b, st->line, st->var->slot, st->var->type, 0);
        }
        return 1;
    }
    if (st->kind == S_ASSIGN) {
        if (st->lval) {
            int sa, rs, addr;
            if (st->op != '=') {
                CBExpr bin;
                memset(&bin, 0, sizeof bin);
                bin.kind = E_BIN;
                bin.type = st->lval->type;
                bin.op = st->op;
                bin.a = st->lval;
                bin.b = st->expr;
                bin.line = st->line;
                if (!eval(cg, &bin, &rs)) return 0;
            } else if (!eval(cg, st->expr, &rs)) return 0;
            if (!eval(cg, st->lval->a, &sa)) return 0;
            addr = pick_addr(b, 0, 0);
            if (addr < 0) {
                cb_error(b, st->line, 1, "ZA5005", "a dereference assignment needs a scratch register");
                return 0;
            }
            load_slot(b, st->line, rs, st->lval->type, 0);
            load_slot(b, st->line, sa, type_ptr(), addr);
            if (st->lval->type.bits == 64) asm_line(b, st->line, "STG %%r0,0(%%r%d)", addr);
            else if (st->lval->type.bits == 32) asm_line(b, st->line, "ST %%r0,0(%%r%d)", addr);
            else asm_line(b, st->line, "STC %%r0,0(%%r%d)", addr);
            return 1;
        }
        if (st->op != '=') {
            CBExpr lhs, bin;
            memset(&lhs, 0, sizeof lhs);
            lhs.kind = E_VAR;
            lhs.type = st->var->type;
            lhs.var = st->var;
            lhs.line = st->line;
            memset(&bin, 0, sizeof bin);
            bin.kind = E_BIN;
            bin.type = st->var->type;
            bin.op = st->op;
            bin.a = &lhs;
            bin.b = st->expr;
            bin.line = st->line;
            if (!eval(cg, &bin, &s)) return 0;
            load_slot(b, st->line, s, st->var->type, 0);
            store_slot(b, st->line, st->var->slot, st->var->type, 0);
            return 1;
        }
        if (!eval(cg, st->expr, &s)) return 0;
        load_slot(b, st->line, s, st->var->type, 0);
        store_slot(b, st->line, st->var->slot, st->var->type, 0);
        return 1;
    }
    if (st->kind == S_EXPR) {
        if (!eval(cg, st->expr, &s)) return 0;
        return 1;
    }
    return 1;
}

/* ---- block compile ------------------------------------------------------ */

static void free_block(CBlock *b) {
    size_t i;
    for (i = 0; i < b->vars.n; i++) {
        free(b->vars.v[i]->name);
        free(b->vars.v[i]->lit);
        free(b->vars.v[i]);
    }
    free(b->vars.v);
    for (i = 0; i < b->byname.cap; i++) free(b->byname.keys[i]);
    free(b->byname.keys);
    free(b->byname.vals);
    for (i = 0; i < b->labels.cap; i++) {
        free(b->labels.keys[i]);
        free(b->labels.vals[i]);
    }
    free(b->labels.keys);
    free(b->labels.vals);
    for (i = 0; i < b->ptypenames.n; i++) free(b->ptypenames.v[i]);
    free(b->ptypenames.v);
    for (i = 0; i < b->nlits; i++) {
        free(b->lits[i].data);
        free(b->lits[i].label);
    }
    free(b->lits);
    free(b->lit_end);
    free(b->name);
}

void cblock_debug_free(CBlockDebugVec *v) {
    size_t i, j;
    if (!v) return;
    for (i = 0; i < v->n; i++) {
        CBlockDebug *d = &v->v[i];
        free(d->file);
        free(d->name);
        free(d->start_label);
        free(d->end_label);
        for (j = 0; j < d->nvars; j++) free(d->vars[j].name);
        free(d->vars);
    }
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

int cblock_compile(const char *file, int line, const char *head, const CBlockLine *body,
                   size_t n, Reporter *rep, SrcVec *out, int *label_counter,
                   CBlockDebugVec *debug) {
    CBlock b;
    Parser p;
    CBStmtVec stmts = {0};
    size_t i, body_start = n;
    int ok = 1;
    size_t out_start = out->n;
    memset(&b, 0, sizeof b);
    memset(&p, 0, sizeof p);
    b.file = file;
    b.line = line;
    b.rep = rep;
    b.out = out;
    b.body = body;
    b.body_n = n;
    b.temp_reg = -1;
    b.name = xstrdup(head && *head ? head : "cblock");
    b.temp_down = 1;
    b.debug = debug;
    if (label_counter) {
        b.label_no = *label_counter;
        b.label_seq = *label_counter;
    }
    p.b = &b;
    if (b.debug) {
        b.start_label = new_label(&b);
        b.end_label = new_label(&b);
        emit_label(&b, line, b.start_label);
    }
    /* metadata */
    for (i = 0; i < n; i++) {
        char *w = NULL, *rest = NULL;
        const char *s = body[i].text;
        const char *q = s;
        while (*q == ' ' || *q == '\t') q++;
        if (!*q) continue;
        if (!pragma_split(s, &w, &rest)) {
            cb_error(&b, body[i].line, 1, "ZA5001", "expected #pragma metadata before #pragma cbegin");
            ok = 0;
            continue;
        }
        if (!strcmp(w, "cbegin")) {
            free(w);
            free(rest);
            body_start = i + 1;
            break;
        }
        if (!strcmp(w, "cin")) ok = binding_parse(&b, rest, body[i].line, V_CIN) && ok;
        else if (!strcmp(w, "cout")) ok = binding_parse(&b, rest, body[i].line, V_COUT) && ok;
        else if (!strcmp(w, "cinout")) ok = binding_parse(&b, rest, body[i].line, V_CINOUT) && ok;
        else if (!strcmp(w, "cscratch")) {
            char *s2 = xstrdup(rest), *pp = s2;
            for (;;) {
                char *comma = strchr(pp, ',');
                char *part, *dash;
                int r1 = -1, r2 = -1;
                if (comma) *comma = 0;
                part = trim(pp);
                dash = strchr(part, '-');
                if (dash) { *dash = 0; if (!reg_parse(trim(dash + 1), &r2)) r2 = -1; }
                if (!reg_parse(trim(part), &r1)) {
                    cb_error(&b, body[i].line, 1, "ZA5005", "\"%s\" is not a register", part);
                    ok = 0;
                } else if (dash && (r2 < r1)) {
                    cb_error(&b, body[i].line, 1, "ZA5005", "invalid cscratch register range");
                    ok = 0;
                } else if (dash) {
                    int r;
                    for (r = r1; r <= r2; r++) add_scratch(&b, r, body[i].line);
                } else add_scratch(&b, r1, body[i].line);
                if (!comma) break;
                pp = comma + 1;
            }
            free(s2);
        } else if (!strcmp(w, "ctemp")) ok = ctemp_parse(&b, rest, body[i].line) && ok;
        else {
            cb_error(&b, body[i].line, 1, "ZA5005", "unknown cblock pragma \"%s\"", w);
            ok = 0;
        }
        free(w);
        free(rest);
    }
    if (!ok) goto done;
    if (body_start == n) {
        cb_error(&b, line, 1, "ZA5001", "a cblock needs #pragma cbegin");
        goto done;
    }
    if (b.temp_reg < 0) {
        cb_error(&b, line, 1, "ZA5006", "cblock needs a ctemp area for its variables and values");
        goto done;
    }
    for (i = 0; i < b.vars.n; i++) {
        if (b.vars.v[i]->reg == 0 || b.vars.v[i]->reg == 1) {
            cb_error(&b, line, 1, "ZA5005", "register %%r%d cannot be a cin/cout binding",
                     b.vars.v[i]->reg);
            goto done;
        }
        if (b.vars.v[i]->reg == b.temp_reg) {
            cb_error(&b, line, 1, "ZA5005", "bound register %%r%d is also the ctemp base",
                     b.vars.v[i]->reg);
            goto done;
        }
    }
    for (i = 0; i < (size_t)b.nscratch; i++) {
        size_t j;
        if (b.scratch[i] == b.temp_reg) {
            cb_error(&b, line, 1, "ZA5005", "the ctemp base register cannot also be cscratch");
            goto done;
        }
        for (j = 0; j < b.vars.n; j++) {
            CBVar *v = b.vars.v[j];
            if (v->reg >= 0 && v->reg == b.scratch[i]) {
                cb_error(&b, line, 1, "ZA5005", "bound register %%r%d also appears in cscratch", v->reg);
                goto done;
            }
        }
    }
    /* source text and per-byte line map */
    {
        Buf src = {0};
        int *lmap = NULL;
        size_t cap = 0, used = 0;
        for (i = body_start; i < n; i++) {
            size_t k, ln = strlen(body[i].text);
            if (used + ln + 2 > cap) {
                cap = (used + ln + 2) * 2;
                lmap = xrealloc(lmap, cap * sizeof *lmap);
            }
            for (k = 0; k < ln; k++) { buf_putc(&src, body[i].text[k]); lmap[used++] = body[i].line; }
            buf_putc(&src, '\n');
            lmap[used++] = body[i].line;
        }
        lmap = xrealloc(lmap, (used + 1) * sizeof *lmap);
        lmap[used] = line;
        memset(&p.l, 0, sizeof p.l);
        p.l.b = &b;
        p.l.src = buf_cstr(&src);
        p.l.line_at = lmap;
        p.l.n = used;
        lex_next(&p.l);
        while (p.l.tok.kind != TK_EOF && !p.err) parse_stmt(&p, &stmts);
        if (p.err) ok = 0;
        if (ok && !check_gotos(&b, &stmts)) ok = 0;
        if (ok) {
            Cg cg;
            b.base_slots = (int)b.vars.n;
            b.save_r0 = b.save_r1 = -1;
            if (b.temp_reg >= 0) {
                int need0 = !has_scratch(&b, 0), need1 = !has_scratch(&b, 1);
                if (need0 && need1 && b.temp_down) {
                    /* keep R0 below R1 in memory so STMG 0,1 stores them
                     * in register order */
                    b.save_r1 = b.base_slots++;
                    b.save_r0 = b.base_slots++;
                } else {
                    if (need0) b.save_r0 = b.base_slots++;
                    if (need1) b.save_r1 = b.base_slots++;
                }
            }
            memset(&cg, 0, sizeof cg);
            cg.b = &b;
            cg.base_slots = b.base_slots;
            cg.slot_top = b.base_slots;
            cg.max_slot = b.base_slots;
            asm_line(&b, line, "* cblock %s", b.name);
            if (b.save_r0 >= 0 && b.save_r1 >= 0)
                asm_line(&b, line, "STMG %%r0,%%r1,%ld(%%r%d)", slot_off(&b, b.save_r0), b.temp_reg);
            else {
                if (b.save_r0 >= 0) store_slot(&b, line, b.save_r0, type_of(64, 1), 0);
                if (b.save_r1 >= 0) store_slot(&b, line, b.save_r1, type_of(64, 1), 1);
            }
            for (i = 0; i < b.vars.n; i++) {
                CBVar *v = b.vars.v[i];
                if (v->kind == V_CIN || v->kind == V_CINOUT) {
                    store_slot(&b, line, v->slot, v->type, v->reg);
                }
            }
            for (i = 0; i < stmts.n && ok; i++) ok = codegen_stmt(&cg, &stmts.v[i]);
            if (ok && cg.max_slot * 8 > b.temp_size) {
                cb_error(&b, line, 1, "ZA5006", "cblock temp area needs %d bytes, but ctemp declares %ld",
                         cg.max_slot * 8, b.temp_size);
                ok = 0;
            }
            if (ok) {
                for (i = 0; i < b.vars.n; i++) {
                    CBVar *v = b.vars.v[i];
                    if (v->kind == V_COUT || v->kind == V_CINOUT) load_slot(&b, line, v->slot, v->type, v->reg);
                }
                if (b.save_r0 >= 0 && b.save_r1 >= 0)
                    asm_line(&b, line, "LMG %%r0,%%r1,%ld(%%r%d)", slot_off(&b, b.save_r0), b.temp_reg);
                else {
                    if (b.save_r1 >= 0) load_slot(&b, line, b.save_r1, type_of(64, 1), 1);
                    if (b.save_r0 >= 0) load_slot(&b, line, b.save_r0, type_of(64, 1), 0);
                }
            }
            if (ok && b.nlits) {
                b.lit_end = new_label(&b);
                asm_line(&b, line, "BRCL 15,%s", b.lit_end);
            }
            if (b.debug) {
                emit_label(&b, line, b.end_label);
                if (ok) {
                    CBlockDebug cd;
                    size_t nv = 0, vi;
                    memset(&cd, 0, sizeof cd);
                    cd.file = xstrdup(file);
                    cd.name = xstrdup(b.name);
                    cd.start_label = b.start_label;
                    cd.end_label = b.end_label;
                    cd.line = line;
                    cd.temp_reg = b.temp_reg;
                    b.start_label = b.end_label = NULL;
                    for (vi = 0; vi < b.vars.n; vi++)
                        if (b.vars.v[vi]->kind == V_LOCAL) nv++;
                    if (nv) cd.vars = xcalloc(nv * sizeof *cd.vars);
                    for (vi = 0; vi < b.vars.n; vi++) {
                        CBVar *v = b.vars.v[vi];
                        CBlockVarDebug *cv;
                        if (v->kind != V_LOCAL) continue;
                        cv = &cd.vars[cd.nvars++];
                        cv->name = xstrdup(v->name);
                        cv->bits = v->type.ptr ? 0 : v->type.bits;
                        cv->unsig = v->type.unsig;
                        cv->ptr = v->type.ptr;
                        cv->off = slot_off(&b, v->slot);
                    }
                    vec_push(b.debug, cd);
                }
            }
            if (ok && b.nlits) emit_literal_data(&b, line);
        }
        for (i = 0; i < p.l.ids.n; i++) free(p.l.ids.v[i]);
        free(p.l.ids.v);
        free(lmap);
        free(src.p);
    }
done:
    free(b.start_label);
    free(b.end_label);
    if (label_counter) {
        int used = b.label_no > b.label_seq ? b.label_no : b.label_seq;
        if (used > *label_counter) *label_counter = used;
    }
    for (i = 0; i < stmts.n; i++) {
        cblock_expr_free(stmts.v[i].expr);
        cblock_expr_free(stmts.v[i].lval);
    }
    if (!ok) {
        size_t k;
        for (k = out_start; k < out->n; k++) free((char *)out->v[k].text);
        out->n = out_start;
    }
    free(stmts.v);
    free_block(&b);
    return ok ? 0 : 1;
}
