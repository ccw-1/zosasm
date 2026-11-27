/* goffedit.c - semantic GOFF <-> JSON converter. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ebcdic.h"
#include "goff.h"
#include "json.h"
#include "util.h"

typedef struct {
    uint32_t id;
    const GoffESD *e;
    char *key;
} KMap;

typedef struct {
    uint32_t esd;
    uint32_t abs;
    int visible;
    char name[24];
} Anchor;

static char *esd_name(const GoffESD *e) {
    char *s = ebcdic_decode(e->name, e->name_len);
    return s ? s : xstrdup("");
}
static uint8_t *ebc_lit(const char *s) {
    size_t n = strlen(s), i;
    uint8_t *p = xmalloc(n + 1);
    for (i = 0; i < n; i++) p[i] = za_to_ebcdic[(unsigned char)s[i]];
    p[n] = 0;
    return p;
}

static char *type_name(int t) {
    switch (t) {
    case GOFF_SD: return "SD"; case GOFF_ED: return "ED"; case GOFF_LD: return "LD";
    case GOFF_PR: return "PR"; default: return "ER";
    }
}
static const char *style_name(int v) { return v == 1 ? "structured" : v == 2 ? "unstructured" : "byte"; }
static const char *ref_name(int v) {
    switch (v) { case GOFF_REF_OFFSET: return "offset"; case GOFF_REF_LENGTH: return "length";
                 case GOFF_REF_RELIMM: return "relimm"; case GOFF_REF_RTYPE: return "rtype";
                 case GOFF_REF_LONGDISP: return "longdisp"; default: return "address"; }
}
static const char *referent_name(int v) {
    return v == 1 ? "element" : v == 2 ? "class" : v == 3 ? "part" : "label";
}
static const char *action_name(int v) { return v == 1 ? "sub" : "add"; }

static void jc(Buf *b, int *first) { if (!*first) buf_putc(b, ','); *first = 0; }
static void jk(Buf *b, int *first, const char *k) { jc(b, first); json_put_cstr(b, k); buf_putc(b, ':'); }
static void jnum(Buf *b, int *first, const char *k, long v) { jk(b, first, k); buf_printf(b, "%ld", v); }
static void jstr(Buf *b, int *first, const char *k, const char *v) { jk(b, first, k); json_put_cstr(b, v ? v : ""); }
static void jhex(Buf *b, int *first, const char *k, const uint8_t *p, size_t n) {
    size_t i; char *s;
    jk(b, first, k);
    buf_putc(b, '"');
    s = xmalloc(n * 2 + 1);
    for (i = 0; i < n; i++) sprintf(s + i * 2, "%02X", p[i]);
    s[n * 2] = 0;
    buf_puts(b, s);
    free(s); buf_putc(b, '"');
}

static KMap *keys_of(const GoffModule *m, size_t *outn) {
    size_t i, j, n = m->esds.n;
    KMap *k = xcalloc(n * sizeof *k);
    for (i = 0; i < n; i++) {
        const GoffESD *e = m->esds.v[i];
        char *nm = esd_name(e);
        char *base = xasprintf("%s:%s", type_name(e->type), nm[0] ? nm : "?");
        char *key = xstrdup(base);
        for (j = 0; j < i; j++) if (!strcmp(k[j].key, key)) break;
        if (j < i) { free(key); key = xasprintf("%s#%lu", base, (unsigned long)i + 1); }
        free(base); free(nm);
        k[i].id = e->id; k[i].e = e; k[i].key = key;
    }
    *outn = n;
    return k;
}
static const char *key_of(KMap *k, size_t n, uint32_t id) {
    size_t i; for (i = 0; i < n; i++) if (k[i].id == id) return k[i].key; return NULL;
}

static Anchor *anchor_find(Anchor *a, size_t n, uint32_t esd, uint32_t abs) {
    size_t i; for (i = 0; i < n; i++) if (a[i].esd == esd && a[i].abs == abs) return &a[i]; return NULL;
}
static Anchor *anchor_add(Anchor **ap, size_t *np, uint32_t esd, uint32_t abs) {
    Anchor *x = anchor_find(*ap, *np, esd, abs);
    if (x) return x;
    *ap = xrealloc(*ap, (*np + 1) * sizeof **ap);
    x = &(*ap)[(*np)++];
    memset(x, 0, sizeof *x);
    x->esd = esd; x->abs = abs;
    snprintf(x->name, sizeof x->name, "a%lu", (unsigned long)*np);
    return x;
}
static void emit_offset(Buf *b, int *first, const char *k, KMap *km, size_t nk, Anchor *a, size_t na,
                        uint32_t esd, uint32_t off) {
    (void)km; (void)nk; (void)a; (void)na; (void)esd;
    jnum(b, first, k, (long)off);
}

static int printable_name(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    for (; *p; p++) if (*p < 0x20 || *p >= 0x7F) return 0;
    return 1;
}

static char *module_json(const GoffModule *m) {
    Buf b = {0};
    KMap *km; size_t nk, i, na = 0;
    Anchor *a = NULL;
    int f = 1;
    km = keys_of(m, &nk);
    /* anchors at ESD, RLD and END locations inside each text piece */
    for (i = 0; i < m->texts.n; i++) {
        const GoffText *t = &m->texts.v[i];
        uint32_t lo = t->offset, hi = t->offset + (uint32_t)t->len;
        size_t j, q;
        int idr_text = 0;
        for (q = 0; q < m->idrs.n; q++) if (m->idrs.v[q].esdid == t->esdid) idr_text = 1;
        if (idr_text) continue;
        for (j = 0; j < m->esds.n; j++) {
            const GoffESD *e = m->esds.v[j];
            if (e->type == GOFF_ED || e->type == GOFF_LD || e->type == GOFF_PR ||
                e->type == GOFF_ER || e->type == GOFF_SD) {
                if (e->offset >= lo && e->offset <= hi) anchor_add(&a, &na, t->esdid, e->offset);
            }
        }
        for (j = 0; j < m->relocs.n; j++) {
            const GoffReloc *r = &m->relocs.v[j];
            if (r->offset >= lo && r->offset <= hi)
                anchor_add(&a, &na, t->esdid, r->offset);
        }
        if (m->end.offset >= lo && m->end.offset <= hi)
            anchor_add(&a, &na, t->esdid, m->end.offset);
    }

    /* only anchors that the text output will actually contain are visible */
    for (i = 0; i < m->texts.n; i++) {
        const GoffText *t = &m->texts.v[i];
        uint32_t lo = t->offset, hi = t->offset + (uint32_t)t->len;
        size_t j, q;
        int idr_text = 0;
        for (q = 0; q < m->idrs.n; q++) if (m->idrs.v[q].esdid == t->esdid) idr_text = 1;
        if (idr_text || !key_of(km, nk, t->esdid)) continue;
        for (j = 0; j < na; j++)
            if (a[j].esd == t->esdid && a[j].abs >= lo && a[j].abs <= hi) a[j].visible = 1;
    }
    buf_putc(&b, '{');
    jnum(&b, &f, "format_version", 1);
    jk(&b, &f, "header"); buf_puts(&b, "{");
    { int hf = 1; jnum(&b, &hf, "architecture_level", m->arch_level); jnum(&b, &hf, "version", m->hdr_version);
      jhex(&b, &hf, "properties", m->props, m->props_len); }
    buf_putc(&b, '}');

    /* ESDs; omit the B_IDRL items when structured idrs are emitted */
    jk(&b, &f, "esds"); buf_putc(&b, '[');
    { int af = 1;
      for (i = 0; i < m->esds.n; i++) {
        const GoffESD *e = &*m->esds.v[i];
        char *nm = esd_name(e);
        int skip = m->idrs.n && e->type == GOFF_ED && !strcmp(nm, "B_IDRL");
        if (!skip) {
            int ef = 1;
            jc(&b, &af); buf_putc(&b, '{');
            jstr(&b, &ef, "key", km[i].key);
            jstr(&b, &ef, "type", type_name(e->type));
            jstr(&b, &ef, "parent", e->parent ? key_of(km, nk, e->parent) : NULL);
            jstr(&b, &ef, "name", nm);
            if (!printable_name(nm)) jhex(&b, &ef, "name_hex", e->name, e->name_len);
            jnum(&b, &ef, "namespace", e->name_space);
            emit_offset(&b, &ef, "offset", km, nk, a, na, e->id, e->offset);
            if ((e->type == GOFF_ED || e->type == GOFF_PR) && km[i].key) {
                uint32_t max = 0; size_t q;
                for (q = 0; q < m->texts.n; q++)
                    if (m->texts.v[q].esdid == e->id && m->texts.v[q].offset + m->texts.v[q].len > max)
                        max = m->texts.v[q].offset + (uint32_t)m->texts.v[q].len;
                if (max == e->length) { jk(&b, &ef, "length"); buf_puts(&b, "\"auto\""); }
                else jnum(&b, &ef, "length", (long)e->length);
            } else jnum(&b, &ef, "length", (long)e->length);
            jk(&b, &ef, "behavior"); buf_putc(&b, '{');
            { int bf = 1;
              jnum(&b, &bf, "amode", e->attr.amode); jnum(&b, &bf, "rmode", e->attr.rmode);
              jnum(&b, &bf, "text_style", e->attr.text_style);
              jnum(&b, &bf, "bind", e->attr.bind_algorithm);
              jnum(&b, &bf, "tasking", e->attr.tasking);
              jnum(&b, &bf, "read_only", e->attr.read_only);
              jnum(&b, &bf, "executable", e->attr.executable);
              jnum(&b, &bf, "duplicate_severity", e->attr.dup_severity);
              jnum(&b, &bf, "bind_strength", e->attr.bind_strength);
              jnum(&b, &bf, "loading", e->attr.loading);
              jnum(&b, &bf, "common", e->attr.common);
              jnum(&b, &bf, "indirect", e->attr.indirect);
              jnum(&b, &bf, "scope", e->attr.scope);
              jnum(&b, &bf, "linkage", e->attr.linkage);
              jnum(&b, &bf, "alignment", e->attr.alignment);
            }
            buf_putc(&b, '}');
            if (e->has_fill) { jk(&b, &ef, "fill"); buf_printf(&b, "%u", (unsigned)e->fill); }
            else { jk(&b, &ef, "fill"); buf_puts(&b, "null"); }
            jnum(&b, &ef, "mangled", e->mangled);
            jnum(&b, &ef, "renameable", e->renameable);
            jnum(&b, &ef, "removable", e->removable);
            jnum(&b, &ef, "reserved_qwords", e->reserved_qwords);
            jnum(&b, &ef, "ada_id", e->ada_id);
            jnum(&b, &ef, "priority", e->priority);
            if (e->ext_attr_id) {
                jk(&b, &ef, "extended_attribute"); buf_putc(&b, '{');
                { int xf = 1; jstr(&b, &xf, "esd", key_of(km, nk, e->ext_attr_id));
                  jnum(&b, &xf, "offset", e->ext_attr_off); }
                buf_putc(&b, '}');
            }
            buf_putc(&b, '}');
        }
        free(nm);
      }
    }
    buf_putc(&b, ']');

    /* text */
    jk(&b, &f, "text"); buf_putc(&b, '[');
    { int af = 1; uint32_t last[1024]; int seen[1024]; size_t q;
      memset(last, 0, sizeof last); memset(seen, 0, sizeof seen);
      for (i = 0; i < m->texts.n; i++) {
        const GoffText *t = &m->texts.v[i];
        const char *ek = key_of(km, nk, t->esdid);
        int tf = 1, skip = 0;
        for (q = 0; q < m->idrs.n; q++) if (m->idrs.v[q].esdid == t->esdid) skip = 1;
        if (skip || !ek) continue;
        jc(&b, &af); buf_putc(&b, '{');
        jstr(&b, &tf, "esd", ek);
        jstr(&b, &tf, "style", style_name(t->style));
        jnum(&b, &tf, "encoding", t->encoding);
        if (t->encoding == 0 && t->true_len == t->len) { jk(&b, &tf, "true_length"); buf_puts(&b, "\"auto\""); }
        else jnum(&b, &tf, "true_length", (long)t->true_len);
        if (t->esdid < 1024 && seen[t->esdid] && last[t->esdid] == t->offset) {
            /* contiguous: omit offset */
        } else jnum(&b, &tf, "offset", (long)t->offset);
        jk(&b, &tf, "segments"); buf_putc(&b, '[');
        { int sf = 1; uint32_t pos = t->offset, end = t->offset + (uint32_t)t->len; size_t x;
          for (x = 0; x < na; x++) {
            Anchor *an = &a[x];
            if (an->esd != t->esdid || an->abs < pos || an->abs > end) continue;
            if (an->abs > pos) {
                int hf = 1;
                jc(&b, &sf); buf_putc(&b, '{');
                jhex(&b, &hf, "hex", t->data + (pos - t->offset), an->abs - pos);
                buf_putc(&b, '}');
            }
            jc(&b, &sf); buf_putc(&b, '{'); { int zf = 1; jstr(&b, &zf, "anchor", an->name); } buf_putc(&b, '}');
            pos = an->abs;
          }
          if (pos < end) {
              int hf = 1;
              jc(&b, &sf); buf_putc(&b, '{');
              jhex(&b, &hf, "hex", t->data + (pos - t->offset), end - pos);
              buf_putc(&b, '}');
          }
        }
        buf_putc(&b, ']');
        if (t->esdid < 1024) { seen[t->esdid] = 1; last[t->esdid] = t->offset + (uint32_t)t->len; }
        buf_putc(&b, '}');
      }
    }
    buf_putc(&b, ']');

    /* relocations */
    jk(&b, &f, "relocs"); buf_putc(&b, '[');
    { int af = 1;
      for (i = 0; i < m->relocs.n; i++) {
        const GoffReloc *r = &m->relocs.v[i]; int rf = 1;
        jc(&b, &af); buf_putc(&b, '{');
        jstr(&b, &rf, "rid", key_of(km, nk, r->rid));
        jstr(&b, &rf, "pid", key_of(km, nk, r->pid));
        emit_offset(&b, &rf, "offset", km, nk, a, na, r->pid, r->offset);
        jstr(&b, &rf, "reference", ref_name(r->reference));
        jstr(&b, &rf, "referent", referent_name(r->referent));
        jstr(&b, &rf, "action", action_name(r->action));
        jnum(&b, &rf, "store", r->store); jnum(&b, &rf, "length", r->length);
        jnum(&b, &rf, "bit_length", r->bit_length); jnum(&b, &rf, "bit_offset", r->bit_offset);
        jnum(&b, &rf, "conditional", r->cond_seq); jnum(&b, &rf, "amode_sensitive", r->amode_sens);
        buf_putc(&b, '}');
      }
    }
    buf_putc(&b, ']');

    /* LEN */
    jk(&b, &f, "lengths"); buf_putc(&b, '[');
    { int af = 1; for (i = 0; i < m->lens.n; i++) { int lf = 1; jc(&b, &af); buf_putc(&b, '{');
        jstr(&b, &lf, "esd", key_of(km, nk, m->lens.v[i].esdid));
        jnum(&b, &lf, "length", (long)m->lens.v[i].length); buf_putc(&b, '}'); } }
    buf_putc(&b, ']');

    /* structured IDRs */
    jk(&b, &f, "idrs"); buf_putc(&b, '[');
    { int af = 1; for (i = 0; i < m->idrs.n; i++) { const GoffIdr *d = &m->idrs.v[i]; int df = 1;
        jc(&b, &af); buf_putc(&b, '{');
        jnum(&b, &df, "format", d->format);
        jstr(&b, &df, "translator", d->translator); jstr(&b, &df, "version", d->version);
        jstr(&b, &df, "release", d->release); jstr(&b, &df, "date", d->date);
        jstr(&b, &df, "time", d->time); buf_putc(&b, '}'); } }
    buf_putc(&b, ']');

    jk(&b, &f, "end"); buf_putc(&b, '{');
    { int ef = 1;
      jstr(&b, &ef, "kind", m->end.kind == 1 ? "by_id" : m->end.kind == 2 ? "by_name" : "none");
      if (m->end.kind == 1) { jstr(&b, &ef, "esd", key_of(km, nk, m->end.esdid));
          emit_offset(&b, &ef, "offset", km, nk, a, na, m->end.esdid, m->end.offset); }
      else if (m->end.kind == 2) { char *en = ebcdic_decode(m->end.name, m->end.name_len); jstr(&b, &ef, "name", en); free(en); }
      jnum(&b, &ef, "amode", m->end.amode);
      jk(&b, &ef, "record_count"); buf_puts(&b, "\"auto\"");
    }
    buf_putc(&b, '}');
    buf_putc(&b, '}');

    for (i = 0; i < nk; i++) free(km[i].key);
    free(km); free(a);
    buf_putc(&b, 0);
    return b.p;
}

/* ---- reverse helpers ---------------------------------------------------- */

typedef struct { char *key; uint32_t id; const Json *j; } RMap;
typedef struct { char *key; uint32_t offset; int has_offset; uint8_t style; uint16_t enc; uint32_t true_len;
                 int true_auto; Buf data; Vec(char *) aname; Vec(uint32_t) aoff; } RText;

static uint32_t rid(RMap *m, size_t n, const char *key) {
    size_t i; if (!key) return 0; for (i = 0; i < n; i++) if (!strcmp(m[i].key, key)) return m[i].id; return 0;
}
static uint32_t off_value(const Json *o, const char *k, RMap *m, size_t n, uint32_t *num, int *is_anchor) {
    (void)m; (void)n;
    Json *v = json_get(o, k); *is_anchor = 0;
    if (!v) return 0;
    if (v->type == JSON_NUM) { *num = (uint32_t)v->num; return *num; }
    if (v->type == JSON_OBJ) { const char *a = json_str(v, "anchor");
        if (a) { *is_anchor = 1; return 0; } }
    return 0;
}
static uint32_t anchor_offset(RText *t, const char *name, int *ok) {
    size_t i; for (i = 0; i < t->aname.n; i++) if (!strcmp(t->aname.v[i], name)) { *ok = 1; return t->aoff.v[i]; } *ok = 0; return 0;
}

static const char *rtype(const Json *j, const char *k, const char *d) { const char *s = json_str(j, k); return s ? s : d; }
static int ref_value(const char *s) {
    if (!strcmp(s, "offset")) return GOFF_REF_OFFSET;
    if (!strcmp(s, "length")) return GOFF_REF_LENGTH;
    if (!strcmp(s, "relimm")) return GOFF_REF_RELIMM;
    if (!strcmp(s, "rtype")) return GOFF_REF_RTYPE;
    if (!strcmp(s, "longdisp")) return GOFF_REF_LONGDISP;
    return GOFF_REF_ADDRESS;
}
static int refent_value(const char *s) {
    if (!strcmp(s, "element")) return GOFF_REFERENT_ELEMENT;
    if (!strcmp(s, "class")) return GOFF_REFERENT_CLASS;
    if (!strcmp(s, "part")) return GOFF_REFERENT_PART;
    return GOFF_REFERENT_LABEL;
}
static int hexdata(const char *s, Buf *b) {
    size_t i, n = s ? strlen(s) : 0;
    if (n % 2) return 0;
    for (i = 0; i < n; i += 2) { int hi = s[i], lo = s[i+1], a, c;
        a = hi >= '0' && hi <= '9' ? hi - '0' : hi >= 'a' && hi <= 'f' ? hi - 'a' + 10 : hi >= 'A' && hi <= 'F' ? hi - 'A' + 10 : -1;
        c = lo >= '0' && lo <= '9' ? lo - '0' : lo >= 'a' && lo <= 'f' ? lo - 'a' + 10 : lo >= 'A' && lo <= 'F' ? lo - 'A' + 10 : -1;
        if (a < 0 || c < 0) return 0;
        buf_putc(b, a * 16 + c);
    }
    return 1;
}
static uint8_t *name_bytes(const Json *e, size_t *n) {
    const char *h = json_str(e, "name_hex"); const char *s = json_str(e, "name");
    if (h) { Buf b = {0}; if (!hexdata(h, &b)) return NULL; *n = b.len; return (uint8_t *)b.p; }
    if (!s) s = "";
    { size_t l = strlen(s), i; uint8_t *p = xmalloc(l + 1); for (i = 0; i < l; i++) p[i] = za_to_ebcdic[(unsigned char)s[i]]; p[l] = 0; *n = l; return p; }
}

static void rmap_free(RMap *m, size_t n) { size_t i; for (i = 0; i < n; i++) free(m[i].key); free(m); }
static void rtext_free(RText *t, size_t n) { size_t i; for (i = 0; i < n; i++) { free(t[i].key); free(t[i].data.p); for (size_t j = 0; j < t[i].aname.n; j++) free(t[i].aname.v[j]); free(t[i].aname.v); free(t[i].aoff.v); } free(t); }

static const char *reverse_module(const Json *jm, Buf *out, char *err, size_t errsz) {
    Json *esds = json_get(jm, "esds"), *texts = json_get(jm, "text"), *relocs = json_get(jm, "relocs");
    Json *lens = json_get(jm, "lengths"), *idrs = json_get(jm, "idrs"), *end = json_get(jm, "end");
    RMap *map = NULL; size_t nm = 0, nt = 0, i, j;
    RText *tx = NULL;
    GoffWriter *w;
    Json *hdr = json_get(jm, "header");
    const uint8_t *props = NULL; uint8_t *pb = NULL; size_t np = 0;
    const char *e;

    if (!esds || esds->type != JSON_ARR) { snprintf(err, errsz, "module has no esds array"); return "bad esds"; }
    nm = esds->n; map = xcalloc((nm ? nm : 1) * sizeof *map);
    for (i = 0; i < nm; i++) {
        Json *e = esds->v[i]; const char *k = json_str(e, "key");
        if (!k) { snprintf(err, errsz, "ESD %lu has no key", (unsigned long)i); return "bad key"; }
        for (j = 0; j < i; j++) if (!strcmp(map[j].key, k)) { snprintf(err, errsz, "duplicate ESD key %s", k); return "bad key"; }
        map[i].key = xstrdup(k); map[i].id = (uint32_t)i + 1; map[i].j = e;
    }
    if (texts && texts->type == JSON_ARR) {
        nt = texts->n; tx = xcalloc((nt ? nt : 1) * sizeof *tx);
        for (i = 0; i < nt; i++) {
            Json *t = texts->v[i]; Json *seg = json_get(t, "segments"); uint32_t pos = 0;
            const char *start = json_str(t, "esd"); int have_off = 0; uint32_t offnum = 0;
            if (!start) { snprintf(err, errsz, "text %lu has no esd", (unsigned long)i); return "bad text"; }
            tx[i].key = xstrdup(start);
            if (json_get(t, "offset")) { off_value(t, "offset", map, nm, &offnum, &have_off); tx[i].offset = offnum; tx[i].has_offset = 1; }
            tx[i].style = !strcmp(rtype(t, "style", "byte"), "structured") ? 1 : !strcmp(rtype(t, "style", "byte"), "unstructured") ? 2 : 0;
            tx[i].enc = (uint16_t)json_int(t, "encoding", 0);
            { Json *tl = json_get(t, "true_length"); tx[i].true_auto = tl && tl->type == JSON_STR && !strcmp(tl->str, "auto"); tx[i].true_len = (uint32_t)json_int(t, "true_length", 0); }
            if (seg && seg->type == JSON_ARR) {
                for (j = 0; j < seg->n; j++) {
                    Json *s = seg->v[j]; const char *hx = json_str(s, "hex"), *an = json_str(s, "anchor");
                    if (hx) { Buf tmp = {0}; if (!hexdata(hx, &tmp)) { snprintf(err, errsz, "bad hex in text"); return "bad hex"; }
                        buf_write(&tx[i].data, tmp.p, tmp.len); pos += (uint32_t)tmp.len; free(tmp.p); }
                    else if (an) { vec_push(&tx[i].aname, xstrdup(an)); vec_push(&tx[i].aoff, pos); }
                }
            }
        }
    }
    /* resolve implicit text offsets */
    { uint32_t last[256]; int seen[256]; memset(last, 0, sizeof last); memset(seen, 0, sizeof seen);
      for (i = 0; i < nt; i++) { uint32_t id = rid(map, nm, tx[i].key);
        if (!tx[i].has_offset && id < 256 && seen[id]) tx[i].offset = last[id];
        if (id < 256) { seen[id] = 1; last[id] = tx[i].offset + (uint32_t)tx[i].data.len; } }
    }
    /* header */
    if (hdr) {
        const char *ph = json_str(hdr, "properties");
        if (ph) { Buf pbx = {0}; if (!hexdata(ph, &pbx)) { snprintf(err, errsz, "bad header properties"); return "bad props"; } pb = (uint8_t *)pbx.p; np = pbx.len; props = pb; }
    }
    w = goff_writer_new_header((uint32_t)json_int(hdr, "architecture_level", 1),
                               (uint8_t)json_int(hdr, "version", 0), props, np);
    free(pb);
    /* ESDs */
    for (i = 0; i < nm; i++) {
        const Json *ejson = map[i].j; GoffESD esd; const char *ty = rtype(ejson, "type", "ER");
        uint32_t off = 0; int isa = 0; const char *par = json_str(ejson, "parent");
        size_t nn = 0;
        memset(&esd, 0, sizeof esd);
        esd.type = !strcmp(ty, "SD") ? GOFF_SD : !strcmp(ty, "ED") ? GOFF_ED : !strcmp(ty, "LD") ? GOFF_LD : !strcmp(ty, "PR") ? GOFF_PR : GOFF_ER;
        esd.parent = par ? rid(map, nm, par) : 0;
        esd.name = name_bytes(ejson, &nn);
        if (!esd.name) { snprintf(err, errsz, "bad ESD name"); return "bad name"; }
        esd.name_len = nn;
        esd.name_space = (uint8_t)json_int(ejson, "namespace", 1);
        off_value(ejson, "offset", map, nm, &off, &isa);
        if (isa) {
            Json *oj = json_get(ejson, "offset");
            const char *an = json_str(oj, "anchor");
            size_t q;
            for (q = 0; q < nt; q++) {
                int ok = 0; uint32_t a = anchor_offset(&tx[q], an, &ok);
                if (ok) { off = tx[q].offset + a; break; }
            }
        }
        esd.offset = off;
        { Json *lj = json_get(ejson, "length"); size_t q;
          if (lj && lj->type == JSON_STR && !strcmp(lj->str, "auto")) {
              uint32_t max = 0; for (q = 0; q < nt; q++) if (!strcmp(tx[q].key, map[i].key) && tx[q].offset + tx[q].data.len > max) max = tx[q].offset + (uint32_t)tx[q].data.len;
              esd.length = max;
          } else esd.length = (uint32_t)json_int(ejson, "length", 0);
        }
        { Json *bh = json_get(ejson, "behavior"); int bf = 1;
          if (bh && bh->type == JSON_OBJ) {
              esd.attr.amode = (uint8_t)json_int(bh, "amode", 0); esd.attr.rmode = (uint8_t)json_int(bh, "rmode", 0);
              esd.attr.text_style = (uint8_t)json_int(bh, "text_style", 0); esd.attr.bind_algorithm = (uint8_t)json_int(bh, "bind", 0);
              esd.attr.tasking = (uint8_t)json_int(bh, "tasking", 0); esd.attr.read_only = (int)json_int(bh, "read_only", 0);
              esd.attr.executable = (uint8_t)json_int(bh, "executable", 0); esd.attr.dup_severity = (uint8_t)json_int(bh, "duplicate_severity", 0);
              esd.attr.bind_strength = (uint8_t)json_int(bh, "bind_strength", 0); esd.attr.loading = (uint8_t)json_int(bh, "loading", 0);
              esd.attr.common = (int)json_int(bh, "common", 0); esd.attr.indirect = (int)json_int(bh, "indirect", 0);
              esd.attr.scope = (uint8_t)json_int(bh, "scope", 0); esd.attr.linkage = (uint8_t)json_int(bh, "linkage", 0);
              esd.attr.alignment = (uint8_t)json_int(bh, "alignment", 0);
              (void)bf;
          }
        }
        { Json *fj = json_get(ejson, "fill"); if (fj && fj->type == JSON_NUM) { esd.has_fill = 1; esd.fill = (uint8_t)fj->num; } }
        esd.mangled = (int)json_int(ejson, "mangled", 0); esd.renameable = (int)json_int(ejson, "renameable", 0);
        esd.removable = (int)json_int(ejson, "removable", 0); esd.reserved_qwords = (uint8_t)json_int(ejson, "reserved_qwords", 0);
        esd.ada_id = (uint32_t)json_int(ejson, "ada_id", 0); esd.priority = (uint32_t)json_int(ejson, "priority", 0);
        { Json *x = json_get(ejson, "extended_attribute"); if (x) { const char *xk = json_str(x, "esd"); esd.ext_attr_id = rid(map, nm, xk); esd.ext_attr_off = (uint32_t)json_int(x, "offset", 0); } }
        {
            const char *we = goff_write_esd(w, &esd);
            free(esd.name);
            if (we) { snprintf(err, errsz, "%s", we); return "write ESD"; }
        }
    }
    /* text */
    for (i = 0; i < nt; i++) {
        uint32_t id = rid(map, nm, tx[i].key);
        uint32_t tl = tx[i].true_auto && tx[i].enc == 0 ? (uint32_t)tx[i].data.len : tx[i].true_len;
        if (!id) { snprintf(err, errsz, "text ESD key %s does not resolve", tx[i].key); return "bad text ref"; }
        e = goff_write_text_full(w, id, tx[i].offset, tx[i].style, tx[i].enc, tl,
                                 (const uint8_t *)tx[i].data.p, tx[i].data.len);
        if (e) { snprintf(err, errsz, "%s", e); return "write text"; }
    }
    /* relocations */
    if (relocs && relocs->type == JSON_ARR) {
        GoffReloc *rl = xcalloc((relocs->n ? relocs->n : 1) * sizeof *rl);
        for (i = 0; i < relocs->n; i++) {
            Json *r = relocs->v[i]; uint32_t off = 0; int isa = 0;
            const char *rk = json_str(r, "rid"), *pk = json_str(r, "pid");
            const char *rr = rtype(r, "reference", "address"), *re = rtype(r, "referent", "label");
            memset(&rl[i], 0, sizeof rl[i]);
            rl[i].rid = rid(map, nm, rk); rl[i].pid = rid(map, nm, pk);
            if (json_get(r, "offset")) { off_value(r, "offset", map, nm, &off, &isa);
                if (isa) { const char *an = json_str(json_get(r, "offset"), "anchor"); int ok = 0; size_t q;
                    for (q = 0; q < nt; q++) {
                        uint32_t a = anchor_offset(&tx[q], an, &ok);
                        if (ok) { off = tx[q].offset + a; break; }
                    }
                    if (!ok) { size_t z; fprintf(stderr, "UNRESOLVED %s\n", an); for (z=0; z<nt; z++) { size_t w; for (w=0; w<tx[z].aname.n; w++) if (!strcmp(tx[z].aname.v[w], an)) fprintf(stderr, "  found in tx %lu key=%s off=%u aoff=%u\n", (unsigned long)z, tx[z].key, tx[z].offset, tx[z].aoff.v[w]); } }
                    }
                rl[i].offset = off; }
            rl[i].reference = (uint8_t)ref_value(rr); rl[i].referent = (uint8_t)refent_value(re);
            rl[i].action = !strcmp(rtype(r, "action", "add"), "sub") ? GOFF_ACTION_SUB : GOFF_ACTION_ADD;
            rl[i].store = (int)json_int(r, "store", 0); rl[i].length = (uint8_t)json_int(r, "length", 4);
            rl[i].bit_length = (uint8_t)json_int(r, "bit_length", 0); rl[i].bit_offset = (uint8_t)json_int(r, "bit_offset", 0);
            rl[i].cond_seq = (int)json_int(r, "conditional", 0); rl[i].amode_sens = (int)json_int(r, "amode_sensitive", 0);
        }
        e = goff_write_rld(w, rl, relocs->n); free(rl);
        if (e) { snprintf(err, errsz, "%s", e); return "write RLD"; }
    }
    /* LEN */
    if (lens && lens->type == JSON_ARR && lens->n) {
        GoffDeferredLen *dl = xcalloc(lens->n * sizeof *dl);
        for (i = 0; i < lens->n; i++) { Json *x = lens->v[i]; const char *k = json_str(x, "esd");
            dl[i].esdid = rid(map, nm, k); dl[i].length = (uint32_t)json_int(x, "length", 0); }
        e = goff_write_len(w, dl, lens->n); free(dl);
        if (e) { snprintf(err, errsz, "%s", e); return "write LEN"; }
    }
    /* IDR */
    if (idrs && idrs->type == JSON_ARR && idrs->n) {
        uint32_t parent = 0; for (i = 0; i < nm; i++) if (map[i].j && !strcmp(rtype(map[i].j, "type", ""), "SD")) { parent = map[i].id; break; }
        for (i = 0; i < idrs->n; i++) {
            Json *x = idrs->v[i]; int fmt = (int)json_int(x, "format", 1);
            const char *tr = rtype(x, "translator", ""), *ve = rtype(x, "version", ""), *re = rtype(x, "release", "");
            const char *da = rtype(x, "date", ""), *ti = rtype(x, "time", "");
            uint8_t data[96]; size_t n = 0, q; GoffESD ed;
            memset(data, 0x40, sizeof data);
            memset(data, 0, 4);
            if (fmt == 3) { n = 30; data[3] = 30; }
            else { n = 19; data[3] = 19; }
            for (q = 0; q < 10 && tr[q]; q++) data[4 + q] = za_to_ebcdic[(unsigned char)tr[q]];
            for (q = 0; q < 2 && ve[q]; q++) data[14 + q] = za_to_ebcdic[(unsigned char)ve[q]];
            for (q = 0; q < 2 && re[q]; q++) data[16 + q] = za_to_ebcdic[(unsigned char)re[q]];
            if (fmt == 3) {
                for (q = 0; q < 7 && da[q]; q++) data[18 + q] = za_to_ebcdic[(unsigned char)da[q]];
                for (q = 0; q < 9 && ti[q]; q++) data[25 + q] = za_to_ebcdic[(unsigned char)ti[q]];
            } else {
                for (q = 0; q < 5 && da[q]; q++) data[18 + q] = za_to_ebcdic[(unsigned char)da[q]];
            }
            memset(&ed, 0, sizeof ed); ed.type = GOFF_ED; ed.parent = parent;
            ed.name = ebc_lit("B_IDRL"); ed.name_len = 6; ed.name_space = 1;
            ed.length = (uint32_t)n + 4; ed.attr.text_style = GOFF_STYLE_STRUCTURED;
            e = goff_write_esd(w, &ed); free(ed.name);
            if (e) { snprintf(err, errsz, "%s", e); return "write IDR ESD"; }
            data[2] = 0; data[3] = (uint8_t)n;
            e = goff_write_text(w, ed.id, 0, GOFF_STYLE_STRUCTURED, data, n + 4);
            if (e) { snprintf(err, errsz, "%s", e); return "write IDR text"; }
        }
    }
    /* END */
    { GoffEnd ge; const char *kind = end ? rtype(end, "kind", "none") : "none"; memset(&ge, 0, sizeof ge);
      if (end) { ge.amode = (uint8_t)json_int(end, "amode", 0);
        if (!strcmp(kind, "by_id")) {
            uint32_t eo = (uint32_t)json_int(end, "offset", 0);
            Json *oj = json_get(end, "offset");
            const char *an = oj && oj->type == JSON_OBJ ? json_str(oj, "anchor") : NULL;
            if (an) { size_t q;
                for (q = 0; q < nt; q++) {
                    int ok = 0; uint32_t a = anchor_offset(&tx[q], an, &ok);
                    if (ok) { eo = tx[q].offset + a; break; }
                }
            }
            ge.kind = GOFF_ENTRY_BYID; ge.esdid = rid(map, nm, json_str(end, "esd")); ge.offset = eo;
        }
        else if (!strcmp(kind, "by_name")) { const char *nm2 = json_str(end, "name"); ge.kind = GOFF_ENTRY_BYNAME; if (nm2) { size_t l = strlen(nm2), q; ge.name = xmalloc(l); for (q = 0; q < l; q++) ge.name[q] = za_to_ebcdic[(unsigned char)nm2[q]]; ge.name_len = l; } }
      }
      {
          const uint8_t *endp;
          size_t endn;
          e = goff_write_end(w, &ge, &endp, &endn);
          free(ge.name);
          if (e) { snprintf(err, errsz, "%s", e); return "write END"; }
          buf_write(out, w->buf.p, w->buf.len);
      }
    }
    goff_writer_free(w); rmap_free(map, nm); rtext_free(tx, nt);
    return NULL;
}

static const char *reverse_file(const Json *root, Buf *out, char *err, size_t errsz) {
    Json *mods = json_get(root, "modules"); size_t i;
    if (!mods || mods->type != JSON_ARR) { snprintf(err, errsz, "no modules array"); return "bad modules"; }
    for (i = 0; i < mods->n; i++) { const char *e = reverse_module(mods->v[i], out, err, errsz); if (e) return e; }
    return NULL;
}

#ifndef GOFFEDIT_NO_MAIN
int main(int argc, char **argv) {
    const char *in = NULL, *outname = NULL; int rev = 0, check = 0, i;
    size_t n = 0; char *data; Buf out = {0}; const char *e;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r")) { rev = 1; continue; }
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { outname = argv[++i]; continue; }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { fprintf(stdout, "usage: goffedit [-r] [-o FILE] FILE\n"); return 0; }
        if (!strcmp(argv[i], "--compact")) continue;
        if (!strcmp(argv[i], "--check")) { check = 1; continue; }
        if (argv[i][0] == '-') { fprintf(stderr, "goffedit: unknown option %s\n", argv[i]); return 2; }
        if (in) { fprintf(stderr, "goffedit: more than one input\n"); return 2; }
        in = argv[i];
    }
    if (!in) { fprintf(stderr, "goffedit: missing input\n"); return 2; }
    if (!outname) outname = xasprintf("%s.%s", in, rev ? "goff" : "json");
    data = read_file(in, &n);
    if (!data) { fprintf(stderr, "goffedit: cannot read %s\n", in); return 1; }
    if (rev) {
        Json *j; const char *je = NULL; char err[256];
        j = json_parse(data, n, &je);
        if (!j) { fprintf(stderr, "goffedit: %s: %s\n", in, je ? je : "invalid JSON"); free(data); return 1; }
        e = reverse_file(j, &out, err, sizeof err);
        json_free(j);
        if (e) { fprintf(stderr, "goffedit: %s: %s\n", in, e); free(data); free(out.p); return 1; }
    } else {
        const char *pe = NULL; GoffFile *f = goff_parse_all((const uint8_t *)data, n, &pe);
        if (!f) { fprintf(stderr, "goffedit: %s: %s\n", in, pe ? pe : "bad GOFF"); free(data); return 1; }
        buf_puts(&out, "{\"format\":\"goffedit\",\"version\":1,\"modules\":[");
        for (i = 0; i < (int)f->modules.n; i++) { char *mj = module_json(f->modules.v[i]); if (i) buf_putc(&out, ','); buf_write(&out, mj, strlen(mj)); free(mj); }
        buf_puts(&out, "]}\n");
        goff_file_free(f);
    }
    if (check) {
        fprintf(stderr, "goffedit: check ok (%lu bytes)\n", (unsigned long)out.len);
        free(data); free(out.p); return 0;
    }
    { FILE *fp = fopen(outname, "wb"); if (!fp || fwrite(out.p, 1, out.len, fp) != out.len) { fprintf(stderr, "goffedit: cannot write %s\n", outname); if (fp) fclose(fp); free(data); free(out.p); return 1; } fclose(fp); }
    fprintf(stderr, "goffedit: wrote %s\n", outname);
    free(data); free(out.p); return 0;
}
#endif
