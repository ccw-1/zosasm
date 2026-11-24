/* literal.c - literals (=F'1', =CL8'TEXT', =A(LABEL)).
 *
 * Literals are collected per pool. A pool is closed by an LTORG statement
 * or, for the last pool, at the end of the source. Closing a pool
 * assembles the constants as DC statements with generated labels. The
 * operand of the instruction is replaced by that label, so the rest of the
 * assembler needs no special handling.
 *
 * Pools are numbered in order of closing, so pass 2 finds the pool of
 * pass 1 by number. */
#include <stdlib.h>
#include <string.h>

#include "asm.h"

/* The pool being collected. In pass 2 it may be NULL when the pool is not
 * known (which cannot happen unless pass 1 failed). */
static LitPool *cur_pool(Asm *a) {
    while (a->pools.n <= (size_t)a->pool_no) {
        LitPool *p;
        if (a->pass != 1) return NULL;
        p = xcalloc(sizeof *p);
        vec_push(&a->pools, p);
    }
    return a->pools.v[a->pool_no];
}

static const char *literal_label(Asm *a, Stmt *st, Operand o, const char *key, const char *shown) {
    LitPool *p = cur_pool(a);
    Literal *l;
    Span sp;
    int lo;
    if (!p) return "0";
    l = strmap_get(&p->by_key, key);
    if (l) return l->name;
    if (a->pass != 1) return "0";
    sp = asm_op_span(st, o);
    if (strlen(key) < 2) {
        diag_help(rep_errorf(a->rep, "ZA2051", sp, "a literal needs a constant after the '='"),
                  "for example =F'1' or =CL4'ABCD'");
        st->bad = 1;
    }
    if (a->sect && a->sect->kind == K_DSECT) {
        rep_errorf(a->rep, "ZA2052", sp, "a literal cannot be used in a dummy section");
        st->bad = 1;
    }
    l = xcalloc(sizeof *l);
    l->key = xstrdup(key);
    l->shown = shown ? xstrdup(shown) : NULL;
    l->name = xasprintf("$LT%d_%d", a->pool_no, (int)p->lits.n);
    l->sp = sp;
    lo = o.off;
    while (o.text[lo - o.off] && is_blank((unsigned char)o.text[lo - o.off])) lo++;
    {
        Operand k;
        Span ksp;
        Stmt *d = xcalloc(sizeof *d);
        k.text = (char *)key;
        k.off = lo;
        ksp = asm_op_span(st, k);
        d->file = st->file;
        d->line = sp.pos.line;
        d->name = xstrdup(l->name);
        d->name_span = sp;
        d->op = xstrdup("DC");
        d->op_span = sp;
        d->operands = xstrdup(key + 1);
        d->remark = xstrdup("");
        d->segs = xcalloc(sizeof(Seg));
        d->segs[0].off = 0;
        d->segs[0].line = sp.pos.line;
        d->segs[0].col = ksp.pos.col + 1;
        d->nsegs = 1;
        l->st = d;
        vec_push(&a->mod->synth, d);
    }
    vec_push(&p->lits, l);
    strmap_put(&p->by_key, key, l);
    return l->name;
}

/* IBM HLASM: a location counter reference (*) in a literal is the address of
 * the instruction that uses the literal, and such a literal is never shared.
 * The * terms are replaced by a hidden label at the instruction; NULL when
 * the literal has none. A * is a term when it follows nothing, ( , + - * / or
 * the equal sign, and is not inside quotes. */
static char *star_rewrite(const char *key, const char *name) {
    Buf b = {0};
    int inq = 0, found = 0;
    char prev = '=';
    const char *p;
    for (p = key; *p; p++) {
        if (*p == '\'') inq = !inq;
        if (!inq && *p == '*' && strchr("(,+-*/=", prev)) {
            buf_puts(&b, name);
            found = 1;
        } else {
            buf_putc(&b, *p);
        }
        if (!is_blank((unsigned char)*p)) prev = *p;
    }
    if (!found) return NULL;
    return xstrdup(buf_cstr(&b));
}

/* Replace literal operands of an instruction by the labels of the
 * constants. */
void asm_literal_operands(Asm *a, Stmt *st, OperandVec *ops) {
    size_t i;
    for (i = 0; i < ops->n; i++) {
        char *key = str_trim(ops->v[i].text);
        if (key[0] != '=') {
            free(key);
            continue;
        }
        {
            char *name = xasprintf("$LS%d", a->star_no);
            char *k2 = star_rewrite(key, name);
            if (k2) {
                a->star_no++;
                if (a->pass == 1 && a->cur && a->sect && a->sect->kind != K_DSECT) {
                    Stmt *h = xcalloc(sizeof *h);
                    asm_align(a, 2);
                    h->file = st->file;
                    h->line = st->line;
                    h->name = name;
                    h->name_span = st->op_span;
                    h->op = "EQU";
                    h->op_span = st->op_span;
                    h->operands = "";
                    h->remark = "";
                    asm_define_label(a, h, 4, 'I');
                }
                ops->v[i].text = (char *)literal_label(a, st, ops->v[i], k2, key);
            } else {
                ops->v[i].text = (char *)literal_label(a, st, ops->v[i], key, NULL);
            }
        }
        free(key);
    }
}

/* Assemble the literals of the current pool at the location counter. Pool
 * constants are ordered by the alignment of their length, largest first. */
void asm_close_pool(Asm *a) {
    LitPool *p = cur_pool(a);
    size_t i;
    if (!p) {
        a->pool_no++;
        return;
    }
    if (!p->sorted && a->pass == 1) {
        Reporter *save = a->rep, *tr = rep_new(0);
        int64_t *align = xmalloc((p->lits.n + 1) * sizeof *align);
        p->sorted = 1;
        a->rep = tr;
        for (i = 0; i < p->lits.n; i++) {
            DcSpec sp;
            Operand o;
            align[i] = 1;
            o.text = p->lits.v[i]->st->operands;
            o.off = 0;
            if (dc_parse(a, p->lits.v[i]->st, o, &sp)) {
                /* IBM HLASM (probe lt1): the pool is ordered by the length of the
                 * literal (multiple of 16, 8, 4, 2, else 1), not by its type alignment */
                DcVec vals = {0};
                if (!sp.bits && dc_values(a, p->lits.v[i]->st, &sp, 1, &vals)) {
                    int64_t total = 0;
                    size_t j;
                    for (j = 0; j < vals.n; j++) total += (int64_t)vals.v[j].n;
                    total *= sp.dup;
                    align[i] = total % 16 == 0 && total > 0 ? 16 : total % 8 == 0 && total > 0 ? 8 : total % 4 == 0 ? 4 : total % 2 == 0 ? 2 : 1;
                } else if (!sp.has_len) {
                    align[i] = dc_alignment(&sp);
                }
                dc_vec_free(&vals);
            }
        }
        rep_free(tr);
        a->rep = save;
        for (i = 0; i < p->lits.n; i++) p->lits.v[i]->st->bad = 0; /* the real run reports */
        /* stable insertion sort, largest alignment first */
        for (i = 1; i < p->lits.n; i++) {
            Literal *l = p->lits.v[i];
            int64_t al = align[i];
            size_t j = i;
            while (j > 0 && align[j - 1] < al) {
                p->lits.v[j] = p->lits.v[j - 1];
                align[j] = align[j - 1];
                j--;
            }
            p->lits.v[j] = l;
            align[j] = al;
        }
        free(align);
    }
    a->pool_no++;
    {
        int save_open = a->lead_open;
        int64_t save_pad = a->lead_pad;
        for (i = 0; i < p->lits.n; i++) {
            Literal *l = p->lits.v[i];
            int64_t loc0 = a->cur->loc;
            if (l->st->bad) continue; /* reported in pass 1 */
            a->lead_open = 1;
            a->lead_pad = 0;
            asm_stmt(a, l->st);
            if (a->pass == 2 && !l->st->bad) { /* for the listing */
                LitRec r;
                r.cls = a->cur->cls;
                r.off = a->cur->start + loc0 + a->lead_pad;
                r.len = a->cur->loc - loc0 - a->lead_pad;
                r.key = xstrdup(l->shown ? l->shown : l->key);
                r.owner = a->lit_owner == (size_t)-1 ? a->mod->zrecs.n : a->lit_owner;
                vec_push(&a->mod->lits, r);
            }
            if (l->st->bad && !asm_lookup(a, l->name)) {
                /* avoid a cascade of "undefined symbol" errors for the users */
                int created;
                Symbol *s = asm_define(a, l->st, l->name, l->sp, &created);
                if (created) s->kind = SYM_ABS;
            }
        }
        a->lead_open = save_open;
        a->lead_pad = save_pad;
    }
}

/* LTORG closes the current pool. */
void dir_ltorg(Asm *a, Stmt *st) {
    if (!asm_need_section(a, st)) {
        a->pool_no++;
        return;
    }
    if (a->sect->kind == K_DSECT) {
        if (a->pass == 1) {
            LitPool *p = cur_pool(a);
            if (p && p->lits.n > 0) {
                rep_errorf(a->rep, "ZA3043", stmt_span(st),
                           "LTORG in a dummy section cannot hold literals");
                /* st->bad stays clear: pass 2 must still close the pool */
            }
        }
        a->pool_no++;
        return;
    }
    asm_align(a, 8);
    asm_define_label(a, st, 1, 0);
    asm_close_pool(a);
}

/* Assemble literals still waiting at the end of the source into the first
 * control section. */
void asm_end_pool(Asm *a) {
    LitPool *p = cur_pool(a);
    Section *first = NULL;
    size_t i;
    if (!p || p->lits.n == 0) return;
    for (i = 0; i < a->mod->sections.n; i++) {
        if (a->mod->sections.v[i]->kind != K_DSECT) {
            first = a->mod->sections.v[i];
            break;
        }
    }
    if (!first || !first->cur) return;
    a->sect = first;
    a->cur = first->cur;
    a->pool_piece = a->cur;
    a->pool_loc = a->cur->loc;
    asm_align(a, 8);
    asm_close_pool(a);
}
