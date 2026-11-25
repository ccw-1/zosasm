/* macro_expr.c - lexer, parser and evaluator of macro expressions (MACRO.md 5). */
#include <stdlib.h>
#include <string.h>

#include "macro_int.h"
#include "ebcdic.h"

#define MAX_PARSE_DEPTH 200
#define MAX_EVAL_DEPTH 200

/* ---- lexer ---------------------------------------------------------------- */

Diag *ps_error(Parser *ps, const char *code, const char *fmt, ...) {
    va_list ap;
    char *m;
    Diag *d;
    va_start(ap, fmt);
    m = xvasprintf(fmt, ap);
    va_end(ap);
    ps->err = 1;
    d = rep_errorf(ms_rep(ps->ms), code, span_make(ps->file, ps->line, ps->col0 + (int)ps->tstart, 1),
                   "%s", m);
    return d;
}

void ps_init(Parser *ps, MacSys *ms, const char *file, int line, int col0, const char *src) {
    memset(ps, 0, sizeof *ps);
    ps->ms = ms;
    ps->file = file;
    ps->line = line;
    ps->col0 = col0;
    ps->src = src;
    ps_next(ps);
}


static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void ps_next(Parser *ps) {
    const char *s = ps->src;
    size_t p = ps->p;
    int c;
    while (s[p] && is_blank((unsigned char)s[p])) p++;
    ps->tstart = p;
    ps->ts = NULL;
    c = (unsigned char)s[p];
    if (c == '/' && s[p + 1] == '/') c = 0; /* a macro comment ends the text */
    if (!c) {
        ps->tk = TK_EOF;
        ps->p = p;
        return;
    }
    if (za_isdigit(c)) {
        size_t q = p;
        char *t;
        int64_t v;
        if (c == '0' && (s[p + 1] == 'x' || s[p + 1] == 'X' || s[p + 1] == 'b' || s[p + 1] == 'B'))
            q = p + 2;
        while (za_is_idc((unsigned char)s[q])) q++;
        t = xstrndup(s + p, q - p);
        if (!v_parse_int(t, &v)) {
            ps->tstart = p;
            ps_error(ps, "ZA4100", "\"%s\" is not a number", t);
            v = 0;
        }
        ps->tk = TK_INT;
        ps->tv = v;
        ps->p = q;
        return;
    }
    if (za_isalpha(c) || c == '_') {
        size_t q = p;
        int t0;
        while (za_is_idc((unsigned char)s[q])) q++;
        t0 = za_toupper(c);
        if (q - p == 1 && (t0 == 'X' || t0 == 'B' || t0 == 'C') && s[q] == '\'') {
            size_t e = q + 1;
            char *t;
            int64_t v;
            for (;; e++) {
                if (!s[e]) break;
                if (s[e] == '\'') {
                    if (s[e + 1] == '\'')
                        e++;
                    else
                        break;
                }
            }
            if (!s[e]) {
                ps_error(ps, "ZA4100", "unterminated %c'...' constant", t0);
                ps->tk = TK_INT;
                ps->tv = 0;
                ps->p = e;
                return;
            }
            t = xstrndup(s + p, e + 1 - p);
            if (!v_parse_int(t, &v)) {
                ps_error(ps, "ZA4100", "%s is not a valid self-defining term", t);
                v = 0;
            }
            ps->tk = TK_INT;
            ps->tv = v;
            ps->p = e + 1;
            return;
        }
        if (q - p == 3 && za_strncmpi(s + p, "sys", 3) == 0 && s[q] == '.' && za_is_idc((unsigned char)s[q + 1])) {
            q++;
            while (za_is_idc((unsigned char)s[q])) q++;
        }
        ps->tk = TK_ID;
        ps->ts = za_strlower_own(xstrndup(s + p, q - p));
        ps->p = q;
        return;
    }
    if (c == '"') {
        Buf b = {0};
        size_t q = p + 1;
        for (;; q++) {
            if (!s[q]) {
                ps_error(ps, "ZA4100", "unterminated string");
                break;
            }
            if (s[q] == '"') {
                q++;
                break;
            }
            if (s[q] == '\\' && s[q + 1]) {
                q++;
                switch (s[q]) {
                case 'n':
                    buf_putc(&b, '\n');
                    break;
                case 't':
                    buf_putc(&b, '\t');
                    break;
                case 'x':
                    if (hexv((unsigned char)s[q + 1]) >= 0 && hexv((unsigned char)s[q + 2]) >= 0) {
                        int v = hexv((unsigned char)s[q + 1]) * 16 + hexv((unsigned char)s[q + 2]);
                        if (v == 0)
                            ps_error(ps, "ZA4100", "\\x00 is not allowed in a string");
                        else
                            buf_putc(&b, v);
                        q += 2;
                    } else {
                        ps_error(ps, "ZA4100", "\\x needs two hexadecimal digits");
                    }
                    break;
                case '"':
                case '\\':
                case '\'':
                    buf_putc(&b, s[q]);
                    break;
                default:
                    ps_error(ps, "ZA4100", "unknown escape \\%c in a string", s[q]);
                    break;
                }
                continue;
            }
            buf_putc(&b, s[q]);
        }
        ps->tk = TK_STR;
        ps->ts = xstrdup(buf_cstr(&b));
        ps->p = q;
        return;
    }
    {
        static const struct {
            const char *t;
            int tk;
        } ops[] = {{"<<=", TK_SHLEQ}, {">>=", TK_SHREQ}, {"==", TK_EQ},    {"!=", TK_NE},
                   {"<=", TK_LE},     {">=", TK_GE},     {"<<", TK_SHL},   {">>", TK_SHR},
                   {"&&", TK_LAND},   {"||", TK_LOR},    {"++", TK_INC},   {"--", TK_DEC},
                   {"+=", TK_ADDEQ},  {"-=", TK_SUBEQ},  {"*=", TK_MULEQ}, {"/=", TK_DIVEQ},
                   {"%=", TK_MODEQ},  {"&=", TK_ANDEQ},  {"|=", TK_OREQ},  {"^=", TK_XOREQ}};
        size_t i;
        for (i = 0; i < sizeof ops / sizeof ops[0]; i++) {
            size_t l = strlen(ops[i].t);
            if (!strncmp(s + p, ops[i].t, l)) {
                ps->tk = ops[i].tk;
                ps->p = p + l;
                return;
            }
        }
    }
    ps->tk = c;
    ps->p = p + 1;
}

/* ---- parser ---------------------------------------------------------------- */

static MExpr *mk(XKind k) {
    MExpr *e = xcalloc(sizeof *e);
    e->k = k;
    return e;
}

static MExpr *parse_cond(Parser *ps);

static MExpr *parse_primary(Parser *ps) {
    MExpr *e;
    switch (ps->tk) {
    case TK_INT:
        e = mk(X_INT);
        e->i = ps->tv;
        ps_next(ps);
        return e;
    case TK_STR:
        e = mk(X_STR);
        e->s = ps->ts;
        ps_next(ps);
        return e;
    case '(':
        ps_next(ps);
        if (++ps->depth > MAX_PARSE_DEPTH) {
            if (!ps->err) ps_error(ps, "ZA4100", "expression nested too deeply");
            return mk(X_INT);
        }
        e = parse_cond(ps);
        ps->depth--;
        if (ps->tk != ')') {
            if (!ps->err) ps_error(ps, "ZA4100", "missing ')'");
        } else
            ps_next(ps);
        return e;
    case TK_ID: {
        char *name = ps->ts;
        ps_next(ps);
        if (!strcmp(name, "true") || !strcmp(name, "false")) {
            e = mk(X_INT);
            e->i = name[0] == 't';
            free(name);
            return e;
        }
        if (ps->tk == '(') {
            const Fn *fn = fn_find(name);
            Vec(MExpr *) args = {0};
            e = mk(X_CALL);
            e->s = name;
            if (++ps->depth > MAX_PARSE_DEPTH) {
                ps_error(ps, "ZA4100", "expression nested too deeply");
                ps->depth--;
                return e;
            }
            ps_next(ps);
            if (ps->tk != ')') {
                for (;;) {
                    MExpr *a;
                    if (ps->depth > MAX_PARSE_DEPTH) {
                        ps_error(ps, "ZA4100", "expression nested too deeply");
                        break;
                    }
                    a = parse_cond(ps);
                    vec_push(&args, a);
                    if (ps->err) break;
                    if (ps->tk == ',') {
                        ps_next(ps);
                        continue;
                    }
                    break;
                }
            }
            if (ps->tk != ')')
                ps_error(ps, "ZA4100", "missing ')' after the arguments of %s", name);
            else
                ps_next(ps);
            ps->depth--;
            e->args = args.v;
            e->nargs = (int)args.n;
            if (!fn) {
                ps_error(ps, "ZA4111", "unknown function \"%s\"", name);
            } else if (e->nargs < fn->min || (fn->max >= 0 && e->nargs > fn->max)) {
                ps_error(ps, "ZA4111", "%s takes %d%s argument%s, not %d", name, fn->min,
                         fn->max < 0 ? " or more" : (fn->max != fn->min ? "..." : ""),
                         fn->min == 1 && fn->max == 1 ? "" : "s", e->nargs);
            }
            return e;
        }
        if (!strncmp(name, "sys.", 4)) {
            e = mk(X_SYS);
            e->s = xstrdup(name + 4);
            free(name);
        } else if (is_builtin_name(name)) {
            e = mk(X_SYS);
            e->s = name;
        } else {
            e = mk(X_VAR);
            e->s = name;
        }
        return e;
    }
    default:
        ps_error(ps, "ZA4100", "an expression is expected here");
        return mk(X_INT);
    }
}

static MExpr *parse_postfix(Parser *ps) {
    MExpr *e = parse_primary(ps);
    while (ps->tk == '[') {
        MExpr *x = mk(X_INDEX);
        ps_next(ps);
        x->a = e;
        if (++ps->depth > MAX_PARSE_DEPTH) {
            if (!ps->err) ps_error(ps, "ZA4100", "expression nested too deeply");
            return e;
        }
        x->b = parse_cond(ps);
        ps->depth--;
        if (ps->tk != ']') {
            if (!ps->err) ps_error(ps, "ZA4100", "missing ']'");
        } else
            ps_next(ps);
        e = x;
        if (ps->err) break;
    }
    return e;
}

static MExpr *parse_unary(Parser *ps) {
    if (ps->tk == '-' || ps->tk == '!' || ps->tk == '~' || ps->tk == '+') {
        MExpr *e = mk(X_UN);
        e->op = ps->tk;
        ps_next(ps);
        if (++ps->depth > MAX_PARSE_DEPTH) {
            ps_error(ps, "ZA4100", "expression nested too deeply");
            return mk(X_INT);
        }
        e->a = parse_unary(ps);
        ps->depth--;
        return e;
    }
    return parse_postfix(ps);
}

static int binprec(const Parser *ps) {
    switch (ps->tk) {
    case TK_LOR:
        return 1;
    case TK_LAND:
        return 2;
    case '|':
        return 3;
    case '^':
        return 4;
    case '&':
        return 5;
    case TK_EQ:
    case TK_NE:
        return 6;
    case '<':
    case '>':
    case TK_LE:
    case TK_GE:
        return 7;
    case TK_ID:
        return !strcmp(ps->ts, "in") ? 7 : 0;
    case TK_SHL:
    case TK_SHR:
        return 8;
    case '+':
    case '-':
        return 9;
    case '*':
    case '/':
    case '%':
        return 10;
    }
    return 0;
}

static MExpr *parse_bin(Parser *ps, int minprec) {
    MExpr *l = parse_unary(ps);
    for (;;) {
        int pr = binprec(ps), op = ps->tk;
        MExpr *e, *r;
        if (pr == 0 || pr < minprec || ps->err) return l;
        if (op == TK_ID) op = 'I';
        if (++ps->depth > MAX_PARSE_DEPTH) {
            ps_error(ps, "ZA4100", "expression nested too deeply");
            return l;
        }
        ps_next(ps);
        r = parse_bin(ps, pr + 1);
        ps->depth--;
        e = mk(op == TK_LOR ? X_LOR : op == TK_LAND ? X_LAND : op == 'I' ? X_IN : X_BIN);
        e->op = op;
        e->a = l;
        e->b = r;
        l = e;
    }
}

static MExpr *parse_cond(Parser *ps) {
    MExpr *c = parse_bin(ps, 1);
    if (ps->tk == '?' && !ps->err) {
        MExpr *e = mk(X_COND);
        ps_next(ps);
        if (++ps->depth > MAX_PARSE_DEPTH) {
            ps_error(ps, "ZA4100", "expression nested too deeply");
            return c;
        }
        e->a = c;
        e->b = parse_cond(ps);
        if (ps->tk != ':') {
            ps_error(ps, "ZA4100", "missing ':' in ?:");
        } else {
            ps_next(ps);
            e->c = parse_cond(ps);
        }
        ps->depth--;
        return e;
    }
    return c;
}

MExpr *ps_expr(Parser *ps) { return parse_cond(ps); }

MExpr *ps_cond_paren(Parser *ps) {
    MExpr *e;
    if (ps->tk != '(') {
        ps_error(ps, "ZA4100", "the condition must be in parentheses");
        return mk(X_INT);
    }
    ps_next(ps);
    e = parse_cond(ps);
    if (ps->tk != ')')
        ps_error(ps, "ZA4100", "missing ')'");
    else
        ps_next(ps);
    return e;
}

/* ---- evaluation --------------------------------------------------------------- */

MVal *var_find(Frame *f, const char *name) {
    size_t i;
    for (i = f->vars.n; i > 0; i--)
        if (!strcmp(f->vars.v[i - 1].name, name)) return f->vars.v[i - 1].slot;
    return NULL;
}

static int to_int(Ctx *c, MVal v, int64_t *out) {
    if (v.t == V_INT) {
        *out = v.i;
        return 1;
    }
    if (v.t == V_STR && v_parse_int(v.s, out)) return 1;
    ma_err(c, "ZA4112", "\"%s\" is not a number", v_text(v));
    *out = 0;
    return 0;
}

/* HLASM character relation: a shorter string is the lesser; equal lengths compare in the
 * EBCDIC order. */
static int cmp_text(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b), i;
    if (la != lb) return la < lb ? -1 : 1;
    for (i = 0; i < la; i++) {
        int x = ZA_E(a[i]), y = ZA_E(b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

static int rel_ok(int op, int cmp) {
    switch (op) {
    case TK_EQ:
        return cmp == 0;
    case TK_NE:
        return cmp != 0;
    case '<':
        return cmp < 0;
    case '>':
        return cmp > 0;
    case TK_LE:
        return cmp <= 0;
    case TK_GE:
        return cmp >= 0;
    }
    return 0;
}

MVal ex_binop(Ctx *c, int op, MVal a, MVal b) {
    int64_t x, y;
    uint64_t ux, uy;
    switch (op) {
    case TK_EQ:
    case TK_NE:
    case '<':
    case '>':
    case TK_LE:
    case TK_GE: {
        int cmp;
        if (a.t == V_INT && b.t == V_INT) {
            cmp = a.i < b.i ? -1 : a.i > b.i;
        } else if (a.t == V_INT || b.t == V_INT) {
            MVal o = a.t == V_INT ? b : a;
            int64_t on;
            if (!v_parse_int(v_text(o), &on)) {
                if (op == TK_EQ) return v_int(0);
                if (op == TK_NE) return v_int(1);
                ma_err(c, "ZA4112", "\"%s\" is not a number", v_text(o));
                return v_int(0);
            }
            x = a.t == V_INT ? a.i : on;
            y = b.t == V_INT ? b.i : on;
            cmp = x < y ? -1 : x > y;
        } else {
            cmp = cmp_text(v_text(a), v_text(b));
            if (op == TK_EQ || op == TK_NE) cmp = strcmp(v_text(a), v_text(b)) != 0;
        }
        return v_int(rel_ok(op, cmp));
    }
    case '+':
        if (a.t == V_INT || b.t == V_INT) {
            /* an int and a numeric string add; anything else concatenates */
            int64_t px = 0, py = 0;
            int ok = a.t == V_INT ? (px = a.i, 1) : (a.t == V_STR && v_parse_int(a.s, &px));
            ok = ok && (b.t == V_INT ? (py = b.i, 1) : (b.t == V_STR && v_parse_int(b.s, &py)));
            if (ok) return v_int((int64_t)((uint64_t)px + (uint64_t)py));
        }
        {
            const char *sa = v_text(a), *sb = v_text(b);
            size_t la = strlen(sa), lb = strlen(sb);
            if (la > MA_MAX_STR || lb > MA_MAX_STR - la) {
                ma_err(c, "ZA4112", "the concatenated string is too large");
                return v_str(xstrdup(""));
            }
            return v_str(xasprintf("%s%s", sa, sb));
        }
    }
    to_int(c, a, &x);
    to_int(c, b, &y);
    ux = (uint64_t)x;
    uy = (uint64_t)y;
    switch (op) {
    case '-':
        return v_int((int64_t)(ux - uy));
    case '*':
        return v_int((int64_t)(ux * uy));
    case '/':
    case '%':
        if (y == 0) {
            ma_err(c, "ZA4112", "division by zero");
            return v_int(0);
        }
        if (y == -1) return v_int(op == '/' ? (int64_t)(0 - ux) : 0);
        return v_int(op == '/' ? x / y : x % y);
    case '&':
        return v_int((int64_t)(ux & uy));
    case '|':
        return v_int((int64_t)(ux | uy));
    case '^':
        return v_int((int64_t)(ux ^ uy));
    case TK_SHL:
        return v_int(y < 0 || y > 63 ? 0 : (int64_t)(ux << y));
    case TK_SHR:
        return v_int(y < 0 || y > 63 ? (x < 0 ? -1 : 0) : (x >> y));
    }
    return v_int(0);
}

static MVal at_index(Ctx *c, MVal base, MVal idx) {
    const MVal *it;
    size_t n;
    int64_t i;
    if (!to_int(c, idx, &i)) return v_str("");
    if (i < 0) {
        ma_err(c, "ZA4112", "the index %lld is negative", (long long)i);
        return v_str("");
    }
    n = v_items(base, &it);
    if ((uint64_t)i >= n) return v_str("");
    return it[i];
}

static MVal ex_defined_arg(Ctx *c, const MExpr *a) {
    return v_int(ma_defined(c, a->s));
}

static MVal ex_eval_depth(Ctx *c, const MExpr *e, int depth) {
    if (depth > MAX_PARSE_DEPTH) {
        ma_err(c, "ZA4100", "expression nested too deeply");
        return v_int(0);
    }
    switch (e->k) {
    case X_INT:
        return v_int(e->i);
    case X_STR:
        return v_str(e->s);
    case X_VAR: {
        MVal *s = var_find(c->f, e->s);
        if (!s) {
            Diag *d = e->i ? ma_err(c, "ZA4110", "&%s is not a variable", e->s)
                           : ma_err(c, "ZA4111", "unknown identifier \"%s\" in a macro expression", e->s);
            if (d) diag_help(d, "declare it with #let, or name a parameter or #global");
            return v_str("");
        }
        return *s;
    }
    case X_SYS: {
        int ok = 0;
        MVal v = sys_value(c, e->s, &ok);
        if (!ok) ma_err(c, "ZA4111", "unknown built-in \"sys.%s\"", e->s);
        return v;
    }
    case X_UN: {
        MVal a = ex_eval_depth(c, e->a, depth + 1);
        int64_t x;
        if (e->op == '!') return v_int(!v_truth(a));
        to_int(c, a, &x);
        if (e->op == '-') return v_int((int64_t)(0 - (uint64_t)x));
        if (e->op == '~') return v_int(~x);
        return v_int(x);
    }
    case X_BIN:
        return ex_binop(c, e->op, ex_eval_depth(c, e->a, depth + 1), ex_eval_depth(c, e->b, depth + 1));
    case X_LAND:
        return v_int(v_truth(ex_eval_depth(c, e->a, depth + 1)) && v_truth(ex_eval_depth(c, e->b, depth + 1)));
    case X_LOR:
        return v_int(v_truth(ex_eval_depth(c, e->a, depth + 1)) || v_truth(ex_eval_depth(c, e->b, depth + 1)));
    case X_COND:
        return v_truth(ex_eval_depth(c, e->a, depth + 1)) ? ex_eval_depth(c, e->b, depth + 1)
                                                          : ex_eval_depth(c, e->c, depth + 1);
    case X_INDEX: {
        MVal b = ex_eval_depth(c, e->a, depth + 1);
        return at_index(c, b, ex_eval_depth(c, e->b, depth + 1));
    }
    case X_IN: {
        MVal x = ex_eval_depth(c, e->a, depth + 1), l = ex_eval_depth(c, e->b, depth + 1);
        const MVal *it;
        size_t n = v_items(l, &it), i;
        for (i = 0; i < n; i++)
            if (v_truth(ex_binop(c, TK_EQ, x, it[i]))) return v_int(1);
        return v_int(0);
    }
    case X_CALL: {
        const Fn *fn = fn_find(e->s);
        MVal *a;
        int i;
        MVal r;
        if (!fn) return v_str("");
        if (!strcmp(e->s, "defined") && e->nargs == 1 && e->args[0]->k == X_VAR)
            return ex_defined_arg(c, e->args[0]);
        if (depth + 1 > MAX_EVAL_DEPTH) { ma_err(c, "ZA4100", "expression nested too deeply"); return v_int(0); }
        a = xcalloc(((size_t)e->nargs + 1) * sizeof *a);
        for (i = 0; i < e->nargs; i++) a[i] = ex_eval_depth(c, e->args[i], depth + 1);
        r = fn->fn(c, a, e->nargs);
        free(a);
        return r;
    }
    }
    return v_str("");
}

MVal ex_eval(Ctx *c, const MExpr *e) { return ex_eval_depth(c, e, 0); }
