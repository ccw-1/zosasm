/* zsym.c - ZSYM, the symbolic information stored in the object (ZSYM.md). */
#include <stdlib.h>
#include <string.h>

#include "ebcdic.h"
#include "isa.h"
#include "zio.h"
#include "zsym.h"

#define BATCH 256 /* symbols or statements per chunk */

/* ---- CRC-32 (IEEE, reflected) -------------------------------------------- */

static uint32_t crc_table[256];
static int crc_ready;

static uint32_t crc_update(uint32_t crc, uint8_t byte) {
    if (!crc_ready) {
        uint32_t i, k;
        for (i = 0; i < 256; i++) {
            uint32_t c = i;
            for (k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = 1;
    }
    return crc_table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
}

uint32_t zsym_crc(const uint8_t *data, size_t datalen, const uint8_t *mask, int64_t off, int64_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    int64_t i;
    if (off < 0 || len < 0 || (uint64_t)off > datalen || (uint64_t)len > datalen - (uint64_t)off) return 0;
    for (i = 0; i < len; i++) {
        size_t at = (size_t)(off + i);
        crc = crc_update(crc, mask && mask[at] ? 0 : data[at]);
    }
    return ~crc;
}

/* ---- quoting and rendering ------------------------------------------------ */

char *zsym_quote(const char *name) {
    size_t n, len = strlen(name);
    unsigned char *e = ebcdic_from_source(name, len, &n);
    char *s = sym_source(e, n);
    free(e);
    return s ? s : xstrdup(name);
}

char *zsym_render(const ZBlock *b, const ZTmpl *t) {
    Buf out = {0};
    size_t i;
    for (i = 0; i < t->n; i++) {
        const ZItem *it = &t->v[i];
        const char *name = it->tag != 't' && it->id < b->syms.n ? b->syms.v[it->id].name : "?";
        if (it->tag == 't') {
            buf_puts(&out, it->text);
        } else {
            char *q = zsym_quote(name);
            if (it->tag == 'a') {
                buf_putc(&out, it->attr);
                buf_putc(&out, '\'');
            }
            buf_puts(&out, q);
        }
    }
    return xstrdup(buf_cstr(&out));
}

/* ---- encoding -------------------------------------------------------------- */

void zsym_put_chunk(Buf *out, int kind, Buf *payload) {
    size_t start = out->len;
    buf_putc(out, 'Z');
    buf_putc(out, 'S');
    buf_putc(out, ZSYM_VERSION);
    buf_putc(out, kind);
    put_u32(out, (uint32_t)payload->len);
    buf_write(out, payload->p, payload->len);
    put_u32(out, zsym_crc((const uint8_t *)out->p, out->len, NULL, (int64_t)start, (int64_t)(out->len - start)));
}

/* ---- templates ------------------------------------------------------------- */

static void item_text(ZTmpl *t, Buf *text) {
    ZItem it;
    if (text->len == 0) return;
    memset(&it, 0, sizeof it);
    it.tag = 't';
    it.text = xstrndup(text->p, text->len);
    vec_push(t, it);
    text->len = 0;
}

static void item_sym(ZTmpl *t, Buf *text, char tag, char attr, const Symbol *s) {
    ZItem it;
    item_text(t, text);
    memset(&it, 0, sizeof it);
    it.tag = tag;
    it.attr = attr;
    it.id = (uint32_t)s->zid;
    vec_push(t, it);
}

/* the symbol written at s[i...]: plain or quoted; *end after it */
static const Symbol *sym_at(const Module *m, const char *s, size_t i, size_t *end) {
    if (s[i] == '"') {
        long j = close_dquote(s, i);
        const char *err = NULL;
        char *name;
        if (j < 0) return NULL;
        name = sym_decode(s + i + 1, (size_t)j - i - 1, &err);
        if (!name) return NULL;
        *end = (size_t)j + 1;
        return module_symbol(m, name);
    } else if (is_sym_start((unsigned char)s[i])) {
        size_t j = i;
        char *w;
        while (is_sym_char((unsigned char)s[j])) j++;
        w = xstrndup(s + i, j - i);
        *end = j;
        return module_symbol(m, w);
    }
    return NULL;
}

/* Split operand text into text, symbol and attribute items. A symbol is an
 * item only when it is defined in the module. */
static void tmpl_build(const Module *m, const char *s, ZTmpl *t) {
    Buf text = {0};
    size_t i = 0;
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        size_t e;
        const Symbol *sym;
        if (c == '\'') {
            long j = close_quote(s, i);
            e = j < 0 ? strlen(s) : (size_t)j + 1;
            buf_write(&text, s + i, e - i);
            i = e;
        } else if (c == '"') {
            sym = sym_at(m, s, i, &e);
            if (sym) {
                item_sym(t, &text, 's', 0, sym);
                i = e;
            } else {
                long j = close_dquote(s, i);
                e = j < 0 ? strlen(s) : (size_t)j + 1;
                buf_write(&text, s + i, e - i);
                i = e;
            }
        } else if (is_sym_start(c)) {
            size_t j = i;
            while (is_sym_char((unsigned char)s[j])) j++;
            if (j - i == 1 && s[j] == '\'' && is_attr_quote(s, j)) {
                size_t k = j + 1;
                sym = sym_at(m, s, k, &e);
                if (sym) {
                    item_sym(t, &text, 'a', (char)c, sym);
                    i = e;
                } else {
                    buf_write(&text, s + i, j + 1 - i);
                    i = j + 1;
                }
            } else if (s[j] == '\'') {
                buf_write(&text, s + i, j - i);
                i = j;
            } else {
                char *w = xstrndup(s + i, j - i);
                sym = module_symbol(m, w);
                if (sym)
                    item_sym(t, &text, 's', 0, sym);
                else
                    buf_write(&text, s + i, j - i);
                i = j;
            }
        } else if (za_isdigit(c) || c == '%') {
            size_t j = i + 1;
            while (is_sym_char((unsigned char)s[j])) j++;
            buf_write(&text, s + i, j - i);
            i = j;
        } else {
            buf_putc(&text, c);
            i++;
        }
    }
    item_text(t, &text);
}

static void put_tmpl(Buf *b, const ZTmpl *t) {
    size_t i;
    put_uv(b, t->n);
    for (i = 0; i < t->n; i++) {
        const ZItem *it = &t->v[i];
        buf_putc(b, it->tag);
        if (it->tag == 't') {
            put_str(b, it->text);
        } else {
            if (it->tag == 'a') buf_putc(b, it->attr);
            put_uv(b, it->id);
        }
    }
}

/* ---- statement kinds ---------------------------------------------------------- */

static int stmt_kind(const char *op) {
    static const struct {
        const char *name;
        int kind;
    } tab[] = {{"DC", ZS_DC},       {"DS", ZS_DS},        {"EQU", ZS_EQU},       {"USING", ZS_USING},
               {"DROP", ZS_DROP},   {"ORG", ZS_ORG},      {"LTORG", ZS_LTORG},   {"CSECT", ZS_SECTION},
               {"RSECT", ZS_SECTION}, {"START", ZS_SECTION}, {"COM", ZS_SECTION}, {"DSECT", ZS_SECTION},
               {"LOCTR", ZS_SECTION}, {"CATTR", ZS_SECTION}, {"EXTRN", ZS_EXTERN}, {"WXTRN", ZS_EXTERN},
               {"ENTRY", ZS_EXTERN}, {"XATTR", ZS_EXTERN}, {"AMODE", ZS_EXTERN}, {"RMODE", ZS_EXTERN},
               {"DXD", ZS_DXD},     {"CXD", ZS_DXD},      {"END", ZS_END}};
    char *u = za_strupper(op);
    size_t i, n = 0;
    int kind = ZS_OTHER;
    for (i = 0; i < sizeof tab / sizeof *tab; i++)
        if (!strcmp(u, tab[i].name)) kind = tab[i].kind;
    if (kind == ZS_OTHER && isa_lookup(u, &n) && n > 0) kind = ZS_INSN;
    free(u);
    return kind;
}

/* ---- the writer ---------------------------------------------------------------- */

static void put_sym(Buf *b, const Symbol *s) {
    static const char kinds[] = {ZY_LABEL, ZY_ABS, ZY_REL, ZY_EXT, ZY_SECT, ZY_DXD};
    int64_t scope = -1, val = 0;
    size_t i;
    int flags = (s->weak ? ZYF_WEAK : 0) | (s->entry ? ZYF_ENTRY : 0);
    int64_t scale = s->scale;
    put_str(b, s->name);
    buf_putc(b, kinds[s->kind]);
    buf_putc(b, flags);
    switch (s->kind) {
    case SYM_LABEL:
        if (s->piece) {
            scope = s->piece->cls->zid;
            val = s->piece->start + s->off;
        }
        break;
    case SYM_ABS:
    case SYM_REL:
        val = s->val.abs;
        break;
    case SYM_SECT:
        if (s->sect && s->sect->classes.n) {
            scope = s->sect->classes.v[0]->zid;
            val = s->sect->classes.v[0]->len;
        }
        break;
    case SYM_DXD:
        val = s->xd_len;
        scale = s->xd_align;
        break;
    default:
        break;
    }
    put_uv(b, (uint64_t)(scope + 1));
    put_sv(b, val);
    put_sv(b, s->len);
    buf_putc(b, s->type);
    put_sv(b, scale);
    if (s->kind != SYM_REL) {
        put_uv(b, 0);
        return;
    }
    put_uv(b, (uint64_t)s->val.nterms);
    for (i = 0; i < (size_t)s->val.nterms; i++) {
        const Term *t = &s->val.terms[i];
        int tkind = 2;
        uint64_t id = 0;
        if (t->base.kind == B_CLASS) {
            tkind = 1;
            id = (uint64_t)((Class *)t->base.p)->zid;
        } else if (t->base.kind == B_PIECE) {
            tkind = 1;
            id = (uint64_t)((Piece *)t->base.p)->cls->zid;
        } else if (t->base.kind == B_SYM) {
            id = (uint64_t)((Symbol *)t->base.p)->zid;
        }
        put_sv(b, t->coef);
        buf_putc(b, tkind);
        put_uv(b, id);
    }
}

/* 1 for the bytes of c covered by a relocation */
static uint8_t *reloc_mask(const Class *c) {
    uint8_t *mask = xcalloc((size_t)c->len + 1);
    size_t i;
    int64_t k;
    for (i = 0; i < c->relocs.n; i++) {
        const Reloc *r = &c->relocs.v[i];
        for (k = 0; k < r->len; k++)
            if (r->off + k >= 0 && r->off + k < c->len) mask[r->off + k] = 1;
    }
    return mask;
}

uint8_t *zsym_build(Module *m, size_t *len) {
    Buf out = {0}, p = {0};
    StrMap files = {0};
    Vec(const char *) filev = {0};
    Vec(Class *) scopes = {0};
    uint8_t **masks;
    size_t si, ci, i, from;
    uint8_t *res;

    *len = 0;
    if (m->zlevel < 1) return NULL;

    for (si = 0; si < m->sections.n; si++) {
        Section *s = m->sections.v[si];
        for (ci = 0; ci < s->classes.n; ci++) {
            s->classes.v[ci]->zid = (int)scopes.n;
            vec_push(&scopes, s->classes.v[ci]);
        }
    }
    for (i = 0; i < m->sym_order.n; i++) m->sym_order.v[i]->zid = (int)i;
    for (i = 0; i < m->zrecs.n; i++) {
        const char *f = m->zrecs.v[i].st->file;
        if (!f) f = "";
        if (!strmap_get(&files, f)) {
            vec_push(&filev, f);
            strmap_put(&files, f, (void *)(uintptr_t)filev.n);
        }
    }
    masks = xcalloc((scopes.n + 1) * sizeof *masks);

    /* MODULE */
    put_str(&p, "zosasm 1");
    buf_putc(&p, m->zlevel);
    put_uv(&p, filev.n);
    for (i = 0; i < filev.n; i++) put_str(&p, filev.v[i]);
    put_uv(&p, scopes.n);
    for (i = 0; i < scopes.n; i++) {
        put_str(&p, scopes.v[i]->sect->name);
        put_str(&p, scopes.v[i]->name);
        buf_putc(&p, scopes.v[i]->sect->kind);
    }
    put_uv(&p, m->sym_order.n);
    put_uv(&p, m->zrecs.n);
    zsym_put_chunk(&out, ZK_MODULE, &p);

    /* SYMS */
    for (from = 0; from < m->sym_order.n; from += BATCH) {
        size_t to = from + BATCH < m->sym_order.n ? from + BATCH : m->sym_order.n;
        p.len = 0;
        put_uv(&p, to - from);
        for (i = from; i < to; i++) put_sym(&p, m->sym_order.v[i]);
        zsym_put_chunk(&out, ZK_SYMS, &p);
    }

    /* STMTS */
    for (from = 0; from < m->zrecs.n; from += BATCH) {
        size_t to = from + BATCH < m->zrecs.n ? from + BATCH : m->zrecs.n;
        p.len = 0;
        put_uv(&p, to - from);
        for (i = from; i < to; i++) {
            const ZRec *r = &m->zrecs.v[i];
            const Stmt *st = r->st;
            ZTmpl label = {0}, ops = {0};
            int64_t off = r->off, slen = r->len;
            uint32_t crc = 0;
            int kind = stmt_kind(st->op ? st->op : "");
            int64_t scope = -1;
            if (r->cls) {
                scope = r->cls->zid;
                if (off < 0 || off > r->cls->len) off = r->cls->len;
                if (slen < 0) slen = 0;
                if (slen > r->cls->len - off) slen = r->cls->len - off;
                if (r->cls->sect->kind != K_DSECT && slen > 0 && r->cls->data) {
                    if (!masks[scope]) masks[scope] = reloc_mask(r->cls);
                    crc = zsym_crc(r->cls->data, (size_t)r->cls->len, masks[scope], off, slen);
                }
            } else {
                off = slen = 0;
            }
            if (st->name && st->name[0]) {
                const Symbol *s = module_symbol(m, st->name);
                if (s) {
                    Buf none = {0};
                    item_sym(&label, &none, 's', 0, s);
                } else {
                    ZItem it;
                    memset(&it, 0, sizeof it);
                    it.tag = 't';
                    it.text = xstrdup(st->name);
                    vec_push(&label, it);
                }
            }
            tmpl_build(m, st->operands ? st->operands : "", &ops);
            buf_putc(&p, kind);
            put_uv(&p, (uint64_t)(uintptr_t)strmap_get(&files, st->file ? st->file : "") - 1);
            put_uv(&p, (uint64_t)st->line);
            put_uv(&p, (uint64_t)(scope + 1));
            put_sv(&p, off);
            put_uv(&p, (uint64_t)slen);
            put_u32(&p, crc);
            put_tmpl(&p, &label);
            put_str(&p, st->op ? st->op : "");
            put_tmpl(&p, &ops);
        }
        zsym_put_chunk(&out, ZK_STMTS, &p);
    }

    /* END */
    p.len = 0;
    put_uv(&p, m->sym_order.n);
    put_uv(&p, m->zrecs.n);
    zsym_put_chunk(&out, ZK_END, &p);

    res = xmalloc(out.len + 1);
    memcpy(res, out.p, out.len);
    *len = out.len;
    return res;
}

/* ---- the reader ---------------------------------------------------------------- */

/* an element count: every element takes at least one byte */
static size_t get_count(Rd *r) {
    uint64_t n = get_uv(r);
    if (r->bad || n > (uint64_t)(r->end - r->p)) {
        r->bad = 1;
        return 0;
    }
    return (size_t)n;
}

static void get_tmpl(Rd *r, ZTmpl *t) {
    size_t n = get_count(r), i;
    for (i = 0; i < n && !r->bad; i++) {
        ZItem it;
        memset(&it, 0, sizeof it);
        it.tag = (char)get_u8(r);
        if (it.tag == 't') {
            it.text = get_str(r);
        } else if (it.tag == 's' || it.tag == 'a') {
            if (it.tag == 'a') it.attr = (char)get_u8(r);
            it.id = (uint32_t)get_uv(r);
        } else {
            r->bad = 1;
        }
        if (!r->bad) vec_push(t, it);
    }
}

static void parse_module(Rd *r, ZBlock *b) {
    size_t n, i;
    b->producer = get_str(r);
    b->level = get_u8(r);
    n = get_count(r);
    for (i = 0; i < n && !r->bad; i++) {
        char *f = get_str(r);
        vec_push(&b->files, f);
    }
    n = get_count(r);
    for (i = 0; i < n && !r->bad; i++) {
        ZScope sc;
        sc.sect = get_str(r);
        sc.cls = get_str(r);
        sc.kind = get_u8(r);
        if (!r->bad) vec_push(&b->scopes, sc);
    }
    b->want_syms = get_uv(r);
    b->want_stmts = get_uv(r);
}

static void parse_syms(Rd *r, ZBlock *b) {
    size_t n = get_count(r), i;
    for (i = 0; i < n && !r->bad; i++) {
        ZSym s;
        size_t nt, k;
        memset(&s, 0, sizeof s);
        s.name = get_str(r);
        s.kind = get_u8(r);
        s.flags = get_u8(r);
        s.scope = (int64_t)get_uv(r) - 1;
        s.val = get_sv(r);
        s.len = get_sv(r);
        s.type = (char)get_u8(r);
        s.scale = get_sv(r);
        nt = get_count(r);
        for (k = 0; k < nt && !r->bad; k++) {
            ZTerm t;
            t.coef = get_sv(r);
            t.tkind = get_u8(r);
            t.id = (uint32_t)get_uv(r);
            if (!r->bad) vec_push(&s.terms, t);
        }
        if (!r->bad) vec_push(&b->syms, s);
    }
}

static void parse_stmts(Rd *r, ZBlock *b) {
    size_t n = get_count(r), i;
    for (i = 0; i < n && !r->bad; i++) {
        ZStmt s;
        memset(&s, 0, sizeof s);
        s.kind = get_u8(r);
        s.file = (uint32_t)get_uv(r);
        s.line = (uint32_t)get_uv(r);
        s.scope = (int64_t)get_uv(r) - 1;
        s.off = get_sv(r);
        s.len = (int64_t)get_uv(r);
        s.crc = get_u32(r);
        get_tmpl(r, &s.label);
        s.op = get_str(r);
        get_tmpl(r, &s.operands);
        if (!r->bad) vec_push(&b->stmts, s);
    }
}

Zsym *zsym_parse(const uint8_t *b, size_t n) {
    Zsym *z = xcalloc(sizeof *z);
    ZBlock *cur = NULL;
    size_t i = 0;
    int in_junk = 0;
    while (i < n) {
        Rd r;
        int ver, kind;
        size_t used = chunk_at(b, n, i, &ver, &kind, &r);
        if (used) {
            in_junk = 0;
            if (ver != ZSYM_VERSION) {
                z->bad_chunks++;
                if (cur) cur->damaged = 1;
            } else if (kind == ZK_MODULE) {
                cur = xcalloc(sizeof *cur);
                vec_push(&z->blocks, cur);
                parse_module(&r, cur);
                if (r.bad) cur->damaged = 1;
            } else if (kind == ZK_SYMS || kind == ZK_STMTS) {
                if (!cur) {
                    z->bad_chunks++;
                } else {
                    if (kind == ZK_SYMS)
                        parse_syms(&r, cur);
                    else
                        parse_stmts(&r, cur);
                    if (r.bad) cur->damaged = 1;
                }
            } else if (kind != ZK_END) {
                /* a checksum-valid chunk of a kind this version does not know */
                z->bad_chunks++;
                if (cur) cur->damaged = 1;
            } else if (kind == ZK_END) {
                if (!cur) {
                    z->bad_chunks++;
                } else {
                    uint64_t ns = get_uv(&r), nst = get_uv(&r);
                    cur->complete = !r.bad && !cur->damaged && ns == cur->syms.n && nst == cur->stmts.n &&
                                    ns == cur->want_syms && nst == cur->want_stmts;
                    if (r.bad) cur->damaged = 1;
                    cur = NULL;
                }
            }
            i += used;
        } else {
            if (b[i] != 0 && !in_junk) {
                z->bad_chunks++;
                if (cur) cur->damaged = 1;
                in_junk = 1;
            }
            i++;
        }
    }
    return z;
}

uint8_t *zsym_extract_class(const GoffModule *m, const char *cls, size_t *len) {
    size_t i, total = 0;
    uint8_t *res;
    int found = 0;
    *len = 0;
    for (i = 0; i < m->esds.n; i++) {
        const GoffESD *e = m->esds.v[i];
        char *nm;
        size_t t;
        if (e->type != GOFF_ED) continue;
        nm = ebcdic_decode(e->name, e->name_len);
        if (strcmp(nm, cls) != 0) {
            free(nm);
            continue;
        }
        free(nm);
        found = 1;
        for (t = 0; t < m->texts.n; t++) {
            const GoffText *x = &m->texts.v[t];
            uint64_t end = (uint64_t)x->offset + x->len;
            if (x->esdid == (uint32_t)i + 1 && end > total && end < ((uint64_t)1 << 30)) total = (size_t)end;
        }
        res = xcalloc(total + 1);
        for (t = 0; t < m->texts.n; t++) {
            const GoffText *x = &m->texts.v[t];
            if (x->esdid == (uint32_t)i + 1 && (uint64_t)x->offset + x->len <= total && x->len)
                memcpy(res + x->offset, x->data, x->len);
        }
        *len = total;
        return res;
    }
    (void)found;
    return NULL;
}

uint8_t *zsym_extract(const GoffModule *m, size_t *len) { return zsym_extract_class(m, ZSYM_CLASS, len); }

static const char *const stmt_names[] = {"other", "insn", "dc", "ds", "equ", "using", "drop",
                                         "org",   "ltorg", "section", "extern", "dxd", "end"};
static const char *const sym_names[] = {"label", "abs", "rel", "ext", "sect", "dxd"};

static void dump_tmpl(const ZBlock *b, const ZTmpl *t, FILE *f) {
    char *s = zsym_render(b, t);
    fputs(s, f);
}

static void tmpl_free(ZTmpl *t) {
    size_t i;
    for (i = 0; i < t->n; i++) free(t->v[i].text);
    free(t->v);
}

void zsym_free(Zsym *z) {
    size_t i, j;
    if (!z) return;
    for (i = 0; i < z->blocks.n; i++) {
        ZBlock *b = z->blocks.v[i];
        free(b->producer);
        for (j = 0; j < b->files.n; j++) free(b->files.v[j]);
        free(b->files.v);
        for (j = 0; j < b->scopes.n; j++) {
            free(b->scopes.v[j].sect);
            free(b->scopes.v[j].cls);
        }
        free(b->scopes.v);
        for (j = 0; j < b->syms.n; j++) {
            free(b->syms.v[j].name);
            free(b->syms.v[j].terms.v);
        }
        free(b->syms.v);
        for (j = 0; j < b->stmts.n; j++) {
            ZStmt *st = &b->stmts.v[j];
            tmpl_free(&st->label);
            free(st->op);
            tmpl_free(&st->operands);
        }
        free(b->stmts.v);
        free(b);
    }
    free(z->blocks.v);
    free(z);
}

void zsym_dump(const Zsym *z, FILE *f) {
    size_t bi, i, k;
    fprintf(f, "ZSYM: %zu block%s", z->blocks.n, z->blocks.n == 1 ? "" : "s");
    if (z->bad_chunks) fprintf(f, ", %d damaged or unknown chunk%s skipped", z->bad_chunks, z->bad_chunks == 1 ? "" : "s");
    fputc('\n', f);
    for (bi = 0; bi < z->blocks.n; bi++) {
        const ZBlock *b = z->blocks.v[bi];
        fprintf(f, "block %zu: %s, level %d, %s\n", bi + 1, b->producer ? b->producer : "?", b->level,
                b->complete ? "complete" : b->damaged ? "DAMAGED" : "INCOMPLETE");
        for (i = 0; i < b->files.n; i++) fprintf(f, "  file %zu: %s\n", i, b->files.v[i]);
        for (i = 0; i < b->scopes.n; i++)
            fprintf(f, "  scope %zu: %s %s %s\n", i,
                    b->scopes.v[i].kind >= K_CSECT && b->scopes.v[i].kind <= K_COM
                        ? sectkind_name((SectKind)b->scopes.v[i].kind)
                        : "?",
                    b->scopes.v[i].sect, b->scopes.v[i].cls);
        for (i = 0; i < b->syms.n; i++) {
            const ZSym *s = &b->syms.v[i];
            char *q = zsym_quote(s->name);
            fprintf(f, "  sym %zu: %s %s", i, q, s->kind >= 0 && s->kind <= ZY_DXD ? sym_names[s->kind] : "?");
            if (s->flags & ZYF_WEAK) fputs(" weak", f);
            if (s->flags & ZYF_ENTRY) fputs(" entry", f);
            if (s->scope >= 0) fprintf(f, " scope %lld", (long long)s->scope);
            fprintf(f, " val %lld len %lld", (long long)s->val, (long long)s->len);
            if (s->type) fprintf(f, " type %c", s->type);
            if (s->scale) fprintf(f, " scale %lld", (long long)s->scale);
            for (k = 0; k < s->terms.n; k++)
                fprintf(f, " %+lld*%s%u", (long long)s->terms.v[k].coef, s->terms.v[k].tkind == 1 ? "scope" : "sym",
                        (unsigned)s->terms.v[k].id);
            fputc('\n', f);
        }
        for (i = 0; i < b->stmts.n; i++) {
            const ZStmt *s = &b->stmts.v[i];
            fprintf(f, "  stmt %zu: %s %u:%u", i, s->kind >= 0 && s->kind <= ZS_END ? stmt_names[s->kind] : "?", s->file,
                    s->line);
            if (s->scope >= 0) fprintf(f, " scope %lld +%lld len %lld crc %08X", (long long)s->scope, (long long)s->off,
                                       (long long)s->len, (unsigned)s->crc);
            fputs(" | ", f);
            dump_tmpl(b, &s->label, f);
            fprintf(f, " %s ", s->op);
            dump_tmpl(b, &s->operands, f);
            fputc('\n', f);
        }
    }
}
