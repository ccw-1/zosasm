/* obj.c - OBJ/XOBJ reader and OBJ -> GOFF converter. */
#include <stdlib.h>
#include <string.h>

#include "dbg.h"
#include "dbg_int.h"
#include "ebcdic.h"
#include "goff.h"
#include "obj.h"

typedef Vec(DSect *) DSectVec;

static uint32_t g16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t g24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint32_t g32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static int blank(const uint8_t *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) if (p[i] != 0x40) return 0;
    return 1;
}
static char *obj_name(const uint8_t *p, size_t n, int *outlen) {
    size_t k = n;
    char *s;
    while (k > 0 && p[k - 1] == 0x40) k--;
    if (k == 0) { k = 1; }
    s = xmalloc(k + 1);
    memcpy(s, p, k);
    if (n == 0 || blank(p, n)) s[0] = ' ';
    s[k] = 0;
    *outlen = (int)k;
    return s;
}

static uint8_t *ebc_lit(const char *s) {
    uint8_t *p = xmalloc(strlen(s) + 1);
    size_t i;
    for (i = 0; s[i]; i++) p[i] = za_to_ebcdic[(unsigned char)s[i]];
    p[i] = 0;
    return p;
}

static void free_module(ObjModule *m) {
    size_t i;
    for (i = 0; i < m->esds.n; i++) free(m->esds.v[i].name);
    for (i = 0; i < m->texts.n; i++) free(m->texts.v[i].data);
    for (i = 0; i < m->syms.n; i++) free(m->syms.v[i].data);
    for (i = 0; i < m->end.idrs.n; i++) free(m->end.idrs.v[i]);
    free(m->end.name);
    free(m->esds.v);
    free(m->texts.v);
    free(m->rlds.v);
    free(m->syms.v);
    free(m->end.idrs.v);
    free(m);
}

void obj_free(ObjFile *f) {
    size_t i;
    if (!f) return;
    for (i = 0; i < f->modules.n; i++) free_module(f->modules.v[i]);
    free(f->modules.v);
    memset(f, 0, sizeof *f);
}

static ObjModule *new_module(ObjFile *f) {
    ObjModule *m = xcalloc(sizeof *m);
    vec_push(&f->modules, m);
    return m;
}

static const char *parse_esd(ObjModule *m, const uint8_t *r) {
    size_t count = g16(r + 10), pos = 16, end = pos + count;
    uint32_t nextid = g16(r + 14);
    int first = 1;
    if (end > 72) end = 72;
    while (pos + 16 <= end) {
        ObjEsd e;
        int nl;
        memset(&e, 0, sizeof e);
        e.name = obj_name(r + pos, 8, &nl);
        e.name_len = nl;
        e.type = r[pos + 8];
        e.addr = g24(r + pos + 9);
        e.flags = r[pos + 12];
        if (!blank(r + pos + 13, 3)) { e.len = g24(r + pos + 13); e.has_len = 1; }
        if (e.type == 1) {
            e.id = 0;
        } else {
            e.id = (uint16_t)nextid++;
            first = 0;
        }
        vec_push(&m->esds, e);
        pos += 16;
    }
    (void)first;
    return NULL;
}

/* XOBJ XSD item: one per record.  The fields used here are the common
 * (non-segmented) form; long names are truncated to the bytes in the record. */
static const char *parse_xsd(ObjModule *m, const uint8_t *r) {
    ObjEsd e;
    size_t nl = g16(r + 18), avail = 40;
    int n;
    if (nl > avail) nl = avail;
    memset(&e, 0, sizeof e);
    e.id = (uint16_t)g16(r + 14);
    e.type = r[24];
    e.flags = r[28];
    if (!blank(r + 29, 3)) { e.len = g24(r + 29); e.has_len = 1; }
    e.name = obj_name(r + 32, nl, &n);
    e.name_len = n;
    e.from_xsd = 1;
    vec_push(&m->esds, e);
    return NULL;
}

static const char *parse_txt(ObjModule *m, const uint8_t *r) {
    ObjText t;
    size_t n = g16(r + 10);
    if (n > 56) n = 56;
    memset(&t, 0, sizeof t);
    t.esdid = (uint16_t)g16(r + 14);
    t.addr = g24(r + 5);
    t.data = xmalloc(n ? n : 1);
    memcpy(t.data, r + 16, n);
    t.len = n;
    vec_push(&m->texts, t);
    return NULL;
}

static const char *parse_rld(ObjModule *m, const uint8_t *r) {
    size_t n = g16(r + 10), p = 16, end = p + n;
    int need_ids = 1;
    uint16_t rid = 0, pid = 0;
    if (end > 72) end = 72;
    while (p < end) {
        ObjRld x;
        memset(&x, 0, sizeof x);
        if (need_ids) {
            if (p + 2 > end) return "OBJ RLD: truncated R/P pointer";
            rid = (uint16_t)g16(r + p); p += 2;
            pid = (uint16_t)g16(r + p); p += 2;
            need_ids = 0;
        }
        if (p + 4 > end) return "OBJ RLD: truncated item";
        x.rid = rid; x.pid = pid; x.flags = r[p]; x.addr = g24(r + p + 1);
        p += 4;
        need_ids = !(x.flags & 0x01);
        vec_push(&m->rlds, x);
    }
    return NULL;
}

static const char *parse_end(ObjModule *m, const uint8_t *r) {
    ObjEnd *e = &m->end;
    int i, n;
    e->kind = 0;
    if (!blank(r + 14, 2)) {
        e->kind = 1;
        e->esdid = (uint16_t)g16(r + 14);
        e->offset = g24(r + 5);
    } else if (!blank(r + 16, 8)) {
        e->kind = 2;
        e->name = obj_name(r + 16, 8, &e->name_len);
        e->offset = g24(r + 5);
    }
    if (!blank(r + 28, 4)) { e->has_len = 1; e->section_len = g32(r + 28); }
    n = r[32] == 0xF1 ? 1 : r[32] == 0xF2 ? 2 : 0;
    for (i = 0; i < n; i++) {
        const uint8_t *p = r + 33 + (size_t)i * 19;
        char *idr = xmalloc(19);
        memcpy(idr, p, 19);
        vec_push(&e->idrs, idr);
    }
    m->has_end = 1;
    return NULL;
}

const char *obj_parse(const uint8_t *data, size_t len, ObjFile *out) {
    ObjModule *m = NULL;
    size_t off;
    if (len == 0 || len % 80) return "OBJ: file size is not a multiple of 80";
    memset(out, 0, sizeof *out);
    for (off = 0; off < len; off += 80) {
        const uint8_t *r = data + off;
        const char *e = NULL;
        if (r[0] != 0x02) return "OBJ: record does not start with X'02'";
        if (!m) m = new_module(out);
        if (!memcmp(r + 1, "\xC5\xE2\xC4", 3)) e = parse_esd(m, r);          /* ESD */
        else if (!memcmp(r + 1, "\xE7\xE2\xC4", 3)) e = parse_xsd(m, r);     /* XSD */
        else if (!memcmp(r + 1, "\xE3\xE7\xE3", 3)) e = parse_txt(m, r);     /* TXT */
        else if (!memcmp(r + 1, "\xD9\xD3\xC4", 3)) e = parse_rld(m, r);     /* RLD */
        else if (!memcmp(r + 1, "\xC5\xD5\xC4", 3)) {                        /* END */
            e = parse_end(m, r);
            if (!e) m = NULL; /* the next subfile starts a new module */
        } else if (!memcmp(r + 1, "\xE2\xE8\xD4", 3)) {                      /* SYM */
            ObjSym s;
            size_t n = g16(r + 10);
            if (n > 56) n = 56;
            s.data = xmalloc(n ? n : 1);
            memcpy(s.data, r + 16, n);
            s.len = n;
            vec_push(&m->syms, s);
        } else {
            return "OBJ: unknown record type";
        }
        if (e) return e;
    }
    if (m) return "OBJ: missing END record";
    return NULL;
}

/* ---- conversion ---------------------------------------------------------- */

typedef struct {
    uint16_t old;
    uint32_t sd, ed, pr, target;
    int type;
} Map;

static Map *map_find(Map *m, size_t n, uint16_t id) {
    size_t i;
    for (i = 0; i < n; i++) if (m[i].old == id) return &m[i];
    return NULL;
}
static uint32_t map_target(Map *m, size_t n, uint16_t id) {
    Map *p = map_find(m, n, id);
    return p ? p->target : 0;
}
static void obj_attr(uint8_t f, GoffBehavior *b, int ed) {
    memset(b, 0, sizeof *b);
    if (ed) b->alignment = GOFF_ALIGN_DBL;
    if (f == 0x40) return; /* spaces mean unspecified */
    b->amode = (f & 0x08) ? GOFF_AMODE_64 : ((f >> 6) & 3) == 2 ? GOFF_AMODE_31 :
               ((f >> 6) & 3) == 3 ? GOFF_AMODE_ANY : ((f >> 6) & 3) == 0 ? 0 : GOFF_AMODE_24;
    b->rmode = (f & 0x04) ? GOFF_RMODE_64 : (f & 0x20) ? GOFF_RMODE_31 : GOFF_RMODE_24;
    b->read_only = (f & 0x10) != 0;
    if (!ed) b->alignment = 0;
}

static uint32_t put_ed(GoffWriter *w, uint32_t parent, const char *name, uint32_t off, uint32_t len,
                       uint8_t flags, int common, const char **err) {
    GoffESD e;
    memset(&e, 0, sizeof e);
    e.type = GOFF_ED;
    e.parent = parent;
    e.offset = off;
    e.length = len;
    e.name_space = GOFF_NS_NORMAL;
    e.attr.text_style = GOFF_STYLE_BYTE;
    obj_attr(flags, &e.attr, 1);
    e.attr.common = common;
    e.name = (uint8_t *)name;
    e.name_len = strlen(name);
    *err = goff_write_esd(w, &e);
    return e.id;
}

static uint32_t put_special_ed(GoffWriter *w, uint32_t parent, char *name, uint32_t len,
                               uint8_t style, uint8_t loading, const char **err) {
    GoffESD e;
    memset(&e, 0, sizeof e);
    e.type = GOFF_ED;
    e.parent = parent;
    e.length = len;
    e.name_space = GOFF_NS_NORMAL;
    e.attr.text_style = style;
    e.attr.bind_algorithm = GOFF_BIND_CAT;
    e.attr.loading = loading;
    e.name = (uint8_t *)name;
    e.name_len = strlen(name);
    *err = goff_write_esd(w, &e);
    return e.id;
}

static uint32_t put_pr(GoffWriter *w, uint32_t parent, const char *name, uint32_t len, uint8_t flags,
                       const char **err) {
    GoffESD e;
    memset(&e, 0, sizeof e);
    e.type = GOFF_PR;
    e.parent = parent;
    e.length = len;
    e.name_space = GOFF_NS_PARTS3;
    e.attr.bind_algorithm = GOFF_BIND_MERGE;
    e.attr.alignment = flags & 0x1f;
    e.name = (uint8_t *)name;
    e.name_len = strlen(name);
    *err = goff_write_esd(w, &e);
    return e.id;
}

static uint32_t put_er(GoffWriter *w, uint32_t parent, const char *name, uint8_t flags, const char **err) {
    GoffESD e;
    memset(&e, 0, sizeof e);
    e.type = GOFF_ER;
    e.parent = parent;
    e.name_space = GOFF_NS_NORMAL;
    obj_attr(flags, &e.attr, 0);
    e.name = (uint8_t *)name;
    e.name_len = strlen(name);
    *err = goff_write_esd(w, &e);
    return e.id;
}

static uint8_t reloc_kind(uint8_t f) {
    switch ((f >> 4) & 3) {
    case 1: return GOFF_REF_ADDRESS; /* V */
    case 2: return GOFF_REF_OFFSET;  /* Q */
    case 3: return GOFF_REF_LENGTH;  /* CXD */
    default: return GOFF_REF_ADDRESS; /* A */
    }
}

static const char *convert_module(const ObjModule *m, Buf *out, int keep_sym, char *err, size_t errsz) {
    GoffWriter *w = goff_writer_new(1);
    Map *map = xcalloc((m->esds.n + 1) * sizeof *map);
    size_t nmap = 0, i;
    uint32_t first_sd = 0, cur_sd = 0, cur_ed = 0, cur_pr = 0;
    const char *e = NULL;
    uint32_t *txt_target = xcalloc((m->texts.n + 1) * sizeof *txt_target);
    GoffReloc *rl = xcalloc((m->rlds.n + 1) * sizeof *rl);
    size_t nrl = 0;
    int common = 0;

    /* sections and their B_TEXT elements */
    for (i = 0; i < m->esds.n; i++) {
        const ObjEsd *x = &m->esds.v[i];
        Map *mp;
        uint8_t type = x->type & 0x0f;
        char *name = x->name;
        if (type == 1) { /* LD: belongs to the current element */
            GoffESD ld;
            memset(&ld, 0, sizeof ld);
            ld.type = GOFF_LD;
            ld.parent = cur_ed;
            ld.offset = x->addr;
            ld.name_space = GOFF_NS_NORMAL;
            obj_attr(x->flags, &ld.attr, 0);
            ld.name = (uint8_t *)name;
            ld.name_len = strlen(name);
            e = goff_write_esd(w, &ld);
            if (e) goto done;
            continue;
        }
        if (type == 2 || type == 0x0a) { /* ER/WX */
            mp = &map[nmap++];
            mp->old = x->id;
            mp->type = type;
            mp->target = put_er(w, cur_sd, name, x->flags, &e);
            if (e) goto done;
            continue;
        }
        if (type == 6 || type == 3) { /* PR */
            mp = &map[nmap++];
            mp->old = x->id;
            mp->type = 6;
            mp->target = put_pr(w, cur_ed, name, x->len, x->flags, &e);
            if (e) goto done;
            mp->pr = mp->target;
            cur_pr = mp->target;
            continue;
        }
        if (type != 0 && type != 4 && type != 5 && type != 0x0d && type != 0x0e && type != 0x0f)
            continue;
        /* SD/PC/CM, including quad aligned forms */
        common = type == 5 || type == 0x0f;
        {
            GoffESD sd;
            memset(&sd, 0, sizeof sd);
            sd.type = GOFF_SD;
            sd.name_space = GOFF_NS_NORMAL;
            sd.attr.common = common;
            sd.attr.alignment = type >= 0x0d ? GOFF_ALIGN_QUAD : 0;
            sd.name = (uint8_t *)name;
            sd.name_len = strlen(name);
            e = goff_write_esd(w, &sd);
            if (e) goto done;
            if (!first_sd) first_sd = sd.id;
            cur_sd = sd.id;
        }
        {
            uint8_t *bt = ebc_lit("B_TEXT");
            cur_ed = put_ed(w, cur_sd, (char *)bt, x->addr, x->len, x->flags, common, &e);
            free(bt);
        }
        if (e) goto done;
        cur_pr = 0;
        if (!common && strcmp(name, " ")) {
            GoffESD ld;
            memset(&ld, 0, sizeof ld);
            ld.type = GOFF_LD;
            ld.parent = cur_ed;
            ld.offset = 0;
            ld.name_space = GOFF_NS_NORMAL;
            obj_attr(x->flags, &ld.attr, 0);
            ld.name = (uint8_t *)name;
            ld.name_len = strlen(name);
            e = goff_write_esd(w, &ld);
            if (e) goto done;
        }
        mp = &map[nmap++];
        mp->old = x->id;
        mp->type = type;
        mp->sd = cur_sd; mp->ed = cur_ed; mp->target = cur_ed;
    }

    /* TXT records point at the element that was remembered for the section */
    for (i = 0; i < m->texts.n; i++) {
        uint32_t id = map_target(map, nmap, m->texts.v[i].esdid);
        txt_target[i] = id;
        if (!id) id = cur_ed;
        e = goff_write_text(w, id, m->texts.v[i].addr, GOFF_STYLE_BYTE,
                            m->texts.v[i].data, m->texts.v[i].len);
        if (e) goto done;
    }

    /* relocations */
    for (i = 0; i < m->rlds.n; i++) {
        const ObjRld *x = &m->rlds.v[i];
        GoffReloc *r = &rl[nrl++];
        uint8_t kind = (x->flags >> 4) & 3;
        uint8_t len = (uint8_t)(((x->flags >> 2) & 3) + 1);
        uint32_t pid = map_target(map, nmap, x->pid);
        memset(r, 0, sizeof *r);
        r->rid = map_target(map, nmap, x->rid);
        r->pid = pid ? pid : cur_ed;
        r->offset = x->addr;
        r->length = len + ((x->flags & 0x40) ? 4 : 0);
        r->action = (x->flags & 0x02) ? GOFF_ACTION_SUB : GOFF_ACTION_ADD;
        r->reference = reloc_kind(x->flags);
        r->referent = kind == 2 || kind == 3 ? GOFF_REFERENT_CLASS : GOFF_REFERENT_LABEL;
        r->store = kind == 1 || kind == 3;
        if (kind == 3 && !r->rid) r->rid = cur_pr ? cur_pr : pid;
    }
    if (nrl) {
        e = goff_write_rld(w, rl, nrl);
        if (e) goto done;
    }

    /* SYM data: preserve the translator symbol records in an unloaded ADATA
     * class.  A later pass can decode them into DWARF. */
    if (keep_sym && m->syms.n) {
        Buf sy = {0};
        uint32_t ed;
        for (i = 0; i < m->syms.n; i++) buf_write(&sy, m->syms.v[i].data, m->syms.v[i].len);
        {
            uint8_t *nm = ebc_lit("C_OBJSYM");
            ed = put_special_ed(w, first_sd, (char *)nm, (uint32_t)sy.len,
                                GOFF_STYLE_UNSTRUCTURED, GOFF_NOLOAD, &e);
            free(nm);
        }
        if (!e) e = goff_write_text(w, ed, 0, GOFF_STYLE_UNSTRUCTURED, (const uint8_t *)sy.p, sy.len);
        free(sy.p);
        if (e) goto done;
    }

    /* END: optional IDR, deferred section length and entry point */
    for (i = 0; i < m->end.idrs.n; i++) {
        uint8_t data[23];
        uint32_t ed;
        memset(data, 0, sizeof data);
        data[1] = 0;
        data[2] = 0; data[3] = 19;
        memcpy(data + 4, m->end.idrs.v[i], 19);
        {
            uint8_t *nm = ebc_lit("B_IDRL");
            ed = put_special_ed(w, first_sd, (char *)nm, 23, GOFF_STYLE_STRUCTURED,
                                GOFF_LOAD_NOW, &e);
            free(nm);
        }
        if (e) goto done;
        e = goff_write_text(w, ed, 0, GOFF_STYLE_STRUCTURED, data, sizeof data);
        if (e) goto done;
    }
    if (m->end.has_len) {
        GoffDeferredLen dl;
        dl.esdid = map_target(map, nmap, m->end.esdid);
        dl.length = m->end.section_len;
        if (dl.esdid) { e = goff_write_len(w, &dl, 1); if (e) goto done; }
    }
    {
        GoffEnd end;
        memset(&end, 0, sizeof end);
        end.kind = GOFF_ENTRY_NONE;
        if (m->end.kind == 1) {
            end.kind = GOFF_ENTRY_BYID;
            end.esdid = map_target(map, nmap, m->end.esdid);
            end.offset = m->end.offset;
            end.amode = 0;
        } else if (m->end.kind == 2) {
            end.kind = GOFF_ENTRY_BYNAME;
            end.name = (uint8_t *)m->end.name;
            end.name_len = (size_t)m->end.name_len;
            end.offset = m->end.offset;
            end.amode = 0;
        }
        {
            const uint8_t *endp;
            size_t endn;
            e = goff_write_end(w, &end, &endp, &endn);
            if (e) goto done;
            buf_write(out, w->buf.p, w->buf.len);
        }
    }

done:
    if (e) snprintf(err, errsz, "%s", e);
    free(map); free(txt_target); free(rl);
    goff_writer_free(w);
    return e;
}

static void add_sym_labels(void *vp, const uint8_t *data, size_t n) {
    DSectVec *sects = vp;
    size_t p = 0;
    while (p + 4 <= n) {
        unsigned org = data[p++];
        uint32_t addr = g24(data + p);
        char *name = NULL;
        int nl = 0;
        p += 3;
        if (!(org & 0x10)) {
            nl = (int)(org & 7) + 1;
            if (p + (size_t)nl > n) break;
            name = ebcdic_decode(data + p, (size_t)nl);
            p += (size_t)nl;
        } else {
            if (p >= n) break;
            p += (size_t)data[p] + 1;
        }
        if (name && *name) {
            size_t i;
            for (i = 0; i < sects->n; i++) {
                DSect *d = sects->v[i];
                if (addr <= (uint32_t)d->len) {
                    DLab l;
                    memset(&l, 0, sizeof l);
                    l.name = name;
                    l.off = addr;
                    l.var = 0;
                    l.type = -1;
                    vec_push(&d->labs, l);
                    name = NULL;
                    break;
                }
            }
        }
        free(name);
        if (org & 1) {
            int dt, lb;
            if (p >= n) break;
            dt = data[p++];
            lb = (dt == 0 || dt == 4) ? 2 : 1;
            if (p + (size_t)lb > n) break;
            p += (size_t)lb;
            if (org & 0x02) p += 3;
            if (org & 0x08) p += 2;
        }
    }
}

const char *obj_to_dwarf(const ObjFile *f, Buf *mod2, Buf *elf, const char *dbg_path,
                         const char *input, char *err, size_t errsz) {
    Dbg g;
    DbgOptions o;
    DbgResult r;
    const ObjModule *m;
    Vec(DSect *) maps = {0};
    DSect *cur = NULL;
    DSect *end_sect = NULL;
    int64_t base = 0;
    size_t i;
    const char *e;
    if (!f->modules.n) { snprintf(err, errsz, "no module for DWARF"); return "no module"; }
    m = f->modules.v[0];
    for (i = 1; i < f->modules.n; i++)
        if (f->modules.v[i]->esds.n > m->esds.n) m = f->modules.v[i];
    memset(&g, 0, sizeof g);
    memset(&o, 0, sizeof o);
    o.cwd = "";
    o.main_file = input;
    o.producer = "obj2goff";
    o.dbg_path = dbg_path;
    g.o = &o;
    g.comp_dir = xstrdup("");
    g.cu_name = xstrdup(input);
    g.elf = 0;
    {
        DFile file;
        DDir dir;
        memset(&dir, 0, sizeof dir);
        dir.path = xstrdup(".");
        vec_push(&g.dirs, dir);
        memset(&file, 0, sizeof file);
        file.name = xstrdup(input);
        file.abs = xstrdup(input);
        file.dir = xstrdup(".");
        file.base = xstrdup(input);
        file.dir_idx = 0;
        vec_push(&g.files, file);
        g.main_off = 0;
    }
    for (i = 0; i < m->esds.n; i++) {
        const ObjEsd *x = &m->esds.v[i];
        uint8_t t = x->type & 0x0f;
        if (t == 0 || t == 4 || t == 5 || t == 0x0d || t == 0x0e || t == 0x0f) {
            DSect *d;
            Section *s;
            Class *c;
            char *nm = ebcdic_decode((const unsigned char *)x->name, (size_t)x->name_len);
            if (!nm || blank((const uint8_t *)x->name, (size_t)x->name_len) || !strcmp(nm, " ")) {
                free(nm);
                cur = NULL;
                continue;
            }
            d = xcalloc(sizeof *d);
            s = xcalloc(sizeof *s);
            c = xcalloc(sizeof *c);
            s->name = nm;
            s->kind = K_CSECT;
            c->name = xstrdup("B_TEXT");
            c->sect = s;
            c->len = (int64_t)x->len;
            d->sect = s;
            d->cls = c;
            d->id = (uint32_t)g.sects.n + 1;
            d->len = (int64_t)x->len;
            d->base = base;
            base += d->len;
            vec_push(&g.sects, d);
            vec_push(&maps, d);
            cur = d;
            if (m->end.kind == 1 && x->id == m->end.esdid) end_sect = d;
        } else if ((t == 1 || t == 6 || t == 3) && cur) {
            char *nm = ebcdic_decode((const unsigned char *)x->name, (size_t)x->name_len);
            if (nm && *nm) {
                DLab l;
                memset(&l, 0, sizeof l);
                l.name = nm;
                l.off = x->addr;
                l.var = 0;
                l.type = -1;
                vec_push(&cur->labs, l);
            } else free(nm);
        }
    }
    for (i = 0; i < m->syms.n; i++)
        add_sym_labels((void *)&g.sects, m->syms.v[i].data, m->syms.v[i].len);
    if (m->end.kind == 1 && end_sect) {
        DLab l;
        memset(&l, 0, sizeof l);
        l.name = xstrdup(end_sect->sect->name);
        l.off = m->end.offset;
        l.var = 0;
        l.type = -1;
        vec_push(&end_sect->labs, l);
    } else if (m->end.kind == 2 && m->end.name && g.sects.n) {
        char *nm = ebcdic_decode((const unsigned char *)m->end.name, (size_t)m->end.name_len);
        if (nm && *nm) {
            DLab l;
            memset(&l, 0, sizeof l);
            l.name = nm;
            l.off = m->end.offset;
            l.var = 0;
            l.type = -1;
            vec_push(&g.sects.v[0]->labs, l);
        } else free(nm);
    }
    free(maps.v);
    if (g.sects.n == 0) {
        snprintf(err, errsz, "no named section for DWARF");
        return "no named section";
    }
    memset(&r, 0, sizeof r);
    e = dbg_build_state(&g, &r, err, errsz) ? NULL : "cannot build DWARF";
    if (e) return e;
    buf_write(mod2, r.mod2, r.mod2_len);
    buf_write(elf, r.elf, r.elf_len);
    free(r.mod2);
    free(r.elf);
    return NULL;
}

const char *obj_to_goff(const ObjFile *f, Buf *out, int keep_sym, int dwarf, char *err, size_t errsz) {
    size_t i;
    (void)dwarf;
    for (i = 0; i < f->modules.n; i++) {
        const char *e = convert_module(f->modules.v[i], out, keep_sym, err, errsz);
        if (e) return e;
    }
    return NULL;
}
