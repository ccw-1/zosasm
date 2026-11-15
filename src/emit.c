/* emit.c - write a Module as a GOFF object file. */
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "ebcdic.h"
#include "goff.h"
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

#define TRY(call)                                                  \
    do {                                                           \
        const char *e_ = (call);                                   \
        if (e_) {                                                  \
            snprintf(err, errsz, "%s", e_);                        \
            return NULL;                                           \
        }                                                          \
    } while (0)

static const char *emit_class(GoffWriter *w, const Module *m, Section *s, Class *c) {
    uint8_t rmode = c->attr.rmode ? c->attr.rmode : s->rmode;
    uint8_t align = c->attr.align >= 0 ? (uint8_t)c->attr.align : GOFF_ALIGN_DBL;
    uint8_t exec = (uint8_t)c->attr.executable;
    uint8_t loading = GOFF_LOAD_NOW;
    uint8_t bind = c->attr.merge ? GOFF_BIND_MERGE : GOFF_BIND_CAT;
    GoffESD ed, pr;
    const char *e;
    if (!strcmp(c->name, DEFAULT_CLASS)) exec = GOFF_EXEC_CODE;
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
    ed.attr.amode = s->amode;
    ed.attr.rmode = rmode;
    ed.attr.bind_algorithm = bind;
    ed.attr.tasking = (uint8_t)c->attr.tasking;
    ed.attr.read_only = c->attr.readonly;
    ed.attr.executable = exec;
    ed.attr.loading = loading;
    ed.attr.alignment = align;
    ed.attr.common = s->kind == K_COM;
    if (c->attr.fill >= 0) {
        ed.has_fill = 1;
        ed.fill = (uint8_t)c->attr.fill;
    }
    if (c->part) {
        ed.length = 0;
        ed.name_space = GOFF_NS_PARTS3;
    }
    if ((e = goff_write_esd(w, &ed)) != NULL) return e;
    c->esd = ed.id;
    if (!c->part) return NULL;

    memset(&pr, 0, sizeof pr);
    pr.type = GOFF_PR;
    pr.parent = ed.id;
    set_name(&pr, c->part);
    pr.name_space = GOFF_NS_PARTS3;
    pr.length = (uint32_t)c->len;
    pr.attr.amode = s->amode;
    pr.attr.bind_algorithm = bind;
    pr.attr.tasking = (uint8_t)c->attr.tasking;
    pr.attr.read_only = c->attr.readonly;
    pr.attr.executable = exec;
    pr.attr.loading = loading;
    pr.attr.alignment = align;
    if (c->attr.priority >= 0) pr.priority = (uint32_t)c->attr.priority;
    apply_xattr(&pr.attr, xattr_of(m, c->part));
    if (c->attr.fill >= 0) {
        pr.has_fill = 1;
        pr.fill = (uint8_t)c->attr.fill;
    }
    if ((e = goff_write_esd(w, &pr)) != NULL) return e;
    c->pr = pr.id;
    return NULL;
}

/* the pseudo register class B_PRV of a section and its pseudo registers
 * (DXD names and DSECTs used in Q and J constants) */
static const char *emit_prv(GoffWriter *w, const Module *m, Section *s) {
    GoffESD ed;
    size_t i;
    int64_t max = 0;
    const char *e;
    for (i = 0; i < m->sym_order.n; i++) {
        Symbol *sym = m->sym_order.v[i];
        if (sym->xd && sym->xd_sect == s && sym->xd_len > max) max = sym->xd_len;
    }
    memset(&ed, 0, sizeof ed);
    ed.type = GOFF_ED;
    ed.parent = s->esd;
    set_name(&ed, "B_PRV");
    ed.name_space = GOFF_NS_NORMAL;
    ed.length = (uint32_t)max;
    ed.attr.amode = s->amode;
    ed.attr.rmode = s->rmode;
    ed.attr.bind_algorithm = GOFF_BIND_MERGE;
    ed.attr.executable = GOFF_EXEC_DATA;
    ed.attr.loading = GOFF_LOAD_NOW;
    ed.attr.alignment = GOFF_ALIGN_DBL;
    if ((e = goff_write_esd(w, &ed)) != NULL) return e;
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
        pr.attr.amode = s->amode;
        pr.attr.bind_algorithm = GOFF_BIND_MERGE;
        pr.attr.executable = GOFF_EXEC_DATA;
        pr.attr.loading = GOFF_LOAD_NOW;
        pr.attr.alignment = lg;
        apply_xattr(&pr.attr, sym->xattr);
        if ((e = goff_write_esd(w, &pr)) != NULL) return e;
        sym->esd = pr.id;
    }
    return NULL;
}

static void init_ld(GoffESD *ld, Section *s, const char *name, const Class *c) {
    uint8_t exec = (uint8_t)c->attr.executable;
    if (!strcmp(c->name, DEFAULT_CLASS)) exec = GOFF_EXEC_CODE;
    memset(ld, 0, sizeof *ld);
    ld->type = GOFF_LD;
    ld->parent = c->part ? c->pr : c->esd;
    set_name(ld, name);
    ld->name_space = GOFF_NS_NORMAL;
    ld->attr.amode = s->amode;
    ld->attr.executable = exec;
    ld->attr.scope = GOFF_SCOPE_MODULE;
}

/* Q, CXD, J and R constants.  These encodings follow the reference and
 * referent types of the GOFF documentation but have not been checked against
 * an IBM assembler object (DESIGN.md section 5.4); keep them in this function. */
static int reloc_xd(const Class *c, const Reloc *r, GoffReloc *g, char *err, size_t errsz) {
    const Symbol *t = r->target;
    switch (r->kind) {
    case REL_CXD:
        g->reference = GOFF_REF_LENGTH;
        g->referent = GOFF_REFERENT_CLASS;
        g->rid = c->sect->prv_esd;
        g->store = 1;
        return 1;
    case REL_Q:
        g->reference = GOFF_REF_OFFSET;
        g->referent = GOFF_REFERENT_LABEL;
        g->rid = t ? t->esd : r->cls->pr;
        return 1;
    case REL_LEN:
        g->reference = GOFF_REF_LENGTH;
        g->store = 1;
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
        g->reference = GOFF_REF_RTYPE;
        g->referent = GOFF_REFERENT_LABEL;
        g->store = 1;
        g->rid = t->kind == SYM_EXT ? t->esd : t->ld_esd;
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
    g->action = GOFF_ACTION_ADD;
    switch (r->kind) {
    case REL_Q:
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
            g->referent = GOFF_REFERENT_CLASS;
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
    uint8_t *copy, *zdata;
    size_t zlen = 0;
    uint32_t zesd = 0;

    for (si = 0; si < m->sections.n; si++)
        if (m->sections.v[si]->kind != K_DSECT) vec_push(&secs, m->sections.v[si]);
    if (secs.n == 0) {
        snprintf(err, errsz, "no control section to emit");
        return NULL;
    }

    /* external references: one ER per symbol, below the first section using it */
#define NOTE(SYM, SECT)                                           \
    do {                                                          \
        Symbol *sym_ = (SYM);                                     \
        int seen_ = 0;                                            \
        size_t q_;                                                \
        if (sym_ && sym_->kind == SYM_EXT) {                      \
            for (q_ = 0; q_ < er_order.n; q_++)                   \
                if (er_order.v[q_] == sym_) seen_ = 1;            \
            if (!seen_) {                                         \
                vec_push(&er_order, sym_);                        \
                vec_push(&er_parent, (SECT));                     \
            }                                                     \
        }                                                         \
    } while (0)
    for (si = 0; si < secs.n; si++)
        for (ci = 0; ci < secs.v[si]->classes.n; ci++) {
            Class *c = secs.v[si]->classes.v[ci];
            SortKey *keys = sorted_relocs(c);
            for (k = 0; k < c->relocs.n; k++) NOTE(c->relocs.v[keys[k].idx].target, secs.v[si]);
            free(keys);
        }
    if (m->entry) {
        Symbol *e = module_symbol(m, m->entry);
        if (e && e->kind == SYM_EXT) NOTE(e, secs.v[0]);
    }
#undef NOTE

    for (si = 0; si < secs.n; si++) {
        Section *s = secs.v[si];
        GoffESD sd;
        memset(&sd, 0, sizeof sd);
        sd.type = GOFF_SD;
        set_name(&sd, sect_name(s));
        sd.attr.common = s->kind == K_COM;
        TRY(goff_write_esd(w, &sd));
        s->esd = sd.id;
        for (ci = 0; ci < s->classes.n; ci++) TRY(emit_class(w, m, s, s->classes.v[ci]));
        if (s->prv) TRY(emit_prv(w, m, s));

        /* label definitions: the section name and ENTRY symbols */
        if (s->name[0]) {
            GoffESD ld;
            Symbol *sym;
            init_ld(&ld, s, s->name, s->classes.v[0]);
            apply_xattr(&ld.attr, xattr_of(m, s->name));
            TRY(goff_write_esd(w, &ld));
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
                init_ld(&ld, s, sym->name, sym->piece->cls);
                ld.offset = (uint32_t)ek[i].off;
                apply_xattr(&ld.attr, sym->xattr);
                TRY(goff_write_esd(w, &ld));
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
            er.attr.amode = s->amode;
            er.attr.scope = GOFF_SCOPE_SECTION;
            if (sym->weak) {
                er.attr.bind_strength = GOFF_STRENGTH_WEAK;
                er.attr.scope = GOFF_SCOPE_MODULE;
            }
            apply_xattr(&er.attr, sym->xattr);
            TRY(goff_write_esd(w, &er));
            sym->esd = er.id;
        }
    }

    /* ZSYM: the symbolic information, a class without load text (ZSYM.md) */
    zdata = zsym_build(m, &zlen);
    if (zdata) {
        GoffESD ed;
        memset(&ed, 0, sizeof ed);
        ed.type = GOFF_ED;
        ed.parent = secs.v[0]->esd;
        set_name(&ed, ZSYM_CLASS);
        ed.name_space = GOFF_NS_NORMAL;
        ed.length = (uint32_t)zlen;
        ed.attr.amode = secs.v[0]->amode;
        ed.attr.rmode = secs.v[0]->rmode;
        ed.attr.text_style = GOFF_STYLE_UNSTRUCTURED;
        ed.attr.bind_algorithm = GOFF_BIND_CAT;
        ed.attr.loading = GOFF_NOLOAD;
        TRY(goff_write_esd(w, &ed));
        zesd = ed.id;
    }

    /* text */
    for (si = 0; si < secs.n; si++)
        for (ci = 0; ci < secs.v[si]->classes.n; ci++) {
            Class *c = secs.v[si]->classes.v[ci];
            if (c->len == 0) continue;
            TRY(goff_write_text(w, class_target(c), 0, GOFF_STYLE_BYTE, c->data, (size_t)c->len));
        }
    if (zdata) TRY(goff_write_text(w, zesd, 0, GOFF_STYLE_UNSTRUCTURED, zdata, zlen));

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
            end.offset = (uint32_t)off;
            end.amode = c->sect->amode;
        }
    }
    TRY(goff_write_end(w, &end, &out, len));
    copy = xmalloc(*len + 1);
    memcpy(copy, out, *len);
    return copy;
}
