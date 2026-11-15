#include "diag.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char *name;
    char *text;
    size_t len;
    size_t *starts; /* offset of each line */
    size_t nlines;
} Source;

typedef struct {
    char *text;  /* expanded text of the line */
    char *macro; /* first macro that changed it */
    Span def;    /* where that macro is defined */
} Expansion;

struct Reporter {
    Vec(Diag *) diags;
    Vec(Source *) sources;
    StrMap expansions; /* "file:line" -> Expansion */
    int max_errors, warn_error, nerr, nwarn;
};

Span span_make(const char *file, int line, int col, int len) {
    Span s;
    s.pos.file = file;
    s.pos.line = line;
    s.pos.col = col;
    s.len = len;
    return s;
}

Reporter *rep_new(int max_errors) {
    Reporter *r = xcalloc(sizeof *r);
    r->max_errors = max_errors;
    return r;
}

void rep_set_warn_error(Reporter *r, int on) { r->warn_error = on; }

void rep_add_source(Reporter *r, const char *name, const char *text, size_t len) {
    Source *s = NULL;
    size_t i, n = 1;
    for (i = 0; i < r->sources.n; i++)
        if (!strcmp(r->sources.v[i]->name, name)) s = r->sources.v[i];
    if (!s) {
        s = xcalloc(sizeof *s);
        s->name = xstrdup(name);
        vec_push(&r->sources, s);
    } else {
        free(s->text);
        free(s->starts);
    }
    s->text = xstrndup(text, len);
    s->len = len;
    for (i = 0; i < len; i++)
        if (text[i] == '\n') n++;
    s->starts = xmalloc(n * sizeof(size_t));
    s->nlines = n;
    s->starts[0] = 0;
    n = 1;
    for (i = 0; i < len; i++)
        if (text[i] == '\n') s->starts[n++] = i + 1;
}

int rep_get_source(const Reporter *r, size_t i, const char **name, const char **text, size_t *len) {
    if (i >= r->sources.n) return 0;
    *name = r->sources.v[i]->name;
    *text = r->sources.v[i]->text;
    *len = r->sources.v[i]->len;
    return 1;
}

void rep_add_expansion(Reporter *r, const char *file, int line, const char *expanded,
                       const char *macro, Span def) {
    char *key = xasprintf("%s:%d", file ? file : "", line);
    Expansion *e;
    if (strmap_get(&r->expansions, key)) { /* the first output line of a source line wins */
        free(key);
        return;
    }
    e = xcalloc(sizeof *e);
    e->text = xstrdup(expanded);
    e->macro = xstrdup(macro);
    e->def = def;
    strmap_put(&r->expansions, key, e);
    free(key);
}

static const Expansion *find_expansion(const Reporter *r, Pos p) {
    char *key;
    const Expansion *e;
    if (!p.file || p.line < 1) return NULL;
    key = xasprintf("%s:%d", p.file, p.line);
    e = strmap_get(&r->expansions, key);
    free(key);
    return e;
}

static Source *find_source(const Reporter *r, const char *name) {
    size_t i;
    if (!name) return NULL;
    for (i = 0; i < r->sources.n; i++)
        if (!strcmp(r->sources.v[i]->name, name)) return r->sources.v[i];
    return NULL;
}

static Diag *add(Reporter *r, Severity sev, const char *code, Span sp, const char *fmt, va_list ap) {
    Diag *d = xcalloc(sizeof *d);
    d->sev = sev;
    if (code) {
        strncpy(d->code, code, sizeof d->code - 1);
    }
    d->span = sp;
    d->msg = xvasprintf(fmt, ap);
    if (sev >= SEV_ERROR)
        r->nerr++;
    else if (sev == SEV_WARNING) {
        r->nwarn++;
        if (r->warn_error) r->nerr++;
    }
    vec_push(&r->diags, d);
    return d;
}

Diag *rep_errorf(Reporter *r, const char *code, Span sp, const char *fmt, ...) {
    va_list ap;
    Diag *d;
    va_start(ap, fmt);
    d = add(r, SEV_ERROR, code, sp, fmt, ap);
    va_end(ap);
    return d;
}

Diag *rep_warnf(Reporter *r, const char *code, Span sp, const char *fmt, ...) {
    va_list ap;
    Diag *d;
    va_start(ap, fmt);
    d = add(r, SEV_WARNING, code, sp, fmt, ap);
    va_end(ap);
    return d;
}

Diag *diag_label(Diag *d, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    d->label = xvasprintf(fmt, ap);
    va_end(ap);
    return d;
}

Diag *diag_note(Diag *d, Span sp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    d->notes = xrealloc(d->notes, (size_t)(d->nnotes + 1) * sizeof(Label));
    d->notes[d->nnotes].span = sp;
    d->notes[d->nnotes].msg = xvasprintf(fmt, ap);
    d->nnotes++;
    va_end(ap);
    return d;
}

Diag *diag_help(Diag *d, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    d->help = xrealloc(d->help, (size_t)(d->nhelp + 1) * sizeof(char *));
    d->help[d->nhelp++] = xvasprintf(fmt, ap);
    va_end(ap);
    return d;
}

int rep_error_count(const Reporter *r) { return r->nerr; }
int rep_warning_count(const Reporter *r) { return r->nwarn; }
int rep_too_many(const Reporter *r) { return r->max_errors > 0 && r->nerr >= r->max_errors; }
size_t rep_count(const Reporter *r) { return r->diags.n; }
const Diag *rep_get(const Reporter *r, size_t i) { return r->diags.v[i]; }

static int cmp_span(const Diag *x, const Diag *y) {
    int c = strcmp(x->span.pos.file ? x->span.pos.file : "", y->span.pos.file ? y->span.pos.file : "");
    if (c) return c;
    if (x->span.pos.line != y->span.pos.line) return x->span.pos.line < y->span.pos.line ? -1 : 1;
    if (x->span.pos.col != y->span.pos.col) return x->span.pos.col < y->span.pos.col ? -1 : 1;
    return 0;
}

const Diag **rep_sorted(const Reporter *r, size_t *n) {
    size_t i, j;
    const Diag **out = xmalloc((r->diags.n + 1) * sizeof *out);
    for (i = 0; i < r->diags.n; i++) out[i] = r->diags.v[i];
    /* insertion sort is stable */
    for (i = 1; i < r->diags.n; i++) {
        const Diag *x = out[i];
        for (j = i; j > 0 && cmp_span(out[j - 1], x) > 0; j--) out[j] = out[j - 1];
        out[j] = x;
    }
    *n = r->diags.n;
    return out;
}

static const char *sev_name(Severity s) {
    switch (s) {
    case SEV_NOTE: return "note";
    case SEV_WARNING: return "warning";
    case SEV_ERROR: return "error";
    default: return "fatal error";
    }
}

static const char *sev_color(Severity s) {
    switch (s) {
    case SEV_NOTE: return "1;36";
    case SEV_WARNING: return "1;33";
    default: return "1;31";
    }
}

static void paint_on(FILE *w, int on, const char *code) {
    if (on) fprintf(w, "\x1b[%sm", code);
}

static void paint_off(FILE *w, int on) {
    if (on) fputs("\x1b[0m", w);
}

static void print_pos(FILE *w, Pos p) {
    if (p.line == 0)
        fputs(p.file ? p.file : "", w);
    else
        fprintf(w, "%s:%d:%d", p.file ? p.file : "", p.line, p.col);
}

/* orig: show the source text even when the line came from a macro expansion */
static void excerpt(const Reporter *r, FILE *w, Span sp, const char *label, Severity sev, int color,
                    int orig) {
    Source *s = find_source(r, sp.pos.file);
    const Expansion *ex = orig ? NULL : find_expansion(r, sp.pos);
    const char *text;
    size_t tlen, col, n, i;
    char num[16];
    int npad, cnt;
    if (ex) {
        text = ex->text;
        tlen = strlen(text);
    } else {
        if (!s || sp.pos.line < 1 || (size_t)sp.pos.line > s->nlines) return;
        text = s->text + s->starts[sp.pos.line - 1];
        tlen = (sp.pos.line == (int)s->nlines ? s->len : s->starts[sp.pos.line]) -
               s->starts[sp.pos.line - 1];
        while (tlen > 0 && (text[tlen - 1] == '\n' || text[tlen - 1] == '\r')) tlen--;
    }
    snprintf(num, sizeof num, "%d", sp.pos.line);
    npad = (int)strlen(num);
    fprintf(w, "%*s |\n", npad, "");
    fprintf(w, "%s | %.*s\n", num, (int)tlen, text);
    col = sp.pos.col > 0 ? (size_t)sp.pos.col - 1 : 0;
    if (col > tlen) col = tlen;
    n = sp.len < 1 ? 1 : (size_t)sp.len;
    if (col + n > tlen && tlen > col) n = tlen - col;
    cnt = (col + n <= tlen) ? (int)utf8_count(text + col, n) : (int)n;
    if (cnt < 1) cnt = 1;
    fprintf(w, "%*s | ", npad, "");
    for (i = 0; i < col; i++) fputc(text[i] == '\t' ? '\t' : ' ', w);
    paint_on(w, color, sev_color(sev));
    for (i = 0; i < (size_t)cnt; i++) fputc('^', w);
    if (label && *label) fprintf(w, " %s", label);
    paint_off(w, color);
    fputc('\n', w);
}

static void print_one(const Reporter *r, FILE *w, const Diag *d, int color) {
    int i;
    const Expansion *ex;
    paint_on(w, color, sev_color(d->sev));
    fputs(sev_name(d->sev), w);
    if (d->code[0]) fprintf(w, "[%s]", d->code);
    paint_off(w, color);
    fprintf(w, ": %s\n", d->msg);
    if (d->span.pos.file && *d->span.pos.file) {
        fputs("  --> ", w);
        print_pos(w, d->span.pos);
        fputc('\n', w);
    }
    excerpt(r, w, d->span, d->label, d->sev, color, 0);
    ex = find_expansion(r, d->span.pos);
    if (ex) {
        Span whole = d->span;
        whole.pos.col = 1;
        whole.len = 1 << 20;
        fprintf(w, "  note: in expansion of macro \"%s\"\n  --> ", ex->macro);
        print_pos(w, d->span.pos);
        fputc('\n', w);
        excerpt(r, w, whole, NULL, SEV_NOTE, color, 1);
        if (ex->def.pos.file) {
            fprintf(w, "  note: macro \"%s\" is defined here\n  --> ", ex->macro);
            print_pos(w, ex->def.pos);
            fputc('\n', w);
            excerpt(r, w, ex->def, NULL, SEV_NOTE, color, 1);
        }
    }
    for (i = 0; i < d->nnotes; i++) {
        fprintf(w, "  note: %s\n  --> ", d->notes[i].msg);
        print_pos(w, d->notes[i].span.pos);
        fputc('\n', w);
        excerpt(r, w, d->notes[i].span, NULL, SEV_NOTE, color, 0);
    }
    for (i = 0; i < d->nhelp; i++) fprintf(w, "  help: %s\n", d->help[i]);
}

char *rep_summary(const Reporter *r) {
    return xasprintf("%d error%s, %d warning%s", r->nerr, r->nerr == 1 ? "" : "s", r->nwarn,
                     r->nwarn == 1 ? "" : "s");
}

void rep_print(Reporter *r, FILE *w, int color) {
    size_t n, i;
    const Diag **ds = rep_sorted(r, &n);
    for (i = 0; i < n; i++) print_one(r, w, ds[i], color);
    free(ds);
    if (r->nerr > 0 || r->nwarn > 0) {
        char *s = rep_summary(r);
        fprintf(w, "%s\n", s);
        free(s);
    }
}

const char *diag_closest(const char *word, const char *const *cands, size_t n) {
    char *w = za_strlower(word);
    const char *best = NULL;
    int best_d = 1 << 30, limit = 1;
    size_t i, wl = strlen(w);
    for (i = 0; i < n; i++) {
        char *c = za_strlower(cands[i]);
        int d = edit_distance(w, c);
        if (d < best_d ||
            (d == best_d && best &&
             (strlen(cands[i]) < strlen(best) ||
              (strlen(cands[i]) == strlen(best) && strcmp(cands[i], best) < 0)))) {
            best = cands[i];
            best_d = d;
        }
        free(c);
    }
    if (wl >= 8)
        limit = 3;
    else if (wl >= 5)
        limit = 2;
    free(w);
    if (!best || best_d > limit || (size_t)best_d >= wl) return NULL;
    return best;
}
