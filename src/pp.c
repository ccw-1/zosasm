/* pp.c - the preprocessor. See pp.h and DESIGN.md section 8.
 *
 * Diagnostic codes: ZA4010-4012 #include, ZA4020-4023 #define/#undef,
 * ZA4030-4034 conditionals, ZA4040-4042 macro calls, ZA4050/4051 #error and
 * #warning, ZA4060/4061 #pragma, ZA4070 trailing comma, ZA4080/4081 the time
 * of __DATE__ and __TIME__. */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pp.h"
#include "ebcdic.h"
#include "macro.h"
#include "version.h"

#define PP_MAX_DEPTH 32
#define PP_MAX_EXPANSION ((size_t)4 << 20)

typedef Vec(char *) StrVec;

typedef struct {
    char *name;
    int fn, nparams;
    char **params;
    char *body; /* a real '\n' marks a line break */
    Span def;
} Macro;

typedef struct {
    int parent_active, taking, taken, seen_else;
    Span sp;
} Cond;

typedef struct Active {
    const Macro *m;
    const struct Active *next;
} Active;

typedef struct {
    Reporter *rep;
    const PpOptions *opt;
    StrMap macros;
    SrcVec *out;
    Vec(Cond) conds;
    const char *file; /* file being read */
    int line;         /* its current logical line */
    int depth;
    /* state of the line being expanded */
    Span use;
    const Macro *first;
    size_t produced;
    int too_big;
    /* trailing comma check */
    int comma;
    /* built-ins */
    const char *base; /* the file given to pp_run */
    long counter;     /* __COUNTER__ */
    int have_now, now_bad;
    time_t now; /* __DATE__ and __TIME__ */
    /* macros (macro.h) */
    MacSys *mac;
    StrMap lib_tried; /* operations that were looked up in the macro directories */
    int mdepth;       /* nesting of macro calls */
    int exp_done;     /* the expansion note of the outermost call is registered */
    const char *exp_macro;
    Span exp_def;
    char *sect, *sectype; /* the last section statement (sys.sect, sys.sectype) */
} Pp;

/* ---- lexical helpers --------------------------------------------------- */

/* end of the identifier (or number) starting at s[i]; "##" ends it */
static size_t ident_end(const char *s, size_t i) {
    size_t j = i;
    if (s[j] == '#' && s[j + 1] == '#') return j;
    if (is_sym_char((unsigned char)s[j])) j++;
    while (is_sym_char((unsigned char)s[j]) && !(s[j] == '#' && s[j + 1] == '#')) j++;
    return j;
}

static const char *skip_blanks(const char *s) {
    while (is_blank((unsigned char)*s)) s++;
    return s;
}

static char *rtrim_dup(const char *s) {
    size_t n = strlen(s);
    while (n > 0 && (is_blank((unsigned char)s[n - 1]) || s[n - 1] == '\r')) n--;
    return xstrndup(s, n);
}

static Span line_span(const Pp *pp, const char *text) {
    return span_make(pp->file, pp->line, 1, (int)strlen(text));
}

static int active(const Pp *pp) {
    return pp->conds.n == 0 || pp->conds.v[pp->conds.n - 1].taking;
}

static int in_active(const Active *a, const Macro *m) {
    for (; a; a = a->next)
        if (a->m == m) return 1;
    return 0;
}

/* ---- built-in names ----------------------------------------------------- */

static const char *const builtin_names[] = {"__FILE__",          "__LINE__",
                                            "__BASE_FILE__",     "__INCLUDE_LEVEL__",
                                            "__COUNTER__",       "__ZASM_VERSION__",
                                            "__DATE__",          "__TIME__"};

static int is_builtin(const char *id) {
    size_t i;
    for (i = 0; i < sizeof builtin_names / sizeof builtin_names[0]; i++)
        if (!strcmp(id, builtin_names[i])) return 1;
    return 0;
}

/* The time for __DATE__ and __TIME__: -D... option, SOURCE_DATE_EPOCH, or the
 * clock; with -reproducible only a fixed time is allowed. Returns 0 (after a
 * diagnostic) when there is none. */
static int pp_now(Pp *pp) {
    const char *env;
    if (pp->have_now) return 1;
    if (pp->now_bad) return 0;
    env = getenv("SOURCE_DATE_EPOCH");
    if (pp->opt->has_epoch) {
        pp->now = (time_t)pp->opt->epoch;
    } else if (env && *env) {
        const char *q;
        int64_t v = 0;
        for (q = env; *q >= '0' && *q <= '9' && v < ((int64_t)1 << 50); q++) v = v * 10 + (*q - '0');
        if (*q || q == env) {
            diag_help(rep_errorf(pp->rep, "ZA4081", pp->use,
                                 "SOURCE_DATE_EPOCH is not a number of seconds: \"%s\"", env),
                      "set it to the seconds since 1970-01-01, for example 1700000000");
            pp->now_bad = 1;
            return 0;
        }
        pp->now = (time_t)v;
    } else if (pp->opt->reproducible) {
        diag_help(rep_errorf(pp->rep, "ZA4080", pp->use,
                             "__DATE__ and __TIME__ need a fixed time with -reproducible"),
                  "set the environment variable SOURCE_DATE_EPOCH, or drop -reproducible");
        pp->now_bad = 1;
        return 0;
    } else {
        pp->now = time(NULL);
    }
    pp->have_now = 1;
    return 1;
}

/* the text of a built-in name; 0 when id is none */
static int builtin_text(Pp *pp, const char *id, Buf *out) {
    if (!strcmp(id, "__FILE__")) {
        buf_puts(out, pp->file);
    } else if (!strcmp(id, "__LINE__")) {
        buf_printf(out, "%d", pp->line);
    } else if (!strcmp(id, "__BASE_FILE__")) {
        buf_puts(out, pp->base);
    } else if (!strcmp(id, "__INCLUDE_LEVEL__")) {
        buf_printf(out, "%d", pp->depth);
    } else if (!strcmp(id, "__COUNTER__")) {
        buf_printf(out, "%ld", pp->counter++);
    } else if (!strcmp(id, "__ZASM_VERSION__")) {
        buf_printf(out, "%d", ZASM_VERSION_NUM);
    } else if (!strcmp(id, "__DATE__") || !strcmp(id, "__TIME__")) {
        struct tm *t;
        if (!pp_now(pp)) {
            buf_puts(out, id);
            return 1;
        }
        t = gmtime(&pp->now);
        if (!t) {
            buf_puts(out, id);
            return 1;
        }
        if (id[2] == 'D')
            buf_printf(out, "%04d-%02d-%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
        else
            buf_printf(out, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
    } else {
        return 0;
    }
    return 1;
}

/* ---- macro expansion --------------------------------------------------- */

static char *expand(Pp *pp, const char *s, const Active *act);

/* s[i] is '('; collect the comma separated arguments; returns the index of
 * the closing ')' or -1 */
static long parse_args(const char *s, size_t i, StrVec *args) {
    int depth = 0;
    size_t j, start = i + 1;
    for (j = i; s[j]; j++) {
        char c = s[j];
        if (c == '\'' && !is_attr_quote(s, j)) {
            long q = close_quote(s, j);
            if (q < 0) return -1;
            j = (size_t)q;
        } else if (c == '"') {
            long q = close_dquote(s, j);
            if (q < 0) return -1;
            j = (size_t)q;
        } else if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (--depth == 0) {
                char *a = xstrndup(s + start, j - start);
                char *t = str_trim(a);
                free(a);
                vec_push(args, t);
                return (long)j;
            }
        } else if (c == ',' && depth == 1) {
            char *a = xstrndup(s + start, j - start);
            char *t = str_trim(a);
            free(a);
            vec_push(args, t);
            start = j + 1;
        }
    }
    return -1;
}

/* Replace the parameters in the body (outside quotes) and apply "##". */
static char *substitute(const Macro *m, char **raw, char **exp) {
    Buf out = {0};
    const char *b = m->body;
    size_t i = 0, n = strlen(b);
    int paste_prev = 0;
    while (i < n) {
        unsigned char c = (unsigned char)b[i];
        if (c == '#' && b[i + 1] == '#') {
            while (out.len > 0 && is_blank((unsigned char)out.p[out.len - 1])) out.len--;
            i += 2;
            while (is_blank((unsigned char)b[i])) i++;
            paste_prev = 1;
            continue;
        }
        if (c == '#' && b[i + 1] == '(' && exp) { /* #(param): the argument as a quoted text */
            size_t k = i + 2, e, f;
            int p = -1, q;
            while (is_blank((unsigned char)b[k])) k++;
            e = is_sym_start((unsigned char)b[k]) ? ident_end(b, k) : k;
            f = e;
            while (is_blank((unsigned char)b[f])) f++;
            if (e > k && b[f] == ')') {
                char *id = xstrndup(b + k, e - k);
                for (q = 0; q < m->nparams; q++)
                    if (!strcmp(m->params[q], id)) p = q;
                free(id);
            }
            if (p >= 0) {
                const char *t = exp[p];
                buf_putc(&out, '\'');
                for (; *t; t++) {
                    if (*t == '\'') buf_putc(&out, '\'');
                    buf_putc(&out, *t);
                }
                buf_putc(&out, '\'');
                i = f + 1;
                paste_prev = 0;
                continue;
            }
            buf_putc(&out, c);
            i++;
        } else if (c == '\'' && !is_attr_quote(b, i)) {
            long q = close_quote(b, i);
            size_t e = q < 0 ? n : (size_t)q + 1;
            buf_write(&out, b + i, e - i);
            i = e;
        } else if (c == '"' && close_dquote(b, i) >= 0) {
            size_t e = (size_t)close_dquote(b, i) + 1;
            buf_write(&out, b + i, e - i);
            i = e;
        } else if (c == '\'') {
            buf_putc(&out, c);
            i++;
        } else if (za_isdigit(c) || is_sym_start(c)) {
            size_t j = ident_end(b, i), k;
            int p = -1, paste_next;
            if (is_sym_start(c) && b[j] != '\'') {
                char *id = xstrndup(b + i, j - i);
                int q;
                for (q = 0; q < m->nparams; q++)
                    if (!strcmp(m->params[q], id)) p = q;
                free(id);
            }
            k = j;
            while (is_blank((unsigned char)b[k])) k++;
            paste_next = b[k] == '#' && b[k + 1] == '#';
            if (p >= 0)
                buf_puts(&out, (paste_prev || paste_next) ? raw[p] : exp[p]);
            else
                buf_write(&out, b + i, j - i);
            i = j;
        } else {
            buf_putc(&out, c);
            i++;
        }
        paste_prev = 0;
    }
    return xstrdup(buf_cstr(&out));
}

static char *expand(Pp *pp, const char *s, const Active *act) {
    Buf out = {0};
    size_t i = 0, n = strlen(s);
    while (i < n && !pp->too_big) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\'') {
            if (is_attr_quote(s, i)) {
                buf_putc(&out, c);
                i++;
            } else {
                long q = close_quote(s, i);
                size_t e = q < 0 ? n : (size_t)q + 1;
                buf_write(&out, s + i, e - i);
                i = e;
            }
        } else if (c == '"' && close_dquote(s, i) >= 0) {
            size_t e = (size_t)close_dquote(s, i) + 1;
            buf_write(&out, s + i, e - i);
            i = e;
        } else if (za_isdigit(c)) {
            size_t j = ident_end(s, i);
            buf_write(&out, s + i, j - i);
            i = j;
        } else if (is_sym_start(c) && !(c == '#' && s[i + 1] == '#')) {
            size_t j = ident_end(s, i);
            char *id = xstrndup(s + i, j - i);
            const Macro *m = NULL;
            if (!act) pp->use = span_make(pp->file, pp->line, (int)i + 1, (int)(j - i));
            if (s[j] == '\'') { /* L'X, C'..': a prefix, not a name */
                buf_puts(&out, id);
                i = j;
            } else if (builtin_text(pp, id, &out)) {
                i = j;
            } else if ((m = strmap_get(&pp->macros, id)) == NULL || in_active(act, m)) {
                buf_puts(&out, id);
                i = j;
            } else {
                char *body = NULL;
                size_t next = j;
                if (m->fn) {
                    StrVec args = {0};
                    long end;
                    if (s[j] != '(') {
                        buf_puts(&out, id);
                        i = j;
                        free(id);
                        continue;
                    }
                    end = parse_args(s, j, &args);
                    if (end < 0) {
                        diag_help(rep_errorf(pp->rep, "ZA4041", pp->use,
                                             "unterminated argument list in call of macro \"%s\"",
                                             id),
                                  "a macro call must be closed on the same line");
                        buf_puts(&out, id);
                        i = j;
                        free(id);
                        continue;
                    }
                    next = (size_t)end + 1;
                    if ((int)args.n != m->nparams && !(m->nparams == 0 && args.n == 1 &&
                                                       args.v[0][0] == 0)) {
                        Diag *d = rep_errorf(pp->rep, "ZA4040", pp->use,
                                             "macro \"%s\" takes %d argument%s, %d given", id,
                                             m->nparams, m->nparams == 1 ? "" : "s", (int)args.n);
                        diag_note(d, m->def, "macro \"%s\" is defined here", id);
                        buf_write(&out, s + i, next - i);
                        i = next;
                        free(id);
                        continue;
                    }
                    {
                        char **raw = args.v, **exp = xmalloc((args.n + 1) * sizeof *exp);
                        size_t q;
                        for (q = 0; q < args.n; q++) exp[q] = expand(pp, args.v[q], act);
                        body = substitute(m, raw, exp);
                    }
                } else {
                    body = substitute(m, NULL, NULL);
                }
                if (!pp->first) pp->first = m;
                {
                    Active na;
                    char *r;
                    na.m = m;
                    na.next = act;
                    r = expand(pp, body, &na);
                    pp->produced += strlen(r);
                    if (pp->produced > PP_MAX_EXPANSION) {
                        rep_errorf(pp->rep, "ZA4042", pp->use,
                                   "the expansion of macro \"%s\" is too large", id);
                        pp->too_big = 1;
                    }
                    buf_puts(&out, r);
                    free(r);
                }
                free(body);
                i = next;
            }
            free(id);
        } else {
            buf_putc(&out, c);
            i++;
        }
    }
    if (pp->too_big && i < n) buf_puts(&out, s + i);
    return xstrdup(buf_cstr(&out));
}

/* ---- macro definition -------------------------------------------------- */

/* "\n" outside quotes becomes a real line break */
static char *convert_breaks(const char *b) {
    Buf out = {0};
    size_t i = 0, n = strlen(b);
    while (i < n) {
        if (b[i] == '\'' && !is_attr_quote(b, i)) {
            long q = close_quote(b, i);
            size_t e = q < 0 ? n : (size_t)q + 1;
            buf_write(&out, b + i, e - i);
            i = e;
        } else if (b[i] == '"' && close_dquote(b, i) >= 0) {
            size_t e = (size_t)close_dquote(b, i) + 1;
            buf_write(&out, b + i, e - i);
            i = e;
        } else if (b[i] == '\\' && b[i + 1] == 'n') {
            buf_putc(&out, '\n');
            i += 2;
        } else {
            buf_putc(&out, b[i++]);
        }
    }
    return xstrdup(buf_cstr(&out));
}

static int same_macro(const Macro *a, const Macro *b) {
    int i;
    if (a->fn != b->fn || a->nparams != b->nparams || strcmp(a->body, b->body)) return 0;
    for (i = 0; i < a->nparams; i++)
        if (strcmp(a->params[i], b->params[i])) return 0;
    return 1;
}

/* p: the text after "#define" */
static void do_define(Pp *pp, const char *p, Span sp) {
    const char *name_p;
    size_t j;
    Macro *m;
    char *trimmed;
    const Macro *old;
    p = skip_blanks(p);
    name_p = p;
    j = is_sym_start((unsigned char)*p) ? ident_end(p, 0) : 0;
    if (j == 0) {
        diag_help(rep_errorf(pp->rep, "ZA4020", sp, "#define needs a macro name"),
                  "for example: #define COUNT 10   or   #define SAVE(r) STM r,r,0(13)");
        return;
    }
    m = xcalloc(sizeof *m);
    m->name = xstrndup(name_p, j);
    m->def = sp;
    if (is_builtin(m->name) || !strcmp(m->name, "defined")) {
        rep_errorf(pp->rep, "ZA4021", sp, "\"%s\" cannot be defined as a macro", m->name);
        return;
    }
    p += j;
    if (*p == '(') { /* function-like: the '(' follows the name directly */
        StrVec ps = {0};
        m->fn = 1;
        p++;
        p = skip_blanks(p);
        if (*p != ')') {
            for (;;) {
                size_t e;
                p = skip_blanks(p);
                e = is_sym_start((unsigned char)*p) ? ident_end(p, 0) : 0;
                if (e == 0) {
                    diag_help(rep_errorf(pp->rep, "ZA4022", sp, "bad parameter list of macro \"%s\"",
                                         m->name),
                              "parameters are names separated by commas");
                    return;
                }
                vec_push(&ps, xstrndup(p, e));
                p = skip_blanks(p + e);
                if (*p == ',') {
                    p++;
                    continue;
                }
                break;
            }
        }
        if (*p != ')') {
            diag_help(rep_errorf(pp->rep, "ZA4022", sp, "bad parameter list of macro \"%s\"",
                                 m->name),
                      "the parameter list must end with ')'");
            return;
        }
        p++;
        m->nparams = (int)ps.n;
        m->params = ps.v;
    }
    p = skip_blanks(p);
    trimmed = rtrim_dup(p);
    m->body = convert_breaks(trimmed);
    free(trimmed);
    old = strmap_get(&pp->macros, m->name);
    if (old && !same_macro(old, m)) {
        Diag *d = rep_warnf(pp->rep, "ZA4023", sp, "macro \"%s\" is redefined", m->name);
        diag_note(d, old->def, "previous definition");
    }
    strmap_put(&pp->macros, m->name, m);
}

/* ---- #if expressions ---------------------------------------------------- */

typedef struct {
    Pp *pp;
    Span sp;
    const char *s;
    size_t i;
    int bad, quiet;
} Ev;

static void ev_fail(Ev *e, const char *code, const char *msg) {
    if (!e->bad && !e->quiet) {
        e->bad = 1;
        rep_errorf(e->pp->rep, code, e->sp, "%s", msg);
    }
    e->bad = 1;
}

static void ev_ws(Ev *e) {
    while (is_blank((unsigned char)e->s[e->i])) e->i++;
}

static int64_t ev_bin(Ev *e, int level);

static int64_t ev_unary(Ev *e) {
    int64_t v;
    ev_ws(e);
    switch (e->s[e->i]) {
    case '!':
        e->i++;
        return !ev_unary(e);
    case '-':
        e->i++;
        return -ev_unary(e);
    case '+':
        e->i++;
        return ev_unary(e);
    case '~':
        e->i++;
        return ~ev_unary(e);
    case '(':
        e->i++;
        v = ev_bin(e, 1);
        ev_ws(e);
        if (e->s[e->i] != ')') {
            ev_fail(e, "ZA4030", "missing ')' in the #if expression");
            return 0;
        }
        e->i++;
        return v;
    }
    if (za_isdigit((unsigned char)e->s[e->i])) {
        size_t j = e->i;
        int64_t n = 0;
        if (e->s[j] == '0' && (e->s[j + 1] == 'x' || e->s[j + 1] == 'X')) {
            j += 2;
            if (!za_isxdigit((unsigned char)e->s[j])) {
                ev_fail(e, "ZA4030", "invalid number in the #if expression");
                return 0;
            }
            for (; za_isxdigit((unsigned char)e->s[j]); j++) {
                int d = za_isdigit((unsigned char)e->s[j]) ? e->s[j] - '0'
                                                            : za_toupper(e->s[j]) - 'A' + 10;
                n = n * 16 + d;
            }
        } else {
            for (; za_isdigit((unsigned char)e->s[j]); j++) n = n * 10 + (e->s[j] - '0');
        }
        if (is_sym_char((unsigned char)e->s[j])) {
            ev_fail(e, "ZA4030", "invalid number in the #if expression");
            return 0;
        }
        e->i = j;
        return n;
    }
    if (is_sym_start((unsigned char)e->s[e->i])) { /* an unknown name is 0 */
        e->i = e->i + ident_end(e->s + e->i, 0);
        return 0;
    }
    ev_fail(e, "ZA4030", e->s[e->i] ? "unexpected text in the #if expression"
                                    : "the #if expression is incomplete");
    return 0;
}

static int match_op(Ev *e, const char *op) {
    size_t n = strlen(op);
    ev_ws(e);
    if (strncmp(e->s + e->i, op, n)) return 0;
    /* '<' must not take the first character of "<=" etc. */
    e->i += n;
    return 1;
}

static int64_t ev_bin(Ev *e, int level) {
    int64_t l, r;
    if (level > 6) return ev_unary(e);
    l = ev_bin(e, level + 1);
    for (;;) {
        ev_ws(e);
        switch (level) {
        case 1:
            if (match_op(e, "||")) {
                int sq = e->quiet;
                if (l) e->quiet = 1;
                r = ev_bin(e, 2);
                e->quiet = sq;
                l = l || r;
                continue;
            }
            break;
        case 2:
            if (match_op(e, "&&")) {
                int sq = e->quiet;
                if (!l) e->quiet = 1;
                r = ev_bin(e, 3);
                e->quiet = sq;
                l = l && r;
                continue;
            }
            break;
        case 3:
            if (match_op(e, "==")) {
                l = l == ev_bin(e, 4);
                continue;
            }
            if (match_op(e, "!=")) {
                l = l != ev_bin(e, 4);
                continue;
            }
            break;
        case 4:
            if (match_op(e, "<=")) {
                l = l <= ev_bin(e, 5);
                continue;
            }
            if (match_op(e, ">=")) {
                l = l >= ev_bin(e, 5);
                continue;
            }
            if (match_op(e, "<")) {
                l = l < ev_bin(e, 5);
                continue;
            }
            if (match_op(e, ">")) {
                l = l > ev_bin(e, 5);
                continue;
            }
            break;
        case 5:
            if (match_op(e, "+")) {
                l = l + ev_bin(e, 6);
                continue;
            }
            if (match_op(e, "-")) {
                l = l - ev_bin(e, 6);
                continue;
            }
            break;
        case 6:
            if (match_op(e, "*")) {
                l = l * ev_bin(e, 7);
                continue;
            }
            if (match_op(e, "/") || match_op(e, "%")) {
                int is_div = e->s[e->i - 1] == '/';
                r = ev_bin(e, 7);
                if (r == 0) {
                    ev_fail(e, "ZA4034", "division by zero in the #if expression");
                    l = 0;
                } else {
                    l = is_div ? l / r : l % r;
                }
                continue;
            }
            break;
        }
        return l;
    }
}

/* replace defined(X) / defined X by 1 or 0 */
static char *replace_defined(Pp *pp, const char *s) {
    Buf out = {0};
    size_t i = 0, n = strlen(s);
    while (i < n) {
        if (is_sym_start((unsigned char)s[i])) {
            size_t j = ident_end(s, i);
            if (j - i == 7 && !strncmp(s + i, "defined", 7)) {
                size_t k = j;
                int paren = 0;
                char *id;
                while (is_blank((unsigned char)s[k])) k++;
                if (s[k] == '(') {
                    paren = 1;
                    k++;
                    while (is_blank((unsigned char)s[k])) k++;
                }
                j = is_sym_start((unsigned char)s[k]) ? ident_end(s, k) : k;
                if (j > k) {
                    id = xstrndup(s + k, j - k);
                    buf_puts(&out, strmap_get(&pp->macros, id) || is_builtin(id) ? "1" : "0");
                    free(id);
                    if (paren) {
                        while (is_blank((unsigned char)s[j])) j++;
                        if (s[j] == ')') j++;
                    }
                    i = j;
                    continue;
                }
            }
            buf_write(&out, s + i, j - i);
            i = j;
        } else {
            buf_putc(&out, s[i++]);
        }
    }
    return xstrdup(buf_cstr(&out));
}

/* evaluates the controlling expression; *ok is 0 after an error */
static int64_t eval_if(Pp *pp, const char *text, Span sp, int *ok) {
    char *a, *b;
    Ev e;
    int64_t v;
    pp->use = sp;
    pp->first = NULL;
    pp->produced = 0;
    pp->too_big = 0;
    a = replace_defined(pp, text);
    b = expand(pp, a, NULL);
    memset(&e, 0, sizeof e);
    e.pp = pp;
    e.sp = sp;
    e.s = b;
    v = ev_bin(&e, 1);
    ev_ws(&e);
    if (!e.bad && e.s[e.i]) ev_fail(&e, "ZA4030", "unexpected text in the #if expression");
    free(a);
    free(b);
    *ok = !e.bad;
    return v;
}

/* ---- directives -------------------------------------------------------- */

static const char *const directive_names[] = {"define", "elif",    "else",  "endif", "error",  "if",
                                              "ifdef",  "ifndef",  "include", "pragma", "undef", "warning"};

/* Recognise "#name"; only these names are directives, so a label such as
 * #COUNT is not affected. */
static const char *directive_of(const char *t, const char **rest) {
    const char *p;
    size_t i, n;
    if (t[0] != '#') return NULL;
    p = skip_blanks(t + 1);
    for (n = 0; za_isalpha((unsigned char)p[n]) && p[n] >= 'a' && p[n] <= 'z'; n++) {
    }
    if (is_sym_char((unsigned char)p[n])) return NULL;
    for (i = 0; i < sizeof directive_names / sizeof directive_names[0]; i++)
        if (strlen(directive_names[i]) == n && !strncmp(directive_names[i], p, n)) {
            *rest = p + n;
            return directive_names[i];
        }
    return NULL;
}

static void push_cond(Pp *pp, int taking, Span sp) {
    Cond c;
    int pa = active(pp);
    c.parent_active = pa;
    c.taking = pa && taking;
    c.taken = c.taking;
    c.seen_else = 0;
    c.sp = sp;
    vec_push(&pp->conds, c);
}

static char *join_path(const char *dir, const char *name) {
    size_t n = strlen(dir);
    if (name[0] == '/' || n == 0) return xstrdup(name);
    if (dir[n - 1] == '/') return xasprintf("%s%s", dir, name);
    return xasprintf("%s/%s", dir, name);
}

static void pp_file(Pp *pp, const char *file, const char *text, size_t len);

static void do_include(Pp *pp, const char *rest, Span sp) {
    const char *p = skip_blanks(rest), *e;
    char open = *p, close;
    char *name, *path = NULL, *src = NULL;
    size_t srclen = 0, i;
    StrVec tried = {0};
    if (open != '"' && open != '<') {
        diag_help(rep_errorf(pp->rep, "ZA4010", sp, "#include needs a file name"),
                  "write #include \"file\" or #include <file>");
        return;
    }
    close = open == '"' ? '"' : '>';
    e = strchr(p + 1, close);
    if (!e || e == p + 1) {
        diag_help(rep_errorf(pp->rep, "ZA4010", sp, "#include needs a file name"),
                  "write #include \"file\" or #include <file>");
        return;
    }
    name = xstrndup(p + 1, (size_t)(e - p - 1));
    if (pp->depth >= PP_MAX_DEPTH) {
        rep_errorf(pp->rep, "ZA4012", sp, "#include nested too deeply (recursive include?)");
        return;
    }
    if (open == '"' && name[0] != '/') { /* the directory of the including file first */
        const char *sl = strrchr(pp->file, '/');
        char *dir = sl ? xstrndup(pp->file, (size_t)(sl - pp->file + 1)) : xstrdup("");
        vec_push(&tried, join_path(dir, name));
        free(dir);
    }
    for (i = 0; i < pp->opt->include_dirs.n; i++)
        vec_push(&tried, join_path(pp->opt->include_dirs.v[i], name));
    for (i = 0; i < tried.n && !src; i++) {
        src = read_file(tried.v[i], &srclen);
        if (src) path = tried.v[i];
    }
    if (!src) {
        Diag *d = rep_errorf(pp->rep, "ZA4011", sp, "cannot open include file \"%s\"", name);
        if (tried.n > 0) {
            Buf b = {0};
            for (i = 0; i < tried.n; i++) buf_printf(&b, "%s%s", i ? ", " : "", tried.v[i]);
            diag_help(d, "searched: %s", buf_cstr(&b));
        } else {
            diag_help(d, "no directory to search; add one with -Idir");
        }
        return;
    }
    rep_add_source(pp->rep, path, src, srclen);
    pp->depth++;
    pp_file(pp, path, src, srclen);
    pp->depth--;
}

static void do_pragma(Pp *pp, const char *rest, Span sp) {
    char *r = str_trim(rest), *word, *arg;
    size_t n = 0;
    while (r[n] && !is_blank((unsigned char)r[n])) n++;
    word = za_strlower(xstrndup(r, n));
    arg = str_trim(r + n);
    if (!strcmp(word, "title")) {
        size_t l = strlen(arg);
        if (l < 2 || (arg[0] != '"' && arg[0] != '\'') || arg[l - 1] != arg[0])
            diag_help(rep_errorf(pp->rep, "ZA4061", sp, "#pragma title needs a quoted text"),
                      "for example: #pragma title \"my program\"");
    } else if (!strcmp(word, "eject")) {
        if (*arg) rep_errorf(pp->rep, "ZA4061", sp, "#pragma eject takes no operand");
    } else if (!strcmp(word, "space")) {
        size_t k;
        for (k = 0; arg[k] && za_isdigit((unsigned char)arg[k]); k++) {
        }
        if (arg[k]) rep_errorf(pp->rep, "ZA4061", sp, "#pragma space takes a number of lines");
    } else if (!strcmp(word, "print")) {
        char *a = za_strlower(xstrdup(arg));
        if (strcmp(a, "on") && strcmp(a, "off") && strcmp(a, "data") && strcmp(a, "nodata") &&
            strcmp(a, "gen") && strcmp(a, "nogen"))
            diag_help(rep_errorf(pp->rep, "ZA4061", sp,
                                 "#pragma print needs on, off, data, nodata, gen or nogen"),
                      "for example: #pragma print off");
    } else {
        Diag *d = rep_warnf(pp->rep, "ZA4060", sp, "unknown #pragma \"%s\" is ignored", word);
        const char *const known[] = {"title", "eject", "space", "print"};
        const char *c = diag_closest(word, known, 4);
        if (c) diag_help(d, "did you mean \"%s\"?", c);
    }
}

static void do_directive(Pp *pp, const char *dir, const char *rest, const char *text) {
    Span sp = line_span(pp, text);
    int is_cond = !strcmp(dir, "if") || !strcmp(dir, "ifdef") || !strcmp(dir, "ifndef") ||
                  !strcmp(dir, "elif") || !strcmp(dir, "else") || !strcmp(dir, "endif");
    if (!is_cond && !active(pp)) return;

    if (!strcmp(dir, "define")) {
        do_define(pp, rest, sp);
    } else if (!strcmp(dir, "undef")) {
        const char *p = skip_blanks(rest);
        size_t j = is_sym_start((unsigned char)*p) ? ident_end(p, 0) : 0;
        if (j == 0) {
            rep_errorf(pp->rep, "ZA4020", sp, "#undef needs a macro name");
        } else {
            char *id = xstrndup(p, j);
            if (strmap_get(&pp->macros, id)) strmap_put(&pp->macros, id, NULL);
            free(id);
        }
    } else if (!strcmp(dir, "include")) {
        do_include(pp, rest, sp);
    } else if (!strcmp(dir, "if")) {
        int ok = 1, v = 0;
        if (active(pp)) v = eval_if(pp, rest, sp, &ok) != 0;
        push_cond(pp, ok && v, sp);
    } else if (!strcmp(dir, "ifdef") || !strcmp(dir, "ifndef")) {
        const char *p = skip_blanks(rest);
        size_t j = is_sym_start((unsigned char)*p) ? ident_end(p, 0) : 0;
        int def = 0;
        if (j == 0) {
            if (active(pp)) rep_errorf(pp->rep, "ZA4030", sp, "#%s needs a macro name", dir);
        } else {
            char *id = xstrndup(p, j);
            def = strmap_get(&pp->macros, id) != NULL || is_builtin(id);
            free(id);
        }
        push_cond(pp, !strcmp(dir, "ifdef") ? def : !def, sp);
    } else if (!strcmp(dir, "elif") || !strcmp(dir, "else")) {
        Cond *c;
        if (pp->conds.n == 0) {
            rep_errorf(pp->rep, "ZA4031", sp, "#%s without a matching #if", dir);
            return;
        }
        c = &pp->conds.v[pp->conds.n - 1];
        if (c->seen_else) {
            Diag *d = rep_errorf(pp->rep, "ZA4032", sp, "#%s after #else", dir);
            diag_note(d, c->sp, "the conditional was opened here");
            return;
        }
        if (!strcmp(dir, "else")) {
            c->seen_else = 1;
            c->taking = c->parent_active && !c->taken;
            c->taken = 1;
        } else if (c->parent_active && !c->taken) {
            int ok = 1;
            int64_t v = eval_if(pp, rest, sp, &ok);
            c->taking = ok && v != 0;
            c->taken = c->taking;
        } else {
            c->taking = 0;
        }
    } else if (!strcmp(dir, "endif")) {
        if (pp->conds.n == 0)
            rep_errorf(pp->rep, "ZA4031", sp, "#endif without a matching #if");
        else
            pp->conds.n--;
    } else if (!strcmp(dir, "error")) {
        char *m = str_trim(rest);
        rep_errorf(pp->rep, "ZA4050", sp, "#error %s", m);
        free(m);
    } else if (!strcmp(dir, "warning")) {
        char *m = str_trim(rest);
        rep_warnf(pp->rep, "ZA4051", sp, "#warning %s", m);
        free(m);
    } else if (!strcmp(dir, "pragma")) {
        do_pragma(pp, rest, sp);
    }
}

/* ---- ordinary lines ----------------------------------------------------- */

/* where the remark of a line starts; text before is subject to expansion */
static size_t remark_start(const char *s) {
    size_t p = 0, n = strlen(s), end;
    if (s[0] == '*' || s[0] == ';') return 0;
    while (s[p] && !is_blank((unsigned char)s[p])) p++; /* name */
    while (is_blank((unsigned char)s[p])) p++;
    if (!s[p]) return n;
    if (!scan_operand_end(s, p, &end)) return n; /* operation, may be NAME(a, b) */
    p = end;
    while (is_blank((unsigned char)s[p])) p++;
    if (!s[p]) return n;
    if (!scan_operand_end(s, p, &end)) return n;
    return end;
}

static int is_comment_or_blank(const char *s) {
    if (s[0] == '*' || s[0] == ';') return 1;
    return *skip_blanks(s) == 0;
}

/* trailing comma check on one output line */
static void check_comma(Pp *pp, const char *part, int line) {
    size_t p = 0, end;
    int continuation = pp->comma;
    if (is_comment_or_blank(part)) return;
    if (pp->comma && !is_blank((unsigned char)part[0]))
        diag_help(rep_warnf(pp->rep, "ZA4070", span_make(pp->file, line, 1, 1),
                            "the previous line ends with a comma, so this line is read as its "
                            "continuation"),
                  "a continuation line should start with a blank; remove the trailing comma if "
                  "this is a new statement");
    pp->comma = 0;
    if (continuation) {
        while (is_blank((unsigned char)part[p])) p++;
    } else {
        while (part[p] && !is_blank((unsigned char)part[p])) p++;
        while (is_blank((unsigned char)part[p])) p++;
        if (!part[p]) return;
        if (!scan_operand_end(part, p, &end)) return;
        p = end;
        while (is_blank((unsigned char)part[p])) p++;
        if (!part[p]) return;
    }
    if (scan_operand_end(part, p, &end) && end > p && part[end - 1] == ',') pp->comma = 1;
}

/* ---- macro calls ----------------------------------------------------------- */

typedef struct {
    char *name, *op, *operands;
} StmtFields;

/* name, operation and operand field of an output line; 0 for a comment, a blank line or a line
 * without an operation */
static int stmt_fields(const char *part, StmtFields *f) {
    size_t p = 0, q, end;
    if (is_comment_or_blank(part)) return 0;
    if (!is_blank((unsigned char)part[0])) {
        while (part[p] && !is_blank((unsigned char)part[p])) p++;
        f->name = xstrndup(part, p);
    } else {
        f->name = xstrdup("");
    }
    while (is_blank((unsigned char)part[p])) p++;
    q = p;
    while (part[q] && !is_blank((unsigned char)part[q])) q++;
    if (q == p) return 0;
    f->op = xstrndup(part + p, q - p);
    p = q;
    while (is_blank((unsigned char)part[p])) p++;
    if (!part[p]) {
        f->operands = xstrdup("");
        return 1;
    }
    if (!scan_operand_end(part, p, &end)) end = strlen(part);
    f->operands = xstrndup(part + p, end - p);
    return 1;
}

static int op_chars_ok(const char *op) {
    size_t i;
    if (!op[0]) return 0;
    for (i = 0; op[i]; i++)
        if (!(za_isalnum((unsigned char)op[i]) || op[i] == '_' || op[i] == '@' || op[i] == '#' || op[i] == '$'))
            return 0;
    return 1;
}

static int try_library_dir(Pp *pp, const char *dir, const char *lname) {
    static const char *const exts[] = {".zmac", ".mac"};
    size_t i;
    for (i = 0; i < 2; i++) {
        char *file = xasprintf("%s%s", lname, exts[i]);
        char *path = join_path(dir, file);
        size_t len;
        char *src = read_file(path, &len);
        if (src) {
            rep_add_source(pp->rep, path, src, len);
            mac_load_library(pp->mac, path, src, len);
            return 1;
        }
    }
    return 0;
}

/* is op a macro call: defined in the source or found in the macro directories? */
static int is_macro_op(Pp *pp, const char *op) {
    char *l;
    size_t i;
    if (mac_defined(pp->mac, op)) return 1;
    if (pp->opt->macro_dirs.n == 0 && pp->opt->include_dirs.n == 0) return 0;
    if (!op_chars_ok(op) || mac_known_op(op)) return 0;
    l = za_strlower(xstrdup(op));
    if (strmap_get(&pp->lib_tried, l)) return 0;
    strmap_put(&pp->lib_tried, l, (void *)1);
    for (i = 0; i < pp->opt->macro_dirs.n; i++)
        if (try_library_dir(pp, pp->opt->macro_dirs.v[i], l)) return mac_defined(pp->mac, op);
    for (i = 0; i < pp->opt->include_dirs.n; i++)
        if (try_library_dir(pp, pp->opt->include_dirs.v[i], l)) return mac_defined(pp->mac, op);
    return 0;
}

static void do_macro_call(Pp *pp, const char *name, const char *op, const char *operands) {
    OperandVec ov = {0};
    char **args;
    size_t i;
    Span cs = span_make(pp->file, pp->line, 1, 1);
    if (operands[0]) split_operands(&ov, operands);
    args = xcalloc((ov.n + 1) * sizeof *args);
    for (i = 0; i < ov.n; i++) args[i] = ov.v[i].text;
    if (pp->mdepth == 0) {
        pp->exp_done = 0;
        pp->exp_macro = za_strupper(op);
        pp->exp_def = mac_def_span(pp->mac, op);
    }
    pp->mdepth++;
    mac_call(pp->mac, op, name, cs, args, ov.n);
    pp->mdepth--;
}

static void emit_line(Pp *pp, const char *text);

/* a statement made of several physical lines (continuation lines joined by '\n') */
static void emit_multi(Pp *pp, const char *text) {
    StrVec parts = {0};
    const char *p = text, *nl;
    size_t i;
    StmtFields f;
    for (;; p = nl + 1) {
        nl = strchr(p, '\n');
        vec_push(&parts, nl ? xstrndup(p, (size_t)(nl - p)) : xstrdup(p));
        if (!nl) break;
    }
    if (!pp->comma && stmt_fields(parts.v[0], &f) && is_macro_op(pp, f.op)) {
        Buf b = {0};
        buf_puts(&b, f.operands);
        for (i = 1; i < parts.n; i++) {
            const char *c = parts.v[i];
            size_t end;
            while (is_blank((unsigned char)*c)) c++;
            if (!scan_operand_end(c, 0, &end)) end = strlen(c);
            buf_write(&b, c, end);
        }
        do_macro_call(pp, f.name, f.op, buf_cstr(&b));
        return;
    }
    for (i = 0; i < parts.n; i++) emit_line(pp, parts.v[i]);
}

static void emit_part(Pp *pp, const char *part) {
    SrcLine l;
    {
        StmtFields f;
        if (!pp->comma && stmt_fields(part, &f)) {
            const char *types[] = {"CSECT", "RSECT", "DSECT", "START", "COM"};
            size_t i;
            char *up = za_strupper(f.op);
            for (i = 0; i < 5; i++)
                if (!strcmp(up, types[i])) {
                    pp->sect = f.name;
                    pp->sectype = xstrdup(types[i]);
                }
        }
    }
    l.file = pp->file;
    l.line = pp->line;
    l.text = xstrdup(part);
    vec_push(pp->out, l);
    check_comma(pp, part, pp->line);
}

static void emit_part_or_call(Pp *pp, const char *part) {
    StmtFields f;
    if (!pp->comma && stmt_fields(part, &f) && is_macro_op(pp, f.op)) {
        do_macro_call(pp, f.name, f.op, f.operands);
        return;
    }
    emit_part(pp, part);
}

static void emit_line(Pp *pp, const char *text) {
    size_t rs;
    char *head, *exp, *full, *p, *nl;
    int changed;
    if (strchr(text, '\n')) {
        emit_multi(pp, text);
        return;
    }
    rs = remark_start(text);
    if (rs == 0) { /* comment line */
        emit_part(pp, text);
        return;
    }
    head = xstrndup(text, rs);
    pp->first = NULL;
    pp->produced = 0;
    pp->too_big = 0;
    exp = expand(pp, head, NULL);
    changed = strcmp(exp, head) != 0;
    full = xasprintf("%s%s", exp, text + rs);
    if (changed && pp->first) {
        char *first_line = xstrdup(full);
        char *brk = strchr(first_line, '\n');
        if (brk) *brk = 0;
        rep_add_expansion(pp->rep, pp->file, pp->line, first_line, pp->first->name, pp->first->def);
        free(first_line);
    }
    for (p = full;; p = nl + 1) {
        nl = strchr(p, '\n');
        if (nl) *nl = 0;
        emit_part_or_call(pp, p);
        if (!nl) break;
    }
    free(head);
    free(exp);
    free(full);
}

static void pp_file(Pp *pp, const char *file, const char *text, size_t len) {
    SrcVec lines = {0};
    size_t i = 0, base = pp->conds.n;
    const char *save_file = pp->file;
    int save_line = pp->line;
    src_split(&lines, file, text, len);
    /* the empty segment after the final newline is not a line */
    if (lines.n > 1 && lines.v[lines.n - 1].text[0] == 0 && len > 0 && text[len - 1] == '\n') lines.n--;
    pp->file = file;
    while (i < lines.n) {
        const char *t = lines.v[i].text;
        int ln = lines.v[i].line;
        size_t n = strlen(t), k = i;
        const char *dir, *rest;
        /* a backslash at the end of a line joins it with the next one */
        if (n > 0 && t[n - 1] == '\\' && k + 1 < lines.n) {
            Buf b = {0};
            for (;;) {
                const char *u = lines.v[k].text;
                size_t un = strlen(u);
                if (un > 0 && u[un - 1] == '\\' && k + 1 < lines.n) {
                    buf_write(&b, u, un - 1);
                    k++;
                } else {
                    buf_puts(&b, u);
                    break;
                }
            }
            t = xstrdup(buf_cstr(&b));
        }
        pp->line = ln;
        {
            int mk = mac_top_kind(t);
            if (mk) {
                /* #macro and the macro statements: a block runs to the matching #} */
                MacText mt;
                size_t used = mac_collect(pp->mac, lines.v, lines.n, i, mk, &mt), j, ei = 0;
                int on = active(pp), rec = pp->opt->events != NULL;
                size_t first = pp->out->n;
                if (rec) ei = pp->opt->events->n;
                for (j = 0; j < used && rec; j++) {
                    PpEvent ev;
                    ev.file = file;
                    ev.line = ev.line_end = lines.v[i + j].line;
                    ev.depth = pp->depth;
                    ev.kind = on ? PE_DIRECTIVE : PE_SKIPPED;
                    ev.text = lines.v[i + j].text;
                    ev.first = first;
                    ev.n = 0;
                    vec_push(pp->opt->events, ev);
                }
                if (on) {
                    if (mk == 1) {
                        mac_define(pp->mac, &mt);
                    } else {
                        mac_exec(pp->mac, &mt);
                        if (rec && pp->out->n > first) {
                            /* the lines made by a block belong to its first line */
                            PpEvent *e0 = &pp->opt->events->v[ei];
                            size_t jj;
                            e0->kind = PE_LINE;
                            e0->n = pp->out->n - first;
                            for (jj = 1; jj < used; jj++) pp->opt->events->v[ei + jj].first = pp->out->n;
                        }
                    }
                }
                pp->line = ln;
                i += used;
                continue;
            }
        }
        dir = directive_of(t, &rest);
        if (!dir && active(pp) && !pp->comma) {
            /* a call of a macro continues on the next lines while its operands end in a comma */
            StmtFields f;
            if (stmt_fields(t, &f) && is_macro_op(pp, f.op)) {
                Buf b = {0};
                int cont = 0;
                buf_puts(&b, t);
                for (;;) {
                    const char *last = cont ? lines.v[k].text : t;
                    StmtFields lf;
                    size_t end, p;
                    const char *ops;
                    if (cont) {
                        p = 0;
                        while (is_blank((unsigned char)last[p])) p++;
                        ops = last + p;
                        if (!*ops || !scan_operand_end(ops, 0, &end)) break;
                        if (end == 0 || ops[end - 1] != ',') break;
                    } else {
                        if (!stmt_fields(last, &lf)) break;
                        end = strlen(lf.operands);
                        if (end == 0 || lf.operands[end - 1] != ',') break;
                    }
                    if (k + 1 >= lines.n) break;
                    k++;
                    buf_putc(&b, '\n');
                    buf_puts(&b, lines.v[k].text);
                    cont = 1;
                }
                if (cont) t = xstrdup(buf_cstr(&b));
            }
        }
        {
            PpEvent ev;
            size_t ei = 0;
            int rec = pp->opt->events != NULL;
            ev.file = file;
            ev.line = ln;
            ev.line_end = lines.v[k].line;
            ev.depth = pp->depth;
            ev.text = t;
            ev.first = pp->out->n;
            ev.n = 0;
            if (dir) {
                int is_cond = !strcmp(dir, "if") || !strcmp(dir, "ifdef") || !strcmp(dir, "ifndef") ||
                              !strcmp(dir, "elif") || !strcmp(dir, "else") || !strcmp(dir, "endif");
                ev.kind = (is_cond || active(pp)) ? PE_DIRECTIVE : PE_SKIPPED;
            } else {
                ev.kind = active(pp) ? PE_LINE : PE_SKIPPED;
            }
            if (rec) {
                ei = pp->opt->events->n;
                vec_push(pp->opt->events, ev);
            }
            if (dir)
                do_directive(pp, dir, rest, t);
            else if (active(pp))
                emit_line(pp, t);
            if (rec && ev.kind == PE_LINE) pp->opt->events->v[ei].n = pp->out->n - ev.first;
        }
        i = k + 1;
    }
    while (pp->conds.n > base) {
        Cond *c = &pp->conds.v[--pp->conds.n];
        rep_errorf(pp->rep, "ZA4033", c->sp, "unterminated #if (no #endif before the end of the file)");
    }
    pp->file = save_file;
    pp->line = save_line;
}

/* ---- the macro host ---------------------------------------------------------- */

static void host_emit_cb(void *user, const char *text, int srcline) {
    Pp *pp = user;
    int save = pp->line;
    if (srcline > 0) pp->line = srcline;
    if (pp->mdepth > 0 && !pp->exp_done) {
        char *first = xstrdup(text), *nl = strchr(first, '\n');
        if (nl) *nl = 0;
        rep_add_expansion(pp->rep, pp->file, pp->line, first, pp->exp_macro, pp->exp_def);
        pp->exp_done = 1;
    }
    emit_line(pp, text);
    pp->line = save;
}

static const char *host_sect_cb(void *user, const char **type) {
    Pp *pp = user;
    *type = pp->sectype ? pp->sectype : "";
    return pp->sect ? pp->sect : "";
}

static int host_defined_cb(void *user, const char *name) {
    Pp *pp = user;
    return strmap_get(&pp->macros, name) != NULL || is_builtin(name);
}

void pp_run(const char *file, const char *text, size_t len, const PpOptions *opt, Reporter *rep,
            SrcVec *out) {
    Pp *pp = xcalloc(sizeof *pp);
    size_t i;
    pp->rep = rep;
    pp->opt = opt;
    pp->out = out;
    pp->file = file;
    pp->base = file;
    {
        MacHost h;
        h.user = pp;
        h.emit = host_emit_cb;
        h.sect = host_sect_cb;
        h.is_defined = host_defined_cb;
        pp->mac = mac_new(rep, &h, opt->macro_steps, opt->elf);
    }
    {
        /* the target: __ELF__ or __GOFF__, __ASCII__ or __EBCDIC__ (before the -D options,
         * which may redefine them) */
        const char *names[2];
        names[0] = opt->elf ? "__ELF__ 1" : "__GOFF__ 1";
        names[1] = za_ascii ? "__ASCII__ 1" : "__EBCDIC__ 1";
        for (i = 0; i < 2; i++) do_define(pp, xstrdup(names[i]), span_make("<command line>", 0, 0, 0));
    }
    for (i = 0; i < opt->defines.n; i++) {
        char *d = xstrdup(opt->defines.v[i]);
        char *eq = strchr(d, '=');
        char *def;
        if (eq) {
            *eq = ' ';
            def = d;
        } else {
            def = xasprintf("%s 1", d);
        }
        do_define(pp, def, span_make("<command line>", 0, 0, 0));
    }
    pp_file(pp, file, text, len);
}
