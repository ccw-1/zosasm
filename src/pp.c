/* pp.c - the preprocessor. See pp.h and DESIGN.md section 8.
 *
 * Diagnostic codes: ZA4010-4012 #include, ZA4020-4023 #define/#undef,
 * ZA4030-4034 conditionals, ZA4040-4042 macro calls, ZA4050/4051 #error and
 * #warning, ZA4060/4061 #pragma, ZA4062 #line, ZA4070 trailing comma, ZA4080/4081 the time
 * of __DATE__ and __TIME__, ZA4036 a comment that is not closed. */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pp.h"
#include "cblock.h"
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
    int mshape; /* the condition was written "(...)", the shape of a macro #if */
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
    int expand_depth; /* nesting of #define expansion (ZA4042) */
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
    int amode, rmode;     /* __AMODE__, __RMODE__: 0 until an AMODE/RMODE statement gives 24, 31 or 64 */
    char *sect, *sectype; /* the last section statement (sys.sect, sys.sectype) */
    char *loc;            /* the last LOCTR name (sys.loc), NULL after a section statement */
    /* aread() reads the lines after the call, #insert queues records */
    SrcVec *rd_lines;
    size_t *rd_next;
    StrVec ains;
    StrVec macro_paths; /* #pragma macro_path directories, owned */
    StrVec env_macro_dirs; /* ZOSASM_MACLIB entries, owned */
    StrVec line_files; /* #line file names, owned */
    StrMap msyn; /* OPSYN of macro names: upper case name -> macro name ("" = deleted) */
    /* the file line being read, blanked and as written (block comments) */
    const char *cur_text, *cur_orig;
    int cblock_label; /* next private label number, shared by all CBLOCKs */
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
                                            "__DATE__",          "__TIME__",
                                            "__AMODE__",         "__RMODE__"};

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
    } else if (!strcmp(id, "__AMODE__")) {
        buf_printf(out, "%d", pp->amode);
    } else if (!strcmp(id, "__RMODE__")) {
        buf_printf(out, "%d", pp->rmode);
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
    if (pp->expand_depth >= 200) {
        if (!pp->too_big)
            rep_errorf(pp->rep, "ZA4042", pp->use, "macro expansion nested too deeply");
        pp->too_big = 1;
        return xstrdup("");
    }
    pp->expand_depth++;
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
    {
        char *r = xstrdup(buf_cstr(&out));
        free(out.p);
        pp->expand_depth--;
        return r;
    }
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
    {
        char *r = xstrdup(buf_cstr(&out));
        free(out.p);
        return r;
    }
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
        free(m->name);
        free(m);
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
                    { size_t qi; for (qi=0; qi<ps.n; qi++) free(ps.v[qi]); free(ps.v); }
                    free(m->body); free(m->name); free(m);
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
            { size_t qi; for (qi=0; qi<ps.n; qi++) free(ps.v[qi]); free(ps.v); }
            free(m->body); free(m->name); free(m);
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
    int depth; /* nesting of the expression being evaluated */
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

/* enters one nesting level; 0 = ok, 1 = too deep (already reported) */
static int ev_deep(Ev *e) {
    if (++e->depth > 200) {
        ev_fail(e, "ZA4030", "the #if expression is nested too deeply");
        return 1;
    }
    return 0;
}

static int64_t ev_bin(Ev *e, int level);

static int64_t ev_unary(Ev *e) {
    int64_t v;
    ev_ws(e);
    switch (e->s[e->i]) {
    case '!':
        e->i++;
        if (ev_deep(e)) return 0;
        v = !ev_unary(e);
        e->depth--;
        return v;
    case '-':
        e->i++;
        if (ev_deep(e)) return 0;
        v = (int64_t)(0 - (uint64_t)ev_unary(e));
        e->depth--;
        return v;
    case '+':
        e->i++;
        if (ev_deep(e)) return 0;
        v = ev_unary(e);
        e->depth--;
        return v;
    case '~':
        e->i++;
        if (ev_deep(e)) return 0;
        v = ~ev_unary(e);
        e->depth--;
        return v;
    case '(':
        e->i++;
        if (ev_deep(e)) return 0;
        v = ev_bin(e, 1);
        e->depth--;
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
                if (n > (INT64_MAX - d) / 16) {
                    ev_fail(e, "ZA4030", "the number in the #if expression is too large");
                    return 0;
                }
                n = n * 16 + d;
            }
        } else {
            for (; za_isdigit((unsigned char)e->s[j]); j++) {
                int d = e->s[j] - '0';
                if (n > (INT64_MAX - d) / 10) {
                    ev_fail(e, "ZA4030", "the number in the #if expression is too large");
                    return 0;
                }
                n = n * 10 + d;
            }
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
                l = (int64_t)((uint64_t)l + (uint64_t)ev_bin(e, 6));
                continue;
            }
            if (match_op(e, "-")) {
                l = (int64_t)((uint64_t)l - (uint64_t)ev_bin(e, 6));
                continue;
            }
            break;
        case 6:
            if (match_op(e, "*")) {
                l = (int64_t)((uint64_t)l * (uint64_t)ev_bin(e, 7));
                continue;
            }
            if (match_op(e, "/") || match_op(e, "%")) {
                int is_div = e->s[e->i - 1] == '/';
                r = ev_bin(e, 7);
                if (r == 0) {
                    ev_fail(e, "ZA4034", "division by zero in the #if expression");
                    l = 0;
                } else if (r == -1) {
                    l = is_div ? (int64_t)(0 - (uint64_t)l) : 0;
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

static const char *const directive_names[] = {"define", "elif",    "else",  "endif", "error",  "if", "line",
                                              "ifdef",  "ifndef",  "include", "pragma", "undef", "warning"};

/* a line that is nothing but the macro block closer "#}" */
static int is_block_close(const char *t) {
    size_t i = 1;
    if (t[0] != '#') return 0;
    while (is_blank((unsigned char)t[i])) i++;
    if (t[i] != '}') return 0;
    for (i++; is_blank((unsigned char)t[i]); i++) {
    }
    return t[i] == 0;
}

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
    c.mshape = 0;
    c.sp = sp;
    vec_push(&pp->conds, c);
}

static char *join_path(const char *dir, const char *name) {
    size_t n = strlen(dir);
    if (name[0] == '/' || n == 0) return xstrdup(name);
    if (dir[n - 1] == '/') return xasprintf("%s%s", dir, name);
    return xasprintf("%s/%s", dir, name);
}

/* the directory part of a source path, with its trailing slash ("" for a bare
 * file name) */
static char *source_dir(const char *file) {
    const char *s = file ? strrchr(file, '/') : NULL;
    if (!file || !*file) return xstrdup(".");
    return s ? xstrndup(file, (size_t)(s - file + 1)) : xstrdup(".");
}

static void clear_lib_tried(Pp *pp) {
    size_t i;
    for (i = 0; i < pp->lib_tried.cap; i++) free(pp->lib_tried.keys[i]);
    free(pp->lib_tried.keys);
    free(pp->lib_tried.vals);
    memset(&pp->lib_tried, 0, sizeof pp->lib_tried);
}

static void read_macro_env(Pp *pp) {
    const char *e = getenv("ZOSASM_MACLIB"), *p = e;
    if (!e) return;
    while (*p) {
        const char *q = strchr(p, ':');
        size_t n = q ? (size_t)(q - p) : strlen(p);
        if (n > 0) vec_push(&pp->env_macro_dirs, xstrndup(p, n));
        if (!q) break;
        p = q + 1;
    }
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

/* is this line "#pragma WORD" (the word case insensitive, first word only)? */
static int pragma_word(const char *line, const char *word) {
    const char *dir, *rest;
    size_t n = 0;
    dir = directive_of(line, &rest);
    if (!dir || strcmp(dir, "pragma")) return 0;
    rest = skip_blanks(rest);
    while (rest[n] && !is_blank((unsigned char)rest[n])) n++;
    return n == strlen(word) && !za_strncmpi(rest, word, n);
}

static void do_pragma(Pp *pp, const char *rest, Span sp) {
    char *r = str_trim(rest), *word, *arg;
    size_t n = 0;
    while (r[n] && !is_blank((unsigned char)r[n])) n++;
    word = za_strlower_own(xstrndup(r, n));
    arg = str_trim(r + n);
    if (!strcmp(word, "title")) {
        size_t l = strlen(arg);
        if (l < 2 || (arg[0] != '"' && arg[0] != '\'') || arg[l - 1] != arg[0])
            diag_help(rep_errorf(pp->rep, "ZA4061", sp, "#pragma title needs a quoted text"),
                      "for example: #pragma title \"my program\"");
    } else if (!strcmp(word, "eject")) {
        if (*arg) rep_errorf(pp->rep, "ZA4061", sp, "#pragma eject takes no operand");
    } else if (!strcmp(word, "ceject")) {
        size_t k;
        for (k = 0; arg[k] && za_isdigit((unsigned char)arg[k]); k++) {
        }
        if (arg[k]) rep_errorf(pp->rep, "ZA4061", sp, "#pragma ceject takes a number of lines");
    } else if (!strcmp(word, "space")) {
        size_t k;
        for (k = 0; arg[k] && za_isdigit((unsigned char)arg[k]); k++) {
        }
        if (arg[k]) rep_errorf(pp->rep, "ZA4061", sp, "#pragma space takes a number of lines");
    } else if (!strcmp(word, "cblock") || !strcmp(word, "cbegin") || !strcmp(word, "cend") ||
               !strcmp(word, "cin") || !strcmp(word, "cout") || !strcmp(word, "cinout") ||
               !strcmp(word, "cscratch") || !strcmp(word, "ctemp")) {
        rep_errorf(pp->rep, "ZA5001", sp,
                   "#pragma %s is only valid inside a #pragma cblock block", word);
    } else if (!strcmp(word, "macro_path")) {
        const char *p = skip_blanks(arg), *e, *q;
        char *name, *base, *path;
        int bad = 0;
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            e = strchr(p, quote);
            if (!e) bad = 1;
            else for (q = e + 1; is_blank((unsigned char)*q); q++) {
            }
            if (!bad && *q) bad = 1;
        } else {
            e = p + strcspn(p, " \t");
            for (q = e; is_blank((unsigned char)*q); q++) {
            }
            if (*q) bad = 1;
        }
        if (bad || e == p) {
            diag_help(rep_errorf(pp->rep, "ZA4061", sp,
                                 "#pragma macro_path needs one directory"),
                      "for example: #pragma macro_path \"macros\"");
        } else {
            name = xstrndup(p, (size_t)(e - p));
            base = source_dir(pp->file);
            path = join_path(base, name);
            vec_push(&pp->macro_paths, path);
            free(base);
            free(name);
            clear_lib_tried(pp);
        }
    } else if (!strcmp(word, "print")) {
        char *a = za_strlower_own(xstrdup(arg));
        if (strcmp(a, "on") && strcmp(a, "off") && strcmp(a, "data") && strcmp(a, "nodata") &&
            strcmp(a, "gen") && strcmp(a, "nogen"))
            diag_help(rep_errorf(pp->rep, "ZA4061", sp,
                                 "#pragma print needs on, off, data, nodata, gen or nogen"),
                      "for example: #pragma print off");
    } else {
        Diag *d = rep_warnf(pp->rep, "ZA4060", sp, "unknown #pragma \"%s\" is ignored", word);
        const char *const known[] = {"title", "eject", "ceject", "space", "print", "macro_path"};
        const char *c = diag_closest(word, known, 6);
        if (c) diag_help(d, "did you mean \"%s\"?", c);
    }
    free(r);
}


/* #line N ["file"]: renumber the remaining lines of the current file. The
 * line of the directive itself is not changed. */
static void do_line(Pp *pp, const char *rest, Span sp) {
    const char *p = skip_blanks(rest), *q;
    char *end = NULL, *file = NULL;
    long n;
    size_t start, j;
    if (!za_isdigit((unsigned char)*p)) {
        diag_help(rep_errorf(pp->rep, "ZA4062", sp, "#line needs a line number"),
                  "for example: #line 100 \"other.za\"");
        return;
    }
    n = strtol(p, &end, 10);
    if (n < 1 || n > 0x7fffffffL || (end && *end && !is_blank((unsigned char)*end))) {
        rep_errorf(pp->rep, "ZA4062", sp, "#line has an invalid line number");
        return;
    }
    q = end;
    while (is_blank((unsigned char)*q)) q++;
    if (*q) {
        char quote;
        if (*q != '"' && *q != '\'') {
            rep_errorf(pp->rep, "ZA4062", sp, "#line file name must be quoted");
            return;
        }
        quote = *q++;
        end = strchr(q, quote);
        if (!end) {
            rep_errorf(pp->rep, "ZA4062", sp, "#line file name has no closing quote");
            return;
        }
        file = xstrndup(q, (size_t)(end - q));
        for (q = end + 1; is_blank((unsigned char)*q); q++) {
        }
        if (*q) {
            rep_errorf(pp->rep, "ZA4062", sp, "#line has text after the file name");
            free(file);
            return;
        }
    }
    if (!pp->rd_lines || !pp->rd_next) {
        rep_errorf(pp->rep, "ZA4062", sp, "#line is only valid in a source file");
        free(file);
        return;
    }
    start = *pp->rd_next;
    for (j = start; j < pp->rd_lines->n; j++) {
        pp->rd_lines->v[j].line = (int)n + (int)(j - start);
        if (file) pp->rd_lines->v[j].file = file;
    }
    if (file) vec_push(&pp->line_files, file);
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
        /* "(cond)" is how a macro #if writes its condition: remember it so the
         * unterminated-#if error can say what probably happened */
        pp->conds.v[pp->conds.n - 1].mshape = *skip_blanks(rest) == '(';
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
    } else if (!strcmp(dir, "line")) {
        do_line(pp, rest, sp);
    } else if (!strcmp(dir, "pragma")) {
        do_pragma(pp, rest, sp);
    }
}

/* ---- ordinary lines ----------------------------------------------------- */

/* where the remark of a line starts; text before is subject to expansion */
static size_t remark_start(const char *s) {
    size_t p = (size_t)(caret_rest(s) - s), n = strlen(s), end;
    if (s[p] == '*' || s[p] == ';') return 0;
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
    s = caret_rest(s);
    if (s[0] == '*' || s[0] == ';') return 1;
    return *skip_blanks(s) == 0;
}

/* trailing comma check on one output line */
static void check_comma(Pp *pp, const char *part, int line) {
    size_t p = 0, end;
    int continuation = pp->comma;
    const char *raw = part;
    part = caret_rest(part);
    if (continuation ? (*skip_blanks(part) == 0 || part[0] == '*' || part[0] == ';')
                     : is_comment_or_blank(raw))
        return;
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
    part = caret_rest(part);
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

static void stmt_fields_free(StmtFields *f) {
    free(f->name);
    free(f->op);
    free(f->operands);
    f->name = f->op = f->operands = NULL;
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
    /* the member is found by its lower case or its upper case name (hlasm2za -macro-dir keeps the
     * case of the HLASM member) */
    for (i = 0; i < 4; i++) {
        char *base = i < 2 ? xstrdup(lname) : za_strupper_own(xstrdup(lname));
        char *file = xasprintf("%s%s", base, exts[i & 1]);
        char *path = join_path(dir, file);
        size_t len;
        char *src = read_file(path, &len);
        free(base);
        free(file);
        if (src) {
            rep_add_source(pp->rep, path, src, len);
            mac_load_library(pp->mac, path, src, len);
            /* path/src live in rep/mac state for diagnostic locations; keep them. */
            return 1;
        }
        free(path);
    }
    return 0;
}

/* the macro that the operation code op names after OPSYN ("" = deleted) */
static const char *mac_alias(Pp *pp, const char *op) {
    const char *r;
    if (pp->msyn.n == 0) return op;
    r = strmap_get(&pp->msyn, za_strupper(op));
    return r ? r : op;
}

/* is op a macro call: defined in the source or found in the macro directories? */
static int is_macro_op(Pp *pp, const char *op) {
    char *l;
    size_t i;
    op = mac_alias(pp, op);
    if (!*op) return 0;
    if (mac_defined(pp->mac, op)) return 1;
    if (!op_chars_ok(op) || mac_known_op(op)) return 0;
    l = za_strlower_own(xstrdup(op));
    if (strmap_get(&pp->lib_tried, l)) { free(l); return 0; }
    strmap_put(&pp->lib_tried, l, (void *)1);
    {
        char *sdir = source_dir(pp->file);
        int found = 0;
        /* source directive, then -M, then the source directory, then the
         * environment, then -I */
        for (i = 0; !found && i < pp->macro_paths.n; i++)
            found = try_library_dir(pp, pp->macro_paths.v[i], l);
        for (i = 0; !found && i < pp->opt->macro_dirs.n; i++)
            found = try_library_dir(pp, pp->opt->macro_dirs.v[i], l);
        if (!found && *sdir) found = try_library_dir(pp, sdir, l);
        for (i = 0; !found && i < pp->env_macro_dirs.n; i++)
            found = try_library_dir(pp, pp->env_macro_dirs.v[i], l);
        for (i = 0; !found && i < pp->opt->include_dirs.n; i++)
            found = try_library_dir(pp, pp->opt->include_dirs.v[i], l);
        free(sdir);
        free(l);
        return found && mac_defined(pp->mac, op);
    }
}

static void flush_inserts(Pp *pp);

static void do_macro_call(Pp *pp, const char *name, const char *op, const char *operands) {
    OperandVec ov = {0};
    char **args;
    size_t i;
    Span cs = span_make(pp->file, pp->line, 1, 1);
    op = mac_alias(pp, op);
    if (operands[0]) split_operands(&ov, operands);
    args = xcalloc((ov.n + 1) * sizeof *args);
    for (i = 0; i < ov.n; i++) args[i] = ov.v[i].text;
    if (pp->mdepth == 0) {
        free((char *)pp->exp_macro);
        pp->exp_done = 0;
        pp->exp_macro = za_strupper(op);
        pp->exp_def = mac_def_span(pp->mac, op);
    }
    pp->mdepth++;
    mac_call(pp->mac, op, name, cs, args, ov.n);
    pp->mdepth--;
    if (pp->mdepth == 0) flush_inserts(pp);
    free(args);
    for (i = 0; i < ov.n; i++) free(ov.v[i].text);
    free(ov.v);
}

static void emit_line(Pp *pp, const char *text, int first_line, const int *plines, const size_t *poffs, size_t nplines);
static void emit_part(Pp *pp, const char *part, int line);

/* the records queued by #insert are read after the outermost macro: they are rescanned like
 * generated lines (so a macro call or a macro definition in them works only as a call) */
static void flush_inserts(Pp *pp) {
    while (pp->ains.n > 0) {
        char *t = pp->ains.v[0];
        memmove(pp->ains.v, pp->ains.v + 1, (pp->ains.n - 1) * sizeof *pp->ains.v);
        pp->ains.n--;
        emit_line(pp, t, pp->line, NULL, NULL, 0);
    }
}

/* a statement made of several physical lines (continuation lines joined by '\n') */
static void emit_multi(Pp *pp, const char *text, int first_line, const int *plines, const size_t *poffs, size_t nplines) {
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
        stmt_fields_free(&f);
        free(b.p);
        for (i = 0; i < parts.n; i++) free(parts.v[i]);
        free(parts.v);
        return;
    }
    if (plines && nplines == parts.n) {
        /* keep as one logical SrcLine; reader uses plines/poffs for physical attribution */
        emit_part(pp, text, first_line);
        {
            SrcLine *sl = &pp->out->v[pp->out->n - 1];
            int *pl = xmalloc(sizeof(int) * nplines);
            size_t *po = xmalloc(sizeof(size_t) * nplines);
            size_t off = 0;
            memcpy(pl, plines, sizeof(int) * nplines);
            if (poffs) memcpy(po, poffs, sizeof(size_t) * nplines);
            else for (i = 0; i < nplines; i++) { po[i] = off; off += strlen(parts.v[i]); if (i + 1 < nplines) off++; }
            sl->plines = pl;
            sl->poffs = po;
            sl->nplines = nplines;
        }
        for (i = 0; i < parts.n; i++) free(parts.v[i]);
        free(parts.v);
        return;
    }
    for (i = 0; i < parts.n; i++) emit_part(pp, parts.v[i], first_line + (int)i);
    for (i = 0; i < parts.n; i++) free(parts.v[i]);
    free(parts.v);
}

static void emit_part(Pp *pp, const char *part, int line) {
    SrcLine l;
    {
        StmtFields f;
        if (!pp->comma && stmt_fields(part, &f)) {
            const char *types[] = {"CSECT", "RSECT", "DSECT", "START", "COM"};
            size_t i;
            char *up = za_strupper(f.op);
            for (i = 0; i < 5; i++)
                if (!strcmp(up, types[i])) {
                    free(pp->sect);
                    pp->sect = xstrdup(f.name);
                    free(pp->sectype);
                    pp->sectype = xstrdup(types[i]);
                    free(pp->loc);
                    pp->loc = NULL;
                }
            if (!strcmp(up, "LOCTR")) {
                free(pp->loc);
                pp->loc = xstrdup(f.name);
            }
            if (!strcmp(up, "AMODE") || !strcmp(up, "RMODE")) {
                char *t = str_trim(f.operands), *v = za_strupper(t);
                int n = !strcmp(v, "24") ? 24 : !strcmp(v, "31") ? 31 : !strcmp(v, "64") ? 64 : 0;
                if (n == 0 && up[0] == 'R' && !strcmp(v, "ANY")) n = 31; /* as the assembler reads it */
                if (n) {
                    if (up[0] == 'A')
                        pp->amode = n;
                    else
                        pp->rmode = n;
                }
                free(v);
                free(t);
            }
            free(up);
            stmt_fields_free(&f);
        }
    }
    l.file = pp->file;
    l.line = line;
    l.text = xstrdup(part);
    l.orig = pp->cur_orig && pp->line == line && !strcmp(part, pp->cur_text) ? xstrdup(pp->cur_orig) : NULL;
    l.plines = NULL;
    l.poffs = NULL;
    l.nplines = 0;
    vec_push(pp->out, l);
    check_comma(pp, part, line);
}

/* name OPSYN op where a macro is involved: the preprocessor keeps the alias and the
 * statement is not passed on (1); otherwise the assembler handles it (0) */
static int opsyn_macro(Pp *pp, const StmtFields *f) {
    char *name = za_strupper(f->name), *tgt = za_strupper(str_trim(f->operands)), *sp;
    if (!*name || !op_chars_ok(name)) return 0;
    sp = strpbrk(tgt, " \t");
    if (sp) *sp = 0;
    if (!*tgt) {
        if (is_macro_op(pp, name)) strmap_put(&pp->msyn, name, xstrdup(""));
        return 0;
    }
    if (is_macro_op(pp, tgt)) {
        strmap_put(&pp->msyn, name, xstrdup(mac_alias(pp, tgt)));
        return 1;
    }
    if (is_macro_op(pp, name)) strmap_put(&pp->msyn, name, xstrdup(""));
    return 0;
}

static void emit_part_or_call(Pp *pp, const char *part, int line) {
    StmtFields f;
    if (!pp->comma && stmt_fields(part, &f)) {
        if (!za_strncmpi(f.op, "OPSYN", 6) && opsyn_macro(pp, &f)) {
            stmt_fields_free(&f);
            return;
        }
        if (is_macro_op(pp, f.op)) {
            do_macro_call(pp, f.name, f.op, f.operands);
            stmt_fields_free(&f);
            return;
        }
        stmt_fields_free(&f);
    }
    emit_part(pp, part, line);
}

static void emit_line(Pp *pp, const char *text, int first_line, const int *plines, const size_t *poffs, size_t nplines) {
    size_t rs;
    char *head, *exp, *full, *p, *nl;
    int changed;
    if (strchr(text, '\n')) {
        emit_multi(pp, text, first_line, plines, poffs, nplines);
        return;
    }
    rs = remark_start(text);
    if (rs == 0) { /* comment line */
        emit_part(pp, text, first_line);
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
    {
        int pline = first_line;
        size_t segi = 0;
        size_t first_out = pp->out->n;
        for (p = full;; p = nl + 1) {
            nl = strchr(p, '\n');
            if (nl) *nl = 0;
            {
                int this_line = (plines && segi < nplines) ? plines[segi] : pline;
                emit_part_or_call(pp, p, this_line);
            }
            pline++;
            segi++;
            if (!nl) break;
        }
        if (plines && nplines > 0 && pp->out->n == first_out + 1) {
            SrcLine *sl = &pp->out->v[first_out];
            if (!sl->plines) {
                int *pl2 = xmalloc(sizeof(int) * nplines);
                size_t *po2 = xmalloc(sizeof(size_t) * nplines);
                memcpy(pl2, plines, sizeof(int) * nplines);
                if (poffs) memcpy(po2, poffs, sizeof(size_t) * nplines);
                else for (segi = 0; segi < nplines; segi++) po2[segi] = 0;
                sl->plines = pl2; sl->poffs = po2; sl->nplines = nplines;
            }
        }
    }
    free(head);
    free(exp);
    free(full);
}

static void pp_file(Pp *pp, const char *file, const char *text, size_t len) {
    SrcVec lines = {0};
    size_t i = 0, base = pp->conds.n, nxt = 0;
    const char *save_file = pp->file;
    int save_line = pp->line;
    SrcVec *save_rd = pp->rd_lines;
    size_t *save_rn = pp->rd_next;
    src_split(&lines, file, text, len);
    {
        char *basebuf = lines.n ? (char *)lines.v[0].text : NULL;
        /* the empty segment after the final newline is not a line */
    if (lines.n > 1 && lines.v[lines.n - 1].text[0] == 0 && len > 0 && text[len - 1] == '\n') lines.n--;
        src_blank_comments(&lines, pp->rep);
        pp->file = file;
    pp->rd_lines = &lines;
    pp->rd_next = &nxt;
    while (i < lines.n) {
        const char *t = lines.v[i].text;
        int ln = lines.v[i].line;
        size_t n = strlen(t), k = i;
        const char *dir, *rest = NULL;
        /* a backslash at the end of a line joins it with the next one */
        char *joined_bs = NULL;
        int *joined_pl = NULL;
        size_t *joined_po = NULL;
        size_t joined_pn = 0;
        if (n > 0 && t[n - 1] == '\\' && k + 1 < lines.n) {
            Buf b = {0};
            size_t cap = 4;
            joined_pl = xmalloc(sizeof(int) * cap);
            joined_po = xmalloc(sizeof(size_t) * cap);
            for (;;) {
                const char *u = lines.v[k].text;
                size_t un = strlen(u);
                if (joined_pn == cap) { cap *= 2; joined_pl = xrealloc(joined_pl, sizeof(int) * cap); joined_po = xrealloc(joined_po, sizeof(size_t) * cap); }
                joined_pl[joined_pn] = lines.v[k].line;
                joined_po[joined_pn] = b.len;
                joined_pn++;
                if (un > 0 && u[un - 1] == '\\' && k + 1 < lines.n) {
                    buf_write(&b, u, un - 1);
                    k++;
                } else {
                    buf_puts(&b, u);
                    break;
                }
            }
            joined_bs = xstrdup(buf_cstr(&b));
            free(b.p);
            t = joined_bs;
        }
        pp->line = ln;
        pp->file = lines.v[i].file;
        pp->cur_text = lines.v[i].text;
        pp->cur_orig = lines.v[i].orig;
        {
            int mk = mac_top_kind(t);
            if (mk) {
                /* #macro and the macro statements: a block runs to the matching #} */
                MacText mt;
                size_t used = mac_collect(pp->mac, lines.v, lines.n, i, mk, &mt), j, ei = 0;
                int on = active(pp), rec = pp->opt->events != NULL;
                size_t first = pp->out->n;
                nxt = i + used;
                if (rec) ei = pp->opt->events->n;
                for (j = 0; j < used && rec; j++) {
                    PpEvent ev;
                    ev.file = file;
                    ev.line = ev.line_end = lines.v[i + j].line;
                    ev.depth = pp->depth;
                    ev.kind = on ? PE_DIRECTIVE : PE_SKIPPED;
                    ev.text = xstrdup(lines.v[i + j].orig ? lines.v[i + j].orig : lines.v[i + j].text);
                    ev.first = first;
                    ev.n = 0;
                    vec_push(pp->opt->events, ev);
                }
                if (on) {
                    if (mk == 1) {
                        mac_define(pp->mac, &mt);
                    } else {
                        mac_exec(pp->mac, &mt);
                        flush_inserts(pp);
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
                mac_text_free(&mt);
                pp->line = ln;
                i = nxt;
                continue;
            }
        }
        dir = directive_of(t, &rest);
        if (dir && !strcmp(dir, "pragma") && pragma_word(t, "cblock")) {
            size_t j, end = lines.n;
            int nested = 0;
            for (j = i + 1; j < lines.n; j++) {
                if (pragma_word(lines.v[j].text, "cend")) { end = j; break; }
                if (pragma_word(lines.v[j].text, "cblock")) nested = 1;
            }
            if (end == lines.n) {
                rep_errorf(pp->rep, "ZA5001", span_make(pp->file, ln, 1, 1),
                           "unterminated #pragma cblock (no #pragma cend)");
                free(joined_bs); free(joined_pl); free(joined_po);
                i = nxt = lines.n;
                continue;
            }
            if (nested)
                rep_errorf(pp->rep, "ZA5001", span_make(pp->file, lines.v[end].line, 1, 1),
                           "a #pragma cblock cannot be nested");
            if (active(pp)) {
                CBlockLineVec body = {0};
                const char *head = skip_blanks(rest), *hp = head;
                size_t first = pp->out->n;
                while (*hp && !is_blank((unsigned char)*hp)) hp++;
                head = skip_blanks(hp);
                for (j = i + 1; j < end; j++) {
                    CBlockLine l;
                    l.text = lines.v[j].text;
                    l.line = lines.v[j].line;
                    vec_push(&body, l);
                }
                cblock_compile(file, ln, head, body.v, body.n, pp->rep, pp->out, &pp->cblock_label,
                               pp->opt->cblock_debug, pp->opt->elf);
                free(body.v);
                if (pp->opt->events) {
                    size_t q;
                    for (q = first; q < pp->out->n; q++) {
                        PpEvent ev;
                        memset(&ev, 0, sizeof ev);
                        ev.file = file;
                        ev.line = ev.line_end = pp->out->v[q].line;
                        ev.depth = pp->depth;
                        ev.kind = PE_LINE;
                        ev.text = xstrdup(pp->out->v[q].orig ? pp->out->v[q].orig : pp->out->v[q].text);
                        ev.first = q;
                        ev.n = 1;
                        vec_push(pp->opt->events, ev);
                    }
                }
            }
            pp->line = lines.v[end].line;
            free(joined_bs); free(joined_pl); free(joined_po);
            i = nxt = end + 1;
            continue;
        }
        if (!dir && active(pp) && !pp->comma) {
            /* a call of a macro continues on the next lines while its operands end in a comma */
            StmtFields f = {0};
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
                        if (end == 0 || lf.operands[end - 1] != ',') {
                            stmt_fields_free(&lf);
                            break;
                        }
                        stmt_fields_free(&lf);
                    }
                    if (k + 1 >= lines.n) break;
                    k++;
                    buf_putc(&b, '\n');
                    buf_puts(&b, lines.v[k].text);
                    cont = 1;
                }
                if (cont) t = xstrdup(buf_cstr(&b));
                free(b.p);
            }
            stmt_fields_free(&f);
        }
        {
            PpEvent ev;
            size_t ei = 0;
            int rec = pp->opt->events != NULL;
            char *joined_t = (t != lines.v[i].text) ? (char *)t : NULL;
            ev.file = file;
            ev.line = ln;
            ev.line_end = lines.v[k].line;
            ev.depth = pp->depth;
            ev.text = rec ? xstrdup(joined_t ? t : (lines.v[i].orig ? lines.v[i].orig : t)) : NULL;
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
            nxt = k + 1;
            if (dir)
                do_directive(pp, dir, rest, t);
            else if (active(pp)) {
                if (is_block_close(t))
                    diag_help(rep_errorf(pp->rep, "ZA4035", span_make(pp->file, pp->line, 1, 2),
                                         "this \"#}\" closes nothing: no block of a macro "
                                         "statement is open"),
                              "a macro #if/#while/#for/#switch needs its \"{\" on the same "
                              "line; the block ends with #}");
                else {
                    if (joined_t || joined_pl) {
                        int *pl = joined_pl ? joined_pl : xmalloc(sizeof(int) * (k - i + 1));
                        size_t *po = joined_po ? joined_po : NULL;
                        size_t pn = joined_pn;
                        if (!joined_pl) {
                            size_t pj, off = 0; pn = 0;
                            po = xmalloc(sizeof(size_t) * (k - i + 1));
                            for (pj = i; pj <= k; pj++) { pl[pn] = lines.v[pj].line; po[pn] = off; off += strlen(lines.v[pj].text) + 1; pn++; }
                        }
                        /* emit with plines/poffs */
                        emit_line(pp, t, ln, pl, po, pn);
                        {
                            SrcLine *sl = &pp->out->v[pp->out->n - 1];
                            if (sl->nplines == 0 && pn > 0) {
                                int *pl2 = xmalloc(sizeof(int)*pn); size_t *po2 = xmalloc(sizeof(size_t)*pn);
                                memcpy(pl2, pl, sizeof(int)*pn); memcpy(po2, po, sizeof(size_t)*pn);
                                sl->plines = pl2; sl->poffs = po2; sl->nplines = pn;
                            }
                        }
                        if (!joined_pl) { free(pl); free(po); }
                    } else {
                        emit_line(pp, t, ln, NULL, NULL, 0);
                    }
                }
            }
            if (rec && ev.kind == PE_LINE) pp->opt->events->v[ei].n = pp->out->n - ev.first;
            if (joined_t == joined_bs) joined_bs = NULL;
            free(joined_t);
        }
        free(joined_bs);
        free(joined_pl);
        free(joined_po);
        i = nxt;
    }
    while (pp->conds.n > base) {
        Cond *c = &pp->conds.v[--pp->conds.n];
        Diag *d = rep_errorf(pp->rep, "ZA4033", c->sp,
                             "unterminated #if (no #endif before the end of the file)");
        if (c->mshape)
            diag_help(d, "if this was a macro #if block, its \"{\" must be on the same line as "
                         "the #if, and the block ends with \"#}\"");
    }
        pp->file = save_file;
        pp->line = save_line;
        pp->rd_lines = save_rd;
        pp->rd_next = save_rn;
        for (i = 0; i < lines.n; i++)
            if (lines.v[i].orig) free((char *)lines.v[i].text);
        free(basebuf);
        free(lines.v);
    }
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
        free(first);
        pp->exp_done = 1;
    }
    emit_line(pp, text, srcline > 0 ? srcline : pp->line, NULL, NULL, 0);
    pp->line = save;
}

static const char *host_sect_cb(void *user, const char **type) {
    Pp *pp = user;
    *type = pp->sectype ? pp->sectype : "";
    return pp->sect ? pp->sect : "";
}

static const char *host_loc_cb(void *user) {
    Pp *pp = user;
    return pp->loc ? pp->loc : pp->sect;
}

static int host_mode_cb(void *user, int is_amode) {
    Pp *pp = user;
    return is_amode ? pp->amode : pp->rmode;
}

static int host_now_cb(void *user, time_t *t) {
    Pp *pp = user;
    pp->use = span_make(pp->file, pp->line, 1, 1);
    if (!pp_now(pp)) return 0;
    *t = pp->now;
    return 1;
}

static const char *host_parm_cb(void *user) {
    Pp *pp = user;
    return pp->opt->sysparm;
}

static char *host_aread_cb(void *user) {
    Pp *pp = user;
    const char *src;
    char *r;
    size_t n;
    if (pp->ains.n > 0) {
        src = pp->ains.v[0];
        memmove(pp->ains.v, pp->ains.v + 1, (pp->ains.n - 1) * sizeof *pp->ains.v);
        pp->ains.n--;
    } else if (pp->rd_lines && pp->rd_next && *pp->rd_next < pp->rd_lines->n) {
        src = pp->rd_lines->v[(*pp->rd_next)++].text;
    } else {
        return NULL;
    }
    n = strlen(src);
    if (n > 80) n = 80;
    r = xmalloc(81);
    memcpy(r, src, n);
    memset(r + n, ' ', 80 - n);
    r[80] = 0;
    return r;
}

static void host_insert_cb(void *user, const char *text, int front) {
    Pp *pp = user;
    char *t = xstrdup(text);
    if (strlen(t) > 80) t[80] = 0;
    vec_push(&pp->ains, t);
    if (front) {
        memmove(pp->ains.v + 1, pp->ains.v, (pp->ains.n - 1) * sizeof *pp->ains.v);
        pp->ains.v[0] = t;
    }
}

static int host_defined_cb(void *user, const char *name) {
    Pp *pp = user;
    return strmap_get(&pp->macros, name) != NULL || is_builtin(name);
}

static void pp_free(Pp *pp) {
    size_t i;
    for (i = 0; i < pp->macros.cap; i++) {
        Macro *m = pp->macros.vals[i];
        if (!m) continue;
        free(pp->macros.keys[i]);
        free(m->name);
        for (size_t j = 0; j < (size_t)m->nparams; j++) free(m->params[j]);
        free(m->params);
        free(m->body);
        free(m);
    }
    free(pp->macros.keys);
    free(pp->macros.vals);
    for (i = 0; i < pp->lib_tried.cap; i++) free(pp->lib_tried.keys[i]);
    free(pp->lib_tried.keys);
    free(pp->lib_tried.vals);
    for (i = 0; i < pp->msyn.cap; i++) {
        free(pp->msyn.keys[i]);
        free(pp->msyn.vals[i]);
    }
    free(pp->msyn.keys);
    free(pp->msyn.vals);
    for (i = 0; i < pp->ains.n; i++) free(pp->ains.v[i]);
    free(pp->ains.v);
    for (i = 0; i < pp->macro_paths.n; i++) free(pp->macro_paths.v[i]);
    free(pp->macro_paths.v);
    for (i = 0; i < pp->env_macro_dirs.n; i++) free(pp->env_macro_dirs.v[i]);
    free(pp->env_macro_dirs.v);
    /* the file names from #line stay with the output SrcLines */
    free(pp->line_files.v);
    free(pp->conds.v);
    free(pp->sect);
    free(pp->sectype);
    free(pp->loc);
    free((char *)pp->exp_macro);
    mac_free(pp->mac);
    free(pp);
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
    read_macro_env(pp);
    {
        MacHost h;
        h.user = pp;
        h.emit = host_emit_cb;
        h.sect = host_sect_cb;
        h.is_defined = host_defined_cb;
        h.loc = host_loc_cb;
        h.parm = host_parm_cb;
        h.now = host_now_cb;
        h.mode = host_mode_cb;
        h.aread = host_aread_cb;
        h.insert = host_insert_cb;
        pp->mac = mac_new(rep, &h, opt->macro_steps, opt->elf);
    }
    {
        /* the target: __ELF__ or __GOFF__, __ASCII__ or __EBCDIC__ (before the -D options,
         * which may redefine them) */
        const char *names[2];
        names[0] = opt->elf ? "__ELF__ 1" : "__GOFF__ 1";
        names[1] = za_ascii ? "__ASCII__ 1" : "__EBCDIC__ 1";
        for (i = 0; i < 2; i++) do_define(pp, names[i], span_make("<command line>", 0, 0, 0));
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
        if (def != d) free(def);
        free(d);
    }
    pp_file(pp, file, text, len);
    pp_free(pp);
}
