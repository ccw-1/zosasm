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

static Arena *value_arena;

void value_set_arena(Arena *a) { value_arena = a; }

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
    r.abs = sign < 0 ? (int64_t)((uint64_t)a.abs - (uint64_t)b.abs)
                     : (int64_t)((uint64_t)a.abs + (uint64_t)b.abs);
    r.defined = a.defined && b.defined;
    ts = arena_alloc(value_arena, (size_t)(a.nterms + b.nterms + 1) * sizeof *ts);
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
    r.abs = (int64_t)(0 - (uint64_t)v.abs);
    if (v.nterms) {
        r.terms = arena_alloc(value_arena, (size_t)v.nterms * sizeof(Term));
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
    int minus; /* the term follows a unary minus: 2147483648 is allowed */
    int depth;
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
static Expr *parse_or(EP *p);

#ifndef MAX_EXPR_DEPTH
#define MAX_EXPR_DEPTH 200
#endif

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
    if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-' || p->s[p->pos] == '!')) {
        char op = p->s[p->pos];
        size_t start = p->pos;
        Expr *x, *e;
        if (++p->depth > MAX_EXPR_DEPTH)
            return ep_fail(p, "ZA1004", start, 1, "expression nested too deeply");
        p->pos++;
        p->minus = op == '-';
        x = parse_unary(p);
        p->minus = 0;
        p->depth--;
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

/* comparisons, && and || (the result is 1 or 0); only ASSERT and the like need
 * them, but they work in every expression */
static Expr *parse_cmp(EP *p) {
    Expr *l = parse_add(p);
    ep_skip(p);
    if (!p->err && p->pos < p->len) {
        const char *s = p->s + p->pos;
        char op = 0;
        size_t n = 0, start = p->pos;
        if (!strncmp(s, "==", 2)) op = 'e', n = 2;
        else if (!strncmp(s, "!=", 2)) op = 'n', n = 2;
        else if (!strncmp(s, "<=", 2)) op = 'l', n = 2;
        else if (!strncmp(s, ">=", 2)) op = 'g', n = 2;
        else if (s[0] == '<') op = '<', n = 1;
        else if (s[0] == '>') op = '>', n = 1;
        if (op) {
            Expr *r;
            p->pos += n;
            r = parse_add(p);
            if (p->err) return NULL;
            return bin(p, op, start, l, r);
        }
    }
    return l;
}

static Expr *parse_and(EP *p) {
    Expr *l = parse_cmp(p);
    while (!p->err) {
        ep_skip(p);
        if (p->pos + 1 < p->len && p->s[p->pos] == '&' && p->s[p->pos + 1] == '&') {
            size_t start = p->pos;
            Expr *r;
            p->pos += 2;
            r = parse_cmp(p);
            if (p->err) return NULL;
            l = bin(p, '&', start, l, r);
            continue;
        }
        break;
    }
    return l;
}

static Expr *parse_or(EP *p) {
    Expr *l = parse_and(p);
    while (!p->err) {
        ep_skip(p);
        if (p->pos + 1 < p->len && p->s[p->pos] == '|' && p->s[p->pos + 1] == '|') {
            size_t start = p->pos;
            Expr *r;
            p->pos += 2;
            r = parse_and(p);
            if (p->err) return NULL;
            l = bin(p, '|', start, l, r);
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

/* after a symbol: ".NAME" makes it a qualifier (the label of a USING); sets
 * *name to the symbol and *qual to the qualifier (NULL if none) */
static int qualified_tail(EP *p, char *first, char **name, char **qual) {
    *name = first;
    *qual = NULL;
    if (p->pos + 1 < p->len && p->s[p->pos] == '.' &&
        (is_sym_start((unsigned char)p->s[p->pos + 1]) || p->s[p->pos + 1] == '"')) {
        *qual = first;
        p->pos++;
        if (p->s[p->pos] == '"') {
            *name = quoted_symbol(p);
            if (!*name) return 0;
        } else {
            size_t ns = p->pos;
            while (p->pos < p->len && is_sym_char((unsigned char)p->s[p->pos])) p->pos++;
            *name = xstrndup(p->s + ns, p->pos - ns);
        }
    }
    return 1;
}

static Expr *symbol_expr(EP *p, size_t start, char *word) {
    char *name, *qual;
    Expr *e;
    if (!qualified_tail(p, word, &name, &qual)) return NULL;
    e = mk(EX_SYM, ep_span(p, start));
    e->name = name;
    e->qual = qual;
    return e;
}

/* ---- sizeof, offsetof, alignof, countof ---------------------------------- */

static const char *const func_names[] = {"sizeof", "offsetof", "alignof", "countof"};

int expr_is_func_name(const char *s, size_t n) {
    size_t i;
    for (i = 0; i < sizeof func_names / sizeof func_names[0]; i++)
        if (strlen(func_names[i]) == n && !strncmp(func_names[i], s, n)) return 1;
    return 0;
}

/* a plain or quoted symbol as the argument of a function */
static Expr *func_arg(EP *p) {
    size_t start;
    Expr *e;
    ep_skip(p);
    start = p->pos;
    if (p->pos < p->len && p->s[p->pos] == '"') {
        char *nm = quoted_symbol(p);
        if (!nm) return NULL;
        e = mk(EX_SYM, ep_span(p, start));
        e->name = nm;
        return e;
    }
    if (p->pos < p->len && is_sym_start((unsigned char)p->s[p->pos])) {
        while (p->pos < p->len && is_sym_char((unsigned char)p->s[p->pos])) p->pos++;
        e = mk(EX_SYM, ep_span(p, start));
        e->name = xstrndup(p->s + start, p->pos - start);
        return e;
    }
    return ep_fail(p, "ZA1040", p->pos, 1, "a symbol name is expected here");
}

/* the word has been scanned and s[pos] is its '(' */
static Expr *parse_func(EP *p, size_t start, const char *word) {
    Expr *e = mk(EX_FUNC, ep_span(p, start));
    int two = word[0] == 'o';
    e->op = word[0];
    e->name = xstrdup(word);
    p->pos++;
    e->l = func_arg(p);
    if (!e->l) return NULL;
    if (two) {
        ep_skip(p);
        if (p->pos >= p->len || p->s[p->pos] != ',')
            return ep_fail(p, "ZA1040", p->pos, 1,
                           "offsetof takes two arguments: offsetof(SECTION,FIELD)");
        p->pos++;
        e->r = func_arg(p);
        if (!e->r) return NULL;
    }
    ep_skip(p);
    if (p->pos >= p->len || p->s[p->pos] != ')')
        return ep_fail(p, "ZA1040", p->pos, 1, "%s takes %s%s", word, two ? "two symbol names" : "one symbol name",
                       " and a closing parenthesis");
    p->pos++;
    e->sp = ep_span(p, start);
    return e;
}

/* X'..', B'..', C'..' self-defining terms and attribute references
 * (L'SYM) after the leading word has been scanned; s[pos] is the quote. */
static Expr *parse_quoted(EP *p, size_t start, const char *word) {
    char u = (char)za_toupper((unsigned char)word[0]);
    size_t wl = strlen(word);
    Buf body = {0};
    int closed = 0;
    int64_t v = 0;
    char cset = 0;
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
            e->name = xstrdup("");
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
            char *first, *nm, *qual;
            while (p->pos < p->len && is_sym_char((unsigned char)p->s[p->pos])) p->pos++;
            first = xstrndup(p->s + ns, p->pos - ns);
            if (!qualified_tail(p, first, &nm, &qual)) return NULL;
            e = mk(EX_ATTR, ep_span(p, start));
            e->attr = u;
            e->name = nm;
            e->qual = qual;
            return e;
        }
    }
    /* CA'..' (ISO-8859-1) and CE'..' (IBM-1047) whatever the default character set */
    if (wl == 2 && u == 'C' && strchr("AEae", word[1])) cset = (char)za_toupper((unsigned char)word[1]);
    if ((wl != 1 && !cset) || (u != 'X' && u != 'B' && u != 'C'))
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
            if ((uint64_t)v > 0xFFFFFFFFu)
                return ep_fail(p, "ZA1007", start, p->pos - start,
                               "hexadecimal term does not fit 32 bits (HLASM: ASMA146E)");
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
            if ((uint64_t)v > 0xFFFFFFFFu)
                return ep_fail(p, "ZA1007", start, p->pos - start,
                               "binary term does not fit 32 bits (HLASM: ASMA146E)");
        } else {
            size_t el;
            unsigned char *eb;
            if (n == 0 || n > 4)
                return ep_fail(p, "ZA1007", start, p->pos - start,
                               "character term must have 1 to 4 characters");
            if (cset) {
                int saved = za_ascii;
                za_ascii = cset == 'A';
                eb = charset_from_source(b, n, &el);
                za_ascii = saved;
            } else {
                eb = charset_from_source(b, n, &el);
            }
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
    int minus = p->minus;
    p->minus = 0;
    ep_skip(p);
    if (p->pos >= p->len) return ep_fail(p, "ZA1001", p->pos, 1, "expression expected here");
    start = p->pos;
    c = (unsigned char)p->s[p->pos];
    if (c == '(') {
        if (++p->depth > MAX_EXPR_DEPTH)
            return ep_fail(p, "ZA1004", start, 1, "expression nested too deeply");
        p->pos++;
        e = parse_or(p);
        p->depth--;
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
        if (over || n > 2147483647LL + (minus && n == 2147483648LL))
            return ep_fail(p, "ZA1004", start, p->pos - start,
                           "number %.*s is too large (HLASM: decimal terms go up to 2147483647)",
                           (int)(p->pos - start), p->s + start);
        e = mk(EX_NUM, ep_span(p, start));
        e->num = n;
        return e;
    }
    if (c == '"') {
        char *nm = quoted_symbol(p);
        if (!nm) return NULL;
        return symbol_expr(p, start, nm);
    }
    if (is_sym_start(c)) {
        char *word;
        while (p->pos < p->len && is_sym_char((unsigned char)p->s[p->pos])) p->pos++;
        word = xstrndup(p->s + start, p->pos - start);
        if (p->pos < p->len && p->s[p->pos] == '\'') return parse_quoted(p, start, word);
        if (p->pos < p->len && p->s[p->pos] == '(' && expr_is_func_name(word, strlen(word)))
            return parse_func(p, start, word);
        return symbol_expr(p, start, word);
    }
    return ep_fail(p, "ZA1005", p->pos, 1, "unexpected character \"%c\" in expression", c);
}

void expr_free(Expr *e) {
    if (!e) return;
    expr_free(e->l);
    expr_free(e->r);
    free(e->name);
    free(e->qual);
    free(e);
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
    e = parse_or(&p);
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
        if (e->qual) return xasprintf("%s.%s", e->qual, e->name);
        return xstrdup(e->name);
    case EX_LOC:
        return xstrdup("*");
    case EX_ATTR:
        return xasprintf("%c'%s", e->attr, e->name);
    case EX_FUNC:
        if (e->r) return xasprintf("%s(%s,%s)", e->name, e->l->name, e->r->name);
        return xasprintf("%s(%s)", e->name, e->l->name);
    default:
        break;
    }
    return xstrdup("expression");
}
