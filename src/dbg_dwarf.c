/* dbg_dwarf.c - the DWARF sections of -g.  The shape follows what z/OS
 * /bin/as -g writes for HLASM (language 0xDE00, the IBM .debug_srcfiles units,
 * the 0xDE line opcode); see DESIGN.md 21. */
#include <stdlib.h>
#include <string.h>

#include "dbg_int.h"

enum { AB_CU = 1, AB_BASE, AB_ARRAY, AB_SUBRANGE, AB_SUBPROGRAM, AB_LABEL, AB_VARIABLE, AB_STRUCT, AB_MEMBER, AB_BLOCK, AB_CONST };

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
    AT_RANGES = 0x55,
    AT_BYTE_SIZE = 0x0b, AT_CONST_VALUE = 0x1c, AT_MEMBER_LOC = 0x38, AT_EXTERNAL = 0x3f, AT_TYPE = 0x49, AT_IBM_SRCFILE = 0x390f, AT_IBM_FILE_DIR = 0x3910,
    AT_IBM_FILE_A = 0x3918, AT_IBM_FILE_B = 0x3915
};
enum { F_ADDR = 0x01, F_DATA2 = 0x05, F_DATA4 = 0x06, F_DATA1 = 0x0b, F_STRING = 0x08, F_REF4 = 0x13,
       F_SEC_OFFSET = 0x17, F_EXPRLOC = 0x18, F_FLAG_PRESENT = 0x19, F_SDATA = 0x0d };

#define TAG_IBM_DIR 0xde15
#define TAG_IBM_FILE 0xde10
#define LANG_ASM 0x8001 /* DW_LANG_Mips_Assembler, what other assemblers write for ELF */

static uint32_t ab_dir_off, ab_file_off;

static void build_abbrev(Dbg *g) {
    Buf *b = &g->sec[SEC_ABBREV].d;
    ab_start(b, AB_CU, 0x11, 1);
    ab_attr(b, AT_LANGUAGE, F_DATA2);
    ab_attr(b, AT_PRODUCER, F_STRING);
    ab_attr(b, AT_COMP_DIR, F_STRING);
    ab_attr(b, AT_NAME, F_STRING);
    if (g->elf) {
        ab_attr(b, AT_RANGES, F_SEC_OFFSET);
    } else {
        ab_attr(b, AT_IBM_SRCFILE, F_SEC_OFFSET);
        ab_attr(b, AT_LOW_PC, F_ADDR);
        ab_attr(b, AT_HIGH_PC, F_ADDR);
    }
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
    ab_start(b, AB_STRUCT, 0x13, 1); /* a DSECT */
    ab_attr(b, AT_NAME, F_STRING);
    ab_attr(b, AT_BYTE_SIZE, F_DATA4);
    ab_end(b);
    ab_start(b, AB_MEMBER, 0x0d, 0);
    ab_attr(b, AT_NAME, F_STRING);
    ab_attr(b, AT_TYPE, F_REF4);
    ab_attr(b, AT_MEMBER_LOC, F_DATA4);
    ab_end(b);
    ab_start(b, AB_CONST, 0x34, 0); /* an EQU */
    ab_attr(b, AT_NAME, F_STRING);
    ab_attr(b, AT_TYPE, F_REF4);
    ab_attr(b, AT_CONST_VALUE, F_SDATA);
    ab_end(b);
    ab_start(b, AB_BLOCK, 0x0b, 1); /* where USINGs of DSECTs hold */
    ab_attr(b, AT_LOW_PC, F_ADDR);
    ab_attr(b, AT_HIGH_PC, F_ADDR);
    ab_end(b);
    d8(b, 0);
    if (g->elf) return;
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

static int cmp_i64(const void *x, const void *y) {
    int64_t a = *(const int64_t *)x, b = *(const int64_t *)y;
    return a < b ? -1 : a > b;
}

/* CBLOCK locals: one lexical block per block, with a variable at the ctemp
 * base register plus the slot offset. */
static void build_cvars(Dbg *g, size_t si, const DSect *d) {
    Buf *b = &g->sec[SEC_INFO].d;
    size_t i = 0;
    while (i < d->cvars.n) {
        int64_t lo = d->cvars.v[i].lo, hi = d->cvars.v[i].hi;
        size_t j = i;
        duleb(b, AB_BLOCK);
        d64rel(g, SEC_INFO, (int)si, (uint64_t)(d->base + lo));
        d64rel(g, SEC_INFO, (int)si, (uint64_t)(d->base + hi));
        while (j < d->cvars.n && d->cvars.v[j].lo == lo && d->cvars.v[j].hi == hi) {
            const DVar *v = &d->cvars.v[j];
            Buf ex = {0};
            duleb(b, AB_VARIABLE);
            dstr(b, v->name);
            d32(b, g->types.v[v->type].off);
            d8(&ex, 0x70 + (unsigned)v->reg); /* DW_OP_breg<reg> */
            dsleb(&ex, v->off);
            duleb(b, ex.len);
            buf_write(b, ex.p, ex.len);
            free(ex.p);
            j++;
        }
        d8(b, 0);
        i = j;
    }
}

/* The USINGs of DSECTs of a CSECT: lexical blocks that do not overlap (a debugger finds one block
 * at an address), each with a variable of the structure type per USING that holds there. The
 * variable is at register - offset of the USING; its name is the USING label or the DSECT name. */
static void build_uses(Dbg *g, size_t si, const DSect *d) {
    Buf *b = &g->sec[SEC_INFO].d;
    size_t n = d->uses.n, np = 0, i, j, k, ncur = 0, nact;
    int64_t *pt, blo = 0, bend = 0;
    size_t *cur, *act;
    if (n == 0) return;
    pt = xcalloc(2 * n * sizeof *pt);
    cur = xcalloc(n * sizeof *cur);
    act = xcalloc(n * sizeof *act);
    for (i = 0; i < n; i++) {
        pt[np++] = d->uses.v[i].lo;
        pt[np++] = d->uses.v[i].hi;
    }
    qsort(pt, np, sizeof *pt, cmp_i64);
    for (j = 0; j < np; j++) { /* the last point only closes the block in progress */
        int64_t lo = pt[j], hi = j + 1 < np ? pt[j + 1] : pt[j];
        nact = 0;
        if (hi > lo)
            for (i = 0; i < n; i++)
                if (d->uses.v[i].lo <= lo && hi <= d->uses.v[i].hi) act[nact++] = i;
        if (j + 1 < np && hi == lo) continue;
        if (ncur > 0 && nact == ncur && lo == bend && !memcmp(act, cur, nact * sizeof *act)) {
            bend = hi;
            continue;
        }
        if (ncur > 0) { /* close the block [blo, bend) */
            duleb(b, AB_BLOCK);
            d64rel(g, SEC_INFO, (int)si, (uint64_t)(d->base + blo));
            d64rel(g, SEC_INFO, (int)si, (uint64_t)(d->base + bend));
            for (k = 0; k < ncur; k++) {
                const DUse *u = &d->uses.v[cur[k]];
                size_t o, dup = 0;
                Buf ex = {0};
                for (o = 0; o < ncur; o++)
                    if (o != k && !strcmp(d->uses.v[cur[o]].name, u->name)) dup = 1;
                duleb(b, AB_VARIABLE);
                dstr(b, dup ? xasprintf("%s_R%d", u->name, u->reg) : u->name);
                d32(b, u->st->off);
                d8(&ex, 0x70 + (unsigned)u->reg); /* DW_OP_breg<reg> */
                dsleb(&ex, u->off);
                duleb(b, ex.len);
                buf_write(b, ex.p, ex.len);
                free(ex.p);
            }
            d8(b, 0);
        }
        ncur = nact;
        if (nact > 0) {
            memcpy(cur, act, nact * sizeof *act);
            blo = lo;
            bend = hi;
        }
    }
    free(pt);
    free(cur);
    free(act);
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
    d16(b, g->elf ? LANG_ASM : 0xde00);
    dstr(b, g->o->producer);
    dstr(b, g->comp_dir);
    dstr(b, g->cu_name);
    if (g->elf) {
        d32rel(g, SEC_INFO, RK_SEC, SEC_RANGES, 0);
    } else {
        d32rel(g, SEC_INFO, RK_SEC, SEC_SRCFILES, g->main_off);
        d64rel(g, SEC_INFO, 0, (uint64_t)g->sects.v[0]->base);
        d64rel(g, SEC_INFO, (int)g->sects.n - 1, (uint64_t)(last->base + last->len));
    }
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
    for (i = 0; i < g->structs.n; i++) { /* DSECTs as structure types */
        DStruct *st = g->structs.v[i];
        st->off = (uint32_t)b->len;
        duleb(b, AB_STRUCT);
        dstr(b, st->sect->name);
        d32(b, (uint32_t)st->len);
        for (k = 0; k < st->mems.n; k++) {
            duleb(b, AB_MEMBER);
            dstr(b, st->mems.v[k].name);
            d32(b, g->types.v[st->mems.v[k].type].off);
            d32(b, (uint32_t)st->mems.v[k].off);
        }
        d8(b, 0);
    }

    for (i = 0; i < g->consts.n; i++) {
        duleb(b, AB_CONST);
        dstr(b, g->consts.v[i].name);
        d32(b, g->types.v[g->consts.v[i].type].off);
        dsleb(b, g->consts.v[i].off);
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
        if (code) build_cvars(g, i, d);
        if (code) build_uses(g, i, d);
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

/* .debug_ranges (ELF form only): the ranges of the CU, one per section, as absolute addresses (the CU has no
 * base address, so each relocated value is the address itself) */
static void build_ranges(Dbg *g) {
    Buf *b = &g->sec[SEC_RANGES].d;
    size_t i;
    for (i = 0; i < g->sects.n; i++) {
        DSect *d = g->sects.v[i];
        if (d->len <= 0) continue;
        d64rel(g, SEC_RANGES, (int)i, (uint64_t)d->base);
        d64rel(g, SEC_RANGES, (int)i, (uint64_t)(d->base + d->len));
    }
    d64(b, 0);
    d64(b, 0);
}

static int cmp_cfi(const void *x, const void *y) {
    const DCfi *a = x, *b = y;
    if (a->off != b->off) return a->off < b->off ? -1 : 1;
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}

/* .debug_frame (ELF form only): a CIE and an FDE for every section with bytes. The CFA is the stack
 * pointer the caller had before the call, so the CIE says CFA = r15 (the stack pointer at entry) and the
 * return address is undefined: with no CFI directives a backtrace stops at the frame where the program is.
 * The CFI directives (instr.c) change the rules from their address on: CFA reg,off; SAVE reg,off (register
 * saved at CFA+off); SAME reg; UNDEF reg (the register has no value in the caller: the outermost frame). */
static void build_frame(Dbg *g) {
    Buf *b = &g->sec[SEC_FRAME].d;
    size_t i, k, cie_at = b->len;
    d32(b, 0); /* length, set below */
    d32(b, 0xFFFFFFFF); /* CIE id */
    d8(b, 1);           /* version */
    d8(b, 0);           /* no augmentation */
    duleb(b, 1);        /* code alignment factor */
    dsleb(b, 1);        /* data alignment factor */
    d8(b, 14);          /* return address register */
    d8(b, 0x0C);        /* DW_CFA_def_cfa r15, 0 */
    duleb(b, 15);
    duleb(b, 0);
    d8(b, 0x07);        /* DW_CFA_undefined r14 */
    duleb(b, 14);
    put32_at(b, cie_at, (uint32_t)(b->len - cie_at - 4));
    for (i = 0; i < g->sects.n; i++) {
        DSect *d = g->sects.v[i];
        size_t fde_at;
        int64_t prev = 0;
        if (d->len <= 0) continue;
        if (d->cfis.n > 1) qsort(d->cfis.v, d->cfis.n, sizeof *d->cfis.v, cmp_cfi);
        fde_at = b->len;
        d32(b, 0);             /* length, set below */
        d32(b, (uint32_t)cie_at); /* the CIE */
        d64rel(g, SEC_FRAME, (int)i, (uint64_t)d->base);
        d64(b, (uint64_t)d->len);
        for (k = 0; k < d->cfis.n; k++) {
            const DCfi *c = &d->cfis.v[k];
            d8(b, 0x04); /* DW_CFA_advance_loc4: the factor is 1 */
            d32(b, (uint32_t)(c->off - prev));
            prev = c->off;
            if (c->op.kind == CFI_CFA) {
                d8(b, 0x12); /* DW_CFA_def_cfa_sf */
                duleb(b, (uint64_t)c->op.reg);
                dsleb(b, c->op.off);
            } else if (c->op.kind == CFI_SAVE) {
                d8(b, 0x11); /* DW_CFA_offset_extended_sf */
                duleb(b, (uint64_t)c->op.reg);
                dsleb(b, c->op.off);
            } else if (c->op.kind == CFI_SAME) {
                d8(b, 0x08); /* DW_CFA_same_value */
                duleb(b, (uint64_t)c->op.reg);
            } else {
                d8(b, 0x07); /* DW_CFA_undefined */
                duleb(b, (uint64_t)c->op.reg);
            }
        }
        put32_at(b, fde_at, (uint32_t)(b->len - fde_at - 4));
    }
}

/* the name of a file in the line table: relative to the compilation directory when below it */
static const char *file_rel(const Dbg *g, const DFile *f) {
    size_t k = strlen(g->comp_dir);
    if (k > 1 && !strncmp(f->abs, g->comp_dir, k) && f->abs[k] == '/') return f->abs + k + 1;
    return f->abs;
}

static void ext_file(Dbg *g, const DFile *f) { /* IBM: define the file of the following rows */
    Buf *b = &g->sec[SEC_LINE].d;
    if (g->elf) return; /* DW_LNS_set_file does it in the ELF form */
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
    static const char *const names[SEC_COUNT] = {".debug_info",    ".debug_line",     ".debug_abbrev", ".debug_aranges",
                                                 ".debug_pubnames", ".debug_srcfiles", ".debug_ranges", ".debug_frame"};
    for (i = 0; i < SEC_COUNT; i++) g->sec[i].name = names[i];
    build_abbrev(g);
    if (!g->elf) build_srcfiles(g);
    build_info(g, &sub_off);
    if (g->nsub) build_line(g);
    build_aranges(g);
    if (g->nsub) build_pubnames(g, sub_off);
    if (g->elf) build_ranges(g);
    if (g->elf) build_frame(g);
    g->have[SEC_INFO] = g->have[SEC_ABBREV] = g->have[SEC_ARANGES] = 1;
    g->have[SEC_SRCFILES] = !g->elf;
    g->have[SEC_RANGES] = g->elf;
    g->have[SEC_FRAME] = g->elf;
    g->have[SEC_LINE] = g->have[SEC_PUBNAMES] = g->nsub > 0;
}
