/* goff_read.c - GOFF reader and dumper (used by tests and `zosasm -dump`). */
#include <stdlib.h>
#include <string.h>

#include "ebcdic.h"
#include "goff.h"

#define CONT_NEXT 0x1
#define CONT_IS 0x2

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

typedef struct {
    int typ;
    const uint8_t *first;
    Buf payload; /* bytes after the 3 byte prefix of every continuation */
    int complete;
} Logical;

static char errbuf[160];

static const char *fail(const char *fmt, int a, int b) {
    snprintf(errbuf, sizeof errbuf, fmt, a, b);
    return errbuf;
}

/* first record tail + continuation payload as one byte string */
static uint8_t *join(const uint8_t *first_tail, size_t ftail, Logical *l, size_t *n) {
    uint8_t *all = xmalloc(ftail + l->payload.len + 1);
    memcpy(all, first_tail, ftail);
    if (l->payload.len) memcpy(all + ftail, l->payload.p, l->payload.len);
    *n = ftail + l->payload.len;
    return all;
}

static const char *decode_rld(GoffModule *m, const uint8_t *d, size_t n) {
    GoffReloc prev;
    memset(&prev, 0, sizeof prev);
    if (m->relocs.n) prev = m->relocs.v[m->relocs.n - 1];
    while (n > 0) {
        const uint8_t *fl = d;
        GoffReloc r;
        size_t off_len = 4, need = 8;
        memset(&r, 0, sizeof r);
        if (n < 8) return "truncated RLD item";
        r.reference = fl[1] >> 4;
        r.referent = fl[1] & 0xF;
        r.action = fl[2] >> 1;
        r.store = (fl[2] & 1) != 0;
        r.length = fl[4];
        r.bit_length = fl[5] >> 5;
        r.cond_seq = (fl[5] & 0x10) != 0;
        r.bit_offset = fl[5] & 7;
        r.amode_sens = (fl[0] & 1) != 0;
        /* flag bits as written by IBM tools and LLVM: 0x02 = 8 byte offset,
         * 0x04 = 8 bytes of extended attributes behind the item */
        if (fl[0] & 0x02) off_len = 8;
        if (fl[0] & 0x04) need += 8;
        r.rid = prev.rid;
        r.pid = prev.pid;
        r.offset = prev.offset;
        if (!(fl[0] & 0x80)) need += 4;
        if (!(fl[0] & 0x40)) need += 4;
        if (!(fl[0] & 0x20)) need += off_len;
        if (n < need) return "truncated RLD item";
        d += 8;
        n -= need;
        if (!(fl[0] & 0x80)) {
            r.rid = get32(d);
            d += 4;
        }
        if (!(fl[0] & 0x40)) {
            r.pid = get32(d);
            d += 4;
        }
        if (!(fl[0] & 0x20)) {
            size_t i;
            r.offset = 0;
            for (i = 0; i < off_len; i++) {
                if (i + 4 < off_len && d[i]) return "RLD offset beyond 4 GB";
                r.offset = r.offset << 8 | d[i];
            }
            d += off_len;
        }
        if (fl[0] & 0x04) d += 8;
        vec_push(&m->relocs, r);
        prev = r;
    }
    return NULL;
}

static const char *decode(GoffModule *m, Logical *l) {
    const uint8_t *f = l->first;
    size_t n, k;
    uint8_t *all;
    switch (l->typ) {
    case GOFF_T_HDR:
        m->arch_level = get32(f + 48);
        break;
    case GOFF_T_ESD: {
        GoffESD *e = xcalloc(sizeof *e);
        uint8_t ba[10];
        size_t nl = get16(f + 70);
        e->type = (GoffESDType)f[3];
        e->id = get32(f + 4);
        e->parent = get32(f + 8);
        e->offset = get32(f + 16);
        e->length = get32(f + 24);
        e->ext_attr_id = get32(f + 28);
        e->ext_attr_off = get32(f + 32);
        e->name_space = f[40];
        if (f[41] & 0x80) {
            e->has_fill = 1;
            e->fill = f[42];
        }
        e->mangled = (f[41] & 0x40) != 0;
        e->renameable = (f[41] & 0x20) != 0;
        e->removable = (f[41] & 0x10) != 0;
        e->reserved_qwords = f[41] & 7;
        e->ada_id = get32(f + 44);
        e->priority = get32(f + 48);
        memcpy(ba, f + 60, 10);
        goff_behavior_unpack(ba, &e->attr);
        all = join(f + 72, GOFF_RECORD_LEN - 72, l, &n);
        if (nl > n) {
            free(all);
            return fail("ESD name length %d exceeds data", (int)nl, 0);
        }
        e->name = all;
        e->name_len = nl;
        vec_push(&m->esds, e);
        break;
    }
    case GOFF_T_TXT: {
        GoffText t;
        size_t len = get16(f + 22);
        memset(&t, 0, sizeof t);
        t.style = f[3] & 0xF;
        t.esdid = get32(f + 4);
        t.offset = get32(f + 12);
        t.true_len = get32(f + 16);
        t.encoding = (uint16_t)get16(f + 20);
        all = join(f + 24, GOFF_RECORD_LEN - 24, l, &n);
        if (len > n) {
            free(all);
            return fail("TXT length %d exceeds data", (int)len, 0);
        }
        t.data = all;
        t.len = len;
        vec_push(&m->texts, t);
        break;
    }
    case GOFF_T_RLD: {
        const char *e;
        size_t len = get16(f + 4);
        all = join(f + 6, GOFF_RECORD_LEN - 6, l, &n);
        if (len > n) {
            free(all);
            return fail("RLD length %d exceeds data", (int)len, 0);
        }
        e = decode_rld(m, all, len);
        free(all);
        return e;
    }
    case GOFF_T_LEN: {
        size_t len = get16(f + 6);
        if (len > GOFF_RECORD_LEN - 8 || len % 12) return fail("bad LEN length %d", (int)len, 0);
        for (k = 0; k < len; k += 12) {
            GoffDeferredLen d;
            d.esdid = get32(f + 8 + k);
            d.length = get32(f + 8 + k + 8);
            vec_push(&m->lens, d);
        }
        break;
    }
    case GOFF_T_END: {
        size_t nl = get16(f + 24);
        m->end.kind = (GoffEntryKind)(f[3] & 3);
        m->end.amode = f[4];
        m->end_count = get32(f + 8);
        m->end.esdid = get32(f + 12);
        m->end.offset = get32(f + 20);
        all = join(f + 26, GOFF_RECORD_LEN - 26, l, &n);
        if (nl > n) {
            free(all);
            return fail("END name length %d exceeds data", (int)nl, 0);
        }
        m->end.name = all;
        m->end.name_len = nl;
        break;
    }
    default:
        return fail("unknown record type 0x%X", l->typ, 0);
    }
    return NULL;
}

static int esd_cmp(const void *a, const void *b) {
    const GoffESD *x = *(const GoffESD *const *)a, *y = *(const GoffESD *const *)b;
    return x->id < y->id ? -1 : x->id > y->id;
}

GoffModule *goff_parse(const uint8_t *b, size_t len, const char **err) {
    GoffModule *m = xcalloc(sizeof *m);
    Vec(Logical *) recs = {0};
    Logical *cur = NULL;
    size_t off, i;
    const char *e;
    if (len == 0) {
        *err = "goff: empty object";
        return NULL;
    }
    if (len % GOFF_RECORD_LEN) {
        *err = fail("goff: size %d is not a multiple of %d", (int)len, GOFF_RECORD_LEN);
        return NULL;
    }
    for (off = 0; off < len; off += GOFF_RECORD_LEN) {
        const uint8_t *r = b + off;
        int typ, is_con, more;
        if (r[0] != 0x03) {
            *err = fail("goff: record %d: bad prefix 0x%02X", (int)(off / GOFF_RECORD_LEN), r[0]);
            return NULL;
        }
        typ = r[1] >> 4;
        is_con = (r[1] & CONT_IS) != 0;
        more = (r[1] & CONT_NEXT) != 0;
        if (is_con) {
            if (!cur || cur->typ != typ || cur->complete) {
                *err = fail("goff: record %d: orphan continuation", (int)(off / GOFF_RECORD_LEN), 0);
                return NULL;
            }
            buf_write(&cur->payload, r + 3, GOFF_RECORD_LEN - 3);
        } else {
            cur = xcalloc(sizeof *cur);
            cur->typ = typ;
            cur->first = r;
            vec_push(&recs, cur);
        }
        if (!more) cur->complete = 1;
        if (typ == GOFF_T_END && !more) { /* end of the first module; more modules (debug) may follow */
            len = off + GOFF_RECORD_LEN;
            break;
        }
    }
    m->bytes = len;
    for (i = 0; i < recs.n; i++) {
        if (i == 0 && recs.v[i]->typ != GOFF_T_HDR) {
            *err = "goff: first record is not HDR";
            return NULL;
        }
        e = decode(m, recs.v[i]);
        if (e) {
            *err = e;
            return NULL;
        }
    }
    /* HLASM writes the ESD records of an object out of ESDID order (LD entries
     * after the ERs); the table is indexed by ESDID - 1 */
    qsort(m->esds.v, m->esds.n, sizeof m->esds.v[0], esd_cmp);
    for (i = 0; i < m->esds.n; i++) {
        if (m->esds.v[i]->id != i + 1) {
            *err = fail("ESDID %d missing or duplicated", (int)i + 1, 0);
            return NULL;
        }
    }
    m->logical = (int)recs.n;
    if (recs.n == 0 || recs.v[recs.n - 1]->typ != GOFF_T_END) {
        *err = "goff: last record is not END";
        return NULL;
    }
    return m;
}

const GoffESD *goff_module_esd(const GoffModule *m, uint32_t id) {
    if (id == 0 || id > m->esds.n) return NULL;
    return m->esds.v[id - 1];
}

void goff_dump(const GoffModule *m, FILE *w) {
    size_t i, k;
    fprintf(w, "HDR arch=%u\n", (unsigned)m->arch_level);
    for (i = 0; i < m->esds.n; i++) {
        const GoffESD *e = m->esds.v[i];
        char *nm = ebcdic_decode(e->name, e->name_len);
        uint8_t ba[10];
        goff_behavior_pack(&e->attr, ba);
        fprintf(w, "ESD %-2d id=%u parent=%u off=%u len=%d ns=%d amode=%d rmode=%d scope=%d exec=%d align=%d "
                   "ba=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X fill=%d,%u fl=%d%d%d q=%u pri=%u ada=%u ext=%u,%u \"%s\"\n",
                (int)e->type, (unsigned)e->id, (unsigned)e->parent, (unsigned)e->offset, (int)(int32_t)e->length,
                e->name_space, e->attr.amode, e->attr.rmode, e->attr.scope, e->attr.executable,
                e->attr.alignment, ba[0], ba[1], ba[2], ba[3], ba[4], ba[5], ba[6], ba[7], ba[8], ba[9],
                e->has_fill, (unsigned)e->fill, e->mangled, e->renameable, e->removable,
                (unsigned)e->reserved_qwords, (unsigned)e->priority, (unsigned)e->ada_id,
                (unsigned)e->ext_attr_id, (unsigned)e->ext_attr_off, nm);
        free(nm);
    }
    for (i = 0; i < m->texts.n; i++) {
        const GoffText *t = &m->texts.v[i];
        const GoffESD *te = goff_module_esd(m, t->esdid);
        char *tn = te ? ebcdic_decode(te->name, te->name_len) : NULL;
        int adata = tn && !strncmp(tn, "C_ADATA", 7);
        free(tn);
        fprintf(w, "TXT esd=%u off=%u len=%u", (unsigned)t->esdid, (unsigned)t->offset, (unsigned)t->len);
        if (!adata) /* symbolic information is shown by -dump as ZSYM */
            for (k = 0; k < t->len; k++) fprintf(w, " %02X", t->data[k]);
        fputc('\n', w);
    }
    for (i = 0; i < m->relocs.n; i++) {
        const GoffReloc *r = &m->relocs.v[i];
        fprintf(w, "RLD r=%u p=%u off=%u ref=%d referent=%d act=%d store=%s len=%d\n", (unsigned)r->rid,
                (unsigned)r->pid, (unsigned)r->offset, r->reference, r->referent, r->action,
                r->store ? "true" : "false", r->length);
    }
    for (i = 0; i < m->lens.n; i++)
        fprintf(w, "LEN esd=%u length=%u\n", (unsigned)m->lens.v[i].esdid, (unsigned)m->lens.v[i].length);
    {
        char *nm = ebcdic_decode(m->end.name, m->end.name_len);
        fprintf(w, "END kind=%d amode=%d esd=%u off=%u name=\"%s\" count=%u\n", (int)m->end.kind, m->end.amode,
                (unsigned)m->end.esdid, (unsigned)m->end.offset, nm, (unsigned)m->end_count);
        free(nm);
    }
}

static int same_esd(const GoffESD *a, const GoffESD *b) {
    uint8_t x[10], y[10];
    goff_behavior_pack(&a->attr, x);
    goff_behavior_pack(&b->attr, y);
    return a->type == b->type && a->id == b->id && a->parent == b->parent && a->offset == b->offset &&
           a->length == b->length && a->ext_attr_id == b->ext_attr_id && a->ext_attr_off == b->ext_attr_off &&
           a->name_space == b->name_space && a->has_fill == b->has_fill && a->fill == b->fill &&
           a->mangled == b->mangled && a->renameable == b->renameable && a->removable == b->removable &&
           a->reserved_qwords == b->reserved_qwords && a->ada_id == b->ada_id && a->priority == b->priority &&
           memcmp(x, y, sizeof x) == 0 && a->name_len == b->name_len && memcmp(a->name, b->name, a->name_len) == 0;
}

static int same_reloc(const GoffReloc *a, const GoffReloc *b) {
    return a->rid == b->rid && a->pid == b->pid && a->offset == b->offset && a->reference == b->reference &&
           a->referent == b->referent && a->action == b->action && a->store == b->store && a->length == b->length &&
           a->bit_length == b->bit_length && a->bit_offset == b->bit_offset && a->cond_seq == b->cond_seq &&
           a->amode_sens == b->amode_sens;
}

int goff_equal(const GoffModule *a, uint32_t skip_a, uint32_t nskip, const GoffModule *b) {
    size_t i, j;
    if (!skip_a) nskip = 0;
#define SKIPPED(ID) (nskip && (ID) >= skip_a && (ID) - skip_a < nskip)
    if (a->esds.n - nskip != b->esds.n) return 0;
    for (i = 0, j = 0; i < a->esds.n; i++) {
        if (SKIPPED(a->esds.v[i]->id)) continue;
        if (!same_esd(a->esds.v[i], b->esds.v[j++])) return 0;
    }
    for (i = 0, j = 0; i < a->texts.n; i++) {
        const GoffText *x = &a->texts.v[i], *y;
        if (SKIPPED(x->esdid)) continue;
        if (j >= b->texts.n) return 0;
        y = &b->texts.v[j++];
        if (x->esdid != y->esdid || x->offset != y->offset || x->style != y->style || x->encoding != y->encoding ||
            x->true_len != y->true_len || x->len != y->len || (x->len && memcmp(x->data, y->data, x->len) != 0))
            return 0;
    }
    if (j != b->texts.n || a->relocs.n != b->relocs.n || a->lens.n != b->lens.n) return 0;
    for (i = 0; i < a->relocs.n; i++)
        if (!same_reloc(&a->relocs.v[i], &b->relocs.v[i])) return 0;
    for (i = 0; i < a->lens.n; i++)
        if (a->lens.v[i].esdid != b->lens.v[i].esdid || a->lens.v[i].length != b->lens.v[i].length) return 0;
    return a->end.kind == b->end.kind && a->end.amode == b->end.amode && a->end.esdid == b->end.esdid &&
           a->end.offset == b->end.offset && a->end.name_len == b->end.name_len &&
           (!a->end.name_len || memcmp(a->end.name, b->end.name, a->end.name_len) == 0);
#undef SKIPPED
}
