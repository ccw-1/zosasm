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

static const char *literal_label(Asm *a, Stmt *st, Operand o, const char *key) {
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
        d->name = l->name;
        d->name_span = sp;
        d->op = "DC";
        d->op_span = sp;
        d->operands = xstrdup(key + 1);
        d->remark = "";
        d->segs = xcalloc(sizeof(Seg));
        d->segs[0].off = 0;
        d->segs[0].line = sp.pos.line;
        d->segs[0].col = ksp.pos.col + 1;
        d->nsegs = 1;
        l->st = d;
    }
    vec_push(&p->lits, l);
    strmap_put(&p->by_key, key, l);
    return l->name;
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
        ops->v[i].text = (char *)literal_label(a, st, ops->v[i], key);
        free(key);
    }
}

/* Assemble the literals of the current pool at the location counter. Pool
 * constants are ordered by alignment, largest first. */
void asm_close_pool(Asm *a) {
    LitPool *p = cur_pool(a);
    size_t i;
    if (!p) {
        a->pool_no++;
        return;
    }
    if (!p->sorted && a->pass == 1) {
        Reporter *save = a->rep;
        int64_t *align = xmalloc((p->lits.n + 1) * sizeof *align);
        p->sorted = 1;
        a->rep = rep_new(0);
        for (i = 0; i < p->lits.n; i++) {
            DcSpec sp;
            Operand o;
            align[i] = 1;
            o.text = p->lits.v[i]->st->operands;
            o.off = 0;
            if (dc_parse(a, p->lits.v[i]->st, o, &sp) && !sp.has_len) align[i] = dc_alignment(&sp);
        }
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
    for (i = 0; i < p->lits.n; i++) {
        Literal *l = p->lits.v[i];
        if (l->st->bad) continue; /* reported in pass 1 */
        asm_stmt(a, l->st);
        if (l->st->bad && !asm_lookup(a, l->name)) {
            /* avoid a cascade of "undefined symbol" errors for the users */
            int created;
            Symbol *s = asm_define(a, l->st, l->name, l->sp, &created);
            if (created) s->kind = SYM_ABS;
        }
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
