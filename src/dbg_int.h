/* dbg_int.h - internals of the -g debug information (dbg.c, dbg_dwarf.c, dbg_elf.c). */
#ifndef ZA_DBG_INT_H
#define ZA_DBG_INT_H

#include <string.h>

#include "dbg.h"

enum { SEC_INFO, SEC_LINE, SEC_ABBREV, SEC_ARANGES, SEC_PUBNAMES, SEC_SRCFILES, SEC_RANGES, SEC_FRAME, SEC_COUNT };
enum { RK_TEXT, RK_SEC }; /* a relocation refers to the text symbol of a section or to a debug section */
enum { R390_32 = 4, R390_64 = 0x16 };

typedef struct {
    uint32_t off; /* in the section */
    int kind, idx, type;
} DRel;

typedef struct {
    const char *name;
    Buf d;
    Vec(DRel) rels;
} DSec;

typedef struct {
    char *name;
    int64_t off;
    int var;  /* a data item (DC/DS), else a code label */
    int type; /* DType index, -1 for none */
} DLab;

/* a base type (count 1) or an array of count elements of it */
typedef struct {
    int enc, size;
    int64_t count;
    uint32_t off; /* DIE offset in .debug_info */
    int base;     /* arrays: index of the element type */
} DType;

typedef struct {
    int64_t addr;
    int file, line, seq;
} DRow;

typedef struct DStruct DStruct;

/* a USING of a DSECT over a range of the CSECT: a variable of the structure type at register - off */
typedef struct {
    int64_t lo, hi; /* addresses in the CSECT */
    DStruct *st;
    int reg;
    int64_t off; /* the structure starts at register + off */
    char *name;
} DUse;

/* a CFI directive: its offset in the section of its class, its source order and what it says */
typedef struct {
    int64_t off;
    int seq;
    CfiOp op;
} DCfi;

typedef struct {
    Section *sect;
    Class *cls; /* B_TEXT */
    uint32_t id; /* ESD id of its ED in the object */
    int64_t len;
    int64_t base; /* start of its text in the B_TEXT class of the module (the binder concatenates the EDs) */
    Vec(DLab) labs;
    Vec(DRow) rows;
    Vec(DUse) uses;
    Vec(DCfi) cfis;
} DSect;

/* a DSECT as a structure type: its data items are the members */
struct DStruct {
    Section *sect;
    int64_t len;
    Vec(DLab) mems;
    uint32_t off; /* DIE offset in .debug_info */
};

typedef struct {
    char *name, *abs, *dir, *base;
    uint32_t off;     /* DIE offset in .debug_srcfiles */
    int dir_idx;      /* index in dirs */
} DFile;

typedef struct {
    char *path;
    uint32_t off;
} DDir;

typedef struct {
    const DbgOptions *o;
    const Module *m;
    char *comp_dir, *cu_name;
    Vec(DSect *) sects;
    Vec(DStruct *) structs;
    Vec(DLab) consts; /* EQU constants: off = the value */
    Vec(DFile) files;
    StrMap filemap; /* file name -> index + 1 */
    Vec(DDir) dirs;
    Vec(DType) types;
    DSec sec[SEC_COUNT];
    int have[SEC_COUNT];
    uint8_t sig[16];
    char *sig_hex;
    uint32_t main_off; /* DIE of the main file in .debug_srcfiles */
    int nsub;          /* subprograms */
    int elf;           /* ELF form (-elf -g): standard DWARF, no .debug_srcfiles, one ELF section per class */
} Dbg;

/* byte output, big endian */
static inline void d8(Buf *b, unsigned v) { buf_putc(b, (int)(v & 0xFF)); }
static inline void d16(Buf *b, unsigned v) {
    d8(b, v >> 8);
    d8(b, v);
}
static inline void d32(Buf *b, uint32_t v) {
    d16(b, v >> 16);
    d16(b, v & 0xFFFF);
}
static inline void d64(Buf *b, uint64_t v) {
    d32(b, (uint32_t)(v >> 32));
    d32(b, (uint32_t)v);
}
static inline void dstr(Buf *b, const char *s) { buf_write(b, s, strlen(s) + 1); }
static inline void duleb(Buf *b, uint64_t v) {
    do {
        unsigned c = v & 0x7F;
        v >>= 7;
        d8(b, v ? c | 0x80 : c);
    } while (v);
}
static inline void dsleb(Buf *b, int64_t v) {
    for (;;) {
        unsigned c = (unsigned)(v & 0x7F);
        v >>= 7;
        if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) {
            d8(b, c);
            return;
        }
        d8(b, c | 0x80);
    }
}
static inline void drel(Dbg *g, int sec, int kind, int idx, int type) {
    DRel r;
    r.off = (uint32_t)g->sec[sec].d.len;
    r.kind = kind;
    r.idx = idx;
    r.type = type;
    vec_push(&g->sec[sec].rels, r);
}
/* a 4 or 8 byte field with a relocation; v is the in-place value */
static inline void d32rel(Dbg *g, int sec, int tkind, int tidx, uint32_t v) {
    drel(g, sec, tkind, tidx, R390_32);
    d32(&g->sec[sec].d, v);
}
static inline void d64rel(Dbg *g, int sec, int tidx, uint64_t v) {
    drel(g, sec, RK_TEXT, tidx, R390_64);
    d64(&g->sec[sec].d, v);
}

/* dbg_dwarf.c: fills g->sec (and g->have) from the collected data */
void dbg_dwarf(Dbg *g);
/* dbg_elf.c: the .dbg file; the section data are final */
uint8_t *dbg_elf(Dbg *g, size_t *len);

#endif
