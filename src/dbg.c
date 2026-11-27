/* dbg.c - the debug information of -g: gathering from the module (paths,
 * sections, labels, source rows) and the GOFF module that links the object
 * to its .dbg file.  The DWARF is made in dbg_dwarf.c, the ELF file in
 * dbg_elf.c.  The layout follows z/OS /bin/as -g (probes in DESIGN.md 21). */
#include <stdlib.h>
#include <string.h>

#include "dbg_int.h"
#include "ebcdic.h"
#include "goff.h"
#include "md5.h"

/* ---- paths ------------------------------------------------------------- */

/* an absolute path without ".", ".." and repeated slashes */
static char *normalize(const char *abs) {
    char *out = xmalloc(strlen(abs) + 2);
    size_t len = 0;
    const char *p = abs;
    while (*p) {
        const char *q;
        size_t l;
        while (*p == '/') p++;
        if (!*p) break;
        q = p;
        while (*q && *q != '/') q++;
        l = (size_t)(q - p);
        if (l == 1 && p[0] == '.') {
            /* nothing */
        } else if (l == 2 && p[0] == '.' && p[1] == '.') {
            while (len > 0 && out[len - 1] != '/') len--;
            if (len > 0) len--;
        } else {
            out[len++] = '/';
            memcpy(out + len, p, l);
            len += l;
        }
        p = q;
    }
    if (len == 0) out[len++] = '/';
    out[len] = 0;
    return out;
}

/* what follows the directory from at the start of p (a path in normal form), or NULL */
static const char *dir_rest(const char *p, const char *from) {
    size_t fl = strlen(from);
    while (fl > 1 && from[fl - 1] == '/') fl--;
    if (fl == 1) return p; /* the root */
    if (strncmp(p, from, fl) != 0 || (p[fl] && p[fl] != '/')) return NULL;
    return p + fl;
}

static char *absolute(const DbgOptions *o, const char *path) {
    char *a = path[0] == '/' ? xstrdup(path) : xasprintf("%s/%s", o->cwd ? o->cwd : "", path);
    char *n = normalize(a);
    free(a);
    return n;
}

char *dbg_host_path(const DbgOptions *o, const char *path) {
    char *norm = absolute(o, path);
    const char *rest;
    size_t i;
    for (i = 0; i < o->map_from.n; i++) {
        char *from = absolute(o, o->map_from.v[i]);
        rest = dir_rest(norm, from);
        if (rest) {
            char *r = normalize(xasprintf("%s/%s", o->map_to.v[i], rest));
            return r;
        }
    }
    if (o->gdir && o->cwd) {
        char *cwd = absolute(o, o->cwd);
        rest = dir_rest(norm, cwd);
        if (rest) return normalize(xasprintf("%s/%s", o->gdir, rest));
    }
    return norm;
}

/* ---- gathering ----------------------------------------------------------- */

static int file_index(Dbg *g, const char *name) {
    DFile f;
    char *slash;
    size_t i;
    void *v;
    if (!name || !*name) name = g->o->main_file;
    v = strmap_get(&g->filemap, name);
    if (v) return (int)(uintptr_t)v - 1;
    memset(&f, 0, sizeof f);
    f.name = xstrdup(name);
    f.abs = dbg_host_path(g->o, name);
    slash = strrchr(f.abs, '/');
    f.base = xstrdup(slash + 1);
    f.dir = slash == f.abs ? xstrdup("/") : xstrndup(f.abs, (size_t)(slash - f.abs));
    for (i = 0; i < g->dirs.n; i++)
        if (!strcmp(g->dirs.v[i].path, f.dir)) break;
    if (i == g->dirs.n) {
        DDir d;
        d.path = f.dir;
        d.off = 0;
        vec_push(&g->dirs, d);
    }
    f.dir_idx = (int)i;
    vec_push(&g->files, f);
    strmap_put(&g->filemap, name, (void *)(uintptr_t)g->files.n);
    return (int)g->files.n - 1;
}

static int type_add(Dbg *g, int enc, int size, int64_t count) {
    size_t i;
    int base = -1;
    DType t;
    for (i = 0; i < g->types.n; i++)
        if (g->types.v[i].enc == enc && g->types.v[i].size == size && g->types.v[i].count == count) return (int)i;
    if (count != 1) base = type_add(g, enc, size, 1);
    t.enc = enc;
    t.size = size;
    t.count = count;
    t.off = 0;
    t.base = base;
    vec_push(&g->types, t);
    return (int)g->types.n - 1;
}

/* the DWARF type of the data a label names (from its type and length attributes) */
static int label_type(Dbg *g, const Symbol *s) {
    int64_t len = s->len, count = s->count;
    int enc = 8, size = 1;
    char t = s->type >= 'a' && s->type <= 'z' ? (char)(s->type - 32) : s->type;
    if (count < 1) count = 1;
    switch (t) {
    case 'F':
    case 'H':
        if (len == 1 || len == 2 || len == 4 || len == 8) enc = 5, size = (int)len;
        break;
    case 'A':
    case 'Y':
    case 'S':
    case 'V':
    case 'Q':
    case 'J':
    case 'R':
        if (len == 2 || len == 4 || len == 8) enc = 7, size = (int)len;
        break;
    case 'D':
    case 'E':
    case 'L':
        if (len == 4 || len == 8 || len == 16) enc = 0xdf, size = (int)len; /* IBM hexadecimal floating point */
        break;
    default:
        break;
    }
    if (size == 1) { /* bytes: one array of len * count */
        count *= len > 0 ? len : 1;
    }
    return type_add(g, enc, size, count);
}

static int is_machine(const char *op) {
    size_t n = 0;
    char *up = za_strupper(op);
    if (asm_is_directive(up)) return 0;
    isa_lookup(op, &n);
    return n > 0;
}

static int cmp_lab(const void *x, const void *y) {
    const DLab *a = x, *b = y;
    if (a->off != b->off) return a->off < b->off ? -1 : 1;
    return strcmp(a->name, b->name);
}

static int cmp_row(const void *x, const void *y) {
    const DRow *a = x, *b = y;
    if (a->addr != b->addr) return a->addr < b->addr ? -1 : 1;
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}

static DStruct *struct_of(Dbg *g, const Section *s) {
    size_t i;
    for (i = 0; i < g->structs.n; i++)
        if (g->structs.v[i]->sect == s) return g->structs.v[i];
    return NULL;
}

static DSect *dsect_of(Dbg *g, const Class *c) {
    size_t i;
    for (i = 0; i < g->sects.n; i++)
        if (g->sects.v[i]->cls == c) return g->sects.v[i];
    return NULL;
}

static void collect(Dbg *g) {
    const Module *m = g->m;
    size_t i, k;
    int64_t text_end = 0;
    char *cdir;
    file_index(g, g->o->main_file); /* the main file is file 1 */
    for (i = 0; i < m->sections.n; i++) {
        Section *s = m->sections.v[i];
        DSect *d;
        if ((s->kind != K_CSECT && s->kind != K_RSECT) || !s->name[0] || s->classes.n == 0) continue;
        d = xcalloc(sizeof *d);
        d->sect = s;
        d->cls = s->classes.v[0];
        d->id = d->cls->esd;
        d->len = d->cls->len;
        if (g->elf) {
            d->base = 0; /* ELF: the class is its own section, so its addresses start at 0 */
        } else {
            int al = d->cls->attr.align >= 0 ? d->cls->attr.align : 3; /* doubleword unless CATTR says otherwise */
            int64_t a = (int64_t)1 << al;
            text_end = (text_end + a - 1) / a * a;
            d->base = text_end;
            text_end += d->len;
        }
        vec_push(&g->sects, d);
    }
    /* DSECTs: a structure type each (IBM writes nothing for them) */
    for (i = 0; i < m->sections.n; i++) {
        Section *s = m->sections.v[i];
        DStruct *st;
        if (s->kind != K_DSECT || !s->name[0] || s->classes.n == 0 || s->classes.v[0]->len <= 0) continue;
        st = xcalloc(sizeof *st);
        st->sect = s;
        st->len = s->classes.v[0]->len;
        vec_push(&g->structs, st);
    }
    /* code rows */
    for (k = 0; k < m->zrecs.n; k++) {
        const ZRec *z = &m->zrecs.v[k];
        DSect *d;
        DRow r;
        int cnop;
        if (!z->cls || !z->st || !z->st->op || z->len <= 0) continue;
        d = dsect_of(g, z->cls);
        if (!d) continue;
        cnop = za_streqi(z->st->op, "CNOP");
        if (!cnop && !is_machine(z->st->op)) continue;
        r.addr = z->off + (cnop ? 0 : z->pad);
        r.file = file_index(g, z->st->file);
        r.line = z->st->line;
        r.seq = (int)k;
        vec_push(&d->rows, r);
    }
    /* CFI directives: a zero-length statement, so its record carries its address */
    for (k = 0; k < m->zrecs.n; k++) {
        const ZRec *z = &m->zrecs.v[k];
        DSect *d;
        DCfi c;
        if (!z->cls || !z->st || !z->st->op || !za_streqi(z->st->op, "DWCFI")) continue;
        d = dsect_of(g, z->cls);
        if (!d || !cfi_operands(z->st->operands, &c.op)) continue;
        c.off = z->off;
        c.seq = (int)k;
        vec_push(&d->cfis, c);
    }
    /* labels and data items */
    for (i = 0; i < m->sym_order.n; i++) {
        const Symbol *s = m->sym_order.v[i];
        const Stmt *def;
        DSect *d;
        DLab l;
        if (s->kind == SYM_ABS && value_is_abs(&s->val) && s->def_idx < m->stmts.n) { /* an EQU constant */
            const Stmt *eq = m->stmts.v[s->def_idx];
            if (eq && eq->op && strcmp(eq->name, s->name) == 0 && za_streqi(eq->op, "EQU")) {
                DLab c;
                c.name = s->name;
                c.off = s->val.abs;
                c.var = 1;
                c.type = type_add(g, 5, 8, 1); /* always 8 bytes: LLDB on a big endian host reads the first half of a 4 byte const_value */
                vec_push(&g->consts, c);
            }
            continue;
        }
        if (s->kind != SYM_LABEL || !s->piece) continue;
        d = dsect_of(g, s->piece->cls);
        def = s->def_idx < m->stmts.n ? m->stmts.v[s->def_idx] : NULL;
        if (!def || strcmp(def->name, s->name) != 0) continue; /* made by the assembler (literals) */
        if (!d) { /* a data item of a DSECT: a member of its structure */
            DStruct *st = s->piece->cls->sect->kind == K_DSECT ? struct_of(g, s->piece->cls->sect) : NULL;
            if (st && s->piece->cls == st->sect->classes.v[0] && def->op &&
                (za_streqi(def->op, "DC") || za_streqi(def->op, "DS")) && s->len > 0 && s->count > 0 &&
                s->off >= 0 && s->off < st->len) {
                l.name = s->name;
                l.off = s->off;
                l.var = 1;
                l.type = label_type(g, s);
                vec_push(&st->mems, l);
            }
            continue;
        }
        l.name = s->name;
        l.off = s->piece->start + s->off;
        l.var = def->op && (za_streqi(def->op, "DC") || za_streqi(def->op, "DS")) && s->len > 0 && s->count > 0;
        l.type = l.var ? label_type(g, s) : -1;
        vec_push(&d->labs, l);
    }
    for (i = 0; i < g->structs.n; i++) {
        DStruct *st = g->structs.v[i];
        if (st->mems.n) qsort(st->mems.v, st->mems.n, sizeof *st->mems.v, cmp_lab);
    }
    /* CBLOCK locals: each block is a lexical range and its locals live at
     * the ctemp base register plus their slot offset. */
    for (i = 0; i < m->cblocks.n; i++) {
        const CBlockDebug *cb = &m->cblocks.v[i];
        Symbol *s0 = module_symbol(m, cb->start_label);
        Symbol *s1 = module_symbol(m, cb->end_label);
        DSect *d;
        int64_t lo, hi;
        size_t j;
        if (!s0 || !s1 || !s0->piece || !s1->piece || s0->piece->cls != s1->piece->cls) continue;
        d = dsect_of(g, s0->piece->cls);
        if (!d) continue;
        lo = s0->piece->start + s0->off;
        hi = s1->piece->start + s1->off;
        if (hi <= lo) continue;
        for (j = 0; j < cb->nvars; j++) {
            const CBlockVarDebug *cv = &cb->vars[j];
            DVar v;
            v.name = xstrdup(cv->name);
            v.lo = lo;
            v.hi = hi;
            v.reg = cb->temp_reg;
            v.off = cv->off;
            v.type = cv->ptr ? type_add(g, 7, 8, 1)
                             : type_add(g, cv->unsig ? 7 : 5, cv->bits / 8, 1);
            vec_push(&d->cvars, v);
        }
    }
    /* USINGs of a DSECT with a base register: the structure is at register - off while it lasts */
    for (i = 0; i < m->usings.n; i++) {
        const UsingRec *u = &m->usings.v[i];
        const Class *kc;
        DStruct *st;
        DSect *d;
        DUse use;
        const ZRec *zb;
        if (u->reg < 0 || u->reg > 15 || u->zbeg >= m->zrecs.n) continue;
        kc = u->key.kind == B_PIECE ? ((Piece *)u->key.p)->cls : u->key.kind == B_CLASS ? (Class *)u->key.p : NULL;
        st = kc && kc->sect->kind == K_DSECT ? struct_of(g, kc->sect) : NULL;
        zb = &m->zrecs.v[u->zbeg];
        d = zb->cls ? dsect_of(g, zb->cls) : NULL;
        if (!st || !d) continue;
        use.st = st;
        use.reg = u->reg;
        use.off = u->bias - u->off;
        use.name = u->label ? u->label : st->sect->name;
        use.lo = zb->off;
        use.hi = d->len;
        if (u->zend < m->zrecs.n && m->zrecs.v[u->zend].cls == zb->cls && m->zrecs.v[u->zend].off < use.hi)
            use.hi = m->zrecs.v[u->zend].off;
        if (use.lo >= use.hi) continue;
        vec_push(&d->uses, use);
    }
    for (i = 0; i < g->sects.n; i++) {
        DSect *d = g->sects.v[i];
        size_t n = 0;
        if (d->labs.n) qsort(d->labs.v, d->labs.n, sizeof *d->labs.v, cmp_lab);
        if (d->rows.n) qsort(d->rows.v, d->rows.n, sizeof *d->rows.v, cmp_row);
        /* the same address and line twice (ORG, CNOP followed by its instruction) */
        for (k = 0; k < d->rows.n; k++) {
            if (n > 0 && d->rows.v[n - 1].addr == d->rows.v[k].addr && d->rows.v[n - 1].line == d->rows.v[k].line &&
                d->rows.v[n - 1].file == d->rows.v[k].file)
                continue;
            d->rows.v[n++] = d->rows.v[k];
        }
        d->rows.n = n;
        if (n) g->nsub++;
    }
    g->comp_dir = dbg_host_path(g->o, g->o->cwd && *g->o->cwd ? g->o->cwd : "/");
    cdir = g->files.v[0].abs;
    k = strlen(g->comp_dir);
    if (!strncmp(cdir, g->comp_dir, k) && cdir[k] == '/' && k > 1)
        g->cu_name = xstrdup(cdir + k + 1);
    else if (k == 1 && cdir[0] == '/')
        g->cu_name = xstrdup(cdir + 1);
    else
        g->cu_name = xstrdup(cdir);
}

/* ---- the module for the object file ---------------------------------------- */

static void ename(GoffESD *e, const char *s) {
    size_t n = strlen(s), i;
    e->name = xmalloc(n + 1);
    for (i = 0; i < n; i++) e->name[i] = za_to_ebcdic[(unsigned char)s[i]];
    e->name_len = n;
}

static const uint8_t BA_ZERO[10] = {0};
static const uint8_t BA_IDRL[10] = {0x00, 0x00, 0x10, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00};
static const uint8_t BA_PRV[10] = {0x00, 0x00, 0x01, 0x00, 0x00, 0x80, 0x03, 0x00, 0x00, 0x00};
static const uint8_t BA_TEXT[10] = {0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00};
static const uint8_t BA_DED[10] = {0x00, 0x03, 0x01, 0x20, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};
static const uint8_t BA_DPR[10] = {0x02, 0x03, 0x01, 0x20, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00};

static uint32_t mk_esd(GoffWriter *w, const char **perr, GoffESDType type, uint32_t parent, const char *name,
                       int ns, uint32_t len, const uint8_t *ba, int amode) {
    GoffESD e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.parent = parent;
    ename(&e, name);
    e.name_space = (uint8_t)ns;
    e.length = len;
    goff_behavior_unpack(ba, &e.attr);
    if (type == GOFF_ER) e.attr.amode = (uint8_t)amode;
    if (!*perr) *perr = goff_write_esd(w, &e);
    return e.id;
}

static uint32_t elf_hash(const unsigned char *s) {
    uint32_t h = 0, x;
    for (; *s; s++) {
        h = (h << 4) + *s;
        x = h & 0xF0000000u;
        if (x) h ^= x >> 24;
        h &= ~x;
    }
    return h;
}

static int build_mod2(Dbg *g, DbgResult *r, char *err, size_t errsz) {
    GoffWriter *w = goff_writer_new(1);
    const char *e = NULL;
    GoffEnd end;
    size_t n = g->sects.n, i, plen = strlen(g->o->dbg_path), tlen;
    uint32_t sd, dbgname, entries, litpr, dbgname_len, lit_len = 0, er0, embedded;
    Buf name = {0}, ent = {0}, lit = {0}, fr = {0};
    GoffReloc *rl = xcalloc((n * 3 + 1) * sizeof *rl);
    size_t nrl = 0;
    const uint8_t *out;
    size_t outlen;

    /* the ELF framed for the class C_ADATAE9C3 */
    d64(&fr, r->elf_len);
    buf_write(&fr, r->elf, r->elf_len);
    /* the text of DBGNAME: padded length, signature, the path of the .dbg in EBCDIC */
    tlen = 8 + 16 + plen + 1;
    dbgname_len = (uint32_t)((tlen + 7) & ~(size_t)7);
    d64(&name, dbgname_len);
    buf_write(&name, g->sig, 16);
    for (i = 0; i < plen; i++) d8(&name, za_to_ebcdic[(unsigned char)g->o->dbg_path[i]]);
    d8(&name, 0);
    for (i = 0; i < n; i++) {
        char *nm = xasprintf(".text_cu1_id%u", (unsigned)g->sects.v[i]->id);
        size_t k, nl = strlen(nm);
        unsigned char *eb = xmalloc(nl + 1);
        for (k = 0; k <= nl; k++) eb[k] = za_to_ebcdic[(unsigned char)nm[k]];
        d64(&ent, 0);
        d64(&ent, 0);
        d32(&ent, 0);
        d32(&ent, elf_hash(eb));
        d64(&ent, (uint64_t)lit.len);
        buf_write(&lit, eb, nl + 1);
    }
    lit_len = (uint32_t)lit.len;

    sd = mk_esd(w, &e, GOFF_SD, 0, " ", GOFF_NS_NORMAL, 0, BA_ZERO, 0);
    mk_esd(w, &e, GOFF_ED, sd, "B_IDRL", GOFF_NS_NORMAL, 0, BA_IDRL, 0);
    mk_esd(w, &e, GOFF_ED, sd, "B_PRV", GOFF_NS_PSEUDO, 0, BA_PRV, 0);
    mk_esd(w, &e, GOFF_ED, sd, "B_TEXT", GOFF_NS_NORMAL, 0, BA_TEXT, 0);
    {
        uint32_t ed = mk_esd(w, &e, GOFF_ED, sd, "D_@@ADBG_DBG", GOFF_NS_PARTS3, 0, BA_DED, 0);
        dbgname = mk_esd(w, &e, GOFF_PR, ed, "DBGNAME", GOFF_NS_PARTS3, dbgname_len, BA_DPR, 0);
        ed = mk_esd(w, &e, GOFF_ED, sd, "D_@@ADBG_MAP", GOFF_NS_PARTS3, 0, BA_DED, 0);
        entries = mk_esd(w, &e, GOFF_PR, ed, "ENTRIES", GOFF_NS_PARTS3, (uint32_t)(32 * n), BA_DPR, 0);
    }
    er0 = 0;
    for (i = 0; i < n; i++) {
        uint32_t id = mk_esd(w, &e, GOFF_ER, sd, g->sects.v[i]->sect->name, GOFF_NS_NORMAL, 0, BA_ZERO,
                             g->sects.v[i]->sect->amode);
        if (!er0) er0 = id;
    }
    {
        uint32_t ed = mk_esd(w, &e, GOFF_ED, sd, "D_@@ADBG_LIT", GOFF_NS_PARTS3, 0, BA_DED, 0);
        litpr = mk_esd(w, &e, GOFF_PR, ed, "LIT", GOFF_NS_PARTS3, lit_len, BA_DPR, 0);
    }
    /* the same ELF inside the object: class C_ADATAE9C3 (NOLOAD, unstructured), each
     * copy framed by its 8-byte big endian length so that several modules can share the class */
    {
        GoffESD ed;
        memset(&ed, 0, sizeof ed);
        ed.type = GOFF_ED;
        ed.parent = sd;
        ename(&ed, DBG_CLASS);
        ed.name_space = GOFF_NS_NORMAL;
        ed.length = (uint32_t)(8 + r->elf_len);
        ed.attr.text_style = GOFF_STYLE_UNSTRUCTURED;
        ed.attr.bind_algorithm = GOFF_BIND_CAT;
        ed.attr.loading = GOFF_NOLOAD;
        if (!e) e = goff_write_esd(w, &ed);
        embedded = ed.id;
    }
    if (!e) e = goff_write_text(w, dbgname, 0, GOFF_STYLE_BYTE, (const uint8_t *)name.p, name.len);
    if (!e) e = goff_write_text(w, entries, 0, GOFF_STYLE_BYTE, (const uint8_t *)ent.p, ent.len);
    if (!e) e = goff_write_text(w, litpr, 0, GOFF_STYLE_BYTE, (const uint8_t *)lit.p, lit.len);
    if (!e) e = goff_write_text(w, embedded, 0, GOFF_STYLE_UNSTRUCTURED, (const uint8_t *)fr.p, fr.len);
    /* the relocations of ENTRIES, grouped by the item they refer to */
#define RL(RID, OFF)                                    \
    do {                                                \
        GoffReloc *x_ = &rl[nrl++];                     \
        x_->rid = (RID);                                \
        x_->pid = entries;                              \
        x_->offset = (uint32_t)(OFF);                   \
        x_->reference = GOFF_REF_ADDRESS;               \
        x_->referent = GOFF_REFERENT_LABEL;             \
        x_->action = GOFF_ACTION_ADD;                   \
        x_->length = 8;                                 \
    } while (0)
    for (i = 0; i < n; i++) RL(dbgname, 32 * i + 8);
    for (i = 0; i < n; i++) RL(er0 + (uint32_t)i, 32 * i);
    for (i = 0; i < n; i++) RL(litpr, 32 * i + 24);
#undef RL
    if (!e) e = goff_write_rld(w, rl, nrl);
    memset(&end, 0, sizeof end);
    if (!e) e = goff_write_end(w, &end, &out, &outlen);
    if (e) {
        snprintf(err, errsz, "%s", e);
        return 0;
    }
    r->mod2 = xmalloc(outlen + 1);
    memcpy(r->mod2, out, outlen);
    r->mod2_len = outlen;
    return 1;
}

/* ---- the whole ----------------------------------------------------------------- */

int dbg_build(Module *m, const uint8_t *obj, size_t objlen, const DbgOptions *o, DbgResult *r, char *err,
              size_t errsz) {
    Dbg *g = xcalloc(sizeof *g);
    Md5 c;
    size_t i;
    static const char hex[] = "0123456789ABCDEF";
    g->o = o;
    g->m = m;
    collect(g);
    if (g->sects.n == 0) {
        snprintf(err, errsz, "-g: no named control section to describe");
        return 0;
    }
    if (strlen(o->dbg_path) > 200) {
        snprintf(err, errsz, "-g: the path of the .dbg file is too long (%zu bytes, at most 200)", strlen(o->dbg_path));
        return 0;
    }
    dbg_dwarf(g);
    md5_init(&c);
    md5_update(&c, obj, objlen);
    for (i = 0; i < SEC_COUNT; i++)
        if (g->have[i]) md5_update(&c, g->sec[i].d.p, g->sec[i].d.len);
    md5_final(&c, g->sig);
    g->sig_hex = xmalloc(33);
    for (i = 0; i < 16; i++) {
        g->sig_hex[2 * i] = hex[g->sig[i] >> 4];
        g->sig_hex[2 * i + 1] = hex[g->sig[i] & 15];
    }
    g->sig_hex[32] = 0;
    r->elf = dbg_elf(g, &r->elf_len);
    return build_mod2(g, r, err, errsz);
}

int dbg_build_state(void *dbg, DbgResult *r, char *err, size_t errsz) {
    Dbg *g = dbg;
    Md5 c;
    size_t i;
    static const char hex[] = "0123456789ABCDEF";
    if (!g || !g->o) { snprintf(err, errsz, "no debug state"); return 0; }
    if (g->sects.n == 0) { snprintf(err, errsz, "no control section to describe"); return 0; }
    if (strlen(g->o->dbg_path) > 200) {
        snprintf(err, errsz, "the path of the .dbg file is too long");
        return 0;
    }
    dbg_dwarf(g);
    md5_init(&c);
    for (i = 0; i < SEC_COUNT; i++)
        if (g->have[i]) md5_update(&c, g->sec[i].d.p, g->sec[i].d.len);
    md5_final(&c, g->sig);
    g->sig_hex = xmalloc(33);
    for (i = 0; i < 16; i++) {
        g->sig_hex[2 * i] = hex[g->sig[i] >> 4];
        g->sig_hex[2 * i + 1] = hex[g->sig[i] & 15];
    }
    g->sig_hex[32] = 0;
    r->elf = dbg_elf(g, &r->elf_len);
    return build_mod2(g, r, err, errsz);
}

/* ---- the ELF form (-elf -g) --------------------------------------------------- */

int dbg_build_elf(Module *m, const DbgOptions *o, DbgElfSec **out, size_t *nout, char *err, size_t errsz) {
    Dbg *g = xcalloc(sizeof *g);
    size_t i, k, n = 0, pos[SEC_COUNT];
    DbgElfSec *s;
    g->o = o;
    g->m = m;
    g->elf = 1;
    collect(g);
    if (g->sects.n == 0) {
        snprintf(err, errsz, "-g: no named control section to describe");
        return 0;
    }
    dbg_dwarf(g);
    for (i = 0; i < SEC_COUNT; i++) pos[i] = n, n += g->have[i] ? 1 : 0;
    s = xcalloc((n + 1) * sizeof *s);
    for (i = 0; i < SEC_COUNT; i++) {
        DbgElfSec *e;
        if (!g->have[i]) continue;
        e = &s[pos[i]];
        e->name = g->sec[i].name;
        e->data = (uint8_t *)g->sec[i].d.p;
        e->len = g->sec[i].d.len;
        e->nrels = g->sec[i].rels.n;
        e->rels = xcalloc((e->nrels + 1) * sizeof *e->rels);
        for (k = 0; k < e->nrels; k++) {
            const DRel *r = &g->sec[i].rels.v[k];
            DbgElfRel *x = &e->rels[k];
            x->off = r->off;
            x->type = (uint32_t)r->type;
            if (r->kind == RK_TEXT) {
                x->cls = g->sects.v[r->idx]->cls;
                x->sec = -1;
            } else {
                x->cls = NULL;
                x->sec = (int)pos[r->idx];
            }
        }
    }
    *out = s;
    *nout = n;
    return 1;
}
