/* dbg_dwarf.c - the DWARF sections of -g.  The shape follows what z/OS
 * /bin/as -g writes for HLASM (language 0xDE00, the IBM .debug_srcfiles units,
 * the 0xDE line opcode); see DESIGN.md 21. */
#include <stdlib.h>
#include <string.h>

#include "dbg_int.h"

enum { AB_CU = 1, AB_BASE, AB_ARRAY, AB_SUBRANGE, AB_SUBPROGRAM, AB_LABEL, AB_VARIABLE };

static void ab_start(Buf *b, unsigned code, unsigned tag, int children) {
    duleb(b, code);
    duleb(b, tag);
    d8(b, children);
}
static void ab_attr(Buf *b, unsigned at, unsigned form) {
    duleb(b, at);
    duleb(b, form);
}
static void ab_end(Buf *b) {
    d8(b, 0);
    d8(b, 0);
}

/* DW_AT_* and DW_FORM_* used here */
enum {
    AT_NAME = 0x03, AT_LOCATION = 0x02, AT_STMT_LIST = 0x10, AT_LOW_PC = 0x11, AT_HIGH_PC = 0x12,
    AT_LANGUAGE = 0x13, AT_COMP_DIR = 0x1b, AT_PRODUCER = 0x25, AT_UPPER_BOUND = 0x2f, AT_ENCODING = 0x3e,
    AT_BYTE_SIZE = 0x0b, AT_EXTERNAL = 0x3f, AT_TYPE = 0x49, AT_IBM_SRCFILE = 0x390f, AT_IBM_FILE_DIR = 0x3910,
    AT_IBM_FILE_A = 0x3918, AT_IBM_FILE_B = 0x3915
};
enum { F_ADDR = 0x01, F_DATA2 = 0x05, F_DATA4 = 0x06, F_DATA1 = 0x0b, F_STRING = 0x08, F_REF4 = 0x13,
       F_SEC_OFFSET = 0x17, F_EXPRLOC = 0x18, F_FLAG_PRESENT = 0x19 };

#define TAG_IBM_DIR 0xde15
#define TAG_IBM_FILE 0xde10

static uint32_t ab_dir_off, ab_file_off;

static void build_abbrev(Dbg *g) {
    Buf *b = &g->sec[SEC_ABBREV].d;
    ab_start(b, AB_CU, 0x11, 1);
    ab_attr(b, AT_LANGUAGE, F_DATA2);
    ab_attr(b, AT_PRODUCER, F_STRING);
    ab_attr(b, AT_COMP_DIR, F_STRING);
    ab_attr(b, AT_NAME, F_STRING);
    ab_attr(b, AT_IBM_SRCFILE, F_SEC_OFFSET);
    ab_attr(b, AT_LOW_PC, F_ADDR);
    ab_attr(b, AT_HIGH_PC, F_ADDR);
    if (g->nsub) ab_attr(b, AT_STMT_LIST, F_SEC_OFFSET);
    ab_end(b);
    ab_start(b, AB_BASE, 0x24, 0);
    ab_attr(b, AT_NAME, F_STRING);
    ab_attr(b, AT_ENCODING, F_DATA1);
    ab_attr(b, AT_BYTE_SIZE, F_DATA1);
    ab_end(b);
    ab_start(b, AB_ARRAY, 0x01, 1);
    ab_attr(b, AT_TYPE, F_REF4);
    ab_end(b);
    ab_start(b, AB_SUBRANGE, 0x21, 0);
    ab_attr(b, AT_UPPER_BOUND, F_DATA4);
    ab_end(b);
    ab_start(b, AB_SUBPROGRAM, 0x2e, 1);
    ab_attr(b, AT_NAME, F_STRING);
    ab_attr(b, AT_EXTERNAL, F_FLAG_PRESENT);
    ab_attr(b, AT_LOW_PC, F_ADDR);
    ab_attr(b, AT_HIGH_PC, F_ADDR);
    ab_end(b);
    ab_start(b, AB_LABEL, 0x0a, 0);
    ab_attr(b, AT_NAME, F_STRING);
    ab_attr(b, AT_LOW_PC, F_ADDR);
    ab_end(b);
    ab_start(b, AB_VARIABLE, 0x34, 0);
    ab_attr(b, AT_NAME, F_STRING);
    ab_attr(b, AT_TYPE, F_REF4);
    ab_attr(b, AT_LOCATION, F_EXPRLOC);
    ab_end(b);
    d8(b, 0);
    /* the units of .debug_srcfiles: a directory and a file */
    ab_dir_off = (uint32_t)b->len;
    ab_start(b, 1, TAG_IBM_DIR, 0);
    ab_attr(b, AT_NAME, F_STRING);
    ab_end(b);
    d8(b, 0);
    ab_file_off = (uint32_t)b->len;
    ab_start(b, 1, TAG_IBM_FILE, 0);
    ab_attr(b, AT_NAME, F_STRING);
    ab_attr(b, AT_IBM_FILE_A, F_DATA1);
    ab_attr(b, AT_LANGUAGE, F_DATA2);
    ab_attr(b, AT_IBM_FILE_DIR, F_SEC_OFFSET);
    ab_attr(b, AT_IBM_FILE_B, F_DATA1);
    ab_end(b);
    d8(b, 0);
}

/* .debug_srcfiles: one unit per directory, then one per source file */
static void build_srcfiles(Dbg *g) {
    Buf *b = &g->sec[SEC_SRCFILES].d;
    size_t i;
    for (i = 0; i < g->dirs.n; i++) {
        size_t n = strlen(g->dirs.v[i].path);
        size_t start = b->len;
        d32(b, (uint32_t)(n + 10));
        d16(b, 4);
        d32rel(g, SEC_SRCFILES, RK_SEC, SEC_ABBREV, ab_dir_off);
        d8(b, 8);
        d8(b, 1);
        dstr(b, g->dirs.v[i].path);
        d8(b, 0);
        g->dirs.v[i].off = (uint32_t)(start + 11);
    }
    for (i = 0; i < g->files.n; i++) {
        DFile *f = &g->files.v[i];
        size_t n = strlen(f->base);
        size_t start = b->len;
        d32(b, (uint32_t)(n + 18));
        d16(b, 4);
        d32rel(g, SEC_SRCFILES, RK_SEC, SEC_ABBREV, ab_file_off);
        d8(b, 8);
        d8(b, 1);
        dstr(b, f->base);
        d8(b, 0);
        d16(b, 0xde00);
        d32rel(g, SEC_SRCFILES, RK_SEC, SEC_SRCFILES, g->dirs.v[f->dir_idx].off);
        d8(b, 0);
        d8(b, 0);
        f->off = (uint32_t)(start + 11);
    }
    g->main_off = g->files.v[0].off;
}

static const char *type_name(const DType *t) {
    if (t->enc == 8) return "unsigned char";
    return xasprintf("%s%d", t->enc == 5 ? "int" : t->enc == 7 ? "uint" : "hfloat", t->size * 8);
}

static void put32_at(Buf *b, size_t at, uint32_t v) {
    b->p[at] = (char)(v >> 24);
    b->p[at + 1] = (char)(v >> 16);
    b->p[at + 2] = (char)(v >> 8);
    b->p[at + 3] = (char)v;
}

static void build_info(Dbg *g, uint32_t **sub_off) {
    Buf *b = &g->sec[SEC_INFO].d;
    DSect *last = g->sects.v[g->sects.n - 1];
    size_t i, k;
    d32(b, 0); /* length, set below */
    d16(b, 4);
    d32rel(g, SEC_INFO, RK_SEC, SEC_ABBREV, 0);
    d8(b, 8);
    duleb(b, AB_CU);
    d16(b, 0xde00);
    dstr(b, g->o->producer);
    dstr(b, g->comp_dir);
    dstr(b, g->cu_name);
    d32rel(g, SEC_INFO, RK_SEC, SEC_SRCFILES, g->main_off);
    d64rel(g, SEC_INFO, 0, (uint64_t)g->sects.v[0]->base);
    d64rel(g, SEC_INFO, (int)g->sects.n - 1, (uint64_t)(last->base + last->len));
    if (g->nsub) d32rel(g, SEC_INFO, RK_SEC, SEC_LINE, 0);
    /* types: the base types first, then the arrays that refer to them */
    for (i = 0; i < g->types.n; i++) {
        DType *t = &g->types.v[i];
        if (t->count != 1) continue;
        t->off = (uint32_t)b->len;
        duleb(b, AB_BASE);
        dstr(b, type_name(t));
        d8(b, (unsigned)t->enc);
        d8(b, (unsigned)t->size);
    }
    for (i = 0; i < g->types.n; i++) {
        DType *t = &g->types.v[i];
        if (t->count == 1) continue;
        t->off = (uint32_t)b->len;
        duleb(b, AB_ARRAY);
        d32(b, g->types.v[t->base].off);
        duleb(b, AB_SUBRANGE);
        d32(b, t->count - 1 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(t->count - 1));
        d8(b, 0);
    }
    *sub_off = xcalloc((g->sects.n + 1) * sizeof **sub_off);
    for (i = 0; i < g->sects.n; i++) {
        DSect *d = g->sects.v[i];
        int code = d->rows.n > 0;
        if (code) {
            (*sub_off)[i] = (uint32_t)b->len;
            duleb(b, AB_SUBPROGRAM);
            dstr(b, d->sect->name);
            d64rel(g, SEC_INFO, (int)i, (uint64_t)d->base);
            d64rel(g, SEC_INFO, (int)i, (uint64_t)(d->base + d->len));
        }
        for (k = 0; k < d->labs.n; k++) {
            DLab *l = &d->labs.v[k];
            if (!l->var) {
                duleb(b, AB_LABEL);
                dstr(b, l->name);
                d64rel(g, SEC_INFO, (int)i, (uint64_t)(d->base + l->off));
            } else {
                duleb(b, AB_VARIABLE);
                dstr(b, l->name);
                d32(b, g->types.v[l->type].off);
                duleb(b, 9);
                d8(b, 0x03); /* DW_OP_addr */
                d64rel(g, SEC_INFO, (int)i, (uint64_t)(d->base + l->off));
            }
        }
        if (code) d8(b, 0);
    }
    d8(b, 0);
    put32_at(b, 0, (uint32_t)(b->len - 4));
}

static void build_aranges(Dbg *g) {
    Buf *b = &g->sec[SEC_ARANGES].d;
    size_t i;
    d32(b, 0);
    d16(b, 2);
    d32rel(g, SEC_ARANGES, RK_SEC, SEC_INFO, 0);
    d8(b, 8);
    d8(b, 0);
    d32(b, 0);
    for (i = 0; i < g->sects.n; i++) {
        if (g->sects.v[i]->len <= 0) continue;
        d64rel(g, SEC_ARANGES, (int)i, (uint64_t)g->sects.v[i]->base);
        d64(b, (uint64_t)g->sects.v[i]->len);
    }
    d64(b, 0);
    d64(b, 0);
    put32_at(b, 0, (uint32_t)(b->len - 4));
}

static void build_pubnames(Dbg *g, const uint32_t *sub_off) {
    Buf *b = &g->sec[SEC_PUBNAMES].d;
    size_t i;
    d32(b, 0);
    d16(b, 2);
    d32rel(g, SEC_PUBNAMES, RK_SEC, SEC_INFO, 0);
    d32(b, (uint32_t)g->sec[SEC_INFO].d.len);
    for (i = 0; i < g->sects.n; i++) {
        if (g->sects.v[i]->rows.n == 0) continue;
        d32(b, sub_off[i]);
        dstr(b, g->sects.v[i]->sect->name);
    }
    d32(b, 0);
    put32_at(b, 0, (uint32_t)(b->len - 4));
}

/* the name of a file in the line table: relative to the compilation directory when below it */
static const char *file_rel(const Dbg *g, const DFile *f) {
    size_t k = strlen(g->comp_dir);
    if (k > 1 && !strncmp(f->abs, g->comp_dir, k) && f->abs[k] == '/') return f->abs + k + 1;
    return f->abs;
}

static void ext_file(Dbg *g, const DFile *f) { /* IBM: define the file of the following rows */
    Buf *b = &g->sec[SEC_LINE].d;
    d8(b, 0);
    d8(b, 5);
    d8(b, 0xDE);
    d32rel(g, SEC_LINE, RK_SEC, SEC_SRCFILES, f->off);
}

static void build_line(Dbg *g) {
    Buf *b = &g->sec[SEC_LINE].d;
    static const uint8_t lens[12] = {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};
    size_t i, k, hl_at, start;
    d32(b, 0);
    d16(b, 3);
    hl_at = b->len;
    d32(b, 0);
    start = b->len;
    d8(b, 2); /* minimum instruction length: instructions are halfword aligned */
    d8(b, 0);
    d8(b, 0xFF);
    d8(b, 4);
    d8(b, 13);
    buf_write(b, lens, sizeof lens);
    d8(b, 0); /* no include directories: names are relative to the compilation directory */
    for (i = 0; i < g->files.n; i++) {
        dstr(b, file_rel(g, &g->files.v[i]));
        d8(b, 0);
        d8(b, 0);
        d8(b, 0);
    }
    d8(b, 0);
    put32_at(b, hl_at, (uint32_t)(b->len - start));
    for (i = 0; i < g->sects.n; i++) {
        DSect *d = g->sects.v[i];
        int64_t addr = 0, end;
        int line = 1, cur;
        if (d->rows.n == 0) continue;
        cur = d->rows.v[0].file;
        ext_file(g, &g->files.v[cur]);
        d8(b, 0);
        d8(b, 9);
        d8(b, 2);
        d64rel(g, SEC_LINE, (int)i, (uint64_t)d->base);
        d8(b, 5); /* column 1 */
        d8(b, 1);
        d8(b, 6); /* is_stmt on */
        if (cur != 0) {
            d8(b, 4);
            duleb(b, (uint64_t)cur + 1);
        }
        for (k = 0; k < d->rows.n; k++) {
            const DRow *r = &d->rows.v[k];
            int64_t adv;
            int lad;
            if ((r->addr & 1) || r->addr >= d->len) continue;
            if (r->file != cur) {
                ext_file(g, &g->files.v[r->file]);
                d8(b, 4);
                duleb(b, (uint64_t)r->file + 1);
                cur = r->file;
            }
            adv = (r->addr - addr) / 2;
            lad = r->line - line;
            if (lad >= -1 && lad <= 2 && adv * 4 + (lad + 1) + 13 <= 255) {
                d8(b, (unsigned)(adv * 4 + (lad + 1) + 13));
            } else {
                if (adv > 0) {
                    d8(b, 2);
                    duleb(b, (uint64_t)adv);
                }
                if (lad != 0) {
                    d8(b, 3);
                    dsleb(b, lad);
                }
                d8(b, 1);
            }
            addr = r->addr;
            line = r->line;
        }
        end = (d->len + 1) & ~(int64_t)1;
        if (end > addr) {
            d8(b, 2);
            duleb(b, (uint64_t)((end - addr) / 2));
        }
        d8(b, 0);
        d8(b, 1);
        d8(b, 1);
    }
    put32_at(b, 0, (uint32_t)(b->len - 4));
}

void dbg_dwarf(Dbg *g) {
    uint32_t *sub_off = NULL;
    size_t i;
    static const char *const names[SEC_COUNT] = {".debug_info",    ".debug_line",     ".debug_abbrev",
                                                 ".debug_aranges", ".debug_pubnames", ".debug_srcfiles"};
    for (i = 0; i < SEC_COUNT; i++) g->sec[i].name = names[i];
    build_abbrev(g);
    build_srcfiles(g);
    build_info(g, &sub_off);
    if (g->nsub) build_line(g);
    build_aranges(g);
    if (g->nsub) build_pubnames(g, sub_off);
    g->have[SEC_INFO] = g->have[SEC_ABBREV] = g->have[SEC_ARANGES] = g->have[SEC_SRCFILES] = 1;
    g->have[SEC_LINE] = g->have[SEC_PUBNAMES] = g->nsub > 0;
}
