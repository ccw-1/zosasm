/* mpost.c - readability passes over the text of a structured macro body (MACRO.md section 10):
 *   1. an else block that holds only an if becomes "#} else if (...) {"
 *   2. a chain "if (x == A) ... else if (x == B) ..." of at least three arms becomes #switch.
 * The body is the text written by mflow.c: structure lines start (after the indentation of four
 * spaces per level) with '#', and a model line never does. */
#include <stdlib.h>
#include <string.h>

#include "mconv_int.h"

typedef Vec(char *) Lines;

static int indent_of(const char *l) {
    int n = 0;
    while (l[n] == ' ') n++;
    return n;
}

static int starts(const char *l, int ind, const char *w) {
    return indent_of(l) == ind && !strncmp(l + ind, w, strlen(w));
}

static Lines split(const char *body) {
    Lines v = {0};
    const char *p = body;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        vec_push(&v, xstrndup(p, n));
        if (!e) break;
        p = e + 1;
    }
    return v;
}

static char *join(const Lines *v) {
    Buf b = {0};
    size_t i;
    for (i = 0; i < v->n; i++) buf_printf(&b, "%s\n", v->v[i]);
    return xstrdup(buf_cstr(&b));
}

static void del(Lines *v, size_t at) {
    memmove(v->v + at, v->v + at + 1, (v->n - at - 1) * sizeof *v->v);
    v->n--;
}

/* the first "#}" at the indentation ind from line i on ("#} else" is skipped unless cont is set);
 * v->n when there is none */
static size_t block_end(const Lines *v, size_t i, int ind, int cont) {
    size_t j;
    for (j = i; j < v->n; j++) {
        const char *l = v->v[j];
        if (indent_of(l) != ind || strncmp(l + ind, "#}", 2)) continue;
        if (!cont && !strncmp(l + ind, "#} else", 7)) continue;
        return j;
    }
    return v->n;
}

static int is_open(const char *l, int ind, const char *head) {
    size_t n = strlen(l);
    return starts(l, ind, head) && n > 2 && !strcmp(l + n - 2, " {");
}

/* all lines between a and b are deeper than ind (or empty) */
static int deeper(const Lines *v, size_t a, size_t b, int ind) {
    size_t j;
    for (j = a; j < b; j++)
        if (v->v[j][0] && indent_of(v->v[j]) < ind) return 0;
    return 1;
}

/* 1. "#} else {" followed by a lone if */
static int fold_else_if(Lines *v) {
    size_t i;
    int changed = 0;
    for (i = 0; i + 2 < v->n; i++) {
        int ind = indent_of(v->v[i]);
        size_t e, k;
        if (strcmp(v->v[i] + ind, "#} else {") || !is_open(v->v[i + 1], ind + 4, "#if (")) continue;
        e = block_end(v, i + 2, ind + 4, 0); /* the end of the inner if with its else parts */
        if (e + 1 >= v->n || strcmp(v->v[e] + ind + 4, "#}") || indent_of(v->v[e]) != ind + 4) continue;
        if (strcmp(v->v[e + 1] + ind, "#}") || indent_of(v->v[e + 1]) != ind) continue;
        if (!deeper(v, i + 2, e, ind + 4)) continue;
        v->v[i] = xasprintf("%*s#} else if %s", ind, "", v->v[i + 1] + ind + 4 + 4);
        for (k = i + 2; k < e; k++)
            if (v->v[k][0]) v->v[k] = xstrdup(v->v[k] + 4);
        del(v, e);
        del(v, i + 1);
        changed = 1;
    }
    return changed;
}

/* ---- 2. switch ------------------------------------------------------------------------------ */

/* scan s outside strings: the position of the next top level (parentheses and brackets) occurrence of
 * the text w from `from`, or -1 */
static long find_top(const char *s, size_t from, const char *w) {
    size_t i, wl = strlen(w);
    int depth = 0;
    for (i = 0; s[i]; i++) {
        if (s[i] == '"') {
            for (i++; s[i] && s[i] != '"'; i++)
                if (s[i] == '\\' && s[i + 1]) i++;
            if (!s[i]) return -1;
            continue;
        }
        if (s[i] == '(' || s[i] == '[') depth++;
        if (s[i] == ')' || s[i] == ']') depth--;
        if (depth == 0 && i >= from && !strncmp(s + i, w, wl)) return (long)i;
    }
    return -1;
}

static int is_literal(const char *s) {
    size_t n = strlen(s), i;
    if (n >= 2 && s[0] == '"') {
        for (i = 1; i + 1 < n; i++) {
            if (s[i] == '"') return 0;
            if (s[i] == '\\') i++;
        }
        return s[n - 1] == '"' && i == n - 1;
    }
    i = s[0] == '-' ? 1 : 0;
    if (i >= n) return 0;
    for (; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

static const char *const NOT_LHS[] = {" == ", " != ", " && ", " || ", " < ", " > ", " <= ", " >= ", " ? ", NULL};

/* cond is "a == 1 || a == 2": the common left side and the literals; 0 when it is not that */
static int parse_eq_or(const char *cond, char **lhs, Lines *lits) {
    const char *p = cond;
    for (;;) {
        long sep = find_top(p, 0, " || "), last = -1, q = 0;
        char *term = sep < 0 ? xstrdup(p) : xstrndup(p, (size_t)sep);
        char *l, *r;
        int k;
        while ((q = find_top(term, (size_t)(q), " == ")) >= 0) {
            last = q;
            q += 4;
        }
        if (last <= 0) return 0;
        l = xstrndup(term, (size_t)last);
        r = term + last + 4;
        if (!is_literal(r)) return 0;
        for (k = 0; NOT_LHS[k]; k++)
            if (find_top(l, 0, NOT_LHS[k]) >= 0) return 0;
        if (*lhs && strcmp(*lhs, l)) return 0;
        *lhs = l;
        vec_push(lits, xstrdup(r));
        if (sep < 0) return 1;
        p += sep + 4;
    }
}

static int has_loop_exit(const Lines *v, size_t a, size_t b) {
    size_t j;
    for (j = a; j < b; j++)
        if (strstr(v->v[j], "#break") || strstr(v->v[j], "#continue")) return 1;
    return 0;
}

typedef struct {
    size_t head, end;       /* the header line and the line after the body (the next header or the close) */
    Lines lits;             /* empty for the else arm */
    int is_else;
} Arm;

/* the chain that starts at line i; returns the line after its closing "#}" or 0 */
static size_t switch_chain(Lines *v, size_t i, Lines *out) {
    int ind = indent_of(v->v[i]);
    Vec(Arm) arms = {0};
    char *lhs = NULL;
    size_t cur = i, k, j, nlit = 0, close;
    for (;;) {
        Arm a;
        const char *h = v->v[cur] + ind;
        size_t e = block_end(v, cur + 1, ind, 1);
        char *cond;
        size_t hl = strlen(h);
        memset(&a, 0, sizeof a);
        if (e >= v->n) return 0;
        a.head = cur;
        a.end = e;
        if (!strncmp(h, "#if (", 5) || !strncmp(h, "#} else if (", 12)) {
            const char *c = h + (h[1] == 'i' ? 5 : 12);
            if (hl < 5 || strcmp(h + hl - 3, ") {") != 0) return 0;
            cond = xstrndup(c, (size_t)(h + hl - 3 - c));
            if (!parse_eq_or(cond, &lhs, &a.lits)) return 0;
            nlit += a.lits.n;
        } else if (!strcmp(h, "#} else {")) {
            a.is_else = 1;
        } else {
            return 0;
        }
        vec_push(&arms, a);
        if (!strncmp(v->v[e] + ind, "#} else if (", 12)) {
            if (a.is_else) return 0;
            cur = e;
            continue;
        }
        if (!strcmp(v->v[e] + ind, "#} else {")) {
            if (a.is_else) return 0;
            cur = e;
            continue;
        }
        if (strcmp(v->v[e] + ind, "#}")) return 0;
        close = e;
        break;
    }
    if (arms.n - (arms.v[arms.n - 1].is_else ? 1 : 0) < 3) return 0;
    if (has_loop_exit(v, i + 1, close)) return 0;
    /* a value may be named once only */
    for (k = 0; k < arms.n; k++)
        for (j = 0; j < arms.v[k].lits.n; j++) {
            size_t k2, j2;
            for (k2 = k; k2 < arms.n; k2++)
                for (j2 = (k2 == k ? j + 1 : 0); j2 < arms.v[k2].lits.n; j2++)
                    if (!strcmp(arms.v[k].lits.v[j], arms.v[k2].lits.v[j2])) return 0;
        }
    vec_push(out, xasprintf("%*s#switch (%s) {", ind, "", lhs));
    for (k = 0; k < arms.n; k++) {
        Buf b = {0};
        if (arms.v[k].is_else) {
            buf_printf(&b, "%*s#default:", ind, "");
        } else {
            buf_printf(&b, "%*s#case ", ind, "");
            for (j = 0; j < arms.v[k].lits.n; j++) buf_printf(&b, "%s%s", j ? ", " : "", arms.v[k].lits.v[j]);
            buf_putc(&b, ':');
        }
        vec_push(out, xstrdup(buf_cstr(&b)));
        for (j = arms.v[k].head + 1; j < arms.v[k].end; j++) vec_push(out, v->v[j]);
    }
    vec_push(out, xasprintf("%*s#}", ind, ""));
    (void)nlit;
    return close + 1;
}

static int fold_switch(Lines *v) {
    Lines out = {0};
    size_t i = 0;
    int changed = 0;
    while (i < v->n) {
        size_t next = 0;
        if (is_open(v->v[i], indent_of(v->v[i]), "#if (")) {
            Lines tmp = {0};
            next = switch_chain(v, i, &tmp);
            if (next) {
                size_t k;
                for (k = 0; k < tmp.n; k++) vec_push(&out, tmp.v[k]);
                i = next;
                changed = 1;
                continue;
            }
        }
        vec_push(&out, v->v[i]);
        i++;
    }
    *v = out;
    return changed;
}

char *mflow_post(const char *body) {
    Lines v = split(body);
    int n;
    for (n = 0; n < 100 && fold_else_if(&v); n++) {}
    for (n = 0; n < 20 && fold_switch(&v); n++) {}
    return join(&v);
}
