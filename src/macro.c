/* macro.c - the C-like macro language: collecting, compiling and running macro code and model
 * statements (MACRO.md).
 *
 * A macro or an open code block is compiled to a flat array of statements with jumps. Every
 * statement records the depth of its block; variables carry the depth they were declared at and
 * are dropped when a statement of a lower depth runs, which gives block scope without a scope
 * stack. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "macro_int.h"
#include "ebcdic.h"
#include "isa.h"
#include "version.h"

#define MAX_DEPTH 64
#define DEFAULT_STEPS 1000000L
#define MAX_LIST ((int64_t)1 << 20)

/* ---- structures ------------------------------------------------------------- */

typedef struct {
    char *text; /* literal when e == NULL */
    MExpr *e;
} Part;

typedef struct {
    Part *p;
    int n;
} Tmpl;

typedef struct {
    Tmpl ops;
    char *remark;
} ContLine;

typedef struct {
    Tmpl label, op, opd;
    char *remark;
    ContLine *conts;
    int nconts;
    char *comment; /* not NULL: a comment line, emitted as written */
} Model;

typedef enum {
    S_NOP, S_LET, S_GLOBAL, S_ASSIGN, S_JUMP, S_JFALSE, S_CASE, S_ITER, S_EMIT, S_INSERT, S_NOTE, S_WARN,
    S_ERROR, S_MODEL, S_RETURN
} SKind;

struct MStmt {
    SKind kind;
    int depth, block;
    const char *file;
    int line, col, len;
    char *name;       /* LET, GLOBAL, ASSIGN, ITER variable; goto label */
    MExpr *e;         /* value, condition */
    MExpr **idx;      /* ASSIGN: subscripts */
    int nidx;
    int aop;          /* ASSIGN: 0 for '=', else the binary operator */
    int target;       /* jumps */
    MExpr **cases;    /* CASE */
    int ncases;
    char *hl, *hi;    /* ITER: hidden list and index variables; CASE: hl = the switch value */
    int vdepth;       /* ITER: depth of the loop variable */
    Model *model;
};

typedef struct {
    MStmt *st;
    size_t n;
} Code;

typedef struct {
    char *name;
    MExpr *def;
    int kwonly, rest;
} Param;

struct Macro {
    char *name; /* upper case */
    char *labelvar;
    Param *params;
    int np;
    Code code;
    Span def;
};

struct MacSys {
    Reporter *rep;
    MacHost host;
    long step_limit, steps;
    uint64_t list_work; /* elements copied by list assignment in this expansion */
    int elf;
    StrMap macros; /* upper case name -> Macro */
    StrMap globals; /* lower case name -> MVal */
    Frame top;      /* open code variables */
    int depth, aborted;
    long ndx, nexp, stmt;
};

static void mstmt_free(MStmt *st);

Reporter *ms_rep(MacSys *ms) { return ms->rep; }

/* ---- small helpers ---------------------------------------------------------------- */


static const char *skipb(const char *s) {
    while (is_blank((unsigned char)*s)) s++;
    return s;
}

static size_t ident_len(const char *s) {
    size_t n = 0;
    if (!(za_isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    while (za_is_idc((unsigned char)s[n])) n++;
    return n;
}

static int is_reserved(const char *n) {
    static const char *const kw[] = {"macro", "let",    "global", "if",       "else",   "while",
                                     "for",   "switch", "case",   "default",  "break",  "continue",
                                     "return", "goto",  "label",  "emit",     "insert", "note",   "warning",
                                     "error", "true",   "false",  "in"};
    size_t i;
    for (i = 0; i < sizeof kw / sizeof kw[0]; i++)
        if (!strcmp(n, kw[i])) return 1;
    return 0;
}

/* is s[i] the start of an X'..' B'..' C'..' constant? returns the index of its closing quote */
static long const_end(const char *s, size_t i) {
    size_t j;
    int c = za_toupper((unsigned char)s[i]);
    if (!(c == 'X' || c == 'B' || c == 'C') || s[i + 1] != '\'') return -1;
    if (i > 0 && za_is_idc((unsigned char)s[i - 1])) return -1;
    for (j = i + 2; s[j]; j++) {
        if (s[j] == '\'') {
            if (s[j + 1] == '\'')
                j++;
            else
                return (long)j;
        }
    }
    return -1;
}

/* Scan macro code text: end of the code (before a // comment, trailing blanks removed), and
 * the bracket and string state. */
typedef struct {
    size_t end;
    int open;      /* unclosed ( or [ */
    int in_string; /* a string is left open */
    int backslash; /* the code ends in a backslash */
} CodeScan;

static CodeScan code_scan(const char *s) {
    CodeScan r = {0, 0, 0, 0};
    size_t i;
    for (i = 0; s[i]; i++) {
        long ce;
        if (s[i] == '"') {
            size_t j = i + 1;
            while (s[j] && s[j] != '"') {
                if (s[j] == '\\' && s[j + 1]) j++;
                j++;
            }
            if (!s[j]) {
                r.in_string = 1;
                i = j - 1;
                break;
            }
            i = j;
        } else if ((ce = const_end(s, i)) >= 0) {
            i = (size_t)ce;
        } else if (s[i] == '/' && s[i + 1] == '/') {
            break;
        } else if (s[i] == '(' || s[i] == '[') {
            r.open++;
        } else if (s[i] == ')' || s[i] == ']') {
            r.open--;
        }
    }
    r.end = i;
    while (r.end > 0 && is_blank((unsigned char)s[r.end - 1])) r.end--;
    r.backslash = r.end > 0 && s[r.end - 1] == '\\';
    return r;
}

/* the change of the block depth made by a macro code line (p is after the '#') */
static int code_delta(const char *p) {
    CodeScan sc;
    int d = 0;
    p = skipb(p);
    if (*p == '}') d--;
    sc = code_scan(p);
    if (sc.end > 0 && p[sc.end - 1] == '{') d++;
    return d;
}

/* ---- model statement fields ---------------------------------------------------------- */

/* the text with the inside of every &{...} replaced by 'x', so that blanks and quotes there do not
 * disturb the scan for fields */
static char *mask_braces(const char *s) {
    char *m = xstrdup(s);
    size_t i;
    for (i = 0; s[i]; i++) {
        if (s[i] == '&' && s[i + 1] == '{') {
            size_t j = i + 2;
            int depth = 1;
            while (s[j]) {
                if (s[j] == '"') {
                    size_t k = j + 1;
                    while (s[k] && s[k] != '"') {
                        if (s[k] == '\\' && s[k + 1]) k++;
                        k++;
                    }
                    if (!s[k]) break;
                    j = k;
                } else if (s[j] == '{') {
                    depth++;
                } else if (s[j] == '}' && --depth == 0) {
                    break;
                }
                j++;
            }
            if (s[j] == '}') {
                size_t k;
                for (k = i + 2; k < j; k++) m[k] = 'x';
                i = j;
            }
        } else if (s[i] == '&' && s[i + 1] == '&') {
            i++;
        }
    }
    return m;
}

typedef struct {
    size_t ls, le;   /* label (without the colon) */
    size_t os, oe;   /* operation */
    size_t ps, pe;   /* operands */
    size_t rs;       /* remark, == strlen when none */
    int has_label;
} Fields;

static void model_fields(const char *line, int is_cont, Fields *f) {
    char *m = mask_braces(line);
    size_t n = strlen(line), p = 0, q, end;
    memset(f, 0, sizeof *f);
    f->rs = n;
    while (is_blank((unsigned char)m[p])) p++;
    if (!is_cont) {
        int anchor = m[p] == '^'; /* "^NAME op": NAME is a label as if it stood in column 1 */
        if (anchor) p++;
        q = p;
        while (m[q] && !is_blank((unsigned char)m[q])) q++;
        if (anchor && q > p) {
            f->has_label = 1;
            f->ls = p;
            f->le = m[q - 1] == ':' && q - p >= 2 ? q - 1 : q;
            p = q;
            while (is_blank((unsigned char)m[p])) p++;
            q = p;
            while (m[q] && !is_blank((unsigned char)m[q])) q++;
        } else if (anchor) {
            while (is_blank((unsigned char)m[p])) p++;
            q = p;
            while (m[q] && !is_blank((unsigned char)m[q])) q++;
        } else if (q - p >= 2 && m[q - 1] == ':') {
            f->has_label = 1;
            f->ls = p;
            f->le = q - 1;
            p = q;
            while (is_blank((unsigned char)m[p])) p++;
            q = p;
            while (m[q] && !is_blank((unsigned char)m[q])) q++;
        }
        f->os = p;
        f->oe = q;
        p = q;
        while (is_blank((unsigned char)m[p])) p++;
    }
    f->ps = f->pe = p;
    if (m[p]) {
        if (scan_operand_end(m, p, &end))
            f->pe = end;
        else
            f->pe = n;
        p = f->pe;
        while (is_blank((unsigned char)m[p])) p++;
        f->rs = p;
    }
    free(m);
}

/* does the model statement line continue on the next line (operands end in a comma)? */
static int model_continues(const char *line, int is_cont) {
    Fields f;
    model_fields(line, is_cont, &f);
    return f.pe > f.ps && line[f.pe - 1] == ',';
}

/* ---- the collector -------------------------------------------------------------------- */

int is_builtin_name(const char *n) {
    return !strcmp(n, "__file__") || !strcmp(n, "__line__") || !strcmp(n, "__goff__") ||
           !strcmp(n, "__elf__") || !strcmp(n, "__ascii__") || !strcmp(n, "__ebcdic__") ||
           !strcmp(n, "__amode__") || !strcmp(n, "__rmode__");
}

int mac_top_kind(const char *t) {
    const char *p;
    size_t n, i;
    static const char *const stmts[] = {"let",  "global", "while", "for",   "switch", "emit",
                                        "insert", "note", "goto",   "label", "break", "continue", "return"};
    static const char *const dirs[] = {"define", "elif",    "else",    "endif", "error", "ifdef",
                                       "ifndef", "include", "pragma",  "undef", "warning"};
    if (t[0] != '#') return 0;
    p = skipb(t + 1);
    n = ident_len(p);
    if (n == 0) return 0;
    if (n == 5 && !strncmp(p, "macro", 5) && !za_is_idc((unsigned char)p[n])) return 1;
    if (n == 2 && !strncmp(p, "if", 2)) return code_delta(t + 1) > 0 ? 2 : 0;
    for (i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
        if (strlen(dirs[i]) == n && !strncmp(dirs[i], p, n)) return 0;
    for (i = 0; i < sizeof stmts / sizeof stmts[0]; i++)
        if (strlen(stmts[i]) == n && !strncmp(stmts[i], p, n)) {
            const char *q = skipb(p + n);
            /* "#return" and friends alone, or with a comment; "#label" etc. take an operand */
            (void)q;
            return 2;
        }
    /* an assignment: name [ [..] ] op */
    {
        const char *q = skipb(p + n);
        while (*q == '[') {
            long j = 0;
            int d = 0;
            for (;; j++) {
                if (!q[j]) return 0;
                if (q[j] == '[') d++;
                if (q[j] == ']' && --d == 0) break;
            }
            q = skipb(q + j + 1);
        }
        if (q[0] == '=' && q[1] != '=') return 2;
        if ((q[0] == '+' && q[1] == '+') || (q[0] == '-' && q[1] == '-')) return 2;
        if (q[0] && strchr("+-*/%&|^", q[0]) && q[1] == '=') return 2;
        if ((q[0] == '<' && q[1] == '<' && q[2] == '=') || (q[0] == '>' && q[1] == '>' && q[2] == '='))
            return 2;
    }
    return 0;
}

static MacLine make_line(int line, const char *text) {
    MacLine l;
    l.line = line;
    l.text = xstrdup(text);
    return l;
}

size_t mac_collect(MacSys *ms, const SrcLine *lines, size_t n, size_t i, int kind, MacText *out) {
    size_t k = i;
    int depth = 0;
    (void)ms;
    memset(out, 0, sizeof *out);
    out->file = lines[i].file;
    out->is_def = kind == 1;
    while (k < n) {
        const char *t = lines[k].text;
        const char *p = skipb(t);
        int line = lines[k].line;
        Buf b = {0};
        buf_puts(&b, t);
        k++;
        if (*p == '#') {
            for (;;) {
                const char *cur = buf_cstr(&b);
                CodeScan sc = code_scan(skipb(cur) + 1);
                if (sc.backslash && k < n) {
                    b.len = (size_t)(skipb(cur) + 1 - cur) + sc.end - 1;
                    buf_putc(&b, ' ');
                    buf_puts(&b, skipb(lines[k++].text));
                } else if ((sc.open > 0 || sc.in_string) && k < n) {
                    buf_putc(&b, ' ');
                    buf_puts(&b, skipb(lines[k++].text));
                } else {
                    break;
                }
            }
            depth += code_delta(skipb(buf_cstr(&b)) + 1);
            vec_push(&out->lines, make_line(line, buf_cstr(&b)));
            free(b.p);
            if (depth <= 0) break;
        } else if (*p == 0 || (p[0] == '/' && p[1] == '/')) {
            /* blank lines and macro comments are dropped */
        } else if (*p == '*') {
            vec_push(&out->lines, make_line(line, t));
            free(b.p);
        } else {
            int cont = 0;
            while (model_continues(cont ? lines[k - 1].text : t, cont) && k < n) {
                buf_putc(&b, '\n');
                buf_puts(&b, lines[k++].text);
                cont = 1;
            }
            vec_push(&out->lines, make_line(line, buf_cstr(&b)));
            free(b.p);
        }
    }
    if (depth > 0 && out->lines.n > 0)
        diag_help(rep_errorf(ms->rep, "ZA4100", span_make(out->file, out->lines.v[0].line, 1, 1),
                             "a block is not closed: #} is missing before the end of the file"),
                  "every line ending in { needs a matching #}");
    return k - i;
}

/* ---- compiler state --------------------------------------------------------------------- */

typedef enum { B_MACRO, B_ROOT, B_IF, B_WHILE, B_FOR, B_ITER, B_SWITCH } BKind;

typedef struct {
    BKind kind;
    int hdepth, bdepth; /* depth of the header statements and of the body */
    int bid;
    int pending;        /* a JFALSE or CASE statement that waits for its target */
    Vec(int) ends, brk, cont;
    int start;          /* loop test */
    int has_else, ncase, sawdefault;
    MStmt step;         /* for: the step statement */
    int has_step;
    char *sw; /* switch: the hidden variable that holds the value */
    int line, col;
} Blk;

typedef struct {
    char *name;
    int idx;
} GotoLabel;

typedef struct {
    MacSys *ms;
    const char *file;
    int is_def, ok, closed, nhidden;
    Vec(MStmt) st;
    Vec(Blk) blk;
    Vec(int) bpar;
    Vec(GotoLabel) labels;
    Vec(int) gotos;
    Macro *mac;
} Cmp;

static Diag *cerr(Cmp *cp, const MacLine *ml, int col, const char *code, const char *fmt, ...) {
    va_list ap;
    char *m;
    va_start(ap, fmt);
    m = xvasprintf(fmt, ap);
    va_end(ap);
    cp->ok = 0;
    return rep_errorf(cp->ms->rep, code, span_make(cp->file, ml->line, col, 1), "%s", m);
}

static Blk *top(Cmp *cp) { return &cp->blk.v[cp->blk.n - 1]; }

static int new_bid(Cmp *cp, int parent) {
    vec_push(&cp->bpar, parent);
    return (int)cp->bpar.n - 1;
}

static int emit_st(Cmp *cp, MStmt *s) {
    vec_push(&cp->st, *s);
    return (int)cp->st.n - 1;
}

static void st_init(Cmp *cp, MStmt *s, const MacLine *ml, const char *t, const char *p, SKind k) {
    memset(s, 0, sizeof *s);
    s->kind = k;
    s->file = cp->file;
    s->line = ml->line;
    s->col = (int)(p - t) + 1;
    s->len = (int)strlen(p) > 0 ? (int)strlen(p) : 1;
    if (s->len > 60) s->len = 60;
    s->depth = top(cp)->bdepth;
    s->block = top(cp)->bid;
    s->target = -1;
}

static int begin_stmt(Cmp *cp, const MacLine *ml, int col) {
    Blk *b = top(cp);
    if (cp->closed) {
        cerr(cp, ml, col, "ZA4100", "text after the end of the macro");
        return 0;
    }
    if (b->kind == B_SWITCH && b->ncase == 0 && !b->sawdefault) {
        cerr(cp, ml, col, "ZA4100", "a statement in #switch must follow #case or #default");
        return 0;
    }
    return 1;
}

/* ---- template compilation (&name substitution) ---------------------------------------------- */

typedef Vec(Part) PartVec;

static void part_text(PartVec *v, Buf *lit) {
    Part p;
    if (lit->len == 0) return;
    p.text = xstrndup(lit->p, lit->len);
    p.e = NULL;
    vec_push(v, p);
    lit->len = 0;
}

/* index of the closer matching the opener at s[i] ('{' or '['), skipping strings; -1 if none */
static long match_close(const char *s, size_t i, char open, char close) {
    int d = 0;
    size_t j;
    for (j = i; s[j]; j++) {
        if (s[j] == '"') {
            size_t k = j + 1;
            while (s[k] && s[k] != '"') {
                if (s[k] == '\\' && s[k + 1]) k++;
                k++;
            }
            if (!s[k]) return -1;
            j = k;
        } else if (s[j] == open) {
            d++;
        } else if (s[j] == close && --d == 0) {
            return (long)j;
        }
    }
    return -1;
}

static Tmpl tmpl_compile(Cmp *cp, const MacLine *ml, const char *line, size_t from, size_t to) {
    PartVec parts = {0};
    Buf lit = {0};
    size_t i = from;
    Tmpl t;
    while (i < to) {
        char c = line[i];
        if (c != '&') {
            buf_putc(&lit, c);
            i++;
            continue;
        }
        if (i + 1 < to && line[i + 1] == '&') {
            buf_putc(&lit, '&');
            i += 2;
            continue;
        }
        if (i + 1 < to && line[i + 1] == '{') {
            long e = match_close(line, i + 1, '{', '}');
            Part p;
            Parser ps;
            char *inner;
            if (e < 0 || (size_t)e >= to) {
                cerr(cp, ml, (int)i + 1, "ZA4100", "&{ is not closed by }");
                i = to;
                break;
            }
            inner = xstrndup(line + i + 2, (size_t)e - i - 2);
            ps_init(&ps, cp->ms, cp->file, ml->line, (int)i + 3, inner);
            p.e = ps_expr(&ps);
            if (ps.tk != TK_EOF && !ps.err) ps_error(&ps, "ZA4100", "unexpected text in &{...}");
            if (ps.err) cp->ok = 0;
            p.text = NULL;
            part_text(&parts, &lit);
            vec_push(&parts, p);
            i = (size_t)e + 1;
            continue;
        }
        {
            size_t n = ident_len(line + i + 1), j;
            Part p;
            MExpr *x;
            char *name;
            if (n == 0 || i + 1 + n > to) {
                cerr(cp, ml, (int)i + 1, "ZA4110", "& must be followed by a variable name, { or &");
                buf_putc(&lit, '&');
                i++;
                continue;
            }
            j = i + 1 + n;
            if (n == 3 && !za_strncmpi(line + i + 1, "sys", 3) && line[j] == '.' &&
                za_is_idc((unsigned char)line[j + 1])) {
                j++;
                while (j < to && za_is_idc((unsigned char)line[j])) j++;
            }
            name = za_strlower_own(xstrndup(line + i + 1, j - i - 1));
            x = xcalloc(sizeof *x);
            if (!strncmp(name, "sys.", 4)) {
                x->k = X_SYS;
                x->s = xstrdup(name + 4);
                free(name);
            } else if (is_builtin_name(name)) {
                x->k = X_SYS;
                x->s = name;
            } else {
                x->k = X_VAR;
                x->s = name;
                x->i = 1; /* from &name: the error is ZA4110 */
            }
            while (j < to && line[j] == '[') {
                long e = match_close(line, j, '[', ']');
                MExpr *ix = xcalloc(sizeof *ix);
                Parser ps;
                char *inner;
                if (e < 0 || (size_t)e >= to) {
                    cerr(cp, ml, (int)j + 1, "ZA4100", "[ is not closed by ]");
                    break;
                }
                inner = xstrndup(line + j + 1, (size_t)e - j - 1);
                ps_init(&ps, cp->ms, cp->file, ml->line, (int)j + 2, inner);
                ix->k = X_INDEX;
                ix->a = x;
                ix->b = ps_expr(&ps);
                if (ps.tk != TK_EOF && !ps.err) ps_error(&ps, "ZA4100", "unexpected text in [...]");
                if (ps.err) cp->ok = 0;
                x = ix;
                j = (size_t)e + 1;
            }
            part_text(&parts, &lit);
            p.text = NULL;
            p.e = x;
            vec_push(&parts, p);
            i = j;
        }
    }
    part_text(&parts, &lit);
    free(lit.p);
    t.p = parts.v;
    t.n = (int)parts.n;
    return t;
}

static char *trim_dup(const char *s, size_t n) {
    while (n > 0 && is_blank((unsigned char)*s)) {
        s++;
        n--;
    }
    while (n > 0 && is_blank((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

static void cp_model(Cmp *cp, const MacLine *ml) {
    const char *t = ml->text, *nl;
    char *first;
    Fields f;
    MStmt s;
    Model *m = xcalloc(sizeof *m);
    Vec(ContLine) conts = {0};
    const char *p = skipb(t);
    if (!begin_stmt(cp, ml, (int)(p - t) + 1)) return;
    st_init(cp, &s, ml, t, p, S_MODEL);
    s.model = m;
    nl = strchr(t, '\n');
    first = nl ? xstrndup(t, (size_t)(nl - t)) : xstrdup(t);
    if (*p == '*') { /* a comment line, emitted as written */
        m->comment = xstrdup(p);
        free(first);
        emit_st(cp, &s);
        return;
    }
    model_fields(first, 0, &f);
    if (f.has_label) m->label = tmpl_compile(cp, ml, first, f.ls, f.le);
    m->op = tmpl_compile(cp, ml, first, f.os, f.oe);
    m->opd = tmpl_compile(cp, ml, first, f.ps, f.pe);
    if (f.rs < strlen(first)) m->remark = trim_dup(first + f.rs, strlen(first) - f.rs);
    while (nl) {
        const char *ls = nl + 1;
        const char *e = strchr(ls, '\n');
        char *line = e ? xstrndup(ls, (size_t)(e - ls)) : xstrdup(ls);
        ContLine c;
        Fields cf;
        model_fields(line, 1, &cf);
        c.ops = tmpl_compile(cp, ml, line, cf.ps, cf.pe);
        c.remark = cf.rs < strlen(line) ? trim_dup(line + cf.rs, strlen(line) - cf.rs) : NULL;
        vec_push(&conts, c);
        free(line);
        nl = e;
    }
    free(first);
    m->conts = conts.v;
    m->nconts = (int)conts.n;
    emit_st(cp, &s);
}

/* ---- statement parsing -------------------------------------------------------------------- */

static int assign_op(int tk) {
    switch (tk) {
    case TK_ADDEQ: return '+';
    case TK_SUBEQ: return '-';
    case TK_MULEQ: return '*';
    case TK_DIVEQ: return '/';
    case TK_MODEQ: return '%';
    case TK_SHLEQ: return TK_SHL;
    case TK_SHREQ: return TK_SHR;
    case TK_ANDEQ: return '&';
    case TK_OREQ: return '|';
    case TK_XOREQ: return '^';
    }
    return -1;
}

static MExpr *int_expr(int64_t v) {
    MExpr *e = xcalloc(sizeof *e);
    e->k = X_INT;
    e->i = v;
    return e;
}

/* name [idx]* (= | op= | ++ | --) expr; fills s (ASSIGN) */
static int parse_assign(Cmp *cp, Parser *ps, MStmt *s) {
    Vec(MExpr *) idx = {0};
    if (ps->tk != TK_ID) {
        ps_error(ps, "ZA4100", "a variable name is expected");
        return 0;
    }
    s->kind = S_ASSIGN;
    s->name = ps->ts;
    ps_next(ps);
    while (ps->tk == '[') {
        ps_next(ps);
        vec_push(&idx, ps_expr(ps));
        if (ps->tk != ']') {
            ps_error(ps, "ZA4100", "missing ']'");
            return 0;
        }
        ps_next(ps);
    }
    s->idx = idx.v;
    s->nidx = (int)idx.n;
    if (ps->tk == '=') {
        s->aop = 0;
        ps_next(ps);
        s->e = ps_expr(ps);
    } else if (ps->tk == TK_INC || ps->tk == TK_DEC) {
        s->aop = ps->tk == TK_INC ? '+' : '-';
        s->e = int_expr(1);
        ps_next(ps);
    } else if (assign_op(ps->tk) >= 0) {
        s->aop = assign_op(ps->tk);
        ps_next(ps);
        s->e = ps_expr(ps);
    } else {
        ps_error(ps, "ZA4100", "an assignment (=, +=, ++ ...) is expected after the variable");
        return 0;
    }
    (void)cp;
    return !ps->err;
}

static int expect_eol(Cmp *cp, Parser *ps, const MacLine *ml) {
    (void)ml;
    if (ps->tk != TK_EOF && !ps->err) {
        ps_error(ps, "ZA4100", "unexpected text at the end of the statement");
    }
    if (ps->err) cp->ok = 0;
    return !ps->err;
}

static int expect_brace(Parser *ps) {
    if (ps->tk != '{') {
        ps_error(ps, "ZA4100", "a '{' is expected at the end of the line");
        return 0;
    }
    ps_next(ps);
    return 1;
}

static void patch(Cmp *cp, int idx, int target) {
    if (idx >= 0) cp->st.v[idx].target = target;
}

static Blk *push_blk(Cmp *cp, BKind k, int hdepth, int bdepth, int parent_bid, const MacLine *ml,
                     int col) {
    Blk b;
    memset(&b, 0, sizeof b);
    b.kind = k;
    b.hdepth = hdepth;
    b.bdepth = bdepth;
    b.bid = new_bid(cp, parent_bid);
    b.pending = -1;
    b.start = -1;
    b.line = ml->line;
    b.col = col;
    vec_push(&cp->blk, b);
    return top(cp);
}

static void close_blk(Cmp *cp) {
    Blk *b = top(cp);
    size_t i;
    int n;
    MStmt j;
    memset(&j, 0, sizeof j);
    j.kind = S_JUMP;
    j.file = cp->file;
    j.line = b->line;
    j.col = b->col;
    j.len = 1;
    j.depth = b->bdepth;
    j.block = b->bid;
    switch (b->kind) {
    case B_IF:
        n = (int)cp->st.n;
        patch(cp, b->pending, n);
        for (i = 0; i < b->ends.n; i++) patch(cp, b->ends.v[i], n);
        break;
    case B_WHILE:
    case B_FOR:
    case B_ITER: {
        int cont_target = b->start;
        if (b->kind == B_FOR && b->has_step) {
            MStmt st = b->step;
            st.depth = b->hdepth;
            cont_target = emit_st(cp, &st);
        }
        j.target = b->start;
        emit_st(cp, &j);
        n = (int)cp->st.n;
        patch(cp, b->pending, n);
        for (i = 0; i < b->brk.n; i++) patch(cp, b->brk.v[i], n);
        for (i = 0; i < b->cont.n; i++) patch(cp, b->cont.v[i], cont_target);
        break;
    }
    case B_SWITCH:
        n = (int)cp->st.n;
        patch(cp, b->pending, n);
        for (i = 0; i < b->ends.n; i++) patch(cp, b->ends.v[i], n);
        for (i = 0; i < b->brk.n; i++) patch(cp, b->brk.v[i], n);
        break;
    default:
        break;
    }
    cp->blk.n--;
}

static int jump_st(Cmp *cp, const MacLine *ml, const char *t, const char *p) {
    MStmt s;
    st_init(cp, &s, ml, t, p, S_JUMP);
    return emit_st(cp, &s);
}

/* #if (c) {  and  } else if (c) {  : returns the pending JFALSE index */
static int cond_jfalse(Cmp *cp, const MacLine *ml, const char *t, const char *p, Parser *ps,
                       int depth) {
    MStmt s;
    st_init(cp, &s, ml, t, p, S_JFALSE);
    s.depth = depth;
    s.e = ps_cond_paren(ps);
    if (!expect_brace(ps)) return -1;
    expect_eol(cp, ps, ml);
    return emit_st(cp, &s);
}

static void cp_close(Cmp *cp, const MacLine *ml, const char *t, const char *p) {
    /* p is at the '}' */
    const char *q = skipb(p + 1);
    int col = (int)(p - t) + 1;
    Blk *b;
    if (cp->closed) {
        cerr(cp, ml, col, "ZA4100", "text after the end of the macro");
        return;
    }
    b = top(cp);
    if (b->kind == B_ROOT) {
        cerr(cp, ml, col, "ZA4100", "#} without an open block");
        return;
    }
    if (!strncmp(q, "else", 4) && !za_is_idc((unsigned char)q[4])) {
        const char *r = skipb(q + 4);
        Parser ps;
        int jidx;
        if (b->kind != B_IF || b->has_else) {
            cerr(cp, ml, col, "ZA4100", "else without a matching #if block");
            return;
        }
        {
            MStmt j;
            st_init(cp, &j, ml, t, p, S_JUMP);
            j.depth = b->bdepth;
            jidx = emit_st(cp, &j);
            vec_push(&b->ends, jidx);
        }
        patch(cp, b->pending, (int)cp->st.n);
        b->pending = -1;
        b->bid = new_bid(cp, cp->bpar.v[b->bid]);
        if (!strncmp(r, "if", 2) && !za_is_idc((unsigned char)r[2])) {
            ps_init(&ps, cp->ms, cp->file, ml->line, (int)(r + 2 - t) + 1, r + 2);
            b->pending = cond_jfalse(cp, ml, t, p, &ps, b->hdepth);
            b = top(cp);
        } else if (*r == '{' && *skipb(r + 1) == 0) {
            b->has_else = 1;
        } else if (*r == '{' && skipb(r + 1)[0] == '/' && skipb(r + 1)[1] == '/') {
            b->has_else = 1;
        } else {
            cerr(cp, ml, (int)(r - t) + 1, "ZA4100", "\"} else\" must be followed by { or if (...) {");
        }
        /* the new branch belongs to a fresh block id */
        return;
    }
    if (*q && !(q[0] == '/' && q[1] == '/')) {
        cerr(cp, ml, (int)(q - t) + 1, "ZA4100", "unexpected text after #}");
        return;
    }
    if (b->kind == B_MACRO) {
        cp->closed = 1;
        return;
    }
    close_blk(cp);
}

static Blk *find_loop(Cmp *cp, int want_loop) {
    size_t i;
    for (i = cp->blk.n; i > 0; i--) {
        Blk *b = &cp->blk.v[i - 1];
        if (b->kind == B_WHILE || b->kind == B_FOR || b->kind == B_ITER) return b;
        if (b->kind == B_SWITCH && !want_loop) return b;
    }
    return NULL;
}

static void cp_for(Cmp *cp, const MacLine *ml, const char *t, const char *p, Parser *ps) {
    Blk *parent = top(cp), *b;
    int hd = parent->bdepth + 1, bd = parent->bdepth + 2, pbid = parent->bid;
    MStmt s;
    char *var = NULL;
    MExpr *list = NULL, *init = NULL, *cond = NULL;
    MStmt step, initst;
    int has_step = 0, has_init_assign = 0, is_iter = 0;
    memset(&step, 0, sizeof step);
    memset(&initst, 0, sizeof initst);
    if (ps->tk != '(') {
        ps_error(ps, "ZA4100", "#for needs (...)");
        cp->ok = 0;
        return;
    }
    ps_next(ps);
    if (ps->tk == TK_ID && !strcmp(ps->ts, "let")) {
        ps_next(ps);
        if (ps->tk != TK_ID) {
            ps_error(ps, "ZA4100", "a variable name is expected after let");
            cp->ok = 0;
            return;
        }
        var = ps->ts;
        ps_next(ps);
        if (ps->tk == ':') {
            ps_next(ps);
            list = ps_expr(ps);
            is_iter = 1;
        } else if (ps->tk == '=') {
            ps_next(ps);
            init = ps_expr(ps);
        } else {
            ps_error(ps, "ZA4100", "= or : is expected after the loop variable");
        }
    } else if (ps->tk != ';') {
        st_init(cp, &initst, ml, t, p, S_ASSIGN);
        parse_assign(cp, ps, &initst);
        has_init_assign = 1;
    }
    if (!is_iter) {
        if (ps->tk != ';') {
            ps_error(ps, "ZA4100", "; is expected in #for (init; cond; step)");
        } else {
            ps_next(ps);
            if (ps->tk != ';') cond = ps_expr(ps);
            if (ps->tk != ';') {
                ps_error(ps, "ZA4100", "; is expected in #for (init; cond; step)");
            } else {
                ps_next(ps);
                if (ps->tk != ')') {
                    st_init(cp, &step, ml, t, p, S_ASSIGN);
                    parse_assign(cp, ps, &step);
                    has_step = 1;
                }
            }
        }
    }
    if (ps->tk != ')') {
        if (!ps->err) ps_error(ps, "ZA4100", "missing ')' in #for");
    } else {
        ps_next(ps);
    }
    if (!expect_brace(ps) || !expect_eol(cp, ps, ml) || ps->err) {
        cp->ok = 0;
        return;
    }
    b = push_blk(cp, is_iter ? B_ITER : B_FOR, hd, bd, pbid, ml, (int)(p - t) + 1);
    if (is_iter) {
        char *hl = xasprintf("%%l%d", cp->nhidden), *hi = xasprintf("%%i%d", cp->nhidden);
        cp->nhidden++;
        st_init(cp, &s, ml, t, p, S_LET);
        s.depth = hd - 1;
        s.vdepth = hd;
        s.block = pbid;
        s.name = hl;
        s.e = list;
        emit_st(cp, &s);
        st_init(cp, &s, ml, t, p, S_LET);
        s.depth = hd;
        s.vdepth = hd;
        s.block = pbid;
        s.name = hi;
        s.e = int_expr(0);
        emit_st(cp, &s);
        b = top(cp);
        b->start = (int)cp->st.n;
        st_init(cp, &s, ml, t, p, S_ITER);
        s.depth = hd;
        s.block = pbid;
        s.name = var;
        s.hl = hl;
        s.hi = hi;
        s.vdepth = bd;
        b->pending = emit_st(cp, &s);
    } else {
        if (var) {
            st_init(cp, &s, ml, t, p, S_LET);
            s.depth = hd - 1;
            s.vdepth = hd;
            s.block = pbid;
            s.name = var;
            s.e = init;
            emit_st(cp, &s);
        } else if (has_init_assign) {
            initst.depth = hd - 1;
            initst.block = pbid;
            emit_st(cp, &initst);
        }
        b = top(cp);
        b->start = (int)cp->st.n;
        if (cond) {
            st_init(cp, &s, ml, t, p, S_JFALSE);
            s.depth = hd;
            s.block = pbid;
            s.e = cond;
            b->pending = emit_st(cp, &s);
        }
        b->step = step;
        b->has_step = has_step;
    }
}

static void cp_code(Cmp *cp, const MacLine *ml, const char *t, const char *hash) {
    const char *p = skipb(hash + 1);
    size_t n = ident_len(p);
    int col = (int)(p - t) + 1;
    Parser ps;
    MStmt s;
    Blk *b;
    if (*p == '}') {
        cp_close(cp, ml, t, p);
        return;
    }
    if (n == 0) {
        cerr(cp, ml, (int)(hash - t) + 1, "ZA4100", "a macro statement is expected after #");
        return;
    }
#define KW(k) (n == strlen(k) && !za_strncmpi(p, k, n))
#define INIT_PS() ps_init(&ps, cp->ms, cp->file, ml->line, (int)(p + n - t) + 1, p + n)
    if (!(KW("case") || KW("default")) && !begin_stmt(cp, ml, col)) return;
    if (KW("macro")) {
        cerr(cp, ml, col, "ZA4100", "#macro is not allowed inside a macro or a block");
        return;
    }
    if (KW("let") || KW("global")) {
        int is_global = KW("global");
        INIT_PS();
        st_init(cp, &s, ml, t, p, is_global ? S_GLOBAL : S_LET);
        if (ps.tk != TK_ID) {
            ps_error(&ps, "ZA4100", "a variable name is expected after #%s", is_global ? "global" : "let");
            cp->ok = 0;
            return;
        }
        if (is_reserved(ps.ts)) {
            ps_error(&ps, "ZA4100", "\"%s\" is a keyword and cannot be a variable", ps.ts);
            cp->ok = 0;
            return;
        }
        s.name = ps.ts;
        ps_next(&ps);
        if (ps.tk == '=') {
            ps_next(&ps);
            s.e = ps_expr(&ps);
        } else {
            s.e = xcalloc(sizeof *s.e);
            s.e->k = X_STR;
            s.e->s = xstrdup("");
        }
        if (expect_eol(cp, &ps, ml)) emit_st(cp, &s);
        return;
    }
    if (KW("if")) {
        Blk *parent = top(cp);
        int hd = parent->bdepth, pbid = parent->bid, jidx;
        INIT_PS();
        jidx = cond_jfalse(cp, ml, t, p, &ps, hd);
        if (jidx < 0) {
            cp->ok = 0;
            return;
        }
        b = push_blk(cp, B_IF, hd, hd + 1, pbid, ml, col);
        b->pending = jidx;
        return;
    }
    if (KW("while")) {
        Blk *parent = top(cp);
        int hd = parent->bdepth, pbid = parent->bid, jidx;
        INIT_PS();
        st_init(cp, &s, ml, t, p, S_JFALSE);
        s.e = ps_cond_paren(&ps);
        if (!expect_brace(&ps) || !expect_eol(cp, &ps, ml)) {
            cp->ok = 0;
            return;
        }
        jidx = emit_st(cp, &s);
        b = push_blk(cp, B_WHILE, hd, hd + 1, pbid, ml, col);
        b->pending = jidx;
        b->start = jidx;
        return;
    }
    if (KW("for")) {
        INIT_PS();
        cp_for(cp, ml, t, p, &ps);
        return;
    }
    if (KW("switch")) {
        Blk *parent = top(cp);
        int hd = parent->bdepth + 1, pbid = parent->bid;
        char *sw = xasprintf("%%s%d", cp->nhidden++);
        INIT_PS();
        st_init(cp, &s, ml, t, p, S_LET);
        s.depth = parent->bdepth;
        s.vdepth = hd;
        s.name = sw;
        s.e = ps_cond_paren(&ps);
        if (!expect_brace(&ps) || !expect_eol(cp, &ps, ml)) {
            cp->ok = 0;
            return;
        }
        emit_st(cp, &s);
        b = push_blk(cp, B_SWITCH, hd, hd + 1, pbid, ml, col);
        b->bdepth = parent->bdepth + 2;
        b->hdepth = hd;
        b->start = -1;
        b->sw = sw;
        return;
    }
    if (KW("case") || KW("default")) {
        int is_case = KW("case");
        MStmt cs;
        Vec(MExpr *) cases = {0};
        b = top(cp);
        if (b->kind != B_SWITCH) {
            cerr(cp, ml, col, "ZA4100", "#%s outside #switch", is_case ? "case" : "default");
            return;
        }
        if (b->sawdefault) {
            cerr(cp, ml, col, "ZA4100", "#%s after #default", is_case ? "case" : "default");
            return;
        }
        INIT_PS();
        if (is_case) {
            for (;;) {
                vec_push(&cases, ps_expr(&ps));
                if (ps.err || ps.tk != ',') break;
                ps_next(&ps);
            }
        }
        if (ps.tk != ':') {
            if (!ps.err) ps_error(&ps, "ZA4100", "a ':' is expected after #%s", is_case ? "case" : "default");
            cp->ok = 0;
            return;
        }
        ps_next(&ps);
        if (!expect_eol(cp, &ps, ml)) return;
        if (b->ncase > 0 || b->sawdefault) {
            int j;
            st_init(cp, &s, ml, t, p, S_JUMP);
            s.depth = b->bdepth;
            j = emit_st(cp, &s);
            vec_push(&b->ends, j);
        }
        patch(cp, b->pending, (int)cp->st.n);
        b->pending = -1;
        b->bid = new_bid(cp, cp->bpar.v[b->bid]);
        if (is_case) {
            char *swname = b->sw;
            st_init(cp, &cs, ml, t, p, S_CASE);
            cs.depth = b->hdepth;
            cs.block = b->bid;
            cs.hl = swname;
            cs.cases = cases.v;
            cs.ncases = (int)cases.n;
            b->pending = emit_st(cp, &cs);
            b->ncase++;
        } else {
            b->sawdefault = 1;
        }
        return;
    }
    if (KW("break") || KW("continue")) {
        int is_break = KW("break"), j;
        Blk *l = find_loop(cp, !is_break);
        INIT_PS();
        if (!expect_eol(cp, &ps, ml)) return;
        if (!l) {
            cerr(cp, ml, col, "ZA4100", "#%s outside a loop%s", is_break ? "break" : "continue",
                 is_break ? " or #switch" : "");
            return;
        }
        j = jump_st(cp, ml, t, p);
        vec_push(is_break ? &l->brk : &l->cont, j);
        return;
    }
    if (KW("return")) {
        INIT_PS();
        if (!expect_eol(cp, &ps, ml)) return;
        if (!cp->is_def) {
            cerr(cp, ml, col, "ZA4100", "#return outside a macro");
            return;
        }
        st_init(cp, &s, ml, t, p, S_RETURN);
        emit_st(cp, &s);
        return;
    }
    if (KW("goto") || KW("label")) {
        int is_goto = KW("goto");
        INIT_PS();
        st_init(cp, &s, ml, t, p, is_goto ? S_JUMP : S_NOP);
        if (ps.tk != TK_ID) {
            ps_error(&ps, "ZA4100", "a label name is expected");
            cp->ok = 0;
            return;
        }
        s.name = ps.ts;
        ps_next(&ps);
        if (!expect_eol(cp, &ps, ml)) return;
        if (is_goto) {
            vec_push(&cp->gotos, (int)cp->st.n);
        } else {
            GotoLabel lb;
            size_t i;
            for (i = 0; i < cp->labels.n; i++)
                if (!strcmp(cp->labels.v[i].name, s.name)) {
                    cerr(cp, ml, col, "ZA4113", "label \"%s\" is defined twice", s.name);
                    return;
                }
            lb.name = s.name;
            lb.idx = (int)cp->st.n;
            vec_push(&cp->labels, lb);
        }
        emit_st(cp, &s);
        return;
    }
    if (KW("insert")) {
        INIT_PS();
        st_init(cp, &s, ml, t, p, S_INSERT);
        s.e = ps_expr(&ps);
        s.target = 0;
        if (!ps.err && ps.tk == ',') {
            ps_next(&ps);
            if (ps.tk == TK_ID && !strcmp(ps.ts, "front")) {
                s.target = 1;
            } else if (!(ps.tk == TK_ID && !strcmp(ps.ts, "back"))) {
                ps_error(&ps, "ZA4103", "#insert: \"front\" or \"back\" is expected");
                cp->ok = 0;
                return;
            }
            ps_next(&ps);
        }
        if (expect_eol(cp, &ps, ml)) emit_st(cp, &s);
        return;
    }
    if (KW("emit") || KW("note") || KW("warning") || KW("error")) {
        SKind k = KW("emit") ? S_EMIT : KW("note") ? S_NOTE : KW("warning") ? S_WARN : S_ERROR;
        INIT_PS();
        st_init(cp, &s, ml, t, p, k);
        s.e = ps_expr(&ps);
        if (expect_eol(cp, &ps, ml)) emit_st(cp, &s);
        return;
    }
    /* an assignment */
    ps_init(&ps, cp->ms, cp->file, ml->line, (int)(p - t) + 1, p);
    st_init(cp, &s, ml, t, p, S_ASSIGN);
    if (!parse_assign(cp, &ps, &s) || !expect_eol(cp, &ps, ml)) {
        cp->ok = 0;
        return;
    }
    emit_st(cp, &s);
#undef KW
#undef INIT_PS
}

static void cp_line(Cmp *cp, const MacLine *ml) {
    const char *t = ml->text, *p = skipb(t);
    if (*p == 0) return;
    if (p[0] == '/' && p[1] == '/') return;
    if (*p == '#') {
        cp_code(cp, ml, t, p);
        return;
    }
    if (*p != '*' && !is_blank((unsigned char)t[0])) {
        diag_help(cerr(cp, ml, 1, "ZA4104", "a statement in a macro body must not start in column 1"),
                  "indent the statement, or write \"label:\" for a label; this looks like a pasted "
                  "assembler line");
        return;
    }
    cp_model(cp, ml);
}

/* ---- compile a text --------------------------------------------------------------------------- */

static int is_asm_op(const char *name) {
    size_t n, i;
    const char *const *dn;
    char *up = za_strupper(name);
    int r = isa_lookup(name, &n) && n > 0;
    if (!r) {
        n = asm_directive_names(&dn);
        for (i = 0; i < n; i++)
            if (!strcmp(dn[i], up)) { r = 1; break; }
    }
    free(up);
    return r;
}

int mac_known_op(const char *name) { return is_asm_op(name); }

static void finish_gotos(Cmp *cp) {
    size_t i, k;
    for (i = 0; i < cp->gotos.n; i++) {
        MStmt *g = &cp->st.v[cp->gotos.v[i]];
        int found = 0;
        for (k = 0; k < cp->labels.n; k++) {
            if (!strcmp(cp->labels.v[k].name, g->name)) {
                int lb = cp->st.v[cp->labels.v[k].idx].block, b = g->block, ok = 0;
                while (b >= 0) {
                    if (b == lb) {
                        ok = 1;
                        break;
                    }
                    b = cp->bpar.v[b];
                }
                if (!ok) {
                    rep_errorf(cp->ms->rep, "ZA4113", span_make(g->file, g->line, g->col, g->len),
                               "#goto %s jumps into a block", g->name);
                    cp->ok = 0;
                }
                g->target = cp->labels.v[k].idx;
                found = 1;
                break;
            }
        }
        if (!found) {
            rep_errorf(cp->ms->rep, "ZA4113", span_make(g->file, g->line, g->col, g->len),
                       "#goto %s: there is no #label %s", g->name, g->name);
            cp->ok = 0;
        }
    }
}

static int compile_body(Cmp *cp, const MacText *t, size_t first) {
    size_t i;
    for (i = first; i < t->lines.n; i++) cp_line(cp, &t->lines.v[i]);
    if (cp->blk.n > 1 || (cp->is_def && !cp->closed)) {
        const Blk *b = top(cp);
        if (cp->is_def && cp->blk.n == 1)
            rep_errorf(cp->ms->rep, "ZA4100", span_make(cp->file, cp->blk.v[0].line, 1, 1),
                       "the macro is not closed: #} is missing");
        else
            diag_help(rep_errorf(cp->ms->rep, "ZA4100", span_make(cp->file, b->line, b->col, 1),
                                 "this block is not closed: #} is missing"),
                      "every line ending in { needs a matching #}");
        cp->ok = 0;
    }
    finish_gotos(cp);
    return cp->ok;
}

static void cmp_init(Cmp *cp, MacSys *ms, const MacText *t, int is_def) {
    memset(cp, 0, sizeof *cp);
    cp->ms = ms;
    cp->file = t->file;
    cp->is_def = is_def;
    cp->ok = 1;
    vec_push(&cp->bpar, -1);
}

/* #macro [lbl:] NAME(p, q = e, rest..., kw = e) { */
static Macro *parse_header(Cmp *cp, const MacLine *ml) {
    const char *t = ml->text, *p = skipb(t) + 1, *q;
    size_t n;
    Macro *m = xcalloc(sizeof *m);
    Parser ps;
    Vec(Param) params = {0};
    int kwonly = 0;
    p = skipb(p);
    p += 5; /* macro */
    p = skipb(p);
    q = p;
    while (is_sym_char((unsigned char)*q)) q++;
    if (q == p) {
        cerr(cp, ml, (int)(p - t) + 1, "ZA4100", "the macro name is missing");
        return NULL;
    }
    n = (size_t)(q - p);
    if (*skipb(q) == ':') {
        m->labelvar = za_strlower_own(xstrndup(p, n));
        if (ident_len(p) != n || is_reserved(m->labelvar)) {
            cerr(cp, ml, (int)(p - t) + 1, "ZA4100", "\"%s\" cannot name the name field variable", m->labelvar);
            return NULL;
        }
        p = skipb(skipb(q) + 1);
        q = p;
        while (is_sym_char((unsigned char)*q)) q++;
        if (q == p) {
            cerr(cp, ml, (int)(p - t) + 1, "ZA4100", "the macro name is missing after the label variable");
            return NULL;
        }
        n = (size_t)(q - p);
    }
    m->name = za_strupper_own(xstrndup(p, n));
    m->def = span_make(cp->file, ml->line, (int)(p - t) + 1, (int)n);
    ps_init(&ps, cp->ms, cp->file, ml->line, (int)(q - t) + 1, q);
    if (ps.tk == '(') {
        ps_next(&ps);
        while (ps.tk != ')' && ps.tk != TK_EOF && !ps.err) {
            Param pr;
            memset(&pr, 0, sizeof pr);
            if (ps.tk == '*') {
                kwonly = 1;
                ps_next(&ps);
            } else if (ps.tk == TK_ID) {
                size_t i;
                pr.name = ps.ts;
                if (is_reserved(pr.name)) {
                    ps_error(&ps, "ZA4100", "\"%s\" is a keyword and cannot be a parameter", pr.name);
                    break;
                }
                for (i = 0; i < params.n; i++)
                    if (!strcmp(params.v[i].name, pr.name))
                        ps_error(&ps, "ZA4101", "parameter \"%s\" is declared twice", pr.name);
                ps_next(&ps);
                if (ps.tk == '.') {
                    ps_next(&ps);
                    if (ps.tk == '.') ps_next(&ps);
                    if (ps.tk == '.') ps_next(&ps);
                    pr.rest = 1;
                    pr.kwonly = kwonly;
                } else {
                    pr.kwonly = kwonly;
                    if (ps.tk == '=') {
                        ps_next(&ps);
                        pr.def = ps_expr(&ps);
                    }
                }
                vec_push(&params, pr);
                if (pr.rest) kwonly = 1;
            } else {
                ps_error(&ps, "ZA4100", "a parameter name is expected");
                break;
            }
            if (ps.tk == ',') ps_next(&ps);
            else if (ps.tk != ')' && !ps.err) {
                ps_error(&ps, "ZA4100", "',' or ')' expected in the parameter list");
                break;
            }
        }
        if (ps.tk == ')')
            ps_next(&ps);
        else if (!ps.err)
            ps_error(&ps, "ZA4100", "missing ')' in the parameter list");
    }
    if (!ps.err && ps.tk != '{') ps_error(&ps, "ZA4100", "a '{' is expected at the end of the #macro line");
    if (ps.err) {
        cp->ok = 0;
        return NULL;
    }
    m->params = params.v;
    m->np = (int)params.n;
    return m;
}


static void cmp_cleanup(Cmp *cp) {
    size_t i;
    for (i = 0; i < cp->blk.n; i++) {
        free(cp->blk.v[i].ends.v);
        free(cp->blk.v[i].brk.v);
        free(cp->blk.v[i].cont.v);
    }
    free(cp->blk.v);
    free(cp->bpar.v);
    free(cp->labels.v);
    free(cp->gotos.v);
}

void mac_define(MacSys *ms, const MacText *t) {
    Cmp cp;
    Macro *m;
    const MacLine *h;
    Blk root;
    if (t->lines.n == 0) return;
    cmp_init(&cp, ms, t, 1);
    h = &t->lines.v[0];
    m = parse_header(&cp, h);
    if (!m) return;
    memset(&root, 0, sizeof root);
    root.kind = B_MACRO;
    root.hdepth = root.bdepth = 0;
    root.bid = 0;
    root.pending = -1;
    root.line = h->line;
    root.col = 1;
    vec_push(&cp.blk, root);
    cp.mac = m;
    if (!compile_body(&cp, t, 1)) return;
    m->code.st = cp.st.v;
    m->code.n = cp.st.n;
    cmp_cleanup(&cp);
    {
        Macro *old = strmap_get(&ms->macros, m->name);
        if (old) {
            diag_note(rep_warnf(ms->rep, "ZA4106", m->def, "macro %s replaces an earlier definition", m->name),
                      old->def, "the earlier definition");
        } else if (is_asm_op(m->name)) {
            diag_help(rep_warnf(ms->rep, "ZA4106", m->def,
                                "macro %s hides the instruction or directive of the same name", m->name),
                      "the macro is used wherever %s is written as an operation", m->name);
        }
    }
    strmap_put(&ms->macros, m->name, m);
}

/* ---- runtime ------------------------------------------------------------------------------------ */

static Span mstmt_span(const MStmt *st) { return span_make(st->file, st->line, st->col, st->len); }

static Diag *ma_report(Ctx *c, int warn, const char *code, const char *fmt, va_list ap) {
    char *m = xvasprintf(fmt, ap);
    Span sp = c->st ? mstmt_span(c->st) : c->f->call;
    Diag *d = warn ? rep_warnf(c->ms->rep, code, sp, "%s", m) : rep_errorf(c->ms->rep, code, sp, "%s", m);
    if (c->f->mac) diag_note(d, c->f->call, "in expansion of macro %s", c->f->mac->name);
    if (rep_too_many(c->ms->rep)) c->ms->aborted = 1;
    return d;
}

Diag *ma_err(Ctx *c, const char *code, const char *fmt, ...) {
    va_list ap;
    Diag *d;
    va_start(ap, fmt);
    d = ma_report(c, 0, code, fmt, ap);
    va_end(ap);
    return d;
}

Diag *ma_warn(Ctx *c, const char *code, const char *fmt, ...) {
    va_list ap;
    Diag *d;
    va_start(ap, fmt);
    d = ma_report(c, 1, code, fmt, ap);
    va_end(ap);
    return d;
}

int mac_defined(const MacSys *ms, const char *op) {
    char *u = za_strupper(op);
    int r = strmap_get(&ms->macros, u) != NULL;
    free(u);
    return r;
}

int ma_defined(Ctx *c, const char *lname) {
    char *u;
    if (var_find(c->f, lname)) return 1;
    if (c->ms->host.is_defined) {
        if (c->ms->host.is_defined(c->ms->host.user, lname)) return 1;
        u = za_strupper(lname);
        if (c->ms->host.is_defined(c->ms->host.user, u)) return 1;
    }
    return mac_defined(c->ms, lname);
}

static MVal declare(Ctx *c, const char *name, int depth, MVal v, MVal *slot) {
    size_t i;
    Var var;
    for (i = c->f->vars.n; i > 0; i--) {
        const Var *o = &c->f->vars.v[i - 1];
        if (strcmp(o->name, name)) continue;
        if (o->depth == depth) {
            ma_err(c, "ZA4100", "\"%s\" is already declared in this block", name);
            return v;
        }
        if (name[0] != '%') ma_warn(c, "ZA4107", "variable \"%s\" hides an outer variable of the same name", name);
        break;
    }
    var.name = xstrdup(name);
    var.depth = depth;
    if (slot) {
        var.slot = slot;
        var.owns_slot = 0;
    } else {
        var.slot = xcalloc(sizeof *var.slot);
        *var.slot = v;
        var.owns_slot = 1;
    }
    vec_push(&c->f->vars, var);
    return v;
}

static char *eval_tmpl(Ctx *c, const Tmpl *t) {
    Buf b = {0};
    int i;
    for (i = 0; i < t->n; i++) {
        if (t->p[i].e)
            buf_puts(&b, v_text(ex_eval(c, t->p[i].e)));
        else
            buf_puts(&b, t->p[i].text);
    }
    {
        char *r = xstrdup(buf_cstr(&b));
        free(b.p);
        return r;
    }
}

static char *field_trim(const char *s) {
    size_t n = strlen(s);
    return trim_dup(s, n);
}

static void host_emit(Ctx *c, const char *text, int line) {
    if (c->ms->aborted || !c->ms->host.emit) return;
    c->ms->host.emit(c->ms->host.user, text, line);
}

static void run_model(Ctx *c, const Model *m) {
    Buf b = {0};
    char *label, *op, *opd;
    int i, line = c->f->open ? c->st->line : 0;
    if (m->comment) {
        host_emit(c, m->comment, line);
        return;
    }
    {
        char *el = eval_tmpl(c, &m->label);
        char *eo = eval_tmpl(c, &m->op);
        char *ed = eval_tmpl(c, &m->opd);
        label = field_trim(el);
        op = field_trim(eo);
        opd = field_trim(ed);
        free(el); free(eo); free(ed);
    }
    if (!*op && !*label) { free(label); free(op); free(opd); return; }
    if (!*op) {
        host_emit(c, label, line);
        free(label); free(op); free(opd);
        return;
    }
    if (*label) {
        buf_puts(&b, label);
        if (strlen(label) < 9) {
            size_t k;
            for (k = strlen(label); k < 9; k++) buf_putc(&b, ' ');
        } else {
            buf_putc(&b, ' ');
        }
    } else {
        buf_puts(&b, "         ");
    }
    buf_puts(&b, op);
    if (*opd) {
        size_t k = strlen(op);
        do buf_putc(&b, ' ');
        while (++k < 6);
        buf_puts(&b, opd);
        if (m->remark) {
            buf_putc(&b, ' ');
            buf_puts(&b, m->remark);
        }
    }
    for (i = 0; i < m->nconts; i++) {
        char *co = field_trim(eval_tmpl(c, &m->conts[i].ops));
        buf_puts(&b, "\n               ");
        buf_puts(&b, co);
        if (m->conts[i].remark && *co) {
            buf_putc(&b, ' ');
            buf_puts(&b, m->conts[i].remark);
        }
        free(co);
    }
    host_emit(c, buf_cstr(&b), line);
    free(b.p);
    free(label); free(op); free(opd);
}

static MVal set_path(Ctx *c, MVal base, MVal *idx, int k, int nidx, MVal nv) {
    const MVal *it;
    size_t n = v_items(base, &it), i;
    int64_t at;
    MVal *a;
    size_t nn;
    if (idx[k].t == V_INT) {
        at = idx[k].i;
    } else if (!v_parse_int(v_text(idx[k]), &at)) {
        ma_err(c, "ZA4112", "\"%s\" is not a valid index", v_text(idx[k]));
        v_items_free(base, &it);
        return base;
    }
    if (at < 0 || at >= MAX_LIST) {
        ma_err(c, "ZA4112", "the index %lld is out of range", (long long)at);
        v_items_free(base, &it);
        return base;
    }
    nn = (size_t)at + 1 > n ? (size_t)at + 1 : n;
    if (c->ms->list_work > 8u * 1024u * 1024u ||
        nn > 8u * 1024u * 1024u - c->ms->list_work) {
        ma_err(c, "ZA4112", "list assignment is too large (index %lld)", (long long)at);
        v_items_free(base, &it);
        return base;
    }
    c->ms->list_work += nn;
    a = xcalloc(nn * sizeof *a);
    for (i = 0; i < nn; i++) a[i] = i < n ? it[i] : v_str("");
    a[at] = k + 1 == nidx ? nv : set_path(c, a[at], idx, k + 1, nidx, nv);
    v_items_free(base, &it);
    return v_list(a, nn);
}

static MVal get_path(MVal base, MVal *idx, int nidx) {
    int k;
    for (k = 0; k < nidx; k++) {
        const MVal *it;
        size_t n = v_items(base, &it);
        int64_t at;
        if (!(idx[k].t == V_INT ? (at = idx[k].i, 1) : v_parse_int(v_text(idx[k]), &at)) || at < 0 ||
            (uint64_t)at >= n) {
            v_items_free(base, &it);
            return v_str("");
        }
        {
            MVal old = base;
            base = it[at];
            v_items_free(old, &it);
        }
    }
    return base;
}


static void frame_pop(Frame *f, int depth) {
    while (f->vars.n > 0 && f->vars.v[f->vars.n - 1].depth > depth) {
        Var *v = &f->vars.v[f->vars.n - 1];
        free(v->name);
        if (v->owns_slot) free(v->slot);
        f->vars.n--;
    }
}

static void run_code(MacSys *ms, const Code *code, Frame *f) {
    size_t pc = 0;
    Ctx c;
    c.ms = ms;
    c.f = f;
    c.st = NULL;
    while (pc < code->n && !ms->aborted) {
        const MStmt *st = &code->st[pc];
        c.st = st;
        ms->stmt++;
        if (++ms->steps > ms->step_limit) {
            ma_err(&c, "ZA4121", "the expansion was stopped after %ld statements", ms->step_limit);
            ms->aborted = 1;
            break;
        }
        frame_pop(f, st->depth);
        switch (st->kind) {
        case S_NOP:
            break;
        case S_LET:
            declare(&c, st->name, st->vdepth ? st->vdepth : st->depth, ex_eval(&c, st->e), NULL);
            break;
        case S_GLOBAL: {
            MVal *g = strmap_get(&ms->globals, st->name);
            if (!g) {
                g = xcalloc(sizeof *g);
                *g = ex_eval(&c, st->e);
                strmap_put(&ms->globals, st->name, g);
            }
            declare(&c, st->name, st->depth, *g, g);
            break;
        }
        case S_ASSIGN: {
            MVal *slot = var_find(f, st->name), val, *ix = NULL;
            int i;
            if (!slot) {
                Diag *d = ma_err(&c, "ZA4111", "assignment to \"%s\", which is not declared", st->name);
                diag_help(d, "declare it first with #let %s = ...", st->name);
                break;
            }
            if (st->nidx) {
                ix = xcalloc((size_t)st->nidx * sizeof *ix);
                for (i = 0; i < st->nidx; i++) ix[i] = ex_eval(&c, st->idx[i]);
            }
            val = ex_eval(&c, st->e);
            if (st->aop) {
                MVal cur = ix ? get_path(*slot, ix, st->nidx) : *slot;
                val = ex_binop(&c, st->aop, cur, val);
            }
            *slot = ix ? set_path(&c, *slot, ix, 0, st->nidx, val) : val;
            break;
        }
        case S_JUMP:
            pc = (size_t)st->target;
            continue;
        case S_JFALSE:
            if (!v_truth(ex_eval(&c, st->e))) {
                pc = (size_t)st->target;
                continue;
            }
            break;
        case S_CASE: {
            MVal *sv = var_find(f, st->hl);
            int i, hit = 0;
            for (i = 0; sv && i < st->ncases && !hit; i++)
                hit = v_truth(ex_binop(&c, TK_EQ, *sv, ex_eval(&c, st->cases[i])));
            if (!hit) {
                pc = (size_t)st->target;
                continue;
            }
            break;
        }
        case S_ITER: {
            MVal *l = var_find(f, st->hl), *ix = var_find(f, st->hi);
            const MVal *it;
            size_t n;
            if (!l || !ix) break;
            n = v_items(*l, &it);
            if ((size_t)ix->i >= n) {
                v_items_free(*l, &it);
                pc = (size_t)st->target;
                continue;
            }
            declare(&c, st->name, st->vdepth, it[ix->i], NULL);
            v_items_free(*l, &it);
            ix->i++;
            break;
        }
        case S_EMIT: {
            const char *s = v_text(ex_eval(&c, st->e)), *nl;
            for (;; s = nl + 1) {
                nl = strchr(s, '\n');
                host_emit(&c, nl ? xstrndup(s, (size_t)(nl - s)) : s, f->open ? st->line : 0);
                if (!nl) break;
            }
            break;
        }
        case S_INSERT: {
            const char *s = v_text(ex_eval(&c, st->e));
            if (ms->host.insert) ms->host.insert(ms->host.user, s, st->target);
            break;
        }
        case S_NOTE:
        case S_WARN:
        case S_ERROR: {
            const char *m = v_text(ex_eval(&c, st->e));
            Span sp = mstmt_span(st);
            Diag *d;
            if (st->kind == S_NOTE)
                d = rep_notef(ms->rep, "ZA4052", sp, "%s", m);
            else if (st->kind == S_WARN)
                d = rep_warnf(ms->rep, "ZA4051", sp, "%s", m);
            else
                d = rep_errorf(ms->rep, "ZA4050", sp, "%s", m);
            if (f->mac) diag_note(d, f->call, "in expansion of macro %s", f->mac->name);
            if (rep_too_many(ms->rep)) ms->aborted = 1;
            break;
        }
        case S_MODEL:
            run_model(&c, st->model);
            break;
        case S_RETURN:
            pc = code->n;
            continue;
        }
        pc++;
    }
    frame_pop(f, 0);
}

char *ma_aread(Ctx *c) { return c->ms->host.aread ? c->ms->host.aread(c->ms->host.user) : NULL; }

/* ---- built-in values ---------------------------------------------------------------------------- */

MVal sys_value(Ctx *c, const char *name, int *ok) {
    *ok = 1;
    if (!strcmp(name, "label")) return v_str(c->f->label ? c->f->label : "");
    if (!strcmp(name, "ndx")) return v_str(c->f->ndx);
    if (!strcmp(name, "macro")) return v_str(c->f->mac ? c->f->mac->name : "");
    if (!strcmp(name, "level")) return v_int(c->f->level);
    if (!strcmp(name, "args")) return c->f->args;
    if (!strcmp(name, "argc")) return v_int((int64_t)c->f->args.n);
    if (!strcmp(name, "sect") || !strcmp(name, "sectype")) {
        const char *type = "", *sect = "";
        if (c->ms->host.sect) {
            sect = c->ms->host.sect(c->ms->host.user, &type);
            if (!sect) sect = "";
            if (!type) type = "";
        }
        return v_str(name[4] ? type : sect);
    }
    if (!strcmp(name, "parm")) {
        const char *l = c->ms->host.parm ? c->ms->host.parm(c->ms->host.user) : NULL;
        return v_str(l ? l : "");
    }
    if (!strcmp(name, "loc")) {
        const char *l = c->ms->host.loc ? c->ms->host.loc(c->ms->host.user) : NULL;
        return v_str(l ? l : "");
    }
    if (!strcmp(name, "date") || !strcmp(name, "datc") || !strcmp(name, "time")) {
        char buf[64];
        time_t t = time(NULL);
        struct tm *tm;
        if (c->ms->host.now && !c->ms->host.now(c->ms->host.user, &t)) return v_str("");
        tm = gmtime(&t);
        if (!tm) return v_str("");
        if (!strcmp(name, "date"))
            snprintf(buf, sizeof buf, "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100);
        else if (!strcmp(name, "datc"))
            snprintf(buf, sizeof buf, "%04d%02d%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
        else
            snprintf(buf, sizeof buf, "%02d.%02d", tm->tm_hour, tm->tm_min);
        return v_str(xstrdup(buf));
    }
    if (!strcmp(name, "__file__")) return v_str(c->f->open ? (c->st ? c->st->file : "") : c->f->call.pos.file);
    if (!strcmp(name, "__line__")) return v_int(c->f->open ? (c->st ? c->st->line : 0) : c->f->call.pos.line);
    if (!strcmp(name, "__goff__")) return v_int(!c->ms->elf);
    if (!strcmp(name, "__elf__")) return v_int(c->ms->elf);
    if (!strcmp(name, "__ascii__")) return v_int(za_ascii != 0);
    if (!strcmp(name, "__ebcdic__")) return v_int(za_ascii == 0);
    if (!strcmp(name, "__amode__") || !strcmp(name, "__rmode__"))
        return v_int(c->ms->host.mode ? c->ms->host.mode(c->ms->host.user, name[2] == 'a') : 0);
    if (!strcmp(name, "asm")) return v_str("zosasm");
    if (!strcmp(name, "ver")) return v_str(ZASM_VERSION_STR);
    if (!strcmp(name, "system_id")) return v_str("zosasm " ZASM_VERSION_STR);
    if (!strcmp(name, "stmt")) return v_int(c->ms->stmt);
    if (!strcmp(name, "opt_elf")) return v_int(c->ms->elf);
    if (!strcmp(name, "opt_strict_registers")) return v_int(za_strict_registers);
    if (!strcmp(name, "opt_ascii")) return v_int(za_ascii != 0);
    if (!strcmp(name, "opt_ebcdic")) return v_int(za_ascii == 0);
    if (!strcmp(name, "clock")) {
        struct timeval tv;
        struct tm *tm;
        char buf[64];
        gettimeofday(&tv, NULL);
        tm = gmtime(&tv.tv_sec);
        if (!tm) return v_str("");
        snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%06ld",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec,
                 (long)tv.tv_usec);
        return v_str(xstrdup(buf));
    }
    *ok = 0;
    return v_str("");
}

/* ---- calls ----------------------------------------------------------------------------------------- */

static int find_param(const Macro *m, const char *lname) {
    int i;
    for (i = 0; i < m->np; i++)
        if (!strcmp(m->params[i].name, lname)) return i;
    return -1;
}

/* "name=value" where name is a parameter of m: returns the parameter index and *val */
static int keyword_arg(const Macro *m, const char *a, const char **val) {
    size_t n = ident_len(a);
    char *l;
    int i;
    if (n == 0 || a[n] != '=') return -1;
    l = za_strlower_own(xstrndup(a, n));
    i = find_param(m, l);
    if (i >= 0 && !m->params[i].rest) {
        *val = a + n + 1;
        return i;
    }
    return -1;
}

void mac_call(MacSys *ms, const char *op, const char *label, Span callsite, char **args,
              size_t nargs) {
    char *opu = za_strupper(op);
    Macro *m = strmap_get(&ms->macros, opu);
    Frame f;
    Ctx c;
    MVal *vals;
    char *given;
    size_t i;
    int next = 0, restp = -1, p;
    Vec(MVal) posargs = {0}, restv = {0};
    free(opu);
    if (!m || ms->aborted) return;
    if (ms->depth == 0) {
        ms->steps = 0;
        ms->list_work = 0;
        ms->aborted = 0;
    }
    if (ms->depth >= MAX_DEPTH) {
        diag_help(rep_errorf(ms->rep, "ZA4120", callsite, "macro nesting is too deep (%d levels) in the call of %s",
                             MAX_DEPTH, m->name),
                  "a macro that calls itself without an end condition?");
        ms->aborted = 1;
        return;
    }
    memset(&f, 0, sizeof f);
    f.mac = m;
    f.label = label ? label : "";
    f.call = callsite;
    f.level = ms->depth + 1;
    snprintf(f.ndx, sizeof f.ndx, "%04ld", ++ms->ndx % 10000);
    c.ms = ms;
    c.f = &f;
    c.st = NULL;
    vals = xcalloc(((size_t)m->np + 1) * sizeof *vals);
    given = xcalloc((size_t)m->np + 1);
    for (p = 0; p < m->np; p++)
        if (m->params[p].rest) restp = p;
    for (i = 0; i < nargs; i++) {
        const char *val;
        int k = keyword_arg(m, args[i], &val);
        if (k >= 0) {
            if (given[k]) {
                rep_errorf(ms->rep, "ZA4101", callsite, "parameter \"%s\" of %s is given twice", m->params[k].name, m->name);
                continue;
            }
            given[k] = 1;
            vals[k] = v_str(xstrdup(val));
            if (!*val) given[k] = 0;
            continue;
        }
        vec_push(&posargs, v_str(args[i]));
        while (next < m->np && (m->params[next].kwonly || m->params[next].rest)) {
            if (m->params[next].rest) break;
            next++;
        }
        if (next < m->np && !m->params[next].rest && !m->params[next].kwonly) {
            if (args[i][0]) {
                if (given[next])
                    rep_errorf(ms->rep, "ZA4101", callsite, "parameter \"%s\" of %s is given twice", m->params[next].name, m->name);
                given[next] = 1;
                vals[next] = v_str(args[i]);
            }
            next++;
        } else if (restp >= 0) {
            vec_push(&restv, v_str(args[i]));
        } else {
            diag_help(rep_errorf(ms->rep, "ZA4102", callsite, "too many arguments for %s (it takes %d)", m->name,
                                 m->np),
                      "the macro is declared at %s:%d", m->def.pos.file, m->def.pos.line);
            ms->aborted = 0;
            return;
        }
    }
    f.args = v_list(posargs.v, posargs.n);
    /* bind in order, so that a default can use the earlier parameters */
    ms->depth++;
    if (m->labelvar) declare(&c, m->labelvar, 0, v_str(f.label), NULL);
    for (p = 0; p < m->np; p++) {
        MVal v;
        if (m->params[p].rest)
            v = v_list(restv.v, restv.n);
        else if (given[p])
            v = vals[p];
        else if (m->params[p].def)
            v = ex_eval(&c, m->params[p].def);
        else
            v = v_str("");
        declare(&c, m->params[p].name, 0, v, NULL);
    }
    run_code(ms, &m->code, &f);
    for (i = 0; i < f.vars.n; i++) {
        free(f.vars.v[i].name);
        if (f.vars.v[i].owns_slot) free(f.vars.v[i].slot);
    }
    free(f.vars.v);
    free(vals);
    free(given);
    free(posargs.v);
    free(restv.v);
    ms->depth--;
    ms->nexp++;
    if (ms->depth == 0) ms->aborted = 0;
}

void mac_exec(MacSys *ms, const MacText *t) {
    Cmp cp;
    Blk root;
    Code code;
    if (t->lines.n == 0) return;
    cmp_init(&cp, ms, t, 0);
    memset(&root, 0, sizeof root);
    root.kind = B_ROOT;
    root.pending = -1;
    root.bid = 0;
    vec_push(&cp.blk, root);
    if (!compile_body(&cp, t, 0)) return;
    code.st = cp.st.v;
    code.n = cp.st.n;
    cmp_cleanup(&cp);
    ms->top.open = 1;
    if (ms->depth == 0) {
        ms->steps = 0;
        ms->aborted = 0;
    }
    ms->depth++;
    run_code(ms, &code, &ms->top);
    ms->depth--;
    if (ms->depth == 0) ms->aborted = 0;
    for (size_t i = 0; i < code.n; i++) mstmt_free(&code.st[i]);
    free(code.st);
}

Span mac_def_span(const MacSys *ms, const char *op) {
    char *u = za_strupper(op);
    const Macro *m = strmap_get(&ms->macros, u);
    Span sp = m ? m->def : span_make("", 0, 0, 0);
    free(u);
    return sp;
}

void mac_load_library(MacSys *ms, const char *file, const char *text, size_t len) {
    SrcVec lines = {0};
    size_t i = 0;
    src_split(&lines, file, text, len);
    src_blank_comments(&lines, ms->rep);
    while (i < lines.n) {
        int kind = mac_top_kind(lines.v[i].text);
        if (kind == 1) {
            MacText mt;
            i += mac_collect(ms, lines.v, lines.n, i, 1, &mt);
            mac_define(ms, &mt);
            mac_text_free(&mt);
        } else {
            i++;
        }
    }
}

long mac_expansions(const MacSys *ms) { return ms->nexp; }

void mac_text_free(MacText *t) {
    size_t i;
    for (i = 0; i < t->lines.n; i++) free(t->lines.v[i].text);
    free(t->lines.v);
    memset(t, 0, sizeof *t);
}


/* ---- cleanup ---------------------------------------------------------------- */

static void me_free(MExpr *e) {
    size_t i;
    if (!e) return;
    free(e->s);
    me_free(e->a);
    me_free(e->b);
    me_free(e->c);
    for (i = 0; i < (size_t)e->nargs; i++) me_free(e->args[i]);
    free(e->args);
    free(e);
}

static void part_free(Part *p) {
    free(p->text);
    me_free(p->e);
}

static void tmpl_free(Tmpl *t) {
    int i;
    for (i = 0; i < t->n; i++) part_free(&t->p[i]);
    free(t->p);
    t->p = NULL;
    t->n = 0;
}

static void model_free(Model *m) {
    int i;
    if (!m) return;
    tmpl_free(&m->label);
    tmpl_free(&m->op);
    tmpl_free(&m->opd);
    free(m->remark);
    for (i = 0; i < m->nconts; i++) {
        tmpl_free(&m->conts[i].ops);
        free(m->conts[i].remark);
    }
    free(m->conts);
    free(m->comment);
    free(m);
}

static void mstmt_free(MStmt *st) {
    int i;
    free(st->name);
    me_free(st->e);
    for (i = 0; i < st->nidx; i++) me_free(st->idx[i]);
    free(st->idx);
    for (i = 0; i < st->ncases; i++) me_free(st->cases[i]);
    free(st->cases);
    /* hl/hi are shared with the S_LET statements that declare them */
    model_free(st->model);
}

static void macro_def_free(Macro *m) {
    size_t i;
    if (!m) return;
    free(m->name);
    free(m->labelvar);
    for (i = 0; i < (size_t)m->np; i++) {
        free(m->params[i].name);
        me_free(m->params[i].def);
    }
    free(m->params);
    for (i = 0; i < m->code.n; i++) mstmt_free(&m->code.st[i]);
    free(m->code.st);
    free(m);
}

void mac_free(MacSys *ms) {
    size_t i;
    if (!ms) return;
    for (i = 0; i < ms->macros.cap; i++) {
        if (!ms->macros.vals[i]) continue;
        free(ms->macros.keys[i]);
        macro_def_free(ms->macros.vals[i]);
    }
    free(ms->macros.keys);
    free(ms->macros.vals);
    for (i = 0; i < ms->globals.cap; i++) {
        free(ms->globals.keys[i]);
        free(ms->globals.vals[i]);
    }
    free(ms->globals.keys);
    free(ms->globals.vals);
    for (i = 0; i < ms->top.vars.n; i++) {
        free(ms->top.vars.v[i].name);
        if (ms->top.vars.v[i].owns_slot) free(ms->top.vars.v[i].slot);
    }
    free(ms->top.vars.v);
    free(ms);
}

MacSys *mac_new(Reporter *rep, const MacHost *host, long step_limit, int elf) {
    MacSys *ms = xcalloc(sizeof *ms);
    ms->rep = rep;
    ms->host = *host;
    ms->step_limit = step_limit > 0 ? step_limit : DEFAULT_STEPS;
    ms->elf = elf;
    ms->top.args = v_list(NULL, 0);
    ms->top.open = 1;
    return ms;
}
