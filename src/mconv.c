/* mconv.c - HLASM macro definition -> zosasm macro (see MACRO.md section 10).
 *
 * The macro text arrives as logical statements. Conditional assembly statements are
 * translated to macro code lines, model statements to model statements, and
 * AIF/AGO/ANOP/MEXIT to a node sequence (mconv_int.h) that mflow.c writes either as
 * #label/#goto (flat) or as structured blocks.
 *
 * Types: parameters and SETC symbols are strings, SETA and SETB symbols are integers.
 * An arithmetic use of a string is written int(x); a string built from an integer
 * is written str(x), so that "+" always joins strings or adds integers. */
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "mconv_int.h"

typedef enum { T_INT, T_STR } Ty;

enum { P_OR = 1, P_AND, P_EQ, P_REL, P_ADD, P_MUL, P_UN, P_ATOM };
enum { OP_NONE, OP_REL, OP_AND, OP_OR, OP_NOT };

typedef struct E E;
struct E {
    char *txt;
    int prec;
    Ty ty;
    int charctx;        /* a character valued operand (quoted string, attribute, function) */
    int lit;            /* integer literal lv */
    int64_t lv;
    char *bt;           /* txt is bt + k (arithmetic with a constant), or NULL */
    int bprec;
    int64_t k;
    int op;             /* structure, for negation */
    int rel;            /* OP_REL: '=' '!' '<' '>' 'l' (<=) 'g' (>=) */
    E *a, *b;
    char *sv;           /* the value of a string literal */
    int isarr;          /* an array symbol without subscript */
};

typedef struct {
    char *hl;   /* upper case HLASM name, without & */
    char *z;    /* name in the zosasm macro */
    Ty ty;
    int array, param, kw, global;
    int dim;    /* array dimension, or -1 */
    char *def;  /* keyword default text */
    int explicit_decl;
} Var;

typedef struct {
    const MConvOptions *o;
    StrMap vars;
    Vec(Var *) decls; /* LCL/GBL symbols in order of declaration */
    Vec(Var *) params;
    StrMap seqdef, seqref;
    MNodeVec nodes;
    char *why;
    int line;
    char *label_var; /* the name field symbol of the prototype (upper case) or NULL */
    char *name;
    Buf pending;     /* macro code produced for the statement in work */
} MC;

typedef struct {
    MC *m;
    const char *s;
    size_t i;
    int depth;
} XP;

#define MCONV_MAX_DEPTH 1024

static void fail(MC *m, const char *fmt, ...) {
    va_list ap;
    if (m->why) return;
    va_start(ap, fmt);
    m->why = xvasprintf(fmt, ap);
    va_end(ap);
}

static void warn(MC *m, const char *fmt, ...) {
    va_list ap;
    char *t;
    va_start(ap, fmt);
    t = xvasprintf(fmt, ap);
    va_end(ap);
    if (m->o->warn) m->o->warn(m->o->user, m->line, t);
}

/* ---- names ----------------------------------------------------------------- */

static int is_hlc(int c) { return za_isalnum(c) || c == '_' || c == '$' || c == '@' || c == '#'; }

static const char *const RESERVED[] = {"let", "global", "if", "else", "while", "for", "switch", "case",
                                       "default", "break", "continue", "return", "goto", "label",
                                       "emit", "insert", "note", "warning", "error", "macro", "define",
                                       "undef", "include", "pragma", "ifdef", "ifndef", "elif",
                                       "endif", "sys", "true", "false", "in", NULL};

static char *zname(const char *hl) {
    Buf b = {0};
    size_t i;
    char *r;
    int k;
    for (i = 0; hl[i]; i++) {
        int c = za_tolower((unsigned char)hl[i]);
        if (c == '$')
            buf_puts(&b, "_d");
        else if (c == '@')
            buf_puts(&b, "_a");
        else if (c == '#')
            buf_puts(&b, "_h");
        else
            buf_putc(&b, c);
    }
    r = xstrdup(buf_cstr(&b));
    for (k = 0; RESERVED[k]; k++)
        if (!strcmp(r, RESERVED[k])) return xasprintf("%s_", r);
    if (za_isdigit((unsigned char)r[0])) return xasprintf("v%s", r);
    return r;
}

/* ---- string literals ---------------------------------------------------------- */

static char *zstr(const char *s, size_t n) {
    Buf b = {0};
    size_t i;
    buf_putc(&b, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            buf_putc(&b, '\\');
            buf_putc(&b, c);
        } else if (c < 32 || c > 126) {
            buf_printf(&b, "\\x%02X", c);
        } else {
            buf_putc(&b, c);
        }
    }
    buf_putc(&b, '"');
    return xstrdup(buf_cstr(&b));
}

/* ---- expression values -------------------------------------------------------- */

static E e_make(char *txt, int prec, Ty ty) {
    E e;
    memset(&e, 0, sizeof e);
    e.txt = txt;
    e.prec = prec;
    e.ty = ty;
    return e;
}

static E e_lit(int64_t v) {
    E e = e_make(xasprintf("%lld", (long long)v), v < 0 ? P_UN : P_ATOM, T_INT);
    e.lit = 1;
    e.lv = v;
    return e;
}

static E e_slit(const char *s, size_t n) {
    E e = e_make(zstr(s, n), P_ATOM, T_STR);
    e.charctx = 1;
    e.sv = xstrndup(s, n);
    return e;
}

static char *par(E e, int p) { return e.prec < p ? xasprintf("(%s)", e.txt) : e.txt; }

static E as_str(E e) {
    if (e.ty == T_STR) return e;
    if (e.lit) {
        char *t = xasprintf("%lld", (long long)e.lv);
        return e_slit(t, strlen(t));
    }
    return e_make(xasprintf("str(%s)", e.txt), P_ATOM, T_STR);
}

static int numeric_text(const char *s) {
    size_t i = 0;
    if (s[0] == '-') i = 1;
    if (!s[i]) return 0;
    for (; s[i]; i++)
        if (!za_isdigit((unsigned char)s[i])) return 0;
    return 1;
}

static E as_int(E e) {
    if (e.ty == T_INT) return e;
    if (e.sv && numeric_text(e.sv)) return e_lit(strtoll(e.sv, NULL, 10));
    return e_make(xasprintf("int(%s)", e.txt), P_ATOM, T_INT);
}

/* e + k */
static E mkoff(E e, int64_t k) {
    const char *bt;
    int bp;
    int64_t kk;
    E r;
    e = as_int(e);
    if (e.lit) return e_lit(e.lv + k);
    if (e.bt) {
        bt = e.bt;
        bp = e.bprec;
        kk = e.k + k;
    } else {
        bt = e.txt;
        bp = e.prec;
        kk = k;
    }
    if (kk == 0) {
        r = e_make(xstrdup(bt), bp, T_INT);
        return r;
    }
    {
        E b = e_make(xstrdup(bt), bp, T_INT);
        char *l = par(b, P_ADD);
        r = e_make(xasprintf("%s %c %lld", l, kk < 0 ? '-' : '+', (long long)(kk < 0 ? -kk : kk)), P_ADD, T_INT);
        r.bt = xstrdup(bt);
        r.bprec = bp;
        r.k = kk;
    }
    return r;
}

static E arith(char op, E a, E b) {
    E r;
    a = as_int(a);
    b = as_int(b);
    if (a.lit && b.lit) {
        int64_t x = a.lv, y = b.lv;
        switch (op) {
        case '+': return e_lit(x + y);
        case '-': return e_lit(x - y);
        case '*': return e_lit(x * y);
        case '/':
            if (y != 0) return e_lit(x / y);
            break;
        }
    }
    if ((op == '+' || op == '-') && b.lit) return mkoff(a, op == '+' ? b.lv : -b.lv);
    if (op == '+' && a.lit) return mkoff(b, a.lv);
    {
        int p = (op == '+' || op == '-') ? P_ADD : P_MUL;
        r = e_make(xasprintf("%s %c %s", par(a, p), op, par(b, p + 1)), p, T_INT);
    }
    return r;
}

static E negate_arith(E a) {
    a = as_int(a);
    if (a.lit) return e_lit(-a.lv);
    return e_make(xasprintf("-%s", par(a, P_UN)), P_UN, T_INT);
}

static const char *rel_text(int rel) {
    switch (rel) {
    case '=': return "==";
    case '!': return "!=";
    case '<': return "<";
    case '>': return ">";
    case 'l': return "<=";
    default: return ">=";
    }
}

static E mk_rel(int rel, E a, E b) {
    E r = e_make(xasprintf("%s %s %s", par(a, P_ADD), rel_text(rel), par(b, P_ADD)),
                 (rel == '=' || rel == '!') ? P_EQ : P_REL, T_INT);
    r.op = OP_REL;
    r.rel = rel;
    r.a = xmalloc(sizeof(E));
    r.b = xmalloc(sizeof(E));
    *r.a = a;
    *r.b = b;
    return r;
}

static E mk_not(E x);

static E mk_logic(int op, E a, E b) {
    E r = e_make(xasprintf("%s %s %s", par(a, op == OP_AND ? P_AND : P_OR),
                           op == OP_AND ? "&&" : "||", par(b, op == OP_AND ? P_AND + 1 : P_OR + 1)),
                 op == OP_AND ? P_AND : P_OR, T_INT);
    r.op = op;
    r.a = xmalloc(sizeof(E));
    r.b = xmalloc(sizeof(E));
    *r.a = a;
    *r.b = b;
    return r;
}

/* the negation, written readably: a != b, De Morgan, !!x = x */
static E mk_not(E x) {
    E r;
    if (x.op == OP_REL) {
        int nr;
        switch (x.rel) {
        case '=': nr = '!'; break;
        case '!': nr = '='; break;
        case '<': nr = 'g'; break;
        case '>': nr = 'l'; break;
        case 'l': nr = '>'; break;
        default: nr = '<'; break;
        }
        return mk_rel(nr, *x.a, *x.b);
    }
    if (x.op == OP_NOT) return *x.a;
    if (x.op == OP_AND) return mk_logic(OP_OR, mk_not(*x.a), mk_not(*x.b));
    if (x.op == OP_OR) return mk_logic(OP_AND, mk_not(*x.a), mk_not(*x.b));
    if (x.lit) return e_lit(!x.lv);
    r = e_make(xasprintf("!%s", par(x, P_UN)), P_UN, T_INT);
    r.op = OP_NOT;
    r.a = xmalloc(sizeof(E));
    *r.a = x;
    return r;
}

/* ---- variables --------------------------------------------------------------- */

static Var *var_find(MC *m, const char *hl) { return strmap_get(&m->vars, hl); }

static Var *var_new(MC *m, const char *hl, Ty ty, int array, int global, int dim) {
    Var *v = xcalloc(sizeof *v);
    v->hl = xstrdup(hl);
    v->z = zname(hl);
    v->ty = ty;
    v->array = array;
    v->global = global;
    v->dim = dim;
    strmap_put(&m->vars, hl, v);
    return v;
}

static void skipb(XP *p) {
    while (is_blank((unsigned char)p->s[p->i])) p->i++;
}

static E parse_or(XP *p);
static int xp_depth(XP *p) {
    if (p->depth >= MCONV_MAX_DEPTH) {
        fail(p->m, "expression nested too deeply");
        return -1;
    }
    p->depth++;
    return 0;
}
static void xp_undepth(XP *p) { if (p->depth > 0) p->depth--; }
static E parse_add(XP *p);
static E parse_prim(XP *p);

static int hl_start(int c) { return za_isalpha(c) || c == '_' || c == '$' || c == '@' || c == '#'; }

/* "(a,b)" at p->i: the arithmetic expressions, as an array (max 4); sets *star when the last is * */
static int parse_subs(XP *p, E *subs, int max, int *star) {
    long e = match_paren(p->s, p->i);
    XP q;
    int n = 0;
    if (star) *star = 0;
    if (e < 0) {
        fail(p->m, "unbalanced parenthesis in a subscript");
        p->i = strlen(p->s);
        return 0;
    }
    q.m = p->m;
    q.s = xstrndup(p->s + p->i + 1, (size_t)e - p->i - 1);
    q.i = 0;
    q.depth = p->depth;
    for (;;) {
        skipb(&q);
        if (q.s[q.i] == '*' && star && n > 0) {
            *star = 1;
            q.i++;
        } else if (n < max) {
            subs[n++] = parse_add(&q);
        } else {
            fail(p->m, "too many subscripts");
            break;
        }
        skipb(&q);
        if (q.s[q.i] == ',') {
            q.i++;
            continue;
        }
        break;
    }
    if (q.s[q.i]) fail(p->m, "cannot read the subscript \"%s\"", q.s);
    p->i = (size_t)e + 1;
    return n;
}

static E sys_ref(XP *p, const char *hl) {
    MC *m = p->m;
    if (!strcmp(hl, "SYSNDX")) return e_make(xstrdup("sys.ndx"), P_ATOM, T_STR);
    if (!strcmp(hl, "SYSECT")) return e_make(xstrdup("sys.sect"), P_ATOM, T_STR);
    if (!strcmp(hl, "SYSSTYP")) return e_make(xstrdup("sys.sectype"), P_ATOM, T_STR);
    if (!strcmp(hl, "SYSMAC")) return e_make(xstrdup("sys.macro"), P_ATOM, T_STR);
    if (!strcmp(hl, "SYSPARM")) return e_make(xstrdup("sys.parm"), P_ATOM, T_STR);
    if (!strcmp(hl, "SYSLOC")) return e_make(xstrdup("sys.loc"), P_ATOM, T_STR);
    if (!strcmp(hl, "SYSDATE")) return e_make(xstrdup("sys.date"), P_ATOM, T_STR);
    if (!strcmp(hl, "SYSDATC")) return e_make(xstrdup("sys.datc"), P_ATOM, T_STR);
    if (!strcmp(hl, "SYSTIME")) return e_make(xstrdup("sys.time"), P_ATOM, T_STR);
    if (!strcmp(hl, "SYSLIST")) {
        E r = e_make(xstrdup("sys.args"), P_ATOM, T_STR);
        if (p->s[p->i] == '(') {
            E subs[4];
            int n = parse_subs(p, subs, 4, NULL), k;
            Buf b = {0};
            if (n >= 1 && subs[0].lit && subs[0].lv == 0 && n == 1) return e_make(xstrdup("sys.label"), P_ATOM, T_STR);
            buf_puts(&b, "sys.args");
            for (k = 0; k < n; k++) buf_printf(&b, "[%s]", mkoff(subs[k], -1).txt);
            r.txt = xstrdup(buf_cstr(&b));
        } else {
            r.isarr = 1;
        }
        return r;
    }
    fail(m, "the system variable &%s is not known", hl);
    return e_make(xstrdup("\"\""), P_ATOM, T_STR);
}

/* the symbol reference at p->s[p->i] == '&'; consumes the end marker "." */
static E parse_ref(XP *p) {
    MC *m = p->m;
    size_t st = p->i + 1, j = st;
    char *hl;
    Var *v;
    E r;
    while (is_hlc((unsigned char)p->s[j])) j++;
    hl = za_strupper(xstrndup(p->s + st, j - st));
    p->i = j;
    if (m->label_var && !strcmp(hl, m->label_var)) {
        r = e_make(xstrdup("sys.label"), P_ATOM, T_STR);
    } else if (!(v = var_find(m, hl)) && !strncmp(hl, "SYS", 3)) {
        r = sys_ref(p, hl);
    } else {
        if (!v) { /* not declared: HLASM stops only when the statement runs; here it is an empty local */
            v = var_new(m, hl, T_STR, 0, 0, -1);
            vec_push(&m->decls, v);
            warn(m, "&%s is not declared: converted as a local variable that is empty", hl);
        }
        E subs[4];
        int ns = 0, k;
        Buf b = {0};
        if (p->s[p->i] == '(' && (v->array || v->param)) ns = parse_subs(p, subs, 4, NULL);
        buf_puts(&b, v->z);
        for (k = 0; k < ns; k++) buf_printf(&b, "[%s]", mkoff(subs[k], -1).txt);
        r = e_make(xstrdup(buf_cstr(&b)), P_ATOM, ns > 0 && !v->param ? v->ty : (v->param ? T_STR : v->ty));
        if (v->array && ns == 0) r.isarr = 1;
    }
    if (p->s[p->i] == '.') p->i++;
    return r;
}

/* ---- strings -------------------------------------------------------------------- */

static E concat2(E a, E b) {
    E r;
    a = as_str(a);
    b = as_str(b);
    r = e_make(xasprintf("%s + %s", par(a, P_ADD), par(b, P_ADD)), P_ADD, T_STR);
    r.charctx = 1;
    return r;
}

static E parse_string(XP *p) {
    Buf lit = {0};
    Vec(E) pieces = {0};
    E r;
    size_t k;
    p->i++;
    for (;;) {
        char c = p->s[p->i];
        if (!c) {
            fail(p->m, "a quoted string is not closed");
            break;
        }
        if (c == '\'') {
            if (p->s[p->i + 1] == '\'') {
                buf_putc(&lit, '\'');
                p->i += 2;
                continue;
            }
            p->i++;
            break;
        }
        if (c == '&') {
            if (p->s[p->i + 1] == '&') {
                buf_putc(&lit, '&');
                p->i += 2;
                continue;
            }
            if (hl_start((unsigned char)p->s[p->i + 1])) {
                E ref;
                if (lit.len) {
                    vec_push(&pieces, e_slit(lit.p, lit.len));
                    lit.len = 0;
                }
                ref = parse_ref(p);
                vec_push(&pieces, as_str(ref));
                continue;
            }
        }
        buf_putc(&lit, c);
        p->i++;
    }
    if (lit.len || pieces.n == 0) vec_push(&pieces, e_slit(lit.p ? lit.p : "", lit.len));
    r = pieces.v[0];
    for (k = 1; k < pieces.n; k++) r = concat2(r, pieces.v[k]);
    r.charctx = 1;
    return r;
}

static E substr_e(E s, E start, E len, int to_end) {
    E r;
    s = as_str(s);
    start = mkoff(start, -1);
    if (to_end)
        r = e_make(xasprintf("substr(%s, %s)", s.txt, start.txt), P_ATOM, T_STR);
    else
        r = e_make(xasprintf("substr(%s, %s, %s)", s.txt, start.txt, as_int(len).txt), P_ATOM, T_STR);
    r.charctx = 1;
    return r;
}

/* substring and concatenation after a string-like primary */
static E str_tail(XP *p, E e) {
    for (;;) {
        if (p->s[p->i] == '(') {
            E subs[2];
            int star, n = parse_subs(p, subs, 2, &star);
            if (n == 1 && star)
                e = substr_e(e, subs[0], subs[0], 1);
            else if (n == 2)
                e = substr_e(e, subs[0], subs[1], 0);
            else
                fail(p->m, "a substring needs (start,length)");
            continue;
        }
        if (p->s[p->i] == '\'') { /* 'A'(1,1)'B': the strings join */
            E r = parse_prim(p);
            e = concat2(e, r);
            continue;
        }
        if (p->s[p->i] == '.' && (p->s[p->i + 1] == '\'' || p->s[p->i + 1] == '&' || p->s[p->i + 1] == '(' ||
                                  za_isalpha((unsigned char)p->s[p->i + 1]))) {
            E r;
            p->i++;
            r = parse_prim(p);
            e = concat2(e, r);
            r.charctx = 1;
            continue;
        }
        break;
    }
    e.charctx = 1;
    return e;
}

/* ---- expressions ------------------------------------------------------------------ */

static int word_at(XP *p, const char *w) {
    size_t n = strlen(w);
    skipb(p);
    if (za_strncmpi(p->s + p->i, w, n) != 0) return 0;
    if (is_hlc((unsigned char)p->s[p->i + n])) return 0;
    p->i += n;
    return 1;
}

static E dummy_str(void) { return e_make(xstrdup("\"\""), P_ATOM, T_STR); }

static E func_call(XP *p, const char *name) {
    E args[4];
    int n = 0;
    MC *m = p->m;
    E r;
    p->i++; /* ( */
    for (;;) {
        skipb(p);
        if (n >= 4) {
            fail(m, "too many arguments of %s", name);
            break;
        }
        args[n++] = parse_or(p);
        skipb(p);
        if (p->s[p->i] == ',') {
            p->i++;
            continue;
        }
        break;
    }
    if (p->s[p->i] != ')') {
        fail(m, "missing ) after the arguments of %s", name);
        return dummy_str();
    }
    p->i++;
    if (!strcmp(name, "UPPER") && n == 1) {
        r = e_make(xasprintf("upper(%s)", as_str(args[0]).txt), P_ATOM, T_STR);
    } else if (!strcmp(name, "LOWER") && n == 1) {
        r = e_make(xasprintf("lower(%s)", as_str(args[0]).txt), P_ATOM, T_STR);
    } else if (!strcmp(name, "BYTE") && n == 1) {
        r = e_make(xasprintf("byte(%s)", as_int(args[0]).txt), P_ATOM, T_STR);
    } else if (!strcmp(name, "DOUBLE") && n == 1) {
        r = e_make(xasprintf("double(%s)", as_str(args[0]).txt), P_ATOM, T_STR);
    } else if (!strcmp(name, "INDEX") && n == 2) {
        r = mkoff(e_make(xasprintf("index(%s, %s)", as_str(args[0]).txt, as_str(args[1]).txt), P_ATOM, T_INT), 1);
        return r;
    } else if ((!strcmp(name, "D2A") || !strcmp(name, "SIGNED")) && n == 1) {
        return as_str(as_int(args[0]));
    } else {
        fail(m, "the built-in function %s is not converted", name);
        return dummy_str();
    }
    r.charctx = 1;
    return r;
}

static E attr_ref(XP *p, int letter) {
    E v;
    p->i++; /* the quote */
    if (p->s[p->i] != '&') {
        fail(p->m, "the attribute %c' of a symbol is not available in macro code", letter);
        return dummy_str();
    }
    v = parse_ref(p);
    if (letter == 'K') return e_make(xasprintf("len(%s)", as_str(v).txt), P_ATOM, T_INT);
    if (letter == 'N') {
        if (!strcmp(v.txt, "sys.args")) return e_make(xstrdup("sys.argc"), P_ATOM, T_INT);
        return e_make(xasprintf("count(%s)", v.txt), P_ATOM, T_INT);
    }
    {
        E r = e_make(xasprintf("attr(%s, \"%c\")", as_str(v).txt, letter), P_ATOM, T_STR);
        r.charctx = letter != 'L';
        return r;
    }
}

static E parse_prim(XP *p) {
    MC *m = p->m;
    char c;
    skipb(p);
    c = p->s[p->i];
    if (c == '\'') {
        E e = parse_string(p);
        return str_tail(p, e);
    }
    if (c == '(') {
        long q = match_paren(p->s, p->i);
        if (q >= 0 && p->s[q + 1] == '\'') { /* duplication factor (n)'text' */
            E n;
            E s;
            p->i++;
            n = parse_add(p);
            skipb(p);
            if (p->s[p->i] == ')') p->i++;
            s = parse_string(p);
            s = e_make(xasprintf("repeat(%s, %s)", as_str(s).txt, as_int(n).txt), P_ATOM, T_STR);
            return str_tail(p, s);
        } else {
            E e;
            if (xp_depth(p)) return dummy_str();
            p->i++;
            {
                static const char *const OPS[] = {"UPPER", "LOWER", "DOUBLE", NULL};
                int k;
                for (k = 0; OPS[k]; k++)
                    if (word_at(p, OPS[k]) && (p->s[p->i] == ' ' || p->s[p->i] == '\'')) {
                        E a = as_str(parse_prim(p));
                        skipb(p);
                        if (p->s[p->i] == ')') p->i++;
                        else fail(m, "missing ) after (%s ...", OPS[k]);
                        e = e_make(xasprintf("%s(%s)", k == 0 ? "upper" : k == 1 ? "lower" : "double", a.txt), P_ATOM, T_STR);
                        e.charctx = 1;
                        xp_undepth(p);
                        return e;
                    }
            }
            e = parse_or(p);
            skipb(p);
            if (p->s[p->i] != ')') {
                fail(m, "missing ) in an expression");
                xp_undepth(p);
                return e;
            }
            p->i++;
            xp_undepth(p);
            return e;
        }
    }
    if (c == '&') {
        return parse_ref(p);
    }
    if (za_isdigit((unsigned char)c)) {
        uint64_t v = 0;
        int ov = 0;
        while (za_isdigit((unsigned char)p->s[p->i])) {
            unsigned d = (unsigned)(p->s[p->i++] - '0');
            if (v > (UINT64_MAX - d) / 10) ov = 1;
            else v = v * 10 + d;
        }
        if (ov || v > 0x7FFFFFFFu) {
            fail(p->m, "SETA constant is out of range (HLASM allows -2147483648..2147483647)");
            return e_lit(0);
        }
        return e_lit((int64_t)v);
    }
    if (za_isalpha((unsigned char)c)) {
        size_t j = p->i;
        char *w;
        while (is_hlc((unsigned char)p->s[j])) j++;
        w = za_strupper(xstrndup(p->s + p->i, j - p->i));
        if (p->s[j] == '\'') {
            if (strlen(w) == 1 && strchr("KNTLDIOS", w[0])) {
                p->i = j;
                return attr_ref(p, w[0]);
            }
            if (strlen(w) == 1 && strchr("BXC", w[0])) { /* self defining term */
                size_t e = j + 1;
                char *txt;
                while (p->s[e] && !(p->s[e] == '\'' && p->s[e + 1] != '\'')) e += p->s[e] == '\'' ? 2 : 1;
                txt = xstrndup(p->s + p->i, e + 1 - p->i);
                p->i = e + 1;
                if (w[0] == 'X') return e_make(xasprintf("0x%.*s", (int)(strlen(txt) - 3), txt + 2), P_ATOM, T_INT);
                if (w[0] == 'B') return e_make(xasprintf("%s", txt), P_ATOM, T_INT);
                fail(m, "the self defining term %s is not converted", txt);
                return e_lit(0);
            }
        } else if (p->s[j] == '(') {
            p->i = j;
            return func_call(p, w);
        }
        fail(m, "cannot read \"%s\" in a conditional assembly expression", w);
        p->i = j;
        return dummy_str();
    }
    fail(m, "unexpected character '%c' in a conditional assembly expression", c ? c : '?');
    if (c) p->i++;
    return dummy_str();
}

static E parse_un(XP *p) {
    E v;
    skipb(p);
    if (p->s[p->i] == '-' || p->s[p->i] == '+') {
        int neg = p->s[p->i] == '-';
        if (xp_depth(p)) return dummy_str();
        p->i++;
        v = parse_un(p);
        xp_undepth(p);
        return neg ? negate_arith(v) : v;
    }
    return parse_prim(p);
}

static E parse_mul(XP *p) {
    E a = parse_un(p);
    for (;;) {
        skipb(p);
        if (p->s[p->i] == '*' || p->s[p->i] == '/') {
            char op = p->s[p->i++];
            a = arith(op, a, parse_un(p));
        } else {
            return a;
        }
    }
}

static E parse_add(XP *p) {
    E a = parse_mul(p);
    for (;;) {
        skipb(p);
        if (p->s[p->i] == '+' || p->s[p->i] == '-') {
            char op = p->s[p->i++];
            a = arith(op, a, parse_mul(p));
        } else {
            return a;
        }
    }
}

static E rel_make(int rel, E a, E b) {
    if (a.charctx || b.charctx) {
        a = as_str(a);
        b = as_str(b);
    } else if (a.ty == T_STR && b.ty == T_STR) {
        a = as_int(a);
        b = as_int(b);
    } /* a string against an integer: the comparison is numeric */
    return mk_rel(rel, a, b);
}

static E parse_rel(XP *p) {
    E a = parse_add(p);
    static const struct {
        const char *w;
        int rel;
    } R[] = {{"EQ", '='}, {"NE", '!'}, {"LT", '<'}, {"GT", '>'}, {"LE", 'l'}, {"GE", 'g'}};
    size_t k;
    for (k = 0; k < sizeof R / sizeof *R; k++)
        if (word_at(p, R[k].w)) return rel_make(R[k].rel, a, parse_add(p));
    return a;
}

static E parse_not(XP *p) {
    if (word_at(p, "NOT")) return mk_not(parse_not(p));
    return parse_rel(p);
}

static E parse_and(XP *p) {
    E a = parse_not(p);
    while (word_at(p, "AND")) a = mk_logic(OP_AND, a, parse_not(p));
    return a;
}

static E parse_or(XP *p) {
    E a = parse_and(p);
    for (;;) {
        if (word_at(p, "OR")) {
            a = mk_logic(OP_OR, a, parse_and(p));
        } else if (word_at(p, "XOR")) {
            E b = parse_and(p);
            a = mk_logic(OP_OR, mk_logic(OP_AND, a, mk_not(b)), mk_logic(OP_AND, mk_not(a), b));
        } else {
            return a;
        }
    }
}

/* ---- nodes and the pre-pass ----------------------------------------------------- */

static void node(MC *m, NKind k, const char *text, const char *cond, const char *ncond, const char *target,
                 const char *remark) {
    MNode n;
    memset(&n, 0, sizeof n);
    n.k = k;
    n.line = m->line;
    n.text = text ? xstrdup(text) : NULL;
    n.cond = cond ? xstrdup(cond) : NULL;
    n.ncond = ncond ? xstrdup(ncond) : NULL;
    n.target = target ? xstrdup(target) : NULL;
    n.remark = remark && *remark ? xstrdup(remark) : NULL;
    vec_push(&m->nodes, n);
}

static E parse_text(MC *m, const char *s) {
    XP p;
    E e;
    p.m = m;
    p.s = s;
    p.i = 0;
    p.depth = 0;
    e = parse_or(&p);
    skipb(&p);
    if (p.s[p.i]) fail(m, "cannot read \"%s\" in a conditional assembly expression", p.s + p.i);
    return e;
}

/* "&NAME" or "&NAME(n)" -> upper case name, *end after the name */
static char *amp_name(const char *t, size_t *end) {
    size_t j = 1;
    if (t[0] != '&') return NULL;
    while (is_hlc((unsigned char)t[j])) j++;
    if (j == 1) return NULL;
    if (end) *end = j;
    return za_strupper(xstrndup(t + 1, j - 1));
}

static Var *declare(MC *m, const char *hl, Ty ty, int array, int global, int dim, int explicit_decl) {
    Var *v = var_find(m, hl);
    if (v) return v;
    v = var_new(m, hl, ty, array, global, dim);
    v->explicit_decl = explicit_decl;
    vec_push(&m->decls, v);
    return v;
}

static void declare_list(MC *m, const char *ops, int global, Ty ty) {
    OperandVec ov = {0};
    size_t k;
    split_operands(&ov, ops);
    for (k = 0; k < ov.n; k++) {
        const char *t = ov.v[k].text;
        size_t e = 0;
        char *hl;
        int dim = -1, array = 0;
        if (!t[0]) continue;
        if (t[0] != '&' && (za_isalpha((unsigned char)t[0]) || strchr("@#$_", t[0]))) { /* a missing & is read as if it were there */
            t = xasprintf("&%s", t);
            warn(m, "the declaration \"%s\" has no &: read as &%s", ov.v[k].text, ov.v[k].text);
        }
        hl = amp_name(t, &e);
        if (!hl) {
            fail(m, "cannot read the declaration \"%s\"", t);
            continue;
        }
        if (t[e] == '(') {
            char *end;
            long d = strtol(t + e + 1, &end, 10);
            if (end == t + e + 1 || *end != ')') {
                fail(m, "the dimension of &%s is not a number", hl);
                continue;
            }
            dim = (int)d;
            array = 1;
        }
        declare(m, hl, ty, array, global, dim, 1);
    }
}

static int is_op(const char *op, const char *w) { return za_streqi(op, w); }

static void prepass(MC *m, const MHStmtVec *st, size_t first) {
    size_t k;
    for (k = first + 1; k < st->n; k++) {
        const MHStmt *s = &st->v[k];
        char *op;
        if (s->kind != MH_STMT) continue;
        m->line = s->line;
        op = s->op;
        if (strlen(op) == 4 && (!za_strncmpi(op, "LCL", 3) || !za_strncmpi(op, "GBL", 3))) {
            char t = (char)za_toupper((unsigned char)op[3]);
            if (t == 'A' || t == 'B' || t == 'C')
                declare_list(m, s->operands, za_toupper((unsigned char)op[0]) == 'G', t == 'C' ? T_STR : T_INT);
        } else if (strlen(op) == 4 && !za_strncmpi(op, "SET", 3) && s->name[0] == '&') {
            char t = (char)za_toupper((unsigned char)op[3]);
            size_t e = 0;
            char *hl = amp_name(s->name, &e);
            if ((t == 'A' || t == 'B' || t == 'C') && hl && !var_find(m, hl))
                declare(m, hl, t == 'C' ? T_STR : T_INT, s->name[e] == '(', 0, -1, 0);
        }
    }
}

/* the prototype: &LABEL NAME &A,&B=def,... */
static void parse_proto(MC *m, const MHStmt *p) {
    OperandVec ov = {0};
    size_t k;
    m->name = za_strupper(p->op);
    if (p->name[0] == '&') {
        m->label_var = amp_name(p->name, NULL);
        if (!m->label_var) fail(m, "cannot read the name field \"%s\" of the prototype", p->name);
    } else if (p->name[0]) {
        fail(m, "the name field of the prototype is not a variable");
    }
    split_operands(&ov, p->operands);
    for (k = 0; k < ov.n; k++) {
        const char *t = ov.v[k].text;
        size_t e = 0;
        char *hl = amp_name(t, &e);
        Var *v;
        if (!t[0]) continue;
        if (!hl) {
            /* HLASM: ASMA025S, but the macro is still defined and the operand is ignored */
            warn(m, "the prototype operand \"%s\" is ignored (HLASM: ASMA025S invalid macro prototype operand)", t);
            continue;
        }
        if (var_find(m, hl)) {
            fail(m, "the parameter &%s is declared twice", hl);
            continue;
        }
        v = var_new(m, hl, T_STR, 0, 0, -1);
        v->param = 1;
        if (t[e] == '=') {
            v->kw = 1;
            v->def = conv_fold(t + e + 1);
        } else if (t[e]) {
            fail(m, "cannot read the prototype operand \"%s\"", t);
        }
        vec_push(&m->params, v);
    }
}

/* ---- model text ------------------------------------------------------------------- */

/* a reference that "&name" can write: name, sys.name, name[digits], sys.args[digits] */
static int simple_ref(const char *t) {
    size_t i = 0;
    if (!(za_isalpha((unsigned char)t[0]) || t[0] == '_')) return 0;
    while (za_is_idc((unsigned char)t[i])) i++;
    if (t[i] == '.') {
        i++;
        if (!za_is_idc((unsigned char)t[i])) return 0;
        while (za_is_idc((unsigned char)t[i])) i++;
    }
    if (t[i] == '[') {
        i++;
        if (!za_isdigit((unsigned char)t[i])) return 0;
        while (za_isdigit((unsigned char)t[i])) i++;
        if (t[i] != ']') return 0;
        i++;
    }
    return t[i] == 0;
}

/* The model text of one field: &refs written as &name or &{expr}, the rest case folded. */
static char *model_text(MC *m, const char *s, int nofold) {
    Buf b = {0};
    Vec(char *) reps = {0}, folds = {0};
    size_t i = 0, k;
    char *f, *r;
    Buf out = {0};
    while (s[i]) {
        if (s[i] == '&') {
            if (s[i + 1] == '&') {
                buf_puts(&b, "&&");
                i += 2;
            } else if (hl_start((unsigned char)s[i + 1])) {
                XP p;
                E e;
                int c;
                p.m = m;
                p.s = s;
                p.i = i;
                p.depth = 0;
                e = parse_ref(&p);
                i = p.i;
                c = (unsigned char)s[i];
                if (simple_ref(e.txt) && !za_is_idc(c) && c != '.' && c != '[')
                    vec_push(&reps, xasprintf("&%s", e.txt));
                else
                    vec_push(&reps, xasprintf("&{%s}", e.txt));
                /* HLASM folds the statement to upper case after the substitution, except
                 * inside quotes: a string value gets upper(); the lower case marker x in the
                 * placeholder tells conv_fold's verdict (X outside quotes) */
                vec_push(&folds, e.ty == T_STR && !nofold ? xasprintf("&{upper(%s)}", e.txt) : NULL);
                buf_printf(&b, "_\x01x%u\x02", (unsigned)(reps.n - 1));
            } else if (s[i + 1] == '(') {
                fail(m, "a created set symbol (&(...)) is not converted");
                buf_putc(&b, '&');
                i++;
            } else {
                buf_puts(&b, "&&"); /* a lone & is a literal & */
                i++;
            }
        } else {
            buf_putc(&b, s[i]);
            i++;
        }
    }
    f = conv_fold(buf_cstr(&b));
    for (i = 0; f[i]; i++) {
        if (f[i] == '_' && f[i + 1] == '\x01' && (f[i + 2] == 'x' || f[i + 2] == 'X')) {
            k = (size_t)strtoul(f + i + 3, &r, 10);
            if (*r == '\x02' && k < reps.n) {
                buf_puts(&out, f[i + 2] == 'X' && folds.v[k] ? folds.v[k] : reps.v[k]);
                i = (size_t)(r - f);
                continue;
            }
        }
        buf_putc(&out, f[i]);
    }
    return xstrdup(buf_cstr(&out));
}

/* ---- statements -------------------------------------------------------------------- */

static char *seq_target(MC *m, const char *t) {
    char *z;
    while (is_blank((unsigned char)*t)) t++;
    if (t[0] != '.' || !is_hlc((unsigned char)t[1])) {
        fail(m, "cannot read the sequence symbol \"%s\"", t);
        return xstrdup("x");
    }
    z = zname(t + 1);
    strmap_put(&m->seqref, z, (void *)1);
    return z;
}

static void conv_set(MC *m, const MHStmt *s, char kind) {
    size_t e = 0, k;
    char *hl = amp_name(s->name, &e);
    Var *v = hl ? var_find(m, hl) : NULL;
    OperandVec ov = {0};
    E subs[4];
    int n = 0;
    if (!v) {
        fail(m, "cannot read the SET symbol \"%s\"", s->name);
        return;
    }
    if (v->param) {
        fail(m, "SET assigns the parameter &%s", v->hl);
        return;
    }
    if (s->name[e] == '(') {
        XP p;
        p.m = m;
        p.s = s->name;
        p.i = e;
        p.depth = 0;
        n = parse_subs(&p, subs, 4, NULL);
    }
    split_operands(&ov, s->operands);
    if (ov.n > 1 && n != 1) {
        fail(m, "several values are assigned to &%s, which has no single subscript", v->hl);
        return;
    }
    for (k = 0; k < ov.n; k++) {
        Buf lhs = {0};
        E rhs;
        if (kind == 'C' && ov.n == 1 && za_streqi(str_trim(ov.v[0].text), "AREAD"))
            rhs = e_make(xstrdup("aread()"), P_ATOM, T_STR);
        else if (kind == 'C' && ov.n == 1 && !za_strncmpi(str_trim(ov.v[0].text), "AREAD", 5) &&
                 !is_hlc((unsigned char)str_trim(ov.v[0].text)[5]))
        {
            fail(m, "AREAD with operands is not converted");
            rhs = dummy_str();
        } else
            rhs = parse_text(m, ov.n == 1 ? s->operands : ov.v[k].text);
        int j;
        char *t;
        buf_puts(&lhs, v->z);
        for (j = 0; j < n; j++) buf_printf(&lhs, "[%s]", mkoff(subs[j], j == 0 ? -1 + (int64_t)k : -1).txt);
        if (kind == 'C' && v->ty == T_STR)
            rhs = as_str(rhs);
        else if (v->ty == T_INT)
            rhs = as_int(rhs);
        else
            rhs = as_str(rhs);
        t = xasprintf("#%s = %s", buf_cstr(&lhs), rhs.txt);
        node(m, N_CODE, t, NULL, NULL, NULL, k + 1 == ov.n ? s->remark : NULL);
    }
}

static void conv_aif(MC *m, const MHStmt *s) {
    OperandVec ov = {0};
    size_t k;
    split_operands(&ov, s->operands);
    for (k = 0; k < ov.n; k++) {
        const char *t = ov.v[k].text;
        XP p;
        E c;
        char *tg;
        p.m = m;
        p.s = t;
        p.i = 0;
        p.depth = 0;
        skipb(&p);
        if (t[p.i] != '(') {
            fail(m, "AIF needs (condition).symbol");
            return;
        }
        c = parse_or(&p);
        skipb(&p);
        if (p.s[p.i] != '.') {
            fail(m, "the sequence symbol of AIF is missing");
            return;
        }
        tg = seq_target(m, p.s + p.i);
        c = as_int(c);
        node(m, N_COND, NULL, c.txt, mk_not(c).txt, tg, k + 1 == ov.n ? s->remark : NULL);
    }
}

static void conv_model(MC *m, const MHStmt *s);

static void conv_mnote(MC *m, const MHStmt *s) {
    OperandVec ov = {0};
    const char *msg;
    const char *sevtxt = NULL;
    int sev = 1;
    E e;
    char *t;
    split_operands(&ov, s->operands);
    if (ov.n == 0) {
        fail(m, "MNOTE without operands");
        return;
    }
    if (ov.n == 1) {
        msg = ov.v[0].text;
    } else {
        const char *a = ov.v[0].text;
        msg = s->operands + ov.v[1].off;
        if (!strcmp(a, "*"))
            sev = 0;
        else if (!*a)
            sev = 1;
        else if (numeric_text(a))
            sev = atoi(a);
        else
            sevtxt = a;
    }
    {
        const char *q = msg;
        size_t ol = strlen(s->operands);
        while (*q == ' ') q++;
        /* HLASM: the message has to be a quoted string (or start with a variable symbol or a
         * parenthesis, which a substitution can turn into one). Otherwise the generated MNOTE
         * statement is passed on, the assembler flags it (ASMA163W) and gives no message. */
        if (ov.n >= 2 ? (!*q || !strchr("'&(", *q)) : (ol > 0 && s->operands[ol - 1] == ',') || (*q && !strchr("'&(", *q))) {
            warn(m, "MNOTE without a quoted message: HLASM passes the statement on with ASMA163W and gives no message");
            conv_model(m, s);
            return;
        }
    }
    {
        XP p;
        p.m = m;
        p.s = msg;
        p.i = 0;
        p.depth = 0;
        e = as_str(parse_or(&p));
        skipb(&p);
        if (p.s[p.i]) warn(m, "text after the MNOTE message is ignored: \"%s\"", p.s + p.i);
    }
    if (sevtxt) {
        E sv = as_int(parse_text(m, sevtxt));
        t = xasprintf("#if (%s >= 5) {\n    #error %s\n#} else if (%s >= 1) {\n    #warning %s\n#} else {\n    #note %s\n#}",
                      par(sv, P_ADD), e.txt, par(sv, P_ADD), e.txt, e.txt);
    } else {
        t = xasprintf("#%s %s", sev == 0 ? "note" : sev < 5 ? "warning" : "error", e.txt);
    }
    node(m, N_CODE, t, NULL, NULL, NULL, s->remark);
}

static void conv_model(MC *m, const MHStmt *s) {
    Buf b = {0};
    /* a macro call passes its operands on as written: the called macro folds them */
    int call = !strchr(s->op, '&') && !asm_is_op(conv_fold(s->op));
    char *nm = s->name[0] && s->name[0] != '.' ? model_text(m, s->name, call) : NULL;
    char *op = model_text(m, s->op, 0);
    char *opd = s->operands[0] ? model_text(m, s->operands, call) : NULL;
    size_t w = nm ? strlen(nm) + 3 : 0;
    if (opd && opd[strlen(opd) - 1] == ',') opd = xasprintf("%s&{\"\"}", opd); /* HLASM ends the operands here; zosasm would continue */
    if (nm && nm[0] == '#') { /* a name starting with # would read as a macro statement */
        char *q = xasprintf("\"%s\"", nm);
        w = strlen(q) + 3;
        nm = q;
    }
    if (nm) buf_printf(&b, "%s:", nm);
    if (w < 10) w = 10;
    while (b.len < w) buf_putc(&b, ' ');
    if (!opd) {
        buf_puts(&b, op);
    } else {
        buf_puts(&b, op);
        buf_putc(&b, ' ');
        while (b.len < w + 6) buf_putc(&b, ' ');
        buf_puts(&b, opd);
    }
    if (s->remark && s->remark[0]) buf_printf(&b, "  %s", s->remark);
    node(m, N_CODE, buf_cstr(&b), NULL, NULL, NULL, NULL);
}

static const char *const MCONV_UNSUPPORTED[] = {"COPY", "SETAF", "SETCF", "MACRO", "MEND", NULL};

/* AINSERT 'record'[,FRONT|BACK] */
static void conv_ainsert(MC *m, const MHStmt *s) {
    OperandVec ov = {0};
    E e;
    char *t;
    int front = 0;
    split_operands(&ov, s->operands);
    if (ov.n < 1 || ov.n > 2) {
        fail(m, "AINSERT needs 'record'[,FRONT|BACK]");
        return;
    }
    if (ov.n == 2) {
        if (za_streqi(ov.v[1].text, "FRONT"))
            front = 1;
        else if (!za_streqi(ov.v[1].text, "BACK")) {
            fail(m, "AINSERT: \"%s\" is not FRONT or BACK", ov.v[1].text);
            return;
        }
    }
    e = as_str(parse_text(m, ov.v[0].text));
    t = xasprintf("#insert %s%s", e.txt, front ? ", front" : "");
    node(m, N_CODE, t, NULL, NULL, NULL, s->remark);
}

static void conv_stmt(MC *m, const MHStmt *s) {
    m->line = s->line;
    if (s->kind == MH_ASMCOMMENT) {
        char *t = xasprintf("*%s", s->text);
        node(m, N_CODE, t, NULL, NULL, NULL, NULL);
        return;
    }
    if (s->kind == MH_MACCOMMENT) {
        char *t = xasprintf("//%s", s->text);
        node(m, N_CODE, t, NULL, NULL, NULL, NULL);
        return;
    }
    if (s->kind != MH_STMT) return;
    {
        const char *op = s->op;
        int k;
        if (s->name[0] == '.') {
            char *z = zname(s->name + 1);
            strmap_put(&m->seqdef, z, (void *)1);
            node(m, N_LABEL, NULL, NULL, NULL, z, NULL);
        }
        for (k = 0; MCONV_UNSUPPORTED[k]; k++)
            if (is_op(op, MCONV_UNSUPPORTED[k])) {
                fail(m, "%s is not converted", MCONV_UNSUPPORTED[k]);
                return;
            }
        if ((!za_strncmpi(op, "LCL", 3) || !za_strncmpi(op, "GBL", 3)) && strlen(op) == 4) return;
        if (is_op(op, "ACTR") || is_op(op, "MHELP")) return;
        if (is_op(op, "SETA")) {
            conv_set(m, s, 'A');
            return;
        }
        if (is_op(op, "SETB")) {
            conv_set(m, s, 'B');
            return;
        }
        if (is_op(op, "SETC")) {
            conv_set(m, s, 'C');
            return;
        }
        if (is_op(op, "AIF")) {
            conv_aif(m, s);
            return;
        }
        if (is_op(op, "AGO")) {
            if (s->operands[0] == '(') {
                fail(m, "a computed AGO is not converted");
                return;
            }
            node(m, N_GOTO, NULL, NULL, NULL, seq_target(m, s->operands), s->remark);
            return;
        }
        if (is_op(op, "ANOP")) return;
        if (is_op(op, "AINSERT")) {
            conv_ainsert(m, s);
            return;
        }
        if (is_op(op, "MEXIT")) {
            node(m, N_RETURN, NULL, NULL, NULL, NULL, s->remark);
            return;
        }
        if (is_op(op, "MNOTE")) {
            conv_mnote(m, s);
            return;
        }
        if (!op[0]) return;
        conv_model(m, s);
    }
}

/* ---- the macro ---------------------------------------------------------------------- */

static int mentions_syslist(const MHStmtVec *st) {
    size_t k;
    for (k = 0; k < st->n; k++) {
        const MHStmt *s = &st->v[k];
        const char *f[3];
        int j;
        if (s->kind != MH_STMT) continue;
        f[0] = s->name;
        f[1] = s->op;
        f[2] = s->operands;
        for (j = 0; j < 3; j++) {
            const char *p;
            for (p = f[j]; *p; p++)
                if (*p == '&' && !za_strncmpi(p + 1, "SYSLIST", 7)) return 1;
        }
    }
    return 0;
}

static char *header(MC *m, int rest) {
    Buf b = {0};
    size_t k;
    int first = 1, star = 0;
    buf_printf(&b, "#macro %s(", m->name);
    for (k = 0; k < m->params.n; k++) {
        Var *v = m->params.v[k];
        if (v->kw) continue;
        buf_printf(&b, "%s%s", first ? "" : ", ", v->z);
        first = 0;
    }
    if (rest) {
        char *rn = xstrdup("rest");
        while (var_find(m, za_strupper(rn))) rn = xasprintf("%s_", rn);
        buf_printf(&b, "%s%s...", first ? "" : ", ", rn);
        first = 0;
    }
    for (k = 0; k < m->params.n; k++) {
        Var *v = m->params.v[k];
        if (!v->kw) continue;
        if (!rest && !star) {
            if (!first) buf_puts(&b, ", ");
            buf_putc(&b, '*');
            first = 0;
            star = 1;
        }
        buf_printf(&b, "%s%s = %s", first ? "" : ", ", v->z, zstr(v->def, strlen(v->def)));
        first = 0;
    }
    buf_puts(&b, ") {");
    return xstrdup(buf_cstr(&b));
}

static void hoisted(MC *m, Buf *out) {
    size_t k;
    for (k = 0; k < m->decls.n; k++) {
        Var *v = m->decls.v[k];
        const char *zero = v->ty == T_INT ? "0" : "\"\"";
        char *init;
        if (v->array && v->dim > 0)
            init = xasprintf("fill(%d, %s)", v->dim, zero);
        else if (v->array)
            init = xstrdup("list()");
        else
            init = xstrdup(zero);
        buf_printf(out, "    #%s %s = %s\n", v->global ? "global" : "let", v->z, init);
    }
}

char *mconv_macro(const MHStmtVec *st, const MConvOptions *o, char **why) {
    MC m;
    size_t k, j, first = 0, pre;
    Buf out = {0}, body = {0}, inner = {0};
    int rest;
    memset(&m, 0, sizeof m);
    m.o = o;
    for (first = 0; first < st->n && st->v[first].kind != MH_STMT; first++) {}
    if (first >= st->n) {
        if (why) *why = xstrdup("the macro has no prototype statement");
        free(out.p); free(body.p); free(inner.p);
        return NULL;
    }
    m.line = st->v[first].line;
    parse_proto(&m, &st->v[first]);
    prepass(&m, st, first);
    for (k = first + 1; k < st->n && !m.why; k++) {
        const MHStmt *s = &st->v[k];
        if (s->kind == MH_STMT && is_op(s->op, "MACRO") && !s->name[0] && !s->operands[0]) {
            /* a macro defined inside a macro: HLASM defines it when the outer macro runs; here it
             * becomes a macro of its own, written before the outer one */
            MHStmtVec sub = {0};
            size_t e = k + 1;
            int depth = 0;
            char *iwhy = NULL, *itext;
            for (; e < st->n; e++) {
                const MHStmt *u = &st->v[e];
                if (u->kind != MH_STMT) continue;
                if (is_op(u->op, "MACRO") && !u->name[0] && !u->operands[0]) depth++;
                if (is_op(u->op, "MEND") && depth-- == 0) break;
            }
            if (e >= st->n) {
                fail(&m, "a nested MACRO has no MEND");
                break;
            }
            m.line = s->line;
            for (j = k + 1; j <= e; j++) vec_push(&sub, st->v[j]);
            itext = mconv_macro(&sub, o, &iwhy);
            if (!itext) {
                fail(&m, "the nested macro is not converted (%s)", iwhy ? iwhy : "?");
                free(iwhy);
                free(sub.v);
                break;
            }
            buf_puts(&inner, itext);
            free(itext);
            free(iwhy);
            free(sub.v);
            k = e;
            continue;
        }
        if (s->kind == MH_STMT && k + 1 == st->n && is_op(s->op, "MEND")) {
            if (s->name[0] == '.') {
                char *z = zname(s->name + 1);
                m.line = s->line;
                strmap_put(&m.seqdef, z, (void *)1);
                node(&m, N_LABEL, NULL, NULL, NULL, z, NULL);
            }
            break;
        }
        conv_stmt(&m, s);
    }
    for (k = 0; k < m.nodes.n && !m.why; k++) {
        MNode *n = &m.nodes.v[k];
        if ((n->k == N_COND || n->k == N_GOTO) && !strmap_get(&m.seqdef, n->target)) {
            char *msg;
            m.line = n->line;
            msg = xasprintf("\"the sequence symbol .%s is not defined\"", n->target);
            warn(&m, "the sequence symbol .%s is not defined: the branch became an #error", n->target);
            if (n->k == N_GOTO)
                n->text = xasprintf("#error %s", msg);
            else
                n->text = xasprintf("#if (%s) {\n    #error %s\n#}", n->cond, msg);
            free(msg);
            n->k = N_CODE;
        }
    }
    if (m.why) {
        if (why) *why = xasprintf("line %d: %s", m.line, m.why);
        free(out.p);
        free(body.p);
        free(inner.p);
        return NULL;
    }
    for (pre = 0; pre < first; pre++) {
        const MHStmt *s = &st->v[pre];
        if (s->kind == MH_ASMCOMMENT || s->kind == MH_MACCOMMENT) buf_printf(&out, "*%s\n", s->text);
    }
    rest = mentions_syslist(st);
    buf_puts(&out, buf_cstr(&inner));
    buf_printf(&out, "%s\n", header(&m, rest));
    hoisted(&m, &out);
    {
        char *pc = xstrdup("pc");
        size_t q;
        int again = 1;
        while (again) {
            again = 0;
            for (q = 0; q < m.params.n && !again; q++)
                if (!strcmp(m.params.v[q]->z, pc)) again = 1;
            for (q = 0; q < m.decls.n && !again; q++)
                if (!strcmp(m.decls.v[q]->z, pc)) again = 1;
            if (again) pc = xasprintf("%s_", pc);
        }
        if (mflow_write(&body, &m.nodes, o->flat ? MF_FLAT : o->keep_goto ? MF_KEEPGOTO : MF_STRUCT, pc) < 0 && o->warn)
            o->warn(o->user, m.line, "flow could not be structured, written with #goto");
        free(pc);
    }
    buf_puts(&out, buf_cstr(&body));
    buf_puts(&out, "#}\n");
    if (why) *why = NULL;
    {
        char *res = xstrdup(buf_cstr(&out));
        free(out.p);
        free(body.p);
        free(inner.p);
        return res;
    }
}
