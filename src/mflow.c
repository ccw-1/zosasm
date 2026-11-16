/* mflow.c - writes the nodes of a converted macro body (mconv_int.h): flat form
 * (#label / #goto) or structured (if/while/switch, see MACRO.md section 10). */
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "mconv_int.h"

static void put_lines(Buf *out, int depth, const char *text, const char *remark) {
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        int d;
        for (d = 0; d < depth; d++) buf_puts(out, "    ");
        buf_write(out, p, n);
        if (!e && remark) buf_printf(out, "  // %s", remark);
        buf_putc(out, '\n');
        if (!e) break;
        p = e + 1;
    }
}

static void put_line(Buf *out, int depth, const char *remark, const char *fmt, ...) {
    va_list ap;
    char *t;
    va_start(ap, fmt);
    t = xvasprintf(fmt, ap);
    va_end(ap);
    put_lines(out, depth, t, remark);
}

static void write_flat(Buf *out, const MNodeVec *nodes) {
    size_t k, n = nodes->n;
    if (n && nodes->v[n - 1].k == N_RETURN && !nodes->v[n - 1].remark) n--;
    for (k = 0; k < n; k++) {
        const MNode *x = &nodes->v[k];
        switch (x->k) {
        case N_CODE: put_lines(out, 1, x->text, x->remark); break;
        case N_COND:
            put_line(out, 1, NULL, "#if (%s) {", x->cond);
            put_line(out, 2, x->remark, "#goto %s", x->target);
            put_line(out, 1, NULL, "#}");
            break;
        case N_GOTO: put_line(out, 1, x->remark, "#goto %s", x->target); break;
        case N_RETURN: put_line(out, 1, x->remark, "#return"); break;
        case N_LABEL: put_line(out, 1, x->remark, "#label %s", x->target); break;
        }
    }
}

/* ---- structure recovery -------------------------------------------------------------
 * The nodes are cut at labels and jumps. A jump target is the "point" of its label: the
 * first node that is not a label. Ranges of nodes are written recursively:
 *   forward COND            -> #if (not cond) { ... }  (with else when the then part ends in a
 *                              GOTO over the else part)
 *   label with back jumps   -> #while (not cond) / #while (true), jumps to the head become
 *                              #continue, jumps to the end #break
 *   jump to the end of the macro, or to a short exit block -> #return (the block is copied)
 * Anything else makes the function fail and the caller writes the flat form. */

typedef struct {
    int head;  /* point of the loop head or -1 */
    int exit;  /* point after the loop or -1 */
    int le;    /* node index after the loop body or -1 */
} Ctx;

typedef struct {
    const MNode *v;
    size_t n;
    int *tl;   /* jump node -> index of its label node, -1 when not a jump */
    int *tp;   /* jump node -> point of the target */
    int *cls;  /* point -> 0 normal, 1 return, 2 short block ending in MEXIT, 3 short block at the end */
    int *need; /* label node -> a #goto targets it */
    int *landed; /* point -> a structured construct ends there (a short block at the end is live only then) */
    int grown;   /* need or landed changed in this pass */
    int *lblk; /* label node -> block it was written in, -1 when not written */
    Vec(int) bpar; /* block -> parent block */
    int cur;       /* current block */
    Vec(int) gl;   /* #goto written: label node and block, pairs */
    Buf out;
} S;

#define EXIT_MAX 6

static int is_jump(const MNode *x) { return x->k == N_COND || x->k == N_GOTO; }

/* a code node that is only macro comment lines (//); it produces no output and does not count as a statement */
static int is_comment(const MNode *x) {
    const char *p;
    if (x->k != N_CODE) return 0;
    for (p = x->text; *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : p + strlen(p))
        if (!(p[0] == '/' && p[1] == '/') && *p != '\n') return 0;
    return 1;
}

static int is_pass(const MNode *x) { return x->k == N_LABEL || is_comment(x); }

static int point_of(const S *s, int i) {
    while ((size_t)i < s->n && is_pass(&s->v[i])) i++;
    return i;
}

static int gstart(const S *s, int p) {
    while (p > 0 && is_pass(&s->v[p - 1])) p--;
    return p;
}

static int classify(const S *s, int p) {
    int k = 0, code = 0;
    if ((size_t)p >= s->n || s->v[p].k == N_RETURN) return 1;
    while ((size_t)p + k < s->n && (s->v[p + k].k == N_CODE || s->v[p + k].k == N_LABEL) && code <= EXIT_MAX) {
        if (s->v[p + k].k == N_CODE && !is_comment(&s->v[p + k])) code++;
        k++;
    }
    if (code == 0 || code > EXIT_MAX) return 0;
    if ((size_t)p + k >= s->n) return 3;
    return s->v[p + k].k == N_RETURN ? 2 : 0;
}

/* jumps from outside [a,b) to a label inside it, except the jump `allow` */
static int closed(const S *s, int a, int b, int allow) {
    size_t j;
    for (j = 0; j < s->n; j++) {
        if (s->tl[j] < 0 || (int)j == allow) continue;
        if ((int)j >= a && (int)j < b) continue;
        if (s->tl[j] >= a && s->tl[j] < b) return 0;
    }
    return 1;
}

static void put_goto(S *s, int depth, int j) {
    if (!s->need[s->tl[j]]) s->grown = 1;
    s->need[s->tl[j]] = 1;
    vec_push(&s->gl, s->tl[j]);
    vec_push(&s->gl, s->cur);
    put_line(&s->out, depth, s->v[j].remark, "#goto %s", s->v[j].target);
}

/* a jump that leaves the current construct; 0 when it is none of the known kinds */
static int put_exit(S *s, int depth, int j, const Ctx *c) {
    int t = s->tp[j];
    const MNode *x = &s->v[j];
    int k;
    if (t == c->head) {
        put_line(&s->out, depth, x->remark, "#continue");
    } else if (t == c->exit) {
        put_line(&s->out, depth, x->remark, "#break");
    } else if (s->cls[t] == 1) {
        put_line(&s->out, depth, x->remark, "#return");
    } else if (s->cls[t] >= 2) {
        for (k = t; (size_t)k < s->n && (s->v[k].k == N_CODE || s->v[k].k == N_LABEL); k++)
            if (s->v[k].k == N_CODE) put_lines(&s->out, depth, s->v[k].text, s->v[k].remark);
        put_line(&s->out, depth, x->remark, "#return");
    } else {
        put_goto(s, depth, j);
    }
    return 1;
}

static void land(S *s, int p) {
    if (!s->landed[p]) s->grown = 1;
    s->landed[p] = 1;
}

/* the jump leaves the construct by break, continue, return or a copy of a short block */
static int is_leave(const S *s, int j, const Ctx *c) {
    int t = s->tp[j];
    return t == c->head || t == c->exit || s->cls[t] != 0;
}

static int is_exit(const S *s, int j, const Ctx *c) {
    int t = s->tp[j];
    return t == c->head || t == c->exit || s->cls[t] == 1 || s->cls[t] == 2;
}

static int live_label(const S *s, int i) {
    size_t j;
    int p = point_of(s, i);
    for (j = 0; j < s->n; j++)
        if (s->tl[j] >= 0 && s->tp[j] == p && s->cls[p] == 0) return 1;
    return s->cls[p] == 3 && s->landed[p];
}

static void emit_range(S *s, int lo, int hi, int depth, Ctx c);

/* writes the labels of the group at i that a #goto needs; returns the point */
static int put_labels(S *s, int i, int depth) {
    int p = point_of(s, i), k;
    for (k = i; k < p; k++) {
        if (is_comment(&s->v[k])) put_lines(&s->out, depth, s->v[k].text, s->v[k].remark);
        if (s->v[k].k == N_LABEL && s->need[k]) {
            s->lblk[k] = s->cur;
            put_line(&s->out, depth, NULL, "#label %s", s->v[k].target);
        }
    }
    return p;
}

/* a loop whose head is the label group at i; returns the index after it or -1 */
static int emit_loop(S *s, int i, int hi, int depth) {
    int p = point_of(s, i), le = 0, j, ok = 0;
    Ctx in;
    for (j = p; (size_t)j < s->n; j++)
        if (s->tl[j] >= 0 && s->tp[j] == p) {
            le = j + 1;
            ok = 1;
        }
    if (!ok || le > hi || !closed(s, p, le, -1)) return -1;
    in.head = p;
    in.exit = point_of(s, le);
    in.le = le;
    put_labels(s, i, depth);
    if (s->v[p].k == N_COND && s->tp[p] == in.exit && p + 1 < le) {
        put_line(&s->out, depth, NULL, "#while (%s) {", s->v[p].ncond);
        emit_range(s, p + 1, le, depth + 1, in);
    } else {
        put_line(&s->out, depth, NULL, "#while (true) {");
        emit_range(s, p, le, depth + 1, in);
    }
    put_line(&s->out, depth, NULL, "#}");
    return le;
}

static int dead_end(const S *s, int i, int hi) {
    while (i < hi && !(s->v[i].k == N_LABEL && live_label(s, i))) i++;
    return i;
}

static int skip_dead(S *s, int i, int hi, int depth) {
    while (i < hi && !(s->v[i].k == N_LABEL && live_label(s, i))) {
        if (is_comment(&s->v[i])) put_lines(&s->out, depth, s->v[i].text, s->v[i].remark);
        i++;
    }
    return i;
}

static void emit_body(S *s, int lo, int hi, int depth, Ctx c) {
    int i = lo;
    while (i < hi) {
        const MNode *x = &s->v[i];
        switch (x->k) {
        case N_CODE: put_lines(&s->out, depth, x->text, x->remark); i++; break;
        case N_RETURN:
            put_line(&s->out, depth, x->remark, "#return");
            i = skip_dead(s, i + 1, hi, depth);
            break;
        case N_LABEL: {
            int next = emit_loop(s, i, hi, depth);
            i = next >= 0 ? next : put_labels(s, i, depth);
            break;
        }
        case N_GOTO: {
            int t = s->tp[i];
            int r = dead_end(s, i + 1, hi);
            if (t == point_of(s, i + 1)) {
                i++;
                break;
            }
            if (t == c.head && c.le == hi && r >= hi) {
                i = skip_dead(s, i + 1, hi, depth);
                break;
            }
            if (r < hi && point_of(s, r) == t) {
                land(s, t);
                i = skip_dead(s, i + 1, hi, depth);
                break;
            }
            put_exit(s, depth, i, &c);
            i = skip_dead(s, i + 1, hi, depth);
            break;
        }
        case N_COND: {
            int t = s->tp[i], q, u = -1;
            if (t == point_of(s, i + 1)) {
                i++;
                break;
            }
            if (t == c.head && c.le == hi && i == hi - 1) {
                put_line(&s->out, depth, NULL, "#if (%s) {", x->ncond);
                put_line(&s->out, depth + 1, x->remark, "#break");
                put_line(&s->out, depth, NULL, "#}");
                i++;
                break;
            }
            q = gstart(s, t);
            if (!is_exit(s, i, &c) && t > i && q <= hi && i + 1 < hi &&
                (s->v[i + 1].k == N_RETURN || (s->v[i + 1].k == N_GOTO && is_leave(s, i + 1, &c))) &&
                dead_end(s, i + 2, hi) < hi && point_of(s, dead_end(s, i + 2, hi)) == t) {
                /* AIF (c).A / MEXIT (or AGO to an exit) / .A : a guard clause */
                land(s, t);
                put_line(&s->out, depth, NULL, "#if (%s) {", x->ncond);
                if (s->v[i + 1].k == N_RETURN)
                    put_line(&s->out, depth + 1, s->v[i + 1].remark, "#return");
                else
                    put_exit(s, depth + 1, i + 1, &c);
                put_line(&s->out, depth, NULL, "#}");
                i = skip_dead(s, i + 2, hi, depth);
                break;
            }
            if (!is_exit(s, i, &c) && t > i && q == i + 2 && q <= hi && s->v[i + 1].k == N_GOTO &&
                !is_exit(s, i + 1, &c) && s->tp[i + 1] > t) {
                /* AIF (c).A / AGO .B / .A ... / .B : the branch to A comes first */
                int ub = gstart(s, s->tp[i + 1]), e = ub - 1;
                while (e > q && is_pass(&s->v[e])) e--;
                if (ub <= hi && closed(s, q, ub, i)) {
                    land(s, t);
                    land(s, s->tp[i + 1]);
                    if (s->v[e].k == N_RETURN || (s->v[e].k == N_GOTO && is_exit(s, e, &c))) {
                        put_line(&s->out, depth, NULL, "#if (%s) {", x->cond);
                        emit_range(s, q, ub, depth + 1, c);
                        put_line(&s->out, depth, NULL, "#}");
                        i = ub;
                        break;
                    }
                    if (s->v[e].k == N_GOTO && s->tp[e] > s->tp[i + 1] && gstart(s, s->tp[e]) <= hi) {
                        int j2 = gstart(s, s->tp[e]);
                        if (closed(s, ub, j2, i + 1)) {
                            land(s, s->tp[e]);
                            put_line(&s->out, depth, NULL, "#if (%s) {", x->cond);
                            emit_range(s, q, e, depth + 1, c);
                            put_line(&s->out, depth, NULL, "#} else {");
                            emit_range(s, ub, j2, depth + 1, c);
                            put_line(&s->out, depth, NULL, "#}");
                            i = j2;
                            break;
                        }
                    }
                }
            }
            if (is_exit(s, i, &c) || t <= i || q > hi || !closed(s, i + 1, q, -1)) {
                put_line(&s->out, depth, NULL, "#if (%s) {", x->cond);
                put_exit(s, depth + 1, i, &c);
                put_line(&s->out, depth, NULL, "#}");
                i++;
                break;
            }
            if (s->v[q - 1].k == N_GOTO && q - 1 > i && !is_exit(s, q - 1, &c)) {
                int ut = s->tp[q - 1];
                if (ut > t) u = gstart(s, ut);
                if (u > q && u <= hi && closed(s, i + 1, q - 1, -1) && closed(s, q, u, i)) {
                    land(s, t);
                    land(s, ut);
                    put_line(&s->out, depth, NULL, "#if (%s) {", x->ncond);
                    emit_range(s, i + 1, q - 1, depth + 1, c);
                    put_line(&s->out, depth, NULL, "#} else {");
                    emit_range(s, q, u, depth + 1, c);
                    put_line(&s->out, depth, NULL, "#}");
                    i = u;
                    break;
                }
            }
            land(s, t);
            put_line(&s->out, depth, NULL, "#if (%s) {", x->ncond);
            emit_range(s, i + 1, q, depth + 1, c);
            put_line(&s->out, depth, NULL, "#}");
            i = q;
            break;
        }
        }
    }
}

static void emit_range(S *s, int lo, int hi, int depth, Ctx c) {
    int saved = s->cur;
    s->cur = (int)s->bpar.n;
    vec_push(&s->bpar, saved);
    emit_body(s, lo, hi, depth, c);
    s->cur = saved;
}

/* every #goto must leave blocks only, never enter one */
static int gotos_valid(const S *s) {
    size_t k;
    for (k = 0; k < s->gl.n; k += 2) {
        int lb = s->lblk[s->gl.v[k]], b = s->gl.v[k + 1];
        if (lb < 0) return 0;
        while (b >= 0 && b != lb) b = s->bpar.v[b];
        if (b < 0) return 0;
    }
    return 1;
}

static int structure(Buf *out, const MNodeVec *nodes) {
    S s;
    size_t j, k, n = nodes->n;
    int pass, hi;
    Ctx c;
    memset(&s, 0, sizeof s);
    s.v = nodes->v;
    s.n = n;
    s.tl = xcalloc((n + 1) * sizeof(int));
    s.tp = xcalloc((n + 1) * sizeof(int));
    s.cls = xcalloc((n + 2) * sizeof(int));
    s.need = xcalloc((n + 1) * sizeof(int));
    s.landed = xcalloc((n + 2) * sizeof(int));
    s.lblk = xcalloc((n + 1) * sizeof(int));
    for (j = 0; j < n; j++) {
        s.tl[j] = -1;
        if (!is_jump(&s.v[j])) continue;
        for (k = 0; k < n; k++)
            if (s.v[k].k == N_LABEL && !strcmp(s.v[k].target, s.v[j].target)) {
                s.tl[j] = (int)k;
                break;
            }
        if (s.tl[j] < 0) return 0;
        s.tp[j] = point_of(&s, s.tl[j]);
    }
    for (j = 0; j <= n; j++) s.cls[j] = classify(&s, (int)j);
    hi = (int)n;
    if (n && s.v[n - 1].k == N_RETURN && !s.v[n - 1].remark) hi--;
    c.head = c.exit = c.le = -1;
    for (pass = 0; pass < 10; pass++) {
        s.grown = 0;
        for (j = 0; j <= n; j++) s.lblk[j] = -1;
        s.bpar.n = 0;
        s.gl.n = 0;
        s.out.len = 0;
        vec_push(&s.bpar, -1);
        s.cur = 0;
        emit_body(&s, 0, hi, 1, c);
        if (!s.grown && pass >= 1) break;
    }
    if (s.grown) return 0;
    if (!gotos_valid(&s)) return 0;
    buf_puts(out, buf_cstr(&s.out));
    return 1;
}

int mflow_write(Buf *out, const MNodeVec *nodes, int flat) {
    if (!flat && structure(out, nodes)) return 1;
    write_flat(out, nodes);
    return flat ? 0 : -1;
}
