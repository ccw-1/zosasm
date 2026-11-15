/* expr.c - assembler expression values and the expression parser. */
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "ebcdic.h"

/* ---- bases and values --------------------------------------------------- */

Base base_piece(Piece *p) {
    Base b;
    b.kind = B_PIECE;
    b.p = p;
    return b;
}

Base base_class(Class *c) {
    Base b;
    b.kind = B_CLASS;
    b.p = c;
    return b;
}

Base base_sym(Symbol *s) {
    Base b;
    b.kind = B_SYM;
    b.p = s;
    return b;
}

int base_eq(Base a, Base b) { return a.kind == b.kind && a.p == b.p; }

Value value_abs(int64_t n) {
    Value v;
    v.abs = n;
    v.terms = NULL;
    v.nterms = 0;
    v.defined = 1;
    return v;
}

int value_is_abs(const Value *v) { return v->nterms == 0; }

int value_simple(const Value *v, Base *b) {
    if (v->nterms == 1 && v->terms[0].coef == 1) {
        if (b) *b = v->terms[0].base;
        return 1;
    }
    return 0;
}

/* add coef*base to the n terms in ts (capacity is sufficient) */
static int add_term(Term *ts, int n, Base base, int coef) {
    int i;
    for (i = 0; i < n; i++) {
        if (base_eq(ts[i].base, base)) {
            ts[i].coef += coef;
            if (ts[i].coef == 0) {
                memmove(ts + i, ts + i + 1, (size_t)(n - i - 1) * sizeof *ts);
                return n - 1;
            }
            return n;
        }
    }
    ts[n].base = base;
    ts[n].coef = coef;
    return n + 1;
}

Value value_add(Value a, Value b, int sign) {
    Value r;
    int i, n;
    Term *ts;
    r.abs = a.abs + (int64_t)sign * b.abs;
    r.defined = a.defined && b.defined;
    ts = xmalloc((size_t)(a.nterms + b.nterms + 1) * sizeof *ts);
    n = a.nterms;
    if (n) memcpy(ts, a.terms, (size_t)n * sizeof *ts);
    for (i = 0; i < b.nterms; i++) n = add_term(ts, n, b.terms[i].base, sign * b.terms[i].coef);
    r.terms = n ? ts : NULL;
    r.nterms = n;
    return r;
}

Value value_neg(Value v) {
    Value r = v;
    int i;
    r.abs = -v.abs;
    if (v.nterms) {
        r.terms = xmalloc((size_t)v.nterms * sizeof(Term));
        for (i = 0; i < v.nterms; i++) {
            r.terms[i].base = v.terms[i].base;
            r.terms[i].coef = -v.terms[i].coef;
        }
    }
    return r;
}

/* ---- parser ------------------------------------------------------------- */

typedef struct {
    const char *s;
    size_t len, pos;
    Stmt *st;
    int base;
    Reporter *rep;
    int err;
} EP;

static Expr *mk(ExprKind k, Span sp) {
    Expr *e = xcalloc(sizeof *e);
    e->kind = k;
    e->sp = sp;
    return e;
}

static Span ep_span(EP *p, size_t from) {
    return stmt_operand_span(p->st, p->base + (int)from, (int)(p->pos - from));
}

static Expr *ep_fail(EP *p, const char *code, size_t at, size_t n, const char *fmt, ...) {
    if (!p->err) {
        va_list ap;
        char *msg;
        va_start(ap, fmt);
        msg = xvasprintf(fmt, ap);
        va_end(ap);
        rep_errorf(p->rep, code, stmt_operand_span(p->st, p->base + (int)at, (int)n), "%s", msg);
    }
    p->err = 1;
    return NULL;
}

static void ep_skip(EP *p) {
    while (p->pos < p->len && is_blank((unsigned char)p->s[p->pos])) p->pos++;
}

static Expr *parse_add(EP *p);

static Expr *bin(EP *p, char op, size_t start, Expr *l, Expr *r) {
    Expr *e = mk(EX_BIN, ep_span(p, start));
    e->op = op;
    e->l = l;
    e->r = r;
    return e;
}

static Expr *parse_primary(EP *p);

static Expr *parse_unary(EP *p) {
    ep_skip(p);
    if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) {
        char op = p->s[p->pos];
        size_t start = p->pos;
        Expr *x, *e;
        p->pos++;
        x = parse_unary(p);
        if (p->err) return NULL;
        e = mk(EX_UN, ep_span(p, start));
        e->op = op;
        e->l = x;
        return e;
    }
    return parse_primary(p);
}

static Expr *parse_mul(EP *p) {
    Expr *l = parse_unary(p);
    while (!p->err) {
        ep_skip(p);
        if (p->pos < p->len && (p->s[p->pos] == '*' || p->s[p->pos] == '/')) {
            char op = p->s[p->pos];
            size_t start = p->pos;
            Expr *r;
            p->pos++;
            r = parse_unary(p);
            if (p->err) return NULL;
            l = bin(p, op, start, l, r);
            continue;
        }
        break;
    }
    return l;
}

static Expr *parse_add(EP *p) {
    Expr *l = parse_mul(p);
    while (!p->err) {
        ep_skip(p);
        if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) {
            char op = p->s[p->pos];
            size_t start = p->pos;
            Expr *r;
            p->pos++;
            r = parse_mul(p);
            if (p->err) return NULL;
            l = bin(p, op, start, l, r);
            continue;
        }
        break;
    }
    return l;
}

/* s[pos] is a double quote: the quoted symbol, decoded; NULL after an error */
static char *quoted_symbol(EP *p) {
    size_t start = p->pos;
    long j = close_dquote(p->s, start);
    const char *why = NULL;
    char *nm;
    if (j < 0 || (size_t)j >= p->len) {
        ep_fail(p, "ZA0004", start, p->len - start, "the quoted symbol is not closed");
        return NULL;
    }
    nm = sym_decode(p->s + start + 1, (size_t)j - start - 1, &why);
    if (!nm) {
        ep_fail(p, "ZA0004", start, (size_t)j + 1 - start, "invalid quoted symbol: %s", why);
        return NULL;
    }
    p->pos = (size_t)j + 1;
    return nm;
}

/* X'..', B'..', C'..' self-defining terms and attribute references
 * (L'SYM) after the leading word has been scanned; s[pos] is the quote. */
static Expr *parse_quoted(EP *p, size_t start, const char *word) {
    char u = (char)za_toupper((unsigned char)word[0]);
    size_t wl = strlen(word);
    Buf body = {0};
    int closed = 0;
    int64_t v = 0;
    Expr *e;
    if (wl == 1 && strchr("LTSIDOKN", u) && p->pos + 1 < p->len &&
        (is_sym_start((unsigned char)p->s[p->pos + 1]) || p->s[p->pos + 1] == '*' ||
         p->s[p->pos + 1] == '"')) {
        p->pos++; /* the quote */
        if (p->s[p->pos] == '*') {
            p->pos++;
            e = mk(EX_ATTR, ep_span(p, start));
            e->attr = u;
            e->loc = 1;
            e->name = "";
            return e;
        } else if (p->s[p->pos] == '"') {
            char *nm = quoted_symbol(p);
            if (!nm) return NULL;
            e = mk(EX_ATTR, ep_span(p, start));
            e->attr = u;
            e->name = nm;
            return e;
        } else {
            size_t ns = p->pos;
            while (p->pos < p->len && is_sym_char((unsigned char)p->s[p->pos])) p->pos++;
            e = mk(EX_ATTR, ep_span(p, start));
            e->attr = u;
            e->name = xstrndup(p->s + ns, p->pos - ns);
            return e;
        }
    }
    if (wl != 1 || (u != 'X' && u != 'B' && u != 'C'))
        return ep_fail(p, "ZA1006", start, p->pos - start + 1,
                       "\"%s'\" is not a self-defining term or attribute", word);
    p->pos++;
    while (p->pos < p->len) {
        if (p->s[p->pos] == '\'') {
            if (p->pos + 1 < p->len && p->s[p->pos + 1] == '\'') {
                buf_putc(&body, '\'');
                p->pos += 2;
                continue;
            }
            p->pos++;
            closed = 1;
            break;
        }
        buf_putc(&body, p->s[p->pos]);
        p->pos++;
    }
    if (!closed)
        return ep_fail(p, "ZA0002", start, p->len - start, "unterminated quoted string");
    {
        const char *b = buf_cstr(&body);
        size_t n = body.len, i;
        if (u == 'X') {
            if (n == 0 || n > 16)
                return ep_fail(p, "ZA1007", start, p->pos - start,
                               "hexadecimal term must have 1 to 16 digits");
            for (i = 0; i < n; i++) {
                if (!za_isxdigit((unsigned char)b[i]))
                    return ep_fail(p, "ZA1007", start, p->pos - start,
                                   "invalid digits in %c'...' term", u);
                v = (int64_t)(((uint64_t)v << 4) |
                              (uint64_t)(za_isdigit((unsigned char)b[i])
                                             ? b[i] - '0'
                                             : za_toupper((unsigned char)b[i]) - 'A' + 10));
            }
        } else if (u == 'B') {
            if (n == 0 || n > 64)
                return ep_fail(p, "ZA1007", start, p->pos - start,
                               "binary term must have 1 to 64 digits");
            for (i = 0; i < n; i++) {
                if (b[i] != '0' && b[i] != '1')
                    return ep_fail(p, "ZA1007", start, p->pos - start,
                                   "invalid digits in %c'...' term", u);
                v = (int64_t)(((uint64_t)v << 1) | (uint64_t)(b[i] - '0'));
            }
        } else {
            size_t el;
            unsigned char *eb;
            if (n == 0 || n > 4)
                return ep_fail(p, "ZA1007", start, p->pos - start,
                               "character term must have 1 to 4 characters");
            eb = ebcdic_from_source(b, n, &el);
            for (i = 0; i < el; i++) v = (int64_t)(((uint64_t)v << 8) | eb[i]);
            free(eb);
        }
    }
    e = mk(EX_NUM, ep_span(p, start));
    e->num = v;
    return e;
}

static Expr *parse_primary(EP *p) {
    size_t start;
    int c;
    Expr *e;
    ep_skip(p);
    if (p->pos >= p->len) return ep_fail(p, "ZA1001", p->pos, 1, "expression expected here");
    start = p->pos;
    c = (unsigned char)p->s[p->pos];
    if (c == '(') {
        p->pos++;
        e = parse_add(p);
        if (p->err) return NULL;
        ep_skip(p);
        if (p->pos >= p->len || p->s[p->pos] != ')')
            return ep_fail(p, "ZA1003", start, 1, "unbalanced parenthesis");
        p->pos++;
        return e;
    }
    if (c == '*') {
        p->pos++;
        return mk(EX_LOC, ep_span(p, start));
    }
    if (za_isdigit(c)) {
        int64_t n = 0;
        int over = 0;
        while (p->pos < p->len && za_isdigit((unsigned char)p->s[p->pos])) {
            int d = p->s[p->pos] - '0';
            if (n > (INT64_MAX - d) / 10)
                over = 1;
            else
                n = n * 10 + d;
            p->pos++;
        }
        if (over)
            return ep_fail(p, "ZA1004", start, p->pos - start, "number %.*s is too large",
                           (int)(p->pos - start), p->s + start);
        e = mk(EX_NUM, ep_span(p, start));
        e->num = n;
        return e;
    }
    if (c == '"') {
        char *nm = quoted_symbol(p);
        if (!nm) return NULL;
        e = mk(EX_SYM, ep_span(p, start));
        e->name = nm;
        return e;
    }
    if (is_sym_start(c)) {
        char *word;
        while (p->pos < p->len && is_sym_char((unsigned char)p->s[p->pos])) p->pos++;
        word = xstrndup(p->s + start, p->pos - start);
        if (p->pos < p->len && p->s[p->pos] == '\'') return parse_quoted(p, start, word);
        e = mk(EX_SYM, ep_span(p, start));
        e->name = word;
        return e;
    }
    return ep_fail(p, "ZA1005", p->pos, 1, "unexpected character \"%c\" in expression", c);
}

Expr *parse_expr(Stmt *st, const char *text, int off, Reporter *rep) {
    EP p;
    Expr *e;
    memset(&p, 0, sizeof p);
    p.s = text;
    p.len = strlen(text);
    p.st = st;
    p.base = off;
    p.rep = rep;
    ep_skip(&p);
    if (p.pos >= p.len) {
        rep_errorf(rep, "ZA1001", stmt_operand_span(st, off, 1), "expression expected");
        return NULL;
    }
    e = parse_add(&p);
    if (p.err) return NULL;
    ep_skip(&p);
    if (p.pos < p.len) {
        rep_errorf(rep, "ZA1002", stmt_operand_span(st, off + (int)p.pos, (int)(p.len - p.pos)),
                   "unexpected \"%s\" after expression", p.s + p.pos);
        return NULL;
    }
    return e;
}

char *expr_describe(const Expr *e) {
    switch (e->kind) {
    case EX_NUM:
        return xasprintf("%lld", (long long)e->num);
    case EX_SYM:
        return xstrdup(e->name);
    case EX_LOC:
        return xstrdup("*");
    case EX_ATTR:
        return xasprintf("%c'%s", e->attr, e->name);
    default:
        break;
    }
    return xstrdup("expression");
}
