/* emit.c - write a Module as a GOFF object file. */
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "ebcdic.h"
#include "goff.h"
#include "zsrc.h"
#include "zsym.h"

/* GOFF name of an unnamed control section */
#define PRIV_NAME "$PRIV000000"
#define DEFAULT_CLASS "B_TEXT"

static const char *sect_name(const Section *s) { return s->name[0] ? s->name : PRIV_NAME; }

static void set_name(GoffESD *e, const char *s) {
    e->name = ebcdic_from_source(s, strlen(s), &e->name_len);
}

static XAttr *xattr_of(const Module *m, const char *name) {
    Symbol *s = module_symbol(m, name);
    return s ? s->xattr : NULL;
}

static void apply_xattr(GoffBehavior *b, const XAttr *x) {
    if (!x) return;
    if (x->linkage >= 0) b->linkage = (uint8_t)x->linkage;
    if (x->scope >= 0) b->scope = (uint8_t)x->scope;
    if (x->reference) {
        if (!strcmp(x->reference, "CODE"))
            b->executable = GOFF_EXEC_CODE;
        else if (!strcmp(x->reference, "DATA"))
            b->executable = GOFF_EXEC_DATA;
    }
    if (x->indirect >= 0) b->indirect = x->indirect;
}

/* the ESD item that carries the text of the class */
static uint32_t class_target(const Class *c) { return c->part ? c->pr : c->esd; }

/* defense in depth before anything is written: the assembler caps a class at
 * ZA_MAX_LOC, but the GOFF records carry lengths and offsets in 32 bits and
 * trust the assembler otherwise. A violation here is a bug in the assembler;
 * it must be an error, not a silently truncated object. */
int module_validate(const Module *m, char *err, size_t errsz) {
    size_t si, ci, i;
    for (si = 0; si < m->sections.n; si++) {
        Section *s = m->sections.v[si];
        for (ci = 0; ci < s->classes.n; ci++) {
            Class *c = s->classes.v[ci];
            if (s->kind != K_DSECT &&
                (c->len < 0 || c->len > (int64_t)0xFFFFFFFF)) {
                snprintf(err, errsz, "the class %s of section \"%s\" is %lld bytes long; GOFF "
                                     "cannot hold more than 4294967295",
                         c->name ? c->name : "", s->name, (long long)c->len);
                return 0;
            }
            for (i = 0; i < c->relocs.n; i++) {
                const Reloc *r = &c->relocs.v[i];
                if (r->off < 0 || r->off + r->len > c->len) {
                    snprintf(err, errsz, "a relocation of the class %s lies outside its text",
                             c->name ? c->name : "");
                    return 0;
                }
            }
        }
    }
    if (m->entry) {
        Symbol *e = module_symbol(m, m->entry);
        if (e && e->piece && (e->piece->start < 0 || e->piece->start > (int64_t)0xFFFFFFFF ||
                              (e->kind == SYM_LABEL &&
                               e->piece->start + e->off > (int64_t)0xFFFFFFFF))) {
            snprintf(err, errsz, "the entry point \"%s\" is beyond 4294967295; GOFF cannot hold it",
                     m->entry);
            return 0;
        }
    }
    return 1;
}

#define TRY(call)                                                  \
    do {                                                           \
        const char *e_ = (call);                                   \
        if (e_) {                                                  \
            snprintf(err, errsz, "%s", e_);                        \
            goto fail;                                             \
        }                                                          \
    } while (0)

/* write an ESD and release the name it carries (the writer copies it) */
static const char *put_esd(GoffWriter *w, GoffESD *e) {
    const char *r = goff_write_esd(w, e);
    free(e->name);
    e->name = NULL;
    return r;
}

static const char *emit_class(GoffWriter *w, const Module *m, Section *s, Class *c) {
    uint8_t rmode = c->attr.rmode == CLASS_RMODE_NONE ? 0 : c->attr.rmode ? c->attr.rmode : s->rmode;
    uint8_t align = c->attr.align >= 0 ? (uint8_t)c->attr.align : GOFF_ALIGN_DBL;
    uint8_t exec = (uint8_t)c->attr.executable;
    uint8_t loading = GOFF_LOAD_NOW;
    uint8_t bind = c->attr.merge || c->part ? GOFF_BIND_MERGE : GOFF_BIND_CAT;
    GoffESD ed, pr;
    const char *e;
    if (c->attr.noload)
        loading = GOFF_NOLOAD;
    else if (c->attr.defload)
        loading = GOFF_LOAD_DEFERRED;

    memset(&ed, 0, sizeof ed);
    ed.type = GOFF_ED;
    ed.parent = s->esd;
    set_name(&ed, c->name);
    ed.name_space = GOFF_NS_NORMAL;
    ed.length = (uint32_t)c->len;
    ed.attr.amode = c->attr.cattr ? 0 : s->amode;
    ed.attr.rmode = rmode;
    ed.attr.bind_algorithm = bind;
    ed.attr.tasking = (uint8_t)c->attr.tasking;
    ed.attr.read_only = c->attr.readonly;
    ed.attr.executable = exec;
    ed.attr.loading = loading;
    ed.attr.alignment = align;
    ed.removable = c->attr.removable;
    if (c->attr.fill >= 0) {
        ed.has_fill = 1;
        ed.fill = (uint8_t)c->attr.fill;
    }
    if (c->part) {
        ed.length = 0;
        ed.name_space = GOFF_NS_PARTS3;
    }
    if ((e = put_esd(w, &ed)) != NULL) return e;
    c->esd = ed.id;
    if (!c->part) return NULL;

    memset(&pr, 0, sizeof pr);
    pr.type = GOFF_PR;
    pr.parent = ed.id;
    set_name(&pr, c->part);
    pr.name_space = GOFF_NS_PARTS3;
    pr.length = (uint32_t)c->len;
    pr.attr.amode = c->attr.cattr ? 0 : s->amode;
    pr.attr.rmode = rmode;
    pr.attr.bind_algorithm = bind;
    pr.attr.tasking = (uint8_t)c->attr.tasking;
    pr.attr.read_only = c->attr.readonly;
    pr.attr.executable = exec;
    pr.attr.loading = loading;
    pr.attr.alignment = align;
    if (c->attr.priority >= 0) pr.priority = (uint32_t)c->attr.priority;
    apply_xattr(&pr.attr, xattr_of(m, c->part));
    if ((e = put_esd(w, &pr)) != NULL) return e;
    c->pr = pr.id;
    return NULL;
}

/* the pseudo register class B_PRV of a section and its pseudo registers
 * (DXD names and DSECTs used in Q and J constants) */
static const char *emit_prv(GoffWriter *w, const Module *m, Section *s) {
    GoffESD ed;
    size_t i;
    const char *e;
    memset(&ed, 0, sizeof ed);
    ed.type = GOFF_ED;
    ed.parent = s->esd;
    set_name(&ed, "B_PRV");
    ed.name_space = GOFF_NS_PSEUDO;
    ed.length = 0;
    ed.attr.bind_algorithm = GOFF_BIND_MERGE;
    ed.attr.loading = GOFF_NOLOAD;
    ed.attr.alignment = GOFF_ALIGN_DBL;
    if ((e = put_esd(w, &ed)) != NULL) return e;
    s->prv_esd = ed.id;
    for (i = 0; i < m->sym_order.n; i++) {
        Symbol *sym = m->sym_order.v[i];
        GoffESD pr;
        uint8_t lg = 0;
        if (!sym->xd || sym->xd_sect != s) continue;
        while (((int64_t)1 << lg) < sym->xd_align && lg < 12) lg++;
        memset(&pr, 0, sizeof pr);
        pr.type = GOFF_PR;
        pr.parent = ed.id;
        set_name(&pr, sym->name);
        pr.name_space = GOFF_NS_PSEUDO;
        pr.length = (uint32_t)sym->xd_len;
        pr.attr.bind_algorithm = GOFF_BIND_MERGE;
        pr.attr.bind_strength = GOFF_STRENGTH_WEAK;
        pr.attr.alignment = lg;
        apply_xattr(&pr.attr, sym->xattr);
        if ((e = put_esd(w, &pr)) != NULL) return e;
        sym->esd = pr.id;
    }
    return NULL;
}

static void init_ld(GoffESD *ld, Section *s, const char *name, const Class *c, int is_sect) {
    uint8_t exec = (uint8_t)c->attr.executable;
    memset(ld, 0, sizeof *ld);
    ld->type = GOFF_LD;
    ld->parent = c->part ? c->pr : c->esd;
    set_name(ld, name);
    ld->name_space = GOFF_NS_NORMAL;
    if (is_sect) {
        ld->attr.amode = s->amode;
        ld->attr.rmode = s->rmode;
    }
    ld->attr.executable = exec;
}

/* Q, CXD, J and R constants.  These encodings follow the reference and
 * referent types of the GOFF documentation but have not been checked against
 * an IBM assembler object (DESIGN.md section 5.4); keep them in this function. */
const GoffRelocPat *reloc_pattern(RelocKind k) {
    /* the flags of each x-con kind; Q accepts a field of 1 to 8 bytes, LEN of
     * 2, 3, 4 or 8, R of 3, 4 or 8 (bit n of lenset = n bytes) */
    static const GoffRelocPat pats[] = {
        {GOFF_REF_OFFSET, GOFF_REFERENT_CLASS, 0, 0x1FE, 0, 0, 0},             /* REL_Q */
        {GOFF_REF_LONGDISP, GOFF_REFERENT_CLASS, 0, 0, 2, 4, 4},               /* REL_QY: 20 bit long
                                                                                displacement, bits 4..23 */
        {GOFF_REF_LENGTH, GOFF_REFERENT_CLASS, 1, 1u << 4, 0, 0, 0},           /* REL_CXD */
        {GOFF_REF_LENGTH, GOFF_PAT_PART_OR_CLASS, 1, (1u << 2) | (1u << 3) | (1u << 4) | (1u << 8),
         0, 0, 0}, /* REL_LEN: PART or CLASS, the target decides */
        {GOFF_REF_RTYPE, GOFF_REFERENT_LABEL, 1, (1u << 3) | (1u << 4) | (1u << 8), 0, 0, 0}, /* REL_R */
    };
    switch (k) {
    case REL_Q: return &pats[0];
    case REL_QY: return &pats[1];
    case REL_CXD: return &pats[2];
    case REL_LEN: return &pats[3];
    case REL_R: return &pats[4];
    default: return NULL; /* A/V/Y constants and relative immediates: no x-con pattern */
    }
}

static int reloc_xd(const Class *c, const Reloc *r, GoffReloc *g, char *err, size_t errsz) {
    const Symbol *t = r->target;
    const GoffRelocPat *p = reloc_pattern(r->kind);
    if (p) { /* the fixed flags come from the table shared with the disassembler */
        g->reference = p->reference;
        g->store = p->store;
        if (p->referent) g->referent = p->referent;
        if (p->length) {
            g->length = p->length;
            g->bit_length = p->bit_length;
            g->bit_offset = p->bit_offset;
        }
    }
    switch (r->kind) {
    case REL_CXD:
        g->rid = c->sect->prv_esd;
        return 1;
    case REL_Q:
    case REL_QY:
        g->rid = t ? t->esd : r->cls->pr;
        return 1;
    case REL_LEN:
        if (t) {
            g->referent = GOFF_REFERENT_PART;
            g->rid = t->esd;
        } else if (r->cls->part) {
            g->referent = GOFF_REFERENT_PART;
            g->rid = r->cls->pr;
        } else {
            g->referent = GOFF_REFERENT_CLASS;
            g->rid = r->cls->esd;
        }
        return 1;
    default: /* REL_R */
        /* every R-type target gets an ER above; it is the R-ID, also for a
         * LOCTR name that never receives an LD of its own (IBM HLASM) */
        g->rid = t->kind == SYM_EXT ? t->esd : t->r_er ? t->r_er : t->ld_esd;
        if (!g->rid) {
            snprintf(err, errsz, "R-type constant: \"%s\" has no label definition", t->name);
            return 0;
        }
        return 1;
    }
}

static int reloc_item(const Class *c, const Reloc *r, GoffReloc *g, char *err, size_t errsz) {
    memset(g, 0, sizeof *g);
    g->pid = class_target(c);
    g->offset = (uint32_t)r->off;
    g->length = (uint8_t)r->len;
    g->action = r->sub ? GOFF_ACTION_SUB : GOFF_ACTION_ADD;
    switch (r->kind) {
    case REL_Q:
    case REL_QY:
    case REL_CXD:
    case REL_LEN:
    case REL_R:
        return reloc_xd(c, r, g, err, errsz);
    case REL_ADDR:
    case REL_V:
        g->reference = GOFF_REF_ADDRESS;
        break;
    case REL_RI:
        g->reference = GOFF_REF_RELIMM;
        break;
    default:
        snprintf(err, errsz, "relocation kind %d is not supported", (int)r->kind);
        return 0;
    }
    if (r->target) {
        g->rid = r->target->esd;
        g->referent = GOFF_REFERENT_LABEL;
        if (r->kind == REL_V) g->store = 1;
    } else if (r->cls) {
        if (r->cls->part) {
            g->rid = r->cls->pr;
            g->referent = GOFF_REFERENT_LABEL;
        } else {
            g->rid = r->cls->esd;
            g->referent = GOFF_REFERENT_LABEL;
        }
    } else {
        snprintf(err, errsz, "relocation without a target");
        return 0;
    }
    return 1;
}

typedef struct {
    int64_t off;
    size_t idx;
} SortKey;

static int cmp_key(const void *x, const void *y) {
    const SortKey *a = x, *b = y;
    if (a->off != b->off) return a->off < b->off ? -1 : 1;
    return a->idx < b->idx ? -1 : a->idx > b->idx;
}

/* the relocations of c in offset order (idx = index in c->relocs) */
static SortKey *sorted_relocs(const Class *c) {
    SortKey *keys = xmalloc((c->relocs.n + 1) * sizeof *keys);
    size_t k;
    for (k = 0; k < c->relocs.n; k++) {
        keys[k].off = c->relocs.v[k].off;
        keys[k].idx = k;
    }
    qsort(keys, c->relocs.n, sizeof *keys, cmp_key);
    return keys;
}

/* ENTRY symbols of a section in address order: class, offset, definition.
 * The object does not depend on the order of the statements this way. */
typedef struct {
    Symbol *sym;
    size_t cls;
    int64_t off;
    size_t idx;
} EntryKey;

static int cmp_entry(const void *x, const void *y) {
    const EntryKey *a = x, *b = y;
    if (a->cls != b->cls) return a->cls < b->cls ? -1 : 1;
    if (a->off != b->off) return a->off < b->off ? -1 : 1;
    return a->idx < b->idx ? -1 : a->idx > b->idx;
}

uint8_t *module_emit(Module *m, size_t *len, char *err, size_t errsz) {
    GoffWriter *w = goff_writer_new(1);
    SectionVec secs = {0};
    SymbolVec er_order = {0};
    SectionVec er_parent = {0};
    GoffReloc *rlds = NULL;
    size_t nrld = 0, caprld = 0;
    GoffEnd end;
    size_t si, ci, i, k;
    const uint8_t *out;
    uint8_t *copy, *adata[2] = {NULL, NULL};
    memset(&end, 0, sizeof end);
    size_t alen[2] = {0, 0};
    uint32_t aesd[2] = {0, 0};

    for (si = 0; si < m->sections.n; si++)
        if (m->sections.v[si]->kind != K_DSECT) vec_push(&secs, m->sections.v[si]);
    if (secs.n == 0) {
        snprintf(err, errsz, "no control section to emit");
        goto fail;
    }
    if (!module_validate(m, err, errsz)) goto fail;

    /* external references: one ER per symbol, below the first section using it */
#define NOTE(SYM, SECT) NOTE_IF(SYM, SECT, sym_->kind == SYM_EXT)
#define NOTE_IF(SYM, SECT, COND)                                  \
    do {                                                          \
        Symbol *sym_ = (SYM);                                     \
        int seen_ = 0;                                            \
        size_t q_;                                                \
        if (sym_ && (COND)) {                                     \
            for (q_ = 0; q_ < er_order.n; q_++)                   \
                if (er_order.v[q_] == sym_) seen_ = 1;            \
            if (!seen_) {                                         \
                vec_push(&er_order, sym_);                        \
                vec_push(&er_parent, (SECT));                     \
            }                                                     \
        }                                                         \
    } while (0)
    /* IBM HLASM (probe er1): every EXTRN and WXTRN makes an ER, used or not, below the
     * section that was current at the statement */
    for (i = 0; i < m->sym_order.n; i++) {
        Symbol *x_ = m->sym_order.v[i];
        Section *par_ = secs.v[0];
        if (x_->kind != SYM_EXT) continue;
        for (si = 0; si < secs.n; si++)
            if (secs.v[si] == x_->ext_sect) par_ = secs.v[si];
        NOTE(x_, par_);
    }
    for (si = 0; si < secs.n; si++)
        for (ci = 0; ci < secs.v[si]->classes.n; ci++) {
            Class *c = secs.v[si]->classes.v[ci];
            SortKey *keys = sorted_relocs(c);
            for (k = 0; k < c->relocs.n; k++) {
                const Reloc *r_ = &c->relocs.v[keys[k].idx];
                /* IBM HLASM makes an ER of the same name for an R-type constant naming a local entry */
                NOTE_IF(r_->target, secs.v[si], sym_->kind == SYM_EXT || r_->kind == REL_R);
            }
            free(keys);
        }
    if (m->entry) {
        Symbol *e = module_symbol(m, m->entry);
        if (e && e->kind == SYM_EXT) NOTE(e, secs.v[0]);
    }
#undef NOTE
#undef NOTE_IF

    for (si = 0; si < secs.n; si++) {
        Section *s = secs.v[si];
        GoffESD sd;
        memset(&sd, 0, sizeof sd);
        sd.type = GOFF_SD;
        sd.name_space = GOFF_NS_NORMAL;
        set_name(&sd, sect_name(s));
        sd.attr.common = s->kind == K_COM;
        TRY(put_esd(w, &sd));
        s->esd = sd.id;
        for (ci = 0; ci < s->classes.n; ci++) TRY(emit_class(w, m, s, s->classes.v[ci]));
        if (s->prv) TRY(emit_prv(w, m, s));

        /* label definitions: the section name and ENTRY symbols */
        if (s->name[0]) {
            GoffESD ld;
            Symbol *sym;
            init_ld(&ld, s, s->name, s->classes.v[0], 1);
            apply_xattr(&ld.attr, xattr_of(m, s->name));
            TRY(put_esd(w, &ld));
            sym = module_symbol(m, s->name);
            if (sym) sym->ld_esd = ld.id;
        }
        {
            EntryKey *ek = xmalloc((m->sym_order.n + 1) * sizeof *ek);
            size_t nek = 0;
            for (i = 0; i < m->sym_order.n; i++) {
                Symbol *sym = m->sym_order.v[i];
                size_t q;
                if (!sym->entry || sym->sect != s || !strcmp(sym->name, s->name)) continue;
                if (sym->kind != SYM_LABEL && sym->kind != SYM_SECT) continue;
                ek[nek].sym = sym;
                ek[nek].cls = 0;
                for (q = 0; q < s->classes.n; q++)
                    if (s->classes.v[q] == sym->piece->cls) ek[nek].cls = q;
                ek[nek].off = sym->piece->start + (sym->kind == SYM_LABEL ? sym->off : 0);
                ek[nek].idx = i;
                nek++;
            }
            qsort(ek, nek, sizeof *ek, cmp_entry);
            for (i = 0; i < nek; i++) {
                Symbol *sym = ek[i].sym;
                GoffESD ld;
                init_ld(&ld, s, sym->name, sym->piece->cls, 0);
                if (sym->amode || sym->rmode) {
                    ld.attr.amode = sym->amode ? sym->amode : GOFF_AMODE_31;
                    ld.attr.rmode = sym->rmode;
                }
                ld.offset = (uint32_t)ek[i].off;
                apply_xattr(&ld.attr, sym->xattr);
                TRY(put_esd(w, &ld));
                sym->ld_esd = ld.id;
            }
            free(ek);
        }
        for (i = 0; i < er_order.n; i++) {
            Symbol *sym = er_order.v[i];
            GoffESD er;
            if (er_parent.v[i] != s) continue;
            memset(&er, 0, sizeof er);
            er.type = GOFF_ER;
            er.parent = s->esd;
            set_name(&er, sym->name);
            er.name_space = GOFF_NS_NORMAL;
            if (sym->weak) er.attr.bind_strength = GOFF_STRENGTH_WEAK;
            apply_xattr(&er.attr, sym->xattr);
            TRY(put_esd(w, &er));
            if (sym->kind == SYM_EXT)
                sym->esd = er.id;
            else
                sym->r_er = er.id;
        }
    }

    /* ZSYM and the original sources: classes without load text (ZSYM.md) */
    adata[0] = zsym_build(m, &alen[0]);
    adata[1] = zsrc_build(m, &alen[1]);
    for (k = 0; k < 2; k++) {
        GoffESD ed;
        if (!adata[k]) continue;
        memset(&ed, 0, sizeof ed);
        ed.type = GOFF_ED;
        ed.parent = secs.v[0]->esd;
        set_name(&ed, k ? ZSRC_CLASS : ZSYM_CLASS);
        ed.name_space = GOFF_NS_NORMAL;
        ed.length = (uint32_t)alen[k];
        ed.attr.amode = secs.v[0]->amode;
        ed.attr.rmode = secs.v[0]->rmode;
        ed.attr.text_style = GOFF_STYLE_UNSTRUCTURED;
        ed.attr.bind_algorithm = GOFF_BIND_CAT;
        ed.attr.loading = GOFF_NOLOAD;
        TRY(put_esd(w, &ed));
        aesd[k] = ed.id;
    }

    /* text */
    for (si = 0; si < secs.n; si++)
        for (ci = 0; ci < secs.v[si]->classes.n; ci++) {
            Class *c = secs.v[si]->classes.v[ci];
            if (c->len == 0 || secs.v[si]->kind == K_COM) continue;
            TRY(goff_write_text(w, class_target(c), 0, GOFF_STYLE_BYTE, c->data, (size_t)c->len));
        }
    for (k = 0; k < 2; k++)
        if (adata[k]) TRY(goff_write_text(w, aesd[k], 0, GOFF_STYLE_UNSTRUCTURED, adata[k], alen[k]));

    /* relocations, in offset order per class */
    for (si = 0; si < secs.n; si++)
        for (ci = 0; ci < secs.v[si]->classes.n; ci++) {
            Class *c = secs.v[si]->classes.v[ci];
            SortKey *keys = sorted_relocs(c);
            for (k = 0; k < c->relocs.n; k++) {
                if (nrld == caprld) {
                    caprld = caprld ? caprld * 2 : 64;
                    rlds = xrealloc(rlds, caprld * sizeof *rlds);
                }
                if (!reloc_item(c, &c->relocs.v[keys[k].idx], &rlds[nrld], err, errsz)) {
                    free(keys);
                    return NULL;
                }
                nrld++;
            }
            free(keys);
        }
    if (nrld > 0) TRY(goff_write_rld(w, rlds, nrld));
    free(rlds);

    /* END */
    memset(&end, 0, sizeof end);
    if (m->entry) {
        Symbol *e = module_symbol(m, m->entry);
        if (e->kind == SYM_EXT) {
            end.kind = GOFF_ENTRY_BYNAME;
            end.name = ebcdic_from_source(e->name, strlen(e->name), &end.name_len);
        } else {
            Class *c = e->piece->cls;
            int64_t off = e->piece->start;
            if (e->kind == SYM_LABEL) off += e->off;
            end.kind = GOFF_ENTRY_BYID;
            end.esdid = class_target(c);
            if (!end.esdid) {
                snprintf(err, errsz, "the entry point \"%s\" is not in a section that was emitted "
                                     "(a DSECT?)", m->entry);
                return NULL;
            }
            end.offset = (uint32_t)off;
            end.amode = c->sect->amode;
        }
    }
    TRY(goff_write_end(w, &end, &out, len));
    copy = xmalloc(*len + 1);
    memcpy(copy, out, *len);
    free(adata[0]);
    free(adata[1]);
    free(secs.v);
    free(er_order.v);
    free(er_parent.v);
    free(end.name);
    goff_writer_free(w);
    return copy;

fail:
    free(rlds);
    free(adata[0]);
    free(adata[1]);
    free(secs.v);
    free(er_order.v);
    free(er_parent.v);
    free(end.name);
    goff_writer_free(w);
    return NULL;
}
