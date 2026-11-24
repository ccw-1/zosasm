/* cblock.c - inline scalar C-like blocks. See PLAN-CBLOCK.md.
 *
 * M1: expressions, bound registers, scalar locals, assignments and the
 * integer operators. Control flow and the full listing integration follow in
 * M2/M3. The parser is deliberately small and reports ZA5xxx diagnostics. */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "cblock.h"
#include "util.h"

typedef struct {
    int bits;  /* 32 or 64 */
    int unsig; /* unsigned */
} CType;

typedef enum { V_CIN, V_COUT, V_CINOUT, V_LOCAL } VKind;

typedef struct CBVar {
    char *name;
    CType type;
    VKind kind;
    int reg;       /* bound register, -1 otherwise */
    int slot;      /* temp-area slot index */
    int initialized;
} CBVar;

typedef Vec(CBVar *) CBVarVec;

typedef enum { E_NUM, E_VAR, E_UN, E_BIN, E_CAST, E_COND, E_INC } CBEKind;

typedef struct CBExpr CBExpr;
struct CBExpr {
    CBEKind kind;
    CType type;
    int op; /* operator character or OP_* */
    int64_t num;
    CBVar *var;
    CBExpr *a, *b, *c;
    int read_before;
    int line, col;
};

typedef enum { S_EXPR, S_ASSIGN, S_DECL, S_IF, S_WHILE, S_DO, S_FOR, S_BREAK, S_CONTINUE } CBSKind;
typedef struct CBStmt CBStmt;
typedef Vec(CBStmt) CBStmtVec;
struct CBStmt {
    CBSKind kind;
    CBVar *var;
    int op; /* assignment operator */
    CBExpr *expr;
    CBExpr *cond;
    CBExpr *step;
    CBStmtVec body;
    CBStmtVec other;
    CBStmtVec init;
    int line;
};

typedef struct {
    const char *file;
    int line;
    char *name;
    CBVarVec vars;
    StrMap byname;
    int scratch[16];
    int nscratch;
    int temp_reg;
    long temp_size;
    int temp_down;
    int label_no;
    Reporter *rep;
    SrcVec *out;
} CBlock;

/* multi-character operators */
enum { OP_SHL = 256, OP_SHR, OP_LE, OP_GE, OP_EQ, OP_NE,
       OP_ADDEQ, OP_SUBEQ, OP_MULEQ, OP_DIVEQ, OP_MODEQ,
       OP_ANDEQ, OP_OREQ, OP_XOREQ, OP_SHLEQ, OP_SHREQ,
       OP_LAND, OP_LOR, OP_INC, OP_DEC };

/* ---- small helpers ------------------------------------------------------ */

static CType type_of(int bits, int unsig) { CType t; t.bits = bits; t.unsig = unsig; return t; }
static int type_same(CType a, CType b) { return a.bits == b.bits && a.unsig == b.unsig; }

static int type_parse(const char *s, CType *t) {
    if (za_streqi(s, "int32_t")) { *t = type_of(32, 0); return 1; }
    if (za_streqi(s, "uint32_t")) { *t = type_of(32, 1); return 1; }
    if (za_streqi(s, "int64_t")) { *t = type_of(64, 0); return 1; }
    if (za_streqi(s, "uint64_t")) { *t = type_of(64, 1); return 1; }
    if (za_streqi(s, "int")) { *t = type_of(32, 0); return 1; }
    if (za_streqi(s, "unsigned")) { *t = type_of(32, 1); return 1; }
    return 0;
}

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

typedef enum { TK_EOF, TK_ID, TK_NUM, TK_OP } TokKind;
typedef struct {
    TokKind kind;
    char *s; /* TK_ID */
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
    if (is_id_start((unsigned char)l->src[l->pos])) {
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

/* ---- parser ------------------------------------------------------------- */

typedef struct {
    CBlock *b;
    Lex l;
    int err;
    int loop_depth;
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
    if (op == OP_EQ || op == OP_NE || op == OP_LE || op == OP_GE || op == '<' || op == '>' ||
        op == OP_LAND || op == OP_LOR) {
        t = type_of(32, 0);
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
    if (p->l.tok.kind != TK_ID || !type_parse(p->l.tok.s, t)) return 0;
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

static void parse_stmt(Parser *p, CBStmtVec *st) {
    CType t;
    CBExpr *e;
    int line = p->l.tok.line;
    if (tok_is(&p->l, ';')) { lex_next(&p->l); return; }
    if (tok_is(&p->l, '{')) {
        lex_next(&p->l);
        while (!tok_is(&p->l, '}') && p->l.tok.kind != TK_EOF && !p->err) parse_stmt(p, st);
        if (tok_is(&p->l, '}')) lex_next(&p->l);
        else cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '}'");
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
    if (p->l.tok.kind == TK_ID && (tok_id(&p->l, "break") || tok_id(&p->l, "continue"))) {
        int brk = tok_id(&p->l, "break");
        if (p->loop_depth == 0) {
            cb_error(p->b, line, p->l.tok.col, "ZA5008", "%s is only valid in a loop",
                     brk ? "break" : "continue");
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
        if (e->kind != E_VAR) {
            cb_error(p->b, e->line, e->col, "ZA5003", "the left side of an assignment must be a variable");
            p->err = 1;
            return;
        }
        v = e->var;
        if (op != '=' && e->read_before) {
            cb_error(p->b, e->line, e->col, "ZA5003", "\"%s\" is read before it is assigned",
                     v->name);
            p->err = 1;
            return;
        }
        if (v->kind == V_CIN) {
            cb_error(p->b, e->line, e->col, "ZA5003", "\"%s\" is a cin variable and cannot be assigned",
                     v->name);
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        rhs = parse_assignment(p);
        if (!rhs) { p->err = 1; return; }
        memset(&s, 0, sizeof s);
        s.kind = S_ASSIGN;
        s.var = v;
        s.op = assign_base(op);
        s.expr = mk_cast(rhs, v->type);
        s.line = line;
        vec_push(st, s);
        v->initialized = 1;
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
    *word = za_strlower(xstrndup(p, n));
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
        char *part = NULL, *decl = NULL, *eq, *vn = NULL, *rn = NULL;
        CType t;
        int reg, i, bad = 0;
        if (comma) *comma = 0;
        part = trim(p);
        if (*part) {
            decl = part;
            while (*decl && *decl != ' ' && *decl != '\t') decl++;
            if (*decl) *decl++ = 0;
            decl = trim(decl);
            eq = strchr(decl, '=');
            if (!*decl || !eq) {
                cb_error(b, line, 1, "ZA5005", "expected \"type name=%%rN\" in cblock metadata");
                bad = 1;
            } else {
                *eq = 0;
                vn = trim(decl);
                rn = trim(eq + 1);
                if (!type_parse(part, &t)) {
                    cb_error(b, line, 1, "ZA5002", "unsupported cblock type \"%s\"", part);
                    bad = 1;
                } else if (!*vn || !reg_parse(rn, &reg)) {
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
            }
        }
        free(vn);
        free(rn);
        free(decl == part ? NULL : decl);
        free(part);
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
        if (reg == 0) {
            cb_error(b, line, 1, "ZA5005", "ctemp cannot use %%r0 as its base register");
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

static void cblock_expr_free(CBExpr *e) {
    if (!e) return;
    cblock_expr_free(e->a);
    cblock_expr_free(e->b);
    cblock_expr_free(e->c);
    free(e);
}

static void cblock_stmt_vec_free(CBStmtVec *v);

static void cblock_stmt_free(CBStmt *s) {
    cblock_expr_free(s->expr);
    cblock_expr_free(s->cond);
    cblock_expr_free(s->step);
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
    int slot_top;
    int max_slot;
    int loop_depth;
    const char *loop_break[16];
    const char *loop_continue[16];
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

static void asm_raw(CBlock *b, int line, char *s) {
    SrcLine sl;
    memset(&sl, 0, sizeof sl);
    sl.file = b->file;
    sl.line = line;
    sl.text = s;
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

static void emit_test(CBlock *b, int line, CType t) {
    asm_line(b, line, t.bits == 64 ? "LTGR %%r0,%%r0" : "LTR %%r0,%%r0");
}

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
        emit_compare(b, e->line, ot, e->op, 0, 1, res);
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
    case E_BIN:
        return eval_bin(cg, e, slotp);
    }
    return 0;
}

static int codegen_stmt(Cg *cg, CBStmt *st);
static int codegen_block(Cg *cg, CBStmtVec *v);

static int emit_cond_zero(Cg *cg, CBExpr *e, const char *label) {
    CBlock *b = cg->b;
    int s;
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

static int codegen_stmt(Cg *cg, CBStmt *st) {
    CBlock *b = cg->b;
    int s;
    cg->slot_top = (int)b->vars.n;
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
    if (st->kind == S_WHILE) {
        char *top = new_label(b), *end = new_label(b);
        if (cg->loop_depth >= 16) {
            cb_error(b, st->line, 1, "ZA5008", "loops nested too deeply");
            free(top); free(end);
            return 0;
        }
        cg->loop_break[cg->loop_depth] = end;
        cg->loop_continue[cg->loop_depth] = top;
        cg->loop_depth++;
        emit_label(b, st->line, top);
        if (!emit_cond_zero(cg, st->cond, end)) goto while_fail;
        if (!codegen_block(cg, &st->body)) goto while_fail;
        asm_line(b, st->line, "BRCL 15,%s", top);
        emit_label(b, st->line, end);
        cg->loop_depth--;
        free(top); free(end);
        return 1;
while_fail:
        cg->loop_depth--;
        free(top); free(end);
        return 0;
    }
    if (st->kind == S_DO) {
        char *top = new_label(b), *cont = new_label(b), *end = new_label(b);
        if (cg->loop_depth >= 16) {
            cb_error(b, st->line, 1, "ZA5008", "loops nested too deeply");
            free(top); free(cont); free(end);
            return 0;
        }
        cg->loop_break[cg->loop_depth] = end;
        cg->loop_continue[cg->loop_depth] = cont;
        cg->loop_depth++;
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
        cg->loop_depth--;
        free(top); free(cont); free(end);
        return 1;
do_fail:
        cg->loop_depth--;
        free(top); free(cont); free(end);
        return 0;
    }
    if (st->kind == S_FOR) {
        char *top = new_label(b), *step = new_label(b), *end = new_label(b);
        if (!codegen_block(cg, &st->init)) { free(top); free(step); free(end); return 0; }
        if (cg->loop_depth >= 16) {
            cb_error(b, st->line, 1, "ZA5008", "loops nested too deeply");
            free(top); free(step); free(end);
            return 0;
        }
        cg->loop_break[cg->loop_depth] = end;
        cg->loop_continue[cg->loop_depth] = step;
        cg->loop_depth++;
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
        cg->loop_depth--;
        free(top); free(step); free(end);
        return 1;
for_fail:
        cg->loop_depth--;
        free(top); free(step); free(end);
        return 0;
    }
    if (st->kind == S_BREAK || st->kind == S_CONTINUE) {
        if (cg->loop_depth <= 0) {
            cb_error(b, st->line, 1, "ZA5008", "break/continue outside a loop");
            return 0;
        }
        asm_line(b, st->line, "BRCL 15,%s",
                 st->kind == S_BREAK ? cg->loop_break[cg->loop_depth - 1]
                                     : cg->loop_continue[cg->loop_depth - 1]);
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
        free(b->vars.v[i]);
    }
    free(b->vars.v);
    for (i = 0; i < b->byname.cap; i++) free(b->byname.keys[i]);
    free(b->byname.keys);
    free(b->byname.vals);
    free(b->name);
}

int cblock_compile(const char *file, int line, const char *head, const CBlockLine *body,
                   size_t n, Reporter *rep, SrcVec *out) {
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
    b.temp_reg = -1;
    b.name = xstrdup(head && *head ? head : "cblock");
    b.temp_down = 1;
    p.b = &b;
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
    if (b.nscratch < 2) {
        cb_error(&b, line, 1, "ZA5005", "cblock needs at least two cscratch registers");
        goto done;
    }
    for (i = 0; i < b.vars.n; i++) {
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
        if (ok) {
            Cg cg;
            memset(&cg, 0, sizeof cg);
            cg.b = &b;
            cg.slot_top = (int)b.vars.n;
            cg.max_slot = (int)b.vars.n;
            asm_line(&b, line, "* cblock %s", b.name);
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
            }
        }
        for (i = 0; i < p.l.ids.n; i++) free(p.l.ids.v[i]);
        free(p.l.ids.v);
        free(lmap);
        free(src.p);
    }
done:
    for (i = 0; i < stmts.n; i++) cblock_expr_free(stmts.v[i].expr);
    if (!ok) {
        size_t k;
        for (k = out_start; k < out->n; k++) free((char *)out->v[k].text);
        out->n = out_start;
    }
    free(stmts.v);
    free_block(&b);
    return ok ? 0 : 1;
}
