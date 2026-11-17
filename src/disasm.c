/* disasm.c - turn a GOFF object back into zosasm source text.
 *
 * The object is rebuilt as the statements that make the assembler produce
 * it again: sections (CSECT/RSECT/COM) with AMODE/RMODE, one class per ED
 * (CATTR), ENTRY/EXTRN/WXTRN/XATTR for the labels and external symbols, the
 * text as instructions (found by following the control flow from the entry
 * points) and DC statements, and the relocations as A/V constants and
 * relative-immediate operands.
 *
 * Everything the assembler derives from the source in a fixed way (the
 * order of ESD items, relocations sorted by offset, ...) is not stored. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "disasm.h"
#include "regops.h"
#include "ebcdic.h"
#include "goff.h"
#include "isa.h"
#include "zsym.h"

#define PRIV_NAME "$PRIV000000"
#define DEFAULT_CLASS "B_TEXT"
#define PRV_CLASS "B_PRV"
#define MAX_CLASS_LEN 0x4000000u /* 64 MiB: a limit of the converter */

typedef struct DSect DSect;
typedef struct DClass DClass;

typedef struct {
    const GoffESD *esd;
    char *name;
    int weak;
    int used; /* some written relocation or the END names it */
    DSect *sect;
} DEr;

typedef struct {
    const GoffESD *esd;
    char *name;
    int64_t len;
    int align; /* bytes */
} DPr;

typedef struct {
    int64_t off;
    int len;
    char xk;      /* Q, J, R or X (CXD) constant, else 0 */
    char *xname;  /* Q, J, R: the name written in the constant */
    int ri;       /* relative-immediate field, else A/V constant */
    DEr *er;      /* external target, or */
    DClass *cls;  /* class target */
    int64_t val;  /* A/V: addend (er) or offset in cls */
    int64_t base; /* offset of the label (LD) the relocation names */
    int vcon;
    int used;
    int hassub;     /* A(P-S+val): a second relocation item subtracts er2 / cls2 */
    DEr *er2;
    DClass *cls2;
} DRel;

typedef struct {
    int64_t off;
    char *name;
    const GoffESD *esd;
    int sect; /* the name of the section: written by CSECT */
    size_t seq;
    int zs; /* a label of the ZSYM information: no ESD, not an ENTRY */
} DName;

typedef struct {
    int64_t off;
    int len;
    const IsaInst *in;
    int64_t v[6];
    int rel;        /* index in the class relocations, or -1 */
    int64_t target; /* branch target in the class (no relocation); with a
                       relocation: the addend (er) or the offset in the class */
} DIns;

struct DClass {
    size_t idx;
    DSect *sect;
    const GoffESD *ed, *pr;
    char *name, *part;
    uint8_t *data;
    size_t len;
    DRel *rels;
    size_t nrels, caprels;
    int32_t *rel_at; /* per byte: relocation starting here + 1 */
    uint8_t *occ;    /* per byte: 1 inside an A/V constant, 2 inside an RI field */
    uint8_t *bad;    /* length of an instruction dropped at this offset */
    int32_t *ins_at; /* per byte: instruction starting here + 1, -1 inside one */
    uint8_t *lab;    /* per byte (len+1): 1 fixed label, 2 label of an instruction, 4 named,
                        8 target of an address constant */
    Vec(DName) names;
    Vec(DIns) ins;
    int code;
};

struct DSect {
    char *name; /* NULL: unnamed */
    const GoffESD *sd;
    const GoffESD *ld;
    int common;
    Vec(DPr *) prs; /* pseudo registers of the class B_PRV (DXD) */
    Vec(DClass *) classes;
};

typedef struct {
    char kind; /* S section, C class (ED or PR), L label, E external,
                  V class B_PRV (p = section), P pseudo register (p = DPr) */
    void *p;
    int64_t off; /* L */
    DClass *cls; /* L */
} IdInfo;

/* The replay of the ZSYM statements (section "Smart disassembly" of ZSYM.md):
 * which sections of the object still match the information. */
typedef struct {
    ZBlock *b;
    int *scope_sect;    /* per scope: index in Ctx.sects, -1 for a dummy section */
    DClass **scope_cls; /* per scope: the class, NULL for a dummy section */
    uint8_t *ok, *done; /* per section: replayed from ZSYM / generic text written */
    int degraded;       /* some section is written by the generic converter */
} Smart;

typedef struct {
    const GoffModule *m;
    FILE *warn;
    int nwarn;
    char *err;
    size_t errsz;
    int failed;
    Vec(DSect *) sects;
    Vec(DClass *) classes;
    Vec(DEr *) ers;
    IdInfo *by_id;
    StrMap taken;
    Buf out;
    DClass *end_cls;
    int64_t end_off;
    DEr *end_er;
    Smart *sm;
    int strict;      /* DISASM_STRICT: write registers as %r1 */
    StrMap zsyms;    /* strict: ZSYM symbol (as written) -> ZSym * */
    StrMap zpct;     /* strict: names made with NAME EQU %r1 */
} Ctx;

static void warnf(Ctx *x, const char *fmt, ...) {
    va_list ap;
    x->nwarn++;
    if (!x->warn) return;
    fprintf(x->warn, "zosasm: warning: ");
    va_start(ap, fmt);
    vfprintf(x->warn, fmt, ap);
    va_end(ap);
    fputc('\n', x->warn);
}

static int failf(Ctx *x, const char *fmt, ...) {
    va_list ap;
    if (!x->failed) {
        va_start(ap, fmt);
        vsnprintf(x->err, x->errsz, fmt, ap);
        va_end(ap);
    }
    x->failed = 1;
    return 0;
}

/* a symbol or class name as source text: the plain symbol, else a quoted
 * symbol ("a.b", with \xHH for unusual characters); NULL when it cannot be
 * written (a NUL character) */
static char *src_name(Ctx *x, const GoffESD *e, const char *what) {
    char *s = sym_source(e->name, e->name_len);
    if (!s) {
        char *t = ebcdic_decode(e->name, e->name_len);
        failf(x, "the %s name \"%s\" cannot be written as a source symbol", what, t);
        return NULL;
    }
    return s;
}

/* ---- loading the ESD items ----------------------------------------------- */

static DClass *class_of(Ctx *x, uint32_t id) {
    if (id == 0 || id > x->m->esds.n || x->by_id[id].kind != 'C') return NULL;
    return x->by_id[id].p;
}

static int load_esds(Ctx *x) {
    const GoffModule *m = x->m;
    size_t i;
    for (i = 0; i < m->esds.n && !x->failed; i++) {
        const GoffESD *e = m->esds.v[i];
        uint32_t id = (uint32_t)i + 1;
        switch (e->type) {
        case GOFF_SD: {
            DSect *s = xcalloc(sizeof *s);
            char *nm = ebcdic_decode(e->name, e->name_len);
            s->sd = e;
            s->common = e->attr.common;
            if (strcmp(nm, PRIV_NAME) != 0) {
                s->name = src_name(x, e, "section");
                if (!s->name) return 0;
            }
            vec_push(&x->sects, s);
            x->by_id[id].kind = 'S';
            x->by_id[id].p = s;
            break;
        }
        case GOFF_ED: {
            DClass *c;
            DSect *s;
            char *nm;
            if (e->parent == 0 || e->parent > m->esds.n || x->by_id[e->parent].kind != 'S')
                return failf(x, "ESD %u: element without a section", id);
            s = x->by_id[e->parent].p;
            nm = ebcdic_decode(e->name, e->name_len);
            if (!strcmp(nm, ZSYM_CLASS) || !strcmp(nm, "C_ADATAE9C2")) {
                x->by_id[id].kind = 'Z'; /* symbolic information, not code or data */
                break;
            }
            if (!strcmp(nm, PRV_CLASS)) {
                x->by_id[id].kind = 'V';
                x->by_id[id].p = s;
                break;
            }
            c = xcalloc(sizeof *c);
            c->idx = x->classes.n;
            c->sect = s;
            c->ed = e;
            c->name = src_name(x, e, "class");
            if (!c->name) return 0;
            vec_push(&s->classes, c);
            vec_push(&x->classes, c);
            x->by_id[id].kind = 'C';
            x->by_id[id].p = c;
            break;
        }
        case GOFF_PR: {
            DClass *c;
            if (e->parent && e->parent <= m->esds.n && x->by_id[e->parent].kind == 'V') {
                DPr *p = xcalloc(sizeof *p);
                DSect *s = x->by_id[e->parent].p;
                p->esd = e;
                p->name = src_name(x, e, "pseudo register");
                if (!p->name) return 0;
                p->len = e->length;
                p->align = 1 << (e->attr.alignment > 12 ? 12 : e->attr.alignment);
                vec_push(&s->prs, p);
                x->by_id[id].kind = 'P';
                x->by_id[id].p = p;
                break;
            }
            c = class_of(x, e->parent);
            if (!c || c->pr) return failf(x, "ESD %u: part without a class", id);
            c->pr = e;
            c->part = src_name(x, e, "part");
            if (!c->part) return 0;
            x->by_id[id].kind = 'C';
            x->by_id[id].p = c;
            break;
        }
        case GOFF_LD: {
            DClass *c = class_of(x, e->parent);
            if (!c) {
                warnf(x, "label ESD %u is not in a class and is ignored", id);
                break;
            }
            x->by_id[id].kind = 'L';
            x->by_id[id].p = c;
            x->by_id[id].cls = c;
            x->by_id[id].off = e->offset;
            break;
        }
        case GOFF_ER: {
            DEr *er = xcalloc(sizeof *er);
            const IdInfo *par = e->parent && e->parent <= m->esds.n ? &x->by_id[e->parent] : NULL;
            er->esd = e;
            er->name = src_name(x, e, "external symbol");
            if (!er->name) return 0;
            er->weak = e->attr.bind_strength == GOFF_STRENGTH_WEAK;
            if (par && par->kind == 'S')
                er->sect = par->p;
            else if (par && par->kind == 'C')
                er->sect = ((DClass *)par->p)->sect;
            else
                er->sect = x->sects.n ? x->sects.v[0] : NULL;
            vec_push(&x->ers, er);
            x->by_id[id].kind = 'E';
            x->by_id[id].p = er;
            break;
        }
        }
    }
    return !x->failed;
}

static uint64_t be_get(const uint8_t *p, int n) {
    uint64_t v = 0;
    int i;
    for (i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}

/* a stored address part: 4 and 8 byte values are signed, 3 bytes unsigned */
static int64_t stored_value(const uint8_t *p, int n) {
    uint64_t v = be_get(p, n);
    if (n == 4) return (int32_t)(uint32_t)v;
    return (int64_t)v;
}

static int load_text(Ctx *x) {
    const GoffModule *m = x->m;
    size_t i;
    for (i = 0; i < x->classes.n; i++) {
        DClass *c = x->classes.v[i];
        const GoffESD *e = c->pr ? c->pr : c->ed;
        c->len = e->length == GOFF_DEFERRED_LENGTH ? 0 : e->length;
    }
    for (i = 0; i < m->lens.n; i++) {
        DClass *c = class_of(x, m->lens.v[i].esdid);
        if (c && m->lens.v[i].length != GOFF_DEFERRED_LENGTH) c->len = m->lens.v[i].length;
    }
    for (i = 0; i < m->texts.n; i++) {
        const GoffText *t = &m->texts.v[i];
        DClass *c = class_of(x, t->esdid);
        uint64_t end;
        if (!c && t->esdid && t->esdid <= m->esds.n && x->by_id[t->esdid].kind == 'Z') continue;
        if (!c) {
            warnf(x, "text for ESD %u is ignored (not a class)", t->esdid);
            continue;
        }
        if (t->style != GOFF_STYLE_BYTE)
            warnf(x, "text record style %d of class %s is taken as bytes", t->style, c->name);
        end = (uint64_t)t->offset + t->len;
        if (end > MAX_CLASS_LEN) return failf(x, "class %s is too large", c->name);
        if (end > c->len) c->len = (size_t)end;
    }
    for (i = 0; i < x->classes.n; i++) {
        DClass *c = x->classes.v[i];
        if (c->len > MAX_CLASS_LEN) return failf(x, "class %s is too large", c->name);
        c->data = xcalloc(c->len + 1);
        c->rel_at = xcalloc((c->len + 1) * sizeof *c->rel_at);
        c->occ = xcalloc(c->len + 1);
        c->ins_at = xcalloc((c->len + 1) * sizeof *c->ins_at);
        c->bad = xcalloc(c->len + 1);
        c->lab = xcalloc(c->len + 2);
    }
    for (i = 0; i < m->texts.n; i++) {
        const GoffText *t = &m->texts.v[i];
        DClass *c = class_of(x, t->esdid);
        if (c && t->len) memcpy(c->data + t->offset, t->data, t->len);
    }
    return 1;
}

static void add_name(DClass *c, int64_t off, char *name, const GoffESD *e, int sect) {
    DName n;
    n.off = off;
    n.name = name;
    n.esd = e;
    n.sect = sect;
    n.zs = 0;
    n.seq = c->names.n;
    vec_push(&c->names, n);
    c->lab[off] |= 1 | 4;
}

static int load_labels(Ctx *x) {
    const GoffModule *m = x->m;
    size_t i;
    for (i = 0; i < m->esds.n && !x->failed; i++) {
        const GoffESD *e = m->esds.v[i];
        const IdInfo *sl = &x->by_id[i + 1];
        DClass *c;
        char *nm;
        if (e->type != GOFF_LD || sl->kind != 'L') continue;
        c = sl->cls;
        nm = src_name(x, e, "label");
        if (!nm) return 0;
        if (sl->off > (int64_t)c->len) {
            warnf(x, "label %s lies outside class %s and is ignored", nm, c->name);
            x->by_id[i + 1].kind = 0;
            continue;
        }
        if (c->sect->name && !strcmp(nm, c->sect->name) && c == c->sect->classes.v[0] && sl->off == 0 &&
            !c->sect->ld) {
            c->sect->ld = e;
            add_name(c, 0, nm, e, 1);
        } else {
            add_name(c, sl->off, nm, e, 0);
        }
    }
    return 1;
}

/* ---- relocations ----------------------------------------------------------- */

static void note_taken(Ctx *x, const char *name) { strmap_put(&x->taken, name, (void *)x); }

/* Q, CXD, J and R constants (the encodings of reloc_xd in emit.c).  Returns
 * 1 and fills d when r is one of them, else 0. */
static int xcon_reloc(Ctx *x, const GoffReloc *r, DRel *d) {
    const GoffModule *m = x->m;
    const IdInfo *t = r->rid && r->rid <= m->esds.n ? &x->by_id[r->rid] : NULL;
    const GoffESD *te = t ? m->esds.v[r->rid - 1] : NULL;
    int n = r->length;
    char k = 0;
    char *name = NULL;
    DEr *er = NULL;
    if (!t || r->reference == GOFF_REF_ADDRESS || r->reference == GOFF_REF_RELIMM) return 0;
    if ((r->reference == GOFF_REF_OFFSET && r->referent == GOFF_REFERENT_CLASS && !r->store &&
         n >= 1 && n <= 8) ||
        (r->reference == GOFF_REF_LONGDISP && r->referent == GOFF_REFERENT_CLASS && !r->store &&
         n == 2 && r->bit_length == 4 && r->bit_offset == 4)) {
        char kind = r->reference == GOFF_REF_LONGDISP ? 'q' : 'Q'; /* q = QY, 3 bytes */
        if (r->reference == GOFF_REF_LONGDISP) n = 3;
        if (t->kind == 'P') {
            k = kind;
            name = ((DPr *)t->p)->name;
        } else if (t->kind == 'C' && te->type == GOFF_PR) {
            k = kind;
            name = ((DClass *)t->p)->part;
        }
    } else if (r->reference == GOFF_REF_LENGTH && r->store && (n == 2 || n == 3 || n == 4 || n == 8)) {
        if (r->referent == GOFF_REFERENT_PART && t->kind == 'P') {
            k = 'J';
            name = ((DPr *)t->p)->name;
        } else if (r->referent == GOFF_REFERENT_PART && t->kind == 'C' && te->type == GOFF_PR) {
            k = 'J';
            name = ((DClass *)t->p)->part;
        } else if (r->referent == GOFF_REFERENT_CLASS && t->kind == 'C' && te->type == GOFF_ED &&
                   !((DClass *)t->p)->part) {
            k = 'J';
            name = ((DClass *)t->p)->name;
        } else if (r->referent == GOFF_REFERENT_CLASS && t->kind == 'V' && n == 4 &&
                   r->offset % 4 == 0) {
            k = 'X';
        }
    } else if (r->reference == GOFF_REF_RTYPE && r->referent == GOFF_REFERENT_LABEL && r->store &&
               (n == 3 || n == 4 || n == 8)) {
        if (t->kind == 'E') {
            size_t q;
            k = 'R';
            er = t->p;
            name = er->name;
            /* an ER named like a label definition of the module is what the assembler
             * makes of R(label): no EXTRN for it (IBM HLASM, probe st6) */
            for (q = 0; q < m->esds.n; q++) {
                const GoffESD *ld = m->esds.v[q];
                if (ld->type == GOFF_LD && ld->name_len == te->name_len &&
                    !memcmp(ld->name, te->name, te->name_len)) {
                    er = NULL;
                    break;
                }
            }
        } else if (t->kind == 'L') {
            k = 'R';
            name = src_name(x, te, "label");
            if (!name) return 0;
        }
    }
    if (!k) return 0;
    memset(d, 0, sizeof *d);
    d->er = er;
    d->off = r->offset;
    d->len = n;
    d->xk = k;
    d->xname = name;
    return 1;
}

/* is there a subtracting item for the same field as r? */
static int has_sub_at(const GoffModule *m, const GoffReloc *r) {
    size_t k;
    for (k = 0; k < m->relocs.n; k++) {
        const GoffReloc *o = &m->relocs.v[k];
        if (o->action == GOFF_ACTION_SUB && o->pid == r->pid && o->offset == r->offset &&
            o->length == r->length)
            return 1;
    }
    return 0;
}

static int load_relocs(Ctx *x) {
    const GoffModule *m = x->m;
    size_t i, b;
    int pass;
    for (pass = 0; pass < 2; pass++)
    for (i = 0; i < m->relocs.n; i++) {
        const GoffReloc *r = &m->relocs.v[i];
        DClass *c = class_of(x, r->pid);
        DRel d;
        int n = r->length, ri = r->reference == GOFF_REF_RELIMM;
        int xk;
        const IdInfo *t;
        if ((r->action == GOFF_ACTION_SUB) != pass) continue; /* additions first */
        if (!c) {
            if (!pass) warnf(x, "relocation in ESD %u is ignored (not a class)", r->pid);
            continue;
        }
        if (pass) { /* the subtrahend of an address constant */
            int32_t at = r->offset < c->len ? c->rel_at[r->offset] : 0;
            DRel *p = at ? &c->rels[at - 1] : NULL;
            int64_t base2 = 0;
            t = r->rid && r->rid <= m->esds.n ? &x->by_id[r->rid] : NULL;
            if (!p && !r->store && r->reference == GOFF_REF_ADDRESS && !r->bit_length &&
                !r->bit_offset && !r->cond_seq && (n == 3 || n == 4 || n == 8) &&
                (uint64_t)r->offset + (uint64_t)n <= c->len && t &&
                (t->kind == 'E' || t->kind == 'C' || t->kind == 'L')) {
                for (b = 0; b < (size_t)n; b++)
                    if (c->occ[r->offset + b]) break;
                if (b == (size_t)n) { /* A(-S+k): nothing is added */
                    memset(&d, 0, sizeof d);
                    d.off = r->offset;
                    d.len = n;
                    d.hassub = 1;
                    if (t->kind == 'E') {
                        d.er2 = t->p;
                    } else {
                        d.cls2 = t->kind == 'C' ? t->p : t->cls;
                        base2 = t->kind == 'L' ? t->off : 0;
                    }
                    d.val = stored_value(c->data + r->offset, n) - base2;
                    goto push;
                }
            }
            if (!p || p->ri || p->xk || p->len != n || p->hassub || p->vcon || r->store ||
                r->reference != GOFF_REF_ADDRESS || r->bit_length || r->bit_offset ||
                r->cond_seq || !t || (t->kind != 'E' && t->kind != 'C' && t->kind != 'L')) {
                warnf(x, "relocation of type %d/%d at %s+%u is not supported and is ignored",
                      r->reference, r->action, c->name, r->offset);
                continue;
            }
            if (t->kind == 'E') {
                p->er2 = t->p;
            } else {
                p->cls2 = t->kind == 'C' ? t->p : t->cls;
                base2 = t->kind == 'L' ? t->off : 0;
            }
            p->hassub = 1;
            p->val = stored_value(c->data + r->offset, n) + p->base - base2;
            continue;
        }
        xk = r->action == GOFF_ACTION_ADD && !r->cond_seq &&
             (r->reference == GOFF_REF_LONGDISP || (!r->bit_length && !r->bit_offset)) &&
             xcon_reloc(x, r, &d);
        if (xk) {
            n = d.len;
            if ((uint64_t)r->offset + (uint64_t)n > c->len) {
                warnf(x, "relocation at %s+%u is outside the class and is ignored", c->name, r->offset);
                continue;
            }
            for (b = 0; b < (size_t)n; b++)
                if (c->occ[r->offset + b]) break;
            if (b < (size_t)n) {
                warnf(x, "overlapping relocations at %s+%u; the later one is ignored", c->name,
                      r->offset);
                continue;
            }
            goto push;
        }
        if (r->action != GOFF_ACTION_ADD || r->bit_length || r->bit_offset || r->cond_seq ||
            (r->reference != GOFF_REF_ADDRESS && !ri)) {
            warnf(x, "relocation of type %d/%d at %s+%u is not supported and is ignored", r->reference,
                  r->action, c->name, r->offset);
            continue;
        }
        if (ri ? (n != 2 && n != 4) : (n != 3 && n != 4 && n != 8)) {
            warnf(x, "relocation of length %d at %s+%u is not supported and is ignored", n, c->name,
                  r->offset);
            continue;
        }
        if ((uint64_t)r->offset + (uint64_t)n > c->len) {
            warnf(x, "relocation at %s+%u is outside the class and is ignored", c->name, r->offset);
            continue;
        }
        for (b = 0; b < (size_t)n; b++)
            if (c->occ[r->offset + b]) break;
        if (b < (size_t)n) {
            warnf(x, "overlapping relocations at %s+%u; the later one is ignored", c->name, r->offset);
            continue;
        }
        memset(&d, 0, sizeof d);
        d.off = r->offset;
        d.len = n;
        d.ri = ri;
        d.vcon = r->store;
        t = r->rid && r->rid <= m->esds.n ? &x->by_id[r->rid] : NULL;
        if (t && t->kind == 'E') {
            d.er = t->p;
        } else if (t && t->kind == 'C') {
            d.cls = t->p;
        } else if (t && t->kind == 'L') {
            d.cls = t->cls;
            d.base = t->off;
        } else {
            warnf(x, "relocation at %s+%u refers to ESD %u, which cannot be a target; ignored", c->name,
                  r->offset, r->rid);
            continue;
        }
        if (!ri) {
            int64_t sv = stored_value(c->data + r->offset, n);
            if (d.er) {
                d.val = sv;
            } else {
                d.val = sv + d.base;
                d.vcon = 0;
                if ((d.val < 0 || d.val > (int64_t)d.cls->len) && !has_sub_at(m, r)) {
                    warnf(x, "address constant at %s+%u points outside class %s; relocation ignored",
                          c->name, r->offset, d.cls->name);
                    continue;
                }
            }
        }
    push:
        if (c->nrels == c->caprels) {
            c->caprels = c->caprels ? c->caprels * 2 : 16;
            c->rels = xrealloc(c->rels, c->caprels * sizeof *c->rels);
        }
        c->rels[c->nrels] = d;
        c->rel_at[d.off] = (int32_t)c->nrels + 1;
        for (b = 0; b < (size_t)n; b++) c->occ[d.off + b] = (uint8_t)(ri ? 2 : 1);
        c->nrels++;
    }
    return 1;
}

/* the labels the address constants need */
static void mark_adcon_targets(Ctx *x) {
    size_t ci, k;
    for (ci = 0; ci < x->classes.n; ci++) {
        DClass *c = x->classes.v[ci];
        for (k = 0; k <= c->len; k++) c->lab[k] &= (uint8_t)~8;
    }
    for (ci = 0; ci < x->classes.n; ci++) {
        DClass *c = x->classes.v[ci];
        for (k = 0; k < c->nrels; k++) {
            const DRel *r = &c->rels[k];
            if (!r->ri && r->cls && c->rel_at[r->off]) r->cls->lab[r->hassub ? 0 : r->val] |= 8;
            if (!r->ri && r->cls2 && c->rel_at[r->off]) r->cls2->lab[0] |= 8;
        }
    }
}

/* a label inside an address constant cannot be written: drop the constant's
 * relocation (the bytes stay) */
static void drop_split_adcons(Ctx *x) {
    size_t ci, k;
    int changed;
    do {
        changed = 0;
        mark_adcon_targets(x);
        for (ci = 0; ci < x->classes.n; ci++) {
            DClass *c = x->classes.v[ci];
            for (k = 0; k < c->nrels; k++) {
                DRel *r = &c->rels[k];
                int b;
                if (r->ri || !c->rel_at[r->off]) continue;
                for (b = 1; b < r->len; b++)
                    if (c->lab[r->off + b]) {
                        int j;
                        warnf(x, "a label lies inside the address constant at %s+%lld; relocation ignored",
                              c->name, (long long)r->off);
                        c->rel_at[r->off] = 0;
                        for (j = 0; j < r->len; j++) c->occ[r->off + j] = 0;
                        changed = 1;
                        break;
                    }
            }
        }
    } while (changed);
}

/* ---- instructions ------------------------------------------------------------ */

static int uncond_branch(const uint8_t *b) {
    if (b[0] == 0x07) return (b[1] >> 4) == 15 && (b[1] & 15) != 0;
    if (b[0] == 0x47) return (b[1] >> 4) == 15;
    if (b[0] == 0xA7) return (b[1] & 15) == 4 && (b[1] >> 4) == 15;
    if (b[0] == 0xC0) return (b[1] & 15) == 4 && (b[1] >> 4) == 15;
    return 0;
}

/* decode the instruction at o; 1 if it can be written back exactly */
static int try_decode(DClass *c, size_t o, DIns *di) {
    int64_t v[6] = {0};
    const IsaInst *in;
    IsaSlots sl;
    int i, len, rel = -1;
    size_t k;
    if (o % 2 || o + 2 > c->len || c->bad[o]) return 0;
    in = isa_decode(c->data + o, c->len - o, v);
    if (!in || !isa_rules_ok(in, v)) return 0;
    len = in->length;
    for (k = 0; k < (size_t)len; k++)
        if (c->ins_at[o + k] || c->occ[o + k] == 1) return 0;
    isa_slots(in, &sl);
    memset(di, 0, sizeof *di);
    di->in = in;
    di->off = (int64_t)o;
    di->len = len;
    di->rel = -1;
    di->target = -1;
    memcpy(di->v, v, sizeof di->v);
    for (i = 0; i < sl.n; i++) {
        const IsaSlot *s = &sl.s[i];
        const IsaOperand *f;
        if (s->kind != SK_REL) continue;
        f = isa_op(in, (size_t)s->field);
        if ((f->bits == 16 || f->bits == 32) && f->shift % 8 == 0) {
            size_t fo = o + f->shift / 8;
            if (c->rel_at[fo] && c->rels[c->rel_at[fo] - 1].ri &&
                c->rels[c->rel_at[fo] - 1].len == f->bits / 8)
                rel = c->rel_at[fo] - 1;
        }
        if (rel < 0) {
            /* HLASM decimal terms stop at 2147483647: *+n cannot be written for a far branch */
            if (2 * v[s->field] > 2147483647 || 2 * v[s->field] < -2147483647) return 0;
            di->target = (int64_t)o + 2 * v[s->field];
        } else {
            DRel *r = &c->rels[rel];
            di->rel = rel;
            if (r->er) {
                di->target = 2 * v[s->field]; /* the addend (HLASM stores the offset from the symbol only) */
            } else {
                int64_t t = r->base + 2 * v[s->field] + (int64_t)o;
                if (t < 0 || t > (int64_t)r->cls->len) return 0;
                di->target = t;
            }
        }
    }
    /* every relative-immediate byte of this instruction must belong to the relocation used */
    for (k = 0; k < (size_t)len; k++)
        if (c->occ[o + k] == 2) {
            const DRel *r;
            if (rel < 0) return 0;
            r = &c->rels[rel];
            if ((int64_t)(o + k) < r->off || (int64_t)(o + k) >= r->off + r->len) return 0;
        }
    return 1;
}

static void discover(Ctx *x, DClass *c) {
    Vec(size_t) work = {0};
    size_t o;
    if (!c->code) return;
    vec_push(&work, (size_t)0);
    for (o = 0; o <= c->len; o++)
        if (c->lab[o] & (1 | 8)) vec_push(&work, o);
    if (x->end_cls == c) vec_push(&work, (size_t)x->end_off);
    while (work.n) {
        size_t p = work.v[--work.n];
        for (;;) {
            DIns di;
            size_t k;
            if (p % 2 || p + 2 > c->len || c->ins_at[p]) break;
            if (c->bad[p]) { /* written as data, the code goes on behind it */
                size_t bl = c->bad[p];
                if (uncond_branch(c->data + p)) break;
                p += bl;
                continue;
            }
            if (!try_decode(c, p, &di)) break;
            vec_push(&c->ins, di);
            c->ins_at[p] = (int32_t)c->ins.n;
            for (k = 1; k < (size_t)di.len; k++) c->ins_at[p + k] = -1;
            if (di.target >= 0 && di.target < (int64_t)c->len && (di.rel < 0 || c->rels[di.rel].cls == c))
                vec_push(&work, (size_t)di.target);
            if (uncond_branch(c->data + p)) break;
            p += (size_t)di.len;
        }
    }
    free(work.v);
}

/* derived labels, and dropping instructions that have a label inside */
static int settle(Ctx *x) {
    int changed, dropped = 0;
    size_t ci, k;
    do {
        changed = 0;
        for (ci = 0; ci < x->classes.n; ci++) {
            DClass *c = x->classes.v[ci];
            for (k = 0; k <= c->len; k++) c->lab[k] &= (uint8_t)~2;
        }
        for (ci = 0; ci < x->classes.n; ci++) {
            DClass *c = x->classes.v[ci];
            for (k = 0; k < c->ins.n; k++) {
                const DIns *di = &c->ins.v[k];
                DClass *tc = NULL;
                if (c->ins_at[di->off] <= 0) continue; /* dropped */
                if (di->rel >= 0) {
                    if (!c->rels[di->rel].er) tc = c->rels[di->rel].cls;
                } else if (di->target >= 0 && di->target < (int64_t)c->len) {
                    tc = c;
                }
                if (!tc) continue;
                if (tc->occ[di->target] == 1 && !tc->rel_at[di->target]) {
                    /* the label would lie inside an address constant */
                    int j;
                    for (j = 0; j < di->len; j++) c->ins_at[di->off + j] = 0;
                    c->bad[di->off] = (uint8_t)di->len;
                    changed = dropped = 1;
                    continue;
                }
                tc->lab[di->target] |= 2;
            }
        }
        for (ci = 0; ci < x->classes.n; ci++) {
            DClass *c = x->classes.v[ci];
            for (k = 0; k < c->ins.n; k++) {
                const DIns *di = &c->ins.v[k];
                int b;
                if (c->ins_at[di->off] <= 0) continue;
                for (b = 1; b < di->len; b++)
                    if (c->lab[di->off + b]) {
                        int j;
                        for (j = 0; j < di->len; j++) c->ins_at[di->off + j] = 0;
                        c->bad[di->off] = (uint8_t)di->len;
                        changed = dropped = 1;
                        break;
                    }
            }
        }
    } while (changed);
    for (ci = 0; ci < x->classes.n; ci++) {
        DClass *c = x->classes.v[ci];
        for (k = 0; k < c->ins.n; k++) {
            const DIns *di = &c->ins.v[k];
            if (c->ins_at[di->off] > 0 && di->rel >= 0) c->rels[di->rel].used = 1;
        }
    }
    return dropped;
}

/* ---- names of labels ------------------------------------------------------------ */

static int cmp_name(const void *a, const void *b) {
    const DName *x = a, *y = b;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static char *gen_name(Ctx *x, const DClass *c, int64_t off) {
    char *n = c->idx ? xasprintf("L%zu_%05llX", c->idx, (unsigned long long)off)
                     : xasprintf("L%05llX", (unsigned long long)off);
    while (strmap_get(&x->taken, n)) {
        char *t = xasprintf("%s_", n);
        free(n);
        n = t;
    }
    return n;
}

static const DName *first_name(const DClass *c, int64_t off, size_t *idx) {
    size_t lo = 0, hi = c->names.n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c->names.v[mid].off < off)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < c->names.n && c->names.v[lo].off == off) {
        if (idx) *idx = lo;
        return &c->names.v[lo];
    }
    return NULL;
}

static char *label_ref(Ctx *x, const DClass *c, int64_t off) {
    const DName *n = first_name(c, off, NULL);
    return n ? xstrdup(n->name) : gen_name(x, c, off);
}

/* ---- output --------------------------------------------------------------------- */

static void stmt(Buf *o, const char *label, const char *op, const char *operands) {
    if (operands && *operands) {
        if (label && *label)
            buf_printf(o, "%-8s %-5s %s\n", label, op, operands);
        else
            buf_printf(o, "         %-5s %s\n", op, operands);
    } else if (label && *label) {
        buf_printf(o, "%-8s %s\n", label, op);
    } else {
        buf_printf(o, "         %s\n", op);
    }
}

static const char *amode_name(uint8_t a) {
    switch (a) {
    case GOFF_AMODE_24: return "24";
    case GOFF_AMODE_31: return "31";
    case GOFF_AMODE_ANY: return "ANY";
    case GOFF_AMODE_64: return "64";
    case GOFF_AMODE_MIN: return "MIN";
    }
    return NULL;
}

static const char *rmode_name(uint8_t r) {
    switch (r) {
    case GOFF_RMODE_24: return "24";
    case GOFF_RMODE_31: return "31";
    case GOFF_RMODE_64: return "64";
    }
    return NULL;
}

static const char *scope_name(uint8_t s) {
    switch (s) {
    case GOFF_SCOPE_SECTION: return "SECTION";
    case GOFF_SCOPE_MODULE: return "MODULE";
    case GOFF_SCOPE_LIBRARY: return "LIBRARY";
    case GOFF_SCOPE_IMPORT_EXPORT: return "EXPORT";
    }
    return NULL;
}

/* the XATTR operands that differ from the defaults the assembler assumes */
static void xattr_stmt(Ctx *x, const char *name, const GoffBehavior *b, int def_scope, int def_exec) {
    Buf ops = {0};
    if (b->scope != def_scope) {
        const char *sc = scope_name(b->scope);
        if (sc)
            buf_printf(&ops, "%sSCOPE(%s)", ops.len ? "," : "", sc);
        else
            warnf(x, "scope %d of %s cannot be written", b->scope, name);
    }
    if (b->linkage == GOFF_LINKAGE_XPLINK) buf_printf(&ops, "%sLINKAGE(XPLINK)", ops.len ? "," : "");
    {
        const char *ref = NULL;
        if (b->executable != def_exec) {
            if (b->executable == GOFF_EXEC_CODE)
                ref = "CODE";
            else if (b->executable == GOFF_EXEC_DATA)
                ref = "DATA";
        }
        if (ref || b->indirect)
            buf_printf(&ops, "%sREFERENCE(%s%s%s)", ops.len ? "," : "", ref ? ref : "",
                       ref && b->indirect ? "," : "", b->indirect ? "INDIRECT" : "");
    }
    if (ops.len) stmt(&x->out, name, "XATTR", buf_cstr(&ops));
}

static int class_exec(const DClass *c) {
    if (!strcmp(c->name, DEFAULT_CLASS)) return GOFF_EXEC_CODE;
    return c->ed->attr.executable;
}

static void reg_text(Buf *o, unsigned flags, int64_t v) {
    buf_printf(o, "%%%c%lld", regops_letter(flags), (long long)v);
}

static void addend_text(Buf *o, int64_t a) {
    if (a > 0)
        buf_printf(o, "+%lld", (long long)a);
    else if (a < 0)
        buf_printf(o, "-%lld", (long long)(0 - a));
}

static char *ins_operands(Ctx *x, const DClass *c, const DIns *di) {
    IsaSlots sl;
    Buf o = {0};
    int i;
    isa_slots(di->in, &sl);
    for (i = 0; i < sl.n; i++) {
        const IsaSlot *s = &sl.s[i];
        const IsaOperand *f = isa_op(di->in, (size_t)s->field);
        int64_t v = di->v[s->field];
        if (i) buf_putc(&o, ',');
        switch (s->kind) {
        case SK_REG:
            reg_text(&o, f->flags, v);
            break;
        case SK_IMM:
            /* a signed immediate that HLASM would warn about when written unsigned */
            if (f->bits < 32 && isa_rule(di->in, 's', i, NULL) && v >= ((int64_t)1 << (f->bits - 1)))
                v -= (int64_t)1 << f->bits;
            buf_printf(&o, "%lld", (long long)v);
            break;
        case SK_REL:
            if (di->rel >= 0) {
                const DRel *r = &c->rels[di->rel];
                if (r->er) {
                    buf_puts(&o, r->er->name);
                    addend_text(&o, di->target);
                } else {
                    char *l = label_ref(x, r->cls, di->target);
                    buf_puts(&o, l);
                    free(l);
                }
            } else if (di->target >= 0 && di->target < (int64_t)c->len) {
                char *l = label_ref(x, c, di->target);
                buf_puts(&o, l);
                free(l);
            } else {
                buf_putc(&o, '*');
                addend_text(&o, 2 * v);
            }
            break;
        case SK_ADDR:
            buf_printf(&o, "%lld", (long long)v);
            if (s->mid >= 0 || s->base >= 0) {
                buf_putc(&o, '(');
                if (s->mid >= 0) {
                    const IsaOperand *mf = isa_op(di->in, (size_t)s->mid);
                    int64_t mv = di->v[s->mid];
                    if (s->mid_kind == 'L')
                        buf_printf(&o, "%lld", (long long)mv + 1);
                    else
                        reg_text(&o, mf->flags, mv);
                    if (s->base >= 0) buf_putc(&o, ',');
                }
                if (s->base >= 0) reg_text(&o, F_GPR, di->v[s->base]);
                buf_putc(&o, ')');
            }
            break;
        }
    }
    return xstrdup(buf_cstr(&o));
}

static int safe_char(unsigned char b) {
    int ch = ZA_A(b);
    return za_isalnum(ch) || ch == ' ' || ch == '.' || ch == ',' || ch == ':' || ch == '-' || ch == '_' ||
           ch == '+' || ch == '=' || ch == '?' || ch == '!' || ch == '<' || ch == '>' || ch == '(' ||
           ch == ')';
}

/* Q, J, R constants and CXD */
static void emit_xcon(Ctx *x, const DRel *r, const char *label) {
    char *o2;
    if (r->xk == 'X') {
        stmt(&x->out, label, "CXD", NULL);
        return;
    }
    if (r->xk == 'q')
        o2 = xasprintf("QY(%s)", r->xname);
    else if (r->len == 8)
        o2 = xasprintf("%cD(%s)", r->xk, r->xname);
    else if (r->len == 4)
        o2 = xasprintf("%c(%s)", r->xk, r->xname);
    else if (r->len > 4)
        o2 = xasprintf("%cDL%d(%s)", r->xk, r->len, r->xname);
    else
        o2 = xasprintf("%cL%d(%s)", r->xk, r->len, r->xname);
    stmt(&x->out, label, "DC", o2);
    free(o2);
}

static void emit_adcon(Ctx *x, const DRel *r, const char *label) {
    Buf ops = {0};
    char *o2;
    if (r->xk) {
        emit_xcon(x, r, label);
        return;
    }
    if (!r->er && !r->cls) {
        /* only a subtraction: A(-S+k) */
    } else if (r->er) {
        buf_puts(&ops, r->er->name);
        if (!r->vcon && !r->hassub) addend_text(&ops, r->val);
    } else {
        char *l = label_ref(x, r->cls, r->hassub ? 0 : r->val);
        buf_puts(&ops, l);
        free(l);
    }
    if (r->hassub) {
        buf_putc(&ops, '-');
        if (r->er2) {
            buf_puts(&ops, r->er2->name);
        } else {
            char *l = label_ref(x, r->cls2, 0);
            buf_puts(&ops, l);
            free(l);
        }
        addend_text(&ops, r->val);
    }
    if (r->len == 8)
        o2 = xasprintf("AD(%s)", buf_cstr(&ops));
    else
        o2 = xasprintf("%cL%d(%s)", r->vcon ? 'V' : 'A', r->len, buf_cstr(&ops));
    stmt(&x->out, label, "DC", o2);
    free(o2);
}

/* data from o up to limit; returns where the next statement starts */
static size_t emit_data(Ctx *x, const DClass *c, size_t o, size_t limit, const char *label) {
    const uint8_t *d = c->data;
    size_t z = 0, s = 0, e;
    Buf ops = {0};
    while (o + z < limit && d[o + z] == 0 && z < 256) z++;
    if (z >= 8) {
        char *t = xasprintf("XL%zu'0'", z);
        stmt(&x->out, label, "DC", t);
        free(t);
        return o + z;
    }
    while (o + s < limit && safe_char(d[o + s]) && s < 60) s++;
    if (s >= 6) {
        size_t i;
        buf_puts(&ops, "C'");
        for (i = 0; i < s; i++) buf_putc(&ops, ZA_A(d[o + i]));
        buf_putc(&ops, '\'');
        stmt(&x->out, label, "DC", buf_cstr(&ops));
        return o + s;
    }
    /* hex up to the next zero or text run */
    e = o;
    while (e < limit && e - o < 16) {
        size_t zz = 0, ss = 0;
        while (e + zz < limit && d[e + zz] == 0 && zz < 8) zz++;
        while (e + ss < limit && safe_char(d[e + ss]) && ss < 6) ss++;
        if (e > o && (zz >= 8 || ss >= 6)) break;
        e++;
    }
    buf_puts(&ops, "X'");
    while (o < e) buf_printf(&ops, "%02X", d[o++]);
    buf_putc(&ops, '\'');
    stmt(&x->out, label, "DC", buf_cstr(&ops));
    return e;
}

static void emit_body(Ctx *x, DClass *c) {
    size_t p = 0, run_end = 0;
    for (;;) {
        Vec(char *) labels = {0};
        size_t i, ni;
        const DName *n;
        if (c->lab[p]) {
            n = first_name(c, (int64_t)p, &ni);
            if (n) {
                for (; ni < c->names.n && c->names.v[ni].off == (int64_t)p; ni++)
                    if (!c->names.v[ni].sect) vec_push(&labels, c->names.v[ni].name);
            } else {
                vec_push(&labels, gen_name(x, c, (int64_t)p));
            }
        }
        /* every label but the last gets its own statement; the last one
         * is written in front of the item */
        for (i = 0; i + 1 < labels.n; i++) stmt(&x->out, labels.v[i], "DS", "0X");
        if (p >= c->len) {
            if (labels.n) stmt(&x->out, labels.v[labels.n - 1], "DS", "0X");
            free(labels.v);
            break;
        }
        {
            const char *label = labels.n ? labels.v[labels.n - 1] : NULL;
            if (c->ins_at[p] > 0) {
                const DIns *di = &c->ins.v[c->ins_at[p] - 1];
                char *ops = ins_operands(x, c, di);
                char *mn = za_strupper(di->in->mnemonic);
                stmt(&x->out, label, mn, ops);
                free(mn);
                free(ops);
                p += (size_t)di->len;
            } else if (c->rel_at[p] && !c->rels[c->rel_at[p] - 1].ri) {
                const DRel *r = &c->rels[c->rel_at[p] - 1];
                emit_adcon(x, r, label);
                p += (size_t)r->len;
            } else {
                /* the end of the plain data is found once for the whole run */
                if (p >= run_end) {
                    run_end = p + 1;
                    while (run_end < c->len && !c->lab[run_end] && c->ins_at[run_end] <= 0 &&
                           !(c->rel_at[run_end] && !c->rels[c->rel_at[run_end] - 1].ri))
                        run_end++;
                }
                p = emit_data(x, c, p, run_end, label);
            }
        }
        free(labels.v);
    }
}

/* ---- sections -------------------------------------------------------------------- */

static void cattr_stmt(Ctx *x, DClass *c) {
    Buf ops = {0};
    const GoffESD *e = c->ed;
    const GoffBehavior *b = &e->attr;
#define ADD(...)                          \
    do {                                  \
        if (ops.len) buf_putc(&ops, ','); \
        buf_printf(&ops, __VA_ARGS__);    \
    } while (0)
    if (b->alignment != GOFF_ALIGN_DBL) ADD("ALIGN(%d)", b->alignment);
    if (b->executable == GOFF_EXEC_CODE) ADD("EXECUTABLE");
    if (b->executable == GOFF_EXEC_DATA) ADD("NOTEXECUTABLE");
    if (e->has_fill) ADD("FILL(%d)", e->fill);
    if (b->bind_algorithm == GOFF_BIND_MERGE) ADD("MERGE");
    if (b->loading == GOFF_NOLOAD) ADD("NOLOAD");
    if (b->loading == GOFF_LOAD_DEFERRED) ADD("DEFLOAD");
    if (c->part) ADD("PART(%s)", c->part);
    if (c->pr && c->pr->priority) ADD("PRIORITY(%u)", (unsigned)c->pr->priority);
    if (b->read_only) ADD("READONLY");
    if (b->tasking == GOFF_TASK_RENT) ADD("RENT");
    if (b->tasking == GOFF_TASK_REUS) ADD("REUS");
    /* a class made by CATTR is NONREUS and RMODE(24) unless told otherwise (IBM HLASM) */
    if (b->tasking == GOFF_TASK_UNSPEC) ADD("TASKING(NONE)");
    if (b->rmode != GOFF_RMODE_24) {
        if (!b->rmode)
            ADD("RMODE(NONE)");
        else if (rmode_name(b->rmode))
            ADD("RMODE(%s)", rmode_name(b->rmode));
    }
#undef ADD
    stmt(&x->out, c->name, "CATTR", buf_cstr(&ops));
}

/* DXD: the alignment as a zero length item, then the length */
static void emit_dxd(Ctx *x, const DPr *p) {
    Buf ops = {0};
    int64_t big = p->len / 32768, rest = p->len % 32768;
    const char *al = p->align <= 1 ? NULL : p->align == 2 ? "H" : p->align == 4 ? "F" : "D";
    if (p->align > 8)
        warnf(x, "the alignment %d of %s is written as 8", p->align, p->name);
    if (al) buf_printf(&ops, "0%s", al);
    if (big) buf_printf(&ops, "%s%lldXL32768", ops.len ? "," : "", (long long)big);
    if (rest) buf_printf(&ops, "%sXL%lld", ops.len ? "," : "", (long long)rest);
    else if (!big && !al) buf_puts(&ops, "0X"); /* a zero length modifier is an error */
    stmt(&x->out, p->name, "DXD", buf_cstr(&ops));
}

static void emit_section(Ctx *x, DSect *s, size_t sect_idx) {
    DClass *first = s->classes.n ? s->classes.v[0] : NULL;
    const char *op = s->common ? "COM" : "CSECT";
    size_t i;
    uint8_t rmode = first ? first->ed->attr.rmode : 0;
    if (!first || strcmp(first->name, DEFAULT_CLASS) != 0)
        warnf(x, "section %s has no %s class first; its layout cannot be reproduced",
              s->name ? s->name : "(unnamed)", DEFAULT_CLASS);
    if (first && first->ed->attr.read_only && !s->common) op = "RSECT";
    if (sect_idx) buf_putc(&x->out, '\n');
    stmt(&x->out, s->name, op, NULL);
    if (first) {
        const char *am = amode_name(first->ed->attr.amode);
        const char *rm = rmode_name(rmode);
        if (am) stmt(&x->out, NULL, "AMODE", am);
        if (rm) stmt(&x->out, NULL, "RMODE", rm);
        if (first->ed->attr.amode && !am) warnf(x, "AMODE %d cannot be written", first->ed->attr.amode);
    }
    for (i = 0; i < s->classes.n; i++) {
        DClass *c = s->classes.v[i];
        size_t k;
        for (k = 0; k < c->names.n; k++)
            if (!c->names.v[k].sect && !c->names.v[k].zs) {
                const GoffBehavior *eb = &c->names.v[k].esd->attr;
                const char *ea = amode_name(eb->amode), *er = rmode_name(eb->rmode);
                stmt(&x->out, NULL, "ENTRY", c->names.v[k].name);
                if (ea && eb->amode) stmt(&x->out, c->names.v[k].name, "AMODE", ea);
                if (er && eb->rmode) stmt(&x->out, c->names.v[k].name, "RMODE", er);
            }
    }
    for (i = 0; i < s->prs.n; i++) emit_dxd(x, s->prs.v[i]);
    for (i = 0; i < x->ers.n; i++) {
        DEr *er = x->ers.v[i];
        size_t q;
        int folded = 0;
        if (er->sect != s) continue;
        /* IBM HLASM writes an ER for every EXTRN, used or not; an ER named like a label
         * definition comes from R(label) and is made again by the assembler */
        for (q = 0; q < x->m->esds.n; q++) {
            const GoffESD *ld = x->m->esds.v[q];
            if (ld->type == GOFF_LD && ld->name_len == er->esd->name_len &&
                !memcmp(ld->name, er->esd->name, ld->name_len))
                folded = 1;
        }
        if (folded) continue;
        stmt(&x->out, NULL, er->weak ? "WXTRN" : "EXTRN", er->name);
        xattr_stmt(x, er->name, &er->esd->attr, GOFF_SCOPE_UNSPEC, 0);
    }
    if (s->ld && s->name) xattr_stmt(x, s->name, &s->ld->attr, GOFF_SCOPE_UNSPEC, first->ed->attr.executable);
    for (i = 0; i < s->classes.n; i++) {
        DClass *c = s->classes.v[i];
        size_t k;
        for (k = 0; k < c->names.n; k++)
            if (!c->names.v[k].sect && !c->names.v[k].zs)
                xattr_stmt(x, c->names.v[k].name, &c->names.v[k].esd->attr, GOFF_SCOPE_UNSPEC,
                           c->ed->attr.executable);
        if (c->pr) xattr_stmt(x, c->part, &c->pr->attr, GOFF_SCOPE_UNSPEC, c->ed->attr.executable);
    }
    for (i = 0; i < s->classes.n; i++) {
        DClass *c = s->classes.v[i];
        if (i || strcmp(c->name, DEFAULT_CLASS) != 0) cattr_stmt(x, c);
        emit_body(x, c);
    }
}

/* ---- replay of the ZSYM statements -------------------------------------------- */

static char *decoded_name(const GoffESD *e) { return ebcdic_decode(e->name, e->name_len); }

/* a template whose symbol numbers are all valid */
static int tmpl_valid(const ZBlock *b, const ZTmpl *t) {
    size_t i;
    for (i = 0; i < t->n; i++)
        if (t->v[i].tag != 't' && t->v[i].id >= b->syms.n) return 0;
    return 1;
}

static void smart_fail(Smart *sm, size_t sec, Ctx *x, const char *why, const char *file, uint32_t line) {
    const char *name = x->sects.v[sec]->name ? x->sects.v[sec]->name : "(unnamed)";
    if (!sm->ok[sec]) return;
    sm->ok[sec] = 0;
    sm->degraded = 1;
    if (file)
        warnf(x, "the ZSYM information does not match section %s (%s, statement %s:%u): the generic converter "
                 "writes it",
              name, why, file, (unsigned)line);
    else
        warnf(x, "the ZSYM information does not match section %s (%s): the generic converter writes it", name, why);
}

/* Decide how the object is converted: a Smart plan when it carries one
 * usable block of ZSYM, else NULL (generic converter). */
static Smart *smart_plan(Ctx *x) {
    size_t zlen, i, k, nsec = x->sects.n;
    uint8_t *zb = zsym_extract(x->m, &zlen);
    Zsym *z;
    ZBlock *b;
    Smart *sm;
    uint8_t **mask, **cov;
    if (!zb) return NULL;
    z = zsym_parse(zb, zlen);
    if (z->blocks.n != 1 || z->bad_chunks || !z->blocks.v[0]->complete) {
        warnf(x, z->blocks.n > 1 ? "the object holds ZSYM of several modules; the generic converter is used"
                                 : "the ZSYM information is damaged or incomplete; the generic converter is used");
        return NULL;
    }
    b = z->blocks.v[0];
    for (i = 0; i < b->stmts.n; i++) {
        const ZStmt *s = &b->stmts.v[i];
        if (s->scope >= (int64_t)b->scopes.n || !s->op[0] || !tmpl_valid(b, &s->label) ||
            !tmpl_valid(b, &s->operands) || s->len < 0 || s->off < 0) {
            warnf(x, "the ZSYM information is inconsistent; the generic converter is used");
            return NULL;
        }
    }
    sm = xcalloc(sizeof *sm);
    sm->b = b;
    sm->scope_sect = xcalloc((b->scopes.n + 1) * sizeof *sm->scope_sect);
    sm->scope_cls = xcalloc((b->scopes.n + 1) * sizeof *sm->scope_cls);
    sm->ok = xcalloc(nsec + 1);
    sm->done = xcalloc(nsec + 1);
    memset(sm->ok, 1, nsec);
    mask = xcalloc((x->classes.n + 1) * sizeof *mask);
    cov = xcalloc((x->classes.n + 1) * sizeof *cov);

    /* scopes -> sections and classes of the object */
    {
        size_t *mapped = xcalloc((nsec + 1) * sizeof *mapped);
        for (i = 0; i < b->scopes.n; i++) {
            const ZScope *sc = &b->scopes.v[i];
            size_t si;
            DClass *found = NULL;
            sm->scope_sect[i] = -1;
            if (sc->kind == K_DSECT) continue;
            for (si = 0; si < nsec && !found; si++) {
                DSect *ds = x->sects.v[si];
                char *nm = decoded_name(ds->sd);
                int same = !strcmp(!strcmp(nm, PRIV_NAME) ? "" : nm, sc->sect);
                free(nm);
                if (!same) continue;
                for (k = 0; k < ds->classes.n; k++) {
                    char *cn = decoded_name(ds->classes.v[k]->ed);
                    int hit = !strcmp(cn, sc->cls);
                    free(cn);
                    if (hit) {
                        found = ds->classes.v[k];
                        sm->scope_sect[i] = (int)si;
                        break;
                    }
                }
            }
            for (k = 0; found && k < i; k++)
                if (sm->scope_cls[k] == found) found = NULL; /* two scopes, one class */
            if (!found) {
                warnf(x, "the ZSYM information names section %s class %s, which the object does not have; "
                         "the generic converter is used",
                      sc->sect[0] ? sc->sect : "(unnamed)", sc->cls);
                return NULL;
            }
            sm->scope_cls[i] = found;
            mapped[sm->scope_sect[i]]++;
        }
        for (i = 0; i < nsec; i++) {
            size_t nc = 0;
            for (k = 0; k < x->sects.v[i]->classes.n; k++) nc++;
            if (nc != mapped[i]) smart_fail(sm, i, x, "the classes differ", NULL, 0);
        }
    }

    /* the bytes the binder may change, and the bytes the statements cover */
    for (i = 0; i < x->classes.n; i++) {
        mask[i] = xcalloc(x->classes.v[i]->len + 1);
        cov[i] = xcalloc(x->classes.v[i]->len + 1);
    }
    for (i = 0; i < x->m->relocs.n; i++) {
        const GoffReloc *r = &x->m->relocs.v[i];
        DClass *c = class_of(x, r->pid);
        size_t kk;
        if (!c) continue;
        for (kk = 0; kk < r->length && r->offset + kk < c->len; kk++) mask[c->idx][r->offset + kk] = 1;
    }
    for (i = 0; i < b->stmts.n; i++) {
        const ZStmt *s = &b->stmts.v[i];
        DClass *c;
        size_t sec;
        const char *file = s->file < b->files.n ? b->files.v[s->file] : "?";
        if (s->scope < 0 || !sm->scope_cls[s->scope]) continue;
        c = sm->scope_cls[s->scope];
        sec = (size_t)sm->scope_sect[s->scope];
        if (!sm->ok[sec] || s->len == 0) continue;
        if ((uint64_t)s->off + (uint64_t)s->len > c->len) {
            smart_fail(sm, sec, x, "a statement lies outside its class", file, s->line);
            continue;
        }
        if (zsym_crc(c->data, c->len, mask[c->idx], s->off, s->len) != s->crc) {
            smart_fail(sm, sec, x, "the bytes of a statement differ", file, s->line);
            continue;
        }
        memset(cov[c->idx] + s->off, 1, (size_t)s->len);
    }
    for (i = 0; i < x->classes.n; i++) {
        DClass *c = x->classes.v[i];
        const GoffESD *e = c->pr ? c->pr : c->ed;
        size_t at, sec = 0;
        for (k = 0; k < nsec; k++)
            if (c->sect == x->sects.v[k]) sec = k;
        if (!sm->ok[sec]) continue;
        for (at = 0; at < c->len; at++)
            if (!cov[i][at] && c->data[at] != (e->has_fill ? e->fill : 0)) {
                smart_fail(sm, sec, x, "bytes no statement wrote differ from the fill", NULL, 0);
                break;
            }
    }

    /* the labels of the ZSYM information name the generic text of a section
     * that no longer matches, so that the other sections still find them */
    for (i = 0; i < b->syms.n; i++) {
        const ZSym *sy = &b->syms.v[i];
        DClass *c;
        size_t kk;
        int dup = 0;
        char *q;
        if (sy->kind != ZY_LABEL || sy->scope < 0 || sy->scope >= (int64_t)b->scopes.n) continue;
        c = sm->scope_cls[sy->scope];
        if (!c || sm->ok[sm->scope_sect[sy->scope]] || sy->val < 0 || (uint64_t)sy->val > c->len) continue;
        q = zsym_quote(sy->name);
        for (kk = 0; kk < c->names.n; kk++)
            if (!strcmp(c->names.v[kk].name, q)) dup = 1;
        if (dup) continue;
        add_name(c, sy->val, q, NULL, 0);
        c->names.v[c->names.n - 1].zs = 1;
    }
    return sm;
}

static void emit_end(Ctx *x) {
    buf_putc(&x->out, '\n');
    if (x->end_cls) {
        char *l = label_ref(x, x->end_cls, x->end_off);
        stmt(&x->out, NULL, "END", l);
        free(l);
    } else if (x->end_er) {
        stmt(&x->out, NULL, "END", x->end_er->name);
    } else {
        stmt(&x->out, NULL, "END", NULL);
    }
}

static void smart_flush(Ctx *x) {
    Smart *sm = x->sm;
    size_t i;
    for (i = 0; i < x->sects.n; i++)
        if (!sm->ok[i] && !sm->done[i]) {
            sm->done[i] = 1;
            emit_section(x, x->sects.v[i], 1);
        }
}

/* ---- -strict-registers notation for the statements of ZSYM ---------------------- */

/* the register number a source operand stands for: a plain number or a name
 * made with EQU of a number; -1 when it is neither (already %r1, an
 * expression, a label...) */
static int64_t strict_reg_value(Ctx *x, const char *text) {
    char *t = str_trim(text), *end;
    long long v;
    const ZSym *sy;
    if (!*t || t[0] == '%' || strmap_get(&x->zpct, t)) return -1;
    v = strtoll(t, &end, 10);
    if (!*end) return (int64_t)v;
    sy = strmap_get(&x->zsyms, t);
    if (sy && sy->kind == ZY_ABS && sy->terms.n == 0) return sy->val;
    return -1;
}

/* regops_map callback: the register position written as %r<n>; NULL keeps it */
static char *strict_visit(void *ctx, const char *text, unsigned flags) {
    int64_t v = strict_reg_value(ctx, text);
    Buf b = {0};
    if (v < 0 || v > ((flags & F_VR) ? 31 : 15)) return NULL;
    reg_text(&b, flags, v);
    return xstrdup(buf_cstr(&b));
}

/* the operands of a statement with every register written as %r1; the text
 * itself when the statement is none of the kinds that are known to take registers */
static char *strict_operands(Ctx *x, const ZStmt *s, const char *operands) {
    char *r = NULL;
    if (s->kind == ZS_USING || s->kind == ZS_DROP)
        r = regops_map("", s->kind == ZS_USING ? RO_USING : RO_DROP, operands, strict_visit, x);
    else if (s->kind == ZS_INSN)
        r = regops_map(s->op, RO_INSN, operands, strict_visit, x);
    return r ? r : xstrdup(operands);
}

static void strict_prepare(Ctx *x, const ZBlock *b) {
    size_t i;
    for (i = 0; i < b->syms.n; i++) {
        char *q = zsym_quote(b->syms.v[i].name);
        strmap_put(&x->zsyms, q, (void *)&b->syms.v[i]);
    }
    for (i = 0; i < b->stmts.n; i++) {
        const ZStmt *s = &b->stmts.v[i];
        char *o;
        if (s->kind != ZS_EQU) continue;
        o = zsym_render(b, &s->operands);
        if (str_trim(o)[0] == '%') {
            char *l = zsym_render(b, &s->label);
            strmap_put(&x->zpct, str_trim(l), (void *)x);
        }
    }
}

/* the statements in source order; a section that no longer matches is
 * written by the generic converter where its first statement was */
static void smart_emit(Ctx *x) {
    Smart *sm = x->sm;
    const ZBlock *b = sm->b;
    size_t i;
    if (x->strict) strict_prepare(x, b);
    for (i = 0; i < b->stmts.n; i++) {
        const ZStmt *s = &b->stmts.v[i];
        int sec = s->scope >= 0 ? sm->scope_sect[s->scope] : -1;
        int off = sec >= 0 && !sm->ok[sec];
        char *l, *o;
        if (s->kind == ZS_END) {
            smart_flush(x);
            if (off) {
                emit_end(x);
                return;
            }
        } else if (off) {
            if (!sm->done[sec]) {
                sm->done[sec] = 1;
                emit_section(x, x->sects.v[sec], 1);
            }
            continue;
        }
        l = zsym_render(b, &s->label);
        o = zsym_render(b, &s->operands);
        if (x->strict) o = strict_operands(x, s, o);
        stmt(&x->out, l, s->op, o);
        free(l);
        free(o);
        if (s->kind == ZS_END) return;
    }
    smart_flush(x);
    emit_end(x);
}

/* ---- the whole module ------------------------------------------------------------ */

char *disasm_ex(const GoffModule *m, int flags, FILE *warn, int *nwarn, int *smart, char *err, size_t errsz) {
    Ctx x;
    size_t i, k;
    memset(&x, 0, sizeof x);
    x.m = m;
    x.warn = warn;
    x.err = err;
    x.errsz = errsz;
    x.strict = (flags & DISASM_STRICT) != 0;
    x.by_id = xcalloc((m->esds.n + 2) * sizeof *x.by_id);
    x.end_off = -1;
    if (smart) *smart = 0;
    if (!load_esds(&x) || !load_text(&x) || !load_labels(&x)) return NULL;
    if (x.sects.n == 0) {
        failf(&x, "the object has no section");
        return NULL;
    }
    if (!(flags & DISASM_GENERIC) && !x.failed) x.sm = smart_plan(&x);
    if (x.sm && !x.sm->degraded) {
        x.warn = NULL; /* the generic analysis is not used */
        x.nwarn = 0;
    }
    for (i = 0; i < x.sects.n; i++)
        if (x.sects.v[i]->name) note_taken(&x, x.sects.v[i]->name);
    for (i = 0; i < x.ers.n; i++) note_taken(&x, x.ers.v[i]->name);
    for (i = 0; i < x.sects.n; i++)
        for (k = 0; k < x.sects.v[i]->prs.n; k++) note_taken(&x, x.sects.v[i]->prs.v[k]->name);
    for (i = 0; i < x.classes.n; i++) {
        DClass *c = x.classes.v[i];
        if (c->part) note_taken(&x, c->part);
        for (k = 0; k < c->names.n; k++) note_taken(&x, c->names.v[k].name);
    }
    if (m->end.kind == GOFF_ENTRY_BYID) {
        DClass *c = class_of(&x, m->end.esdid);
        if (c && m->end.offset <= c->len) {
            x.end_cls = c;
            x.end_off = m->end.offset;
            c->lab[m->end.offset] |= 1;
        } else {
            warnf(&x, "the entry point of the object is not in a class and is ignored");
        }
    } else if (m->end.kind == GOFF_ENTRY_BYNAME) {
        char *nm = sym_source(m->end.name, m->end.name_len);
        for (i = 0; nm && i < x.ers.n; i++)
            if (!strcmp(x.ers.v[i]->name, nm)) x.end_er = x.ers.v[i];
        if (!x.end_er) warnf(&x, "the entry point %s is not an external symbol and is ignored", nm);
    }
    if (!load_relocs(&x)) return NULL;
    drop_split_adcons(&x);
    for (i = 0; i < x.classes.n; i++) {
        DClass *c = x.classes.v[i];
        c->code = class_exec(c) == GOFF_EXEC_CODE;
        if (c->names.n > 1) qsort(c->names.v, c->names.n, sizeof *c->names.v, cmp_name);
    }
    do {
        /* a dropped instruction can hide code behind a label: decode again */
        for (i = 0; i < x.classes.n; i++) {
            DClass *c = x.classes.v[i];
            c->ins.n = 0;
            memset(c->ins_at, 0, (c->len + 1) * sizeof *c->ins_at);
            for (k = 0; k < c->nrels; k++) c->rels[k].used = 0;
            discover(&x, c);
        }
    } while (settle(&x));
    for (i = 0; i < x.classes.n; i++) {
        DClass *c = x.classes.v[i];
        for (k = 0; k < c->nrels; k++)
            if (c->rels[k].ri && !c->rels[k].used && c->rel_at[c->rels[k].off])
                warnf(&x,
                      "relative relocation at %s+%lld is not part of a decodable instruction and is "
                      "ignored",
                      c->name, (long long)c->rels[k].off);
    }
    for (i = 0; i < x.classes.n; i++) {
        DClass *c = x.classes.v[i];
        for (k = 0; k < c->nrels; k++) {
            const DRel *r = &c->rels[k];
            if (r->er && (r->ri ? r->used : c->rel_at[r->off])) r->er->used = 1;
            if (r->er2 && c->rel_at[r->off]) r->er2->used = 1;
        }
    }
    if (x.end_er) x.end_er->used = 1;
    if (x.sm) {
        smart_emit(&x);
        if (smart) *smart = 1;
    } else {
        for (i = 0; i < x.sects.n; i++) emit_section(&x, x.sects.v[i], i);
        emit_end(&x);
    }
    if (x.failed) return NULL;
    if (nwarn) *nwarn = x.nwarn;
    return xstrdup(buf_cstr(&x.out));
}

char *disasm_module(const GoffModule *m, FILE *warn, int *nwarn, char *err, size_t errsz) {
    return disasm_ex(m, 0, warn, nwarn, NULL, err, errsz);
}
