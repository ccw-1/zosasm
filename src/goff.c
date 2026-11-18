#include "goff.h"

#include <stdlib.h>
#include <string.h>

#include "ebcdic.h"

#define CONT_NEXT 0x1
#define CONT_IS 0x2
#define CON_PAYLOAD (GOFF_RECORD_LEN - 3)
#define MAX_RLD 32000

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void goff_behavior_pack(const GoffBehavior *b, uint8_t o[10]) {
    memset(o, 0, 10);
    o[0] = b->amode;
    o[1] = b->rmode;
    o[2] = (uint8_t)((b->text_style & 0xF) << 4 | (b->bind_algorithm & 0xF));
    o[3] = (uint8_t)((b->tasking & 7) << 5 | (b->read_only ? 1 : 0) << 3 | (b->executable & 7));
    o[4] = (uint8_t)((b->dup_severity & 3) << 4 | (b->bind_strength & 0xF));
    o[5] = (uint8_t)((b->loading & 3) << 6 | (b->common ? 1 : 0) << 5 | (b->indirect ? 1 : 0) << 4 |
                     (b->scope & 0xF));
    o[6] = (uint8_t)((b->linkage & 1) << 5 | (b->alignment & 0x1F));
}

void goff_behavior_unpack(const uint8_t o[10], GoffBehavior *b) {
    memset(b, 0, sizeof *b);
    b->amode = o[0];
    b->rmode = o[1];
    b->text_style = o[2] >> 4;
    b->bind_algorithm = o[2] & 0xF;
    b->tasking = o[3] >> 5;
    b->read_only = (o[3] & 0x08) != 0;
    b->executable = o[3] & 7;
    b->dup_severity = (o[4] >> 4) & 3;
    b->bind_strength = o[4] & 0xF;
    b->loading = o[5] >> 6;
    b->common = (o[5] & 0x20) != 0;
    b->indirect = (o[5] & 0x10) != 0;
    b->scope = o[5] & 0xF;
    b->linkage = (o[6] >> 5) & 1;
    b->alignment = o[6] & 0x1F;
}

/* ---- writer ---------------------------------------------------------- */

GoffWriter *goff_writer_new(uint32_t arch_level) {
    GoffWriter *w = xcalloc(sizeof *w);
    uint8_t r[GOFF_RECORD_LEN] = {0};
    r[0] = 0x03;
    r[1] = (uint8_t)(GOFF_T_HDR << 4);
    put32(r + 48, arch_level);
    buf_write(&w->buf, r, sizeof r);
    w->logical++;
    return w;
}

/* One logical record: PTV type, a fixed header filling the start of the
 * first physical record, and a payload continuing into as many
 * continuation records as needed. */
static void emit(GoffWriter *w, int typ, const uint8_t *header, size_t hlen, const uint8_t *payload,
                 size_t plen) {
    size_t room = GOFF_RECORD_LEN - 3 - hlen;
    int first = 1;
    for (;;) {
        uint8_t r[GOFF_RECORD_LEN] = {0};
        size_t n;
        uint8_t b1;
        r[0] = 0x03;
        if (first) {
            memcpy(r + 3, header, hlen);
            n = plen > room ? room : plen;
            if (n) memcpy(r + 3 + hlen, payload, n);
        } else {
            n = plen > CON_PAYLOAD ? CON_PAYLOAD : plen;
            if (n) memcpy(r + 3, payload, n);
        }
        payload += n;
        plen -= n;
        b1 = (uint8_t)(typ << 4);
        if (!first) b1 |= CONT_IS;
        if (plen > 0) b1 |= CONT_NEXT;
        r[1] = b1;
        buf_write(&w->buf, r, sizeof r);
        first = 0;
        if (plen == 0) break;
    }
    w->logical++;
}

const char *goff_write_esd(GoffWriter *w, GoffESD *e) {
    uint8_t h[72 - 3];
    uint8_t ba[10];
    uint8_t fl = 0;
    if (w->ended) return "goff: write after END";
    if (e->name_len == 0) return "goff: ESD with empty name";
    if (e->name_len > 32767) return "goff: ESD name too long";
    w->nesd++;
    e->id = w->nesd;
    memset(h, 0, sizeof h);
    h[0] = (uint8_t)e->type;
    put32(h + 4 - 3, e->id);
    put32(h + 8 - 3, e->parent);
    put32(h + 16 - 3, e->offset);
    put32(h + 24 - 3, e->length);
    put32(h + 28 - 3, e->ext_attr_id);
    put32(h + 32 - 3, e->ext_attr_off);
    h[40 - 3] = e->name_space;
    if (e->has_fill) {
        fl |= 0x80;
        h[42 - 3] = e->fill;
    }
    if (e->mangled) fl |= 0x40;
    if (e->renameable) fl |= 0x20;
    if (e->removable) fl |= 0x10;
    fl |= e->reserved_qwords & 7;
    h[41 - 3] = fl;
    put32(h + 44 - 3, e->ada_id);
    put32(h + 48 - 3, e->priority);
    goff_behavior_pack(&e->attr, ba);
    memcpy(h + 60 - 3, ba, 10);
    put16(h + 70 - 3, (uint32_t)e->name_len);
    emit(w, GOFF_T_ESD, h, sizeof h, e->name, e->name_len);
    return NULL;
}

const char *goff_write_text(GoffWriter *w, uint32_t esdid, uint32_t offset, uint8_t style,
                            const uint8_t *data, size_t n) {
    if (w->ended) return "goff: write after END";
    while (n > 0) {
        size_t k = n > GOFF_MAX_TEXT ? GOFF_MAX_TEXT : n;
        uint8_t h[24 - 3];
        memset(h, 0, sizeof h);
        h[0] = style & 0xF;
        put32(h + 4 - 3, esdid);
        put32(h + 12 - 3, offset);
        put16(h + 22 - 3, (uint32_t)k);
        emit(w, GOFF_T_TXT, h, sizeof h, data, k);
        data += k;
        n -= k;
        offset += (uint32_t)k;
    }
    return NULL;
}

/* encode one RLD item; sequencing bits compare against prev */
static void reloc_bytes(Buf *out, const GoffReloc *r, const GoffReloc *prev) {
    uint8_t fl[8] = {0}, t[4];
    int same_r = prev && r->rid == prev->rid;
    int same_p = prev && r->pid == prev->pid;
    int same_o = prev && r->offset == prev->offset;
    if (same_r) fl[0] |= 0x80;
    if (same_p) fl[0] |= 0x40;
    if (same_o) fl[0] |= 0x20;
    if (r->amode_sens) fl[0] |= 0x01;
    fl[1] = (uint8_t)((r->reference & 0xF) << 4 | (r->referent & 0xF));
    fl[2] = (uint8_t)((r->action & 0x7F) << 1);
    if (r->store) fl[2] |= 1;
    fl[4] = r->length;
    fl[5] = (uint8_t)((r->bit_length & 7) << 5 | (r->cond_seq ? 1 : 0) << 4 | (r->bit_offset & 7));
    buf_write(out, fl, 8);
    if (!same_r) {
        put32(t, r->rid);
        buf_write(out, t, 4);
    }
    if (!same_p) {
        put32(t, r->pid);
        buf_write(out, t, 4);
    }
    if (!same_o) {
        put32(t, r->offset);
        buf_write(out, t, 4);
    }
}

int goff_reloc_is(const GoffReloc *r, const GoffRelocPat *p) {
    if (r->reference != p->reference || r->store != p->store) return 0;
    if (p->referent == GOFF_PAT_PART_OR_CLASS) {
        if (r->referent != GOFF_REFERENT_CLASS && r->referent != GOFF_REFERENT_PART) return 0;
    } else if (r->referent != p->referent) {
        return 0;
    }
    if (p->length) {
        if (r->length != p->length || r->bit_length != p->bit_length || r->bit_offset != p->bit_offset)
            return 0;
    } else if (r->bit_length || r->bit_offset) {
        return 0;
    }
    if (p->lenset && (r->length > 15 || !(p->lenset & (1u << r->length)))) return 0;
    return 1;
}

const char *goff_write_rld(GoffWriter *w, const GoffReloc *items, size_t n) {
    Buf data = {0};
    const GoffReloc *prev = NULL;
    size_t i;
    if (w->ended) return "goff: write after END";
#define FLUSH()                                                               \
    do {                                                                      \
        if (data.len) {                                                       \
            uint8_t h[6 - 3] = {0};                                           \
            put16(h + 4 - 3, (uint32_t)data.len);                             \
            emit(w, GOFF_T_RLD, h, sizeof h, (const uint8_t *)data.p, data.len); \
            data.len = 0;                                                     \
            prev = NULL;                                                      \
        }                                                                     \
    } while (0)
    for (i = 0; i < n; i++) {
        Buf one = {0};
        reloc_bytes(&one, &items[i], prev);
        if (data.len + one.len > MAX_RLD) {
            FLUSH();
            one.len = 0;
            reloc_bytes(&one, &items[i], NULL);
        }
        buf_write(&data, one.p, one.len);
        free(one.p);
        prev = &items[i];
    }
    FLUSH();
#undef FLUSH
    free(data.p);
    return NULL;
}

const char *goff_write_len(GoffWriter *w, const GoffDeferredLen *items, size_t n) {
    const size_t per_rec = (GOFF_RECORD_LEN - 8) / 12;
    if (w->ended) return "goff: write after END";
    while (n > 0) {
        size_t k = n > per_rec ? per_rec : n, i;
        uint8_t h[8 - 3] = {0}, p[12 * 6];
        put16(h + 6 - 3, (uint32_t)(k * 12));
        memset(p, 0, sizeof p);
        for (i = 0; i < k; i++) {
            put32(p + i * 12, items[i].esdid);
            put32(p + i * 12 + 8, items[i].length);
        }
        emit(w, GOFF_T_LEN, h, sizeof h, p, k * 12);
        items += k;
        n -= k;
    }
    return NULL;
}

const char *goff_write_end(GoffWriter *w, const GoffEnd *e, const uint8_t **out, size_t *len) {
    uint8_t h[26 - 3] = {0};
    if (w->ended) return "goff: END written twice";
    w->ended = 1;
    h[0] = (uint8_t)(e->kind & 3);
    h[4 - 3] = e->amode;
    put32(h + 8 - 3, w->logical + 1);
    if (e->kind == GOFF_ENTRY_BYID) {
        put32(h + 12 - 3, e->esdid);
        put32(h + 20 - 3, e->offset);
    } else if (e->kind == GOFF_ENTRY_BYNAME) {
        if (e->name_len == 0) return "goff: entry by name without a name";
        put16(h + 24 - 3, (uint32_t)e->name_len);
    }
    emit(w, GOFF_T_END, h, sizeof h, e->kind == GOFF_ENTRY_BYNAME ? e->name : NULL,
         e->kind == GOFF_ENTRY_BYNAME ? e->name_len : 0);
    *out = (const uint8_t *)w->buf.p;
    *len = w->buf.len;
    return NULL;
}
