/* zsym.h - ZSYM: symbolic information stored in the object (ZSYM.md).
 *
 * The assembler keeps the intent of the source that the GOFF records cannot
 * hold (symbols, EQUs, DSECT layouts, the operands of every statement) in the
 * class C_ADATAE9C1 (NOLOAD, unstructured), as a stream of chunks:
 *
 *   chunk = 'Z' 'S' version:u8 kind:u8 length:u32(BE) payload crc32:u32(BE)
 *   kind  = 1 MODULE, 2 SYMS, 3 STMTS, 4 END
 *
 * The payload uses LEB128 integers ("uv" unsigned, "sv" zigzag signed) and
 * strings (uv length, bytes). A module block is MODULE SYMS* STMTS* END; the
 * binder concatenates the classes of several modules, so a reader finds
 * several blocks. */
#ifndef ZA_ZSYM_H
#define ZA_ZSYM_H

#include <stdio.h>

#include "asm.h"
#include "goff.h"

#define ZSYM_CLASS "C_ADATAE9C1"
#define ZSYM_VERSION 1

enum { ZK_MODULE = 1, ZK_SYMS = 2, ZK_STMTS = 3, ZK_END = 4 };

/* statement kinds */
enum {
    ZS_OTHER = 0,
    ZS_INSN,
    ZS_DC,
    ZS_DS,
    ZS_EQU,
    ZS_USING,
    ZS_DROP,
    ZS_ORG,
    ZS_LTORG,
    ZS_SECTION, /* CSECT RSECT START COM DSECT LOCTR CATTR */
    ZS_EXTERN,  /* EXTRN WXTRN ENTRY XATTR AMODE RMODE */
    ZS_DXD,
    ZS_END
};

/* symbol kinds */
enum { ZY_LABEL = 0, ZY_ABS, ZY_REL, ZY_EXT, ZY_SECT, ZY_DXD };
enum { ZYF_WEAK = 1, ZYF_ENTRY = 2 };

/* one piece of a template: text, a symbol reference or an attribute reference */
typedef struct {
    char tag; /* 't', 's' (symbol id), 'a' (attribute letter + symbol id) */
    char attr;
    uint32_t id;
    char *text;
} ZItem;

typedef Vec(ZItem) ZTmpl;

typedef struct {
    char *sect, *cls;
    int kind; /* SectKind */
} ZScope;

typedef struct {
    int64_t coef;
    int tkind; /* 1 class (scope id), 2 symbol */
    uint32_t id;
} ZTerm;

typedef struct {
    char *name;
    int kind, flags;
    int64_t scope; /* scope id, -1 = none */
    int64_t val;   /* offset (label), value (EQU), size (section) */
    int64_t len;
    char type;
    int64_t scale;
    Vec(ZTerm) terms;
} ZSym;

typedef struct {
    int kind;
    uint32_t file, line;
    int64_t scope; /* -1 = none */
    int64_t off, len;
    uint32_t crc;
    ZTmpl label;
    char *op;
    ZTmpl operands;
} ZStmt;

typedef struct {
    char *producer;
    int level;
    Vec(char *) files;
    Vec(ZScope) scopes;
    Vec(ZSym) syms;
    Vec(ZStmt) stmts;
    uint64_t want_syms, want_stmts;
    int complete; /* END seen, the counts agree and no chunk of the block was damaged */
    int damaged;  /* a chunk of the block was skipped */
} ZBlock;

typedef struct {
    Vec(ZBlock *) blocks;
    int bad_chunks; /* chunks skipped (unknown version/kind, bad checksum) */
} Zsym;

/* ---- writer -------------------------------------------------------------- */

/* The chunks for the module (ZSYM.md); NULL when m->zlevel is 0. */
uint8_t *zsym_build(Module *m, size_t *len);

/* ---- reader -------------------------------------------------------------- */

/* Parse a stream of chunks. Never fails on content: damaged chunks are
 * counted in bad_chunks and the block they belong to is incomplete. */
Zsym *zsym_parse(const uint8_t *b, size_t n);
/* the concatenated text of the class C_ADATAE9C1, or NULL when absent */
uint8_t *zsym_extract(const GoffModule *m, size_t *len);
void zsym_dump(const Zsym *z, FILE *f);
/* text of a template; symbols are written by their name in b */
char *zsym_render(const ZBlock *b, const ZTmpl *t);
/* a symbol name as source text (quoted when needed) */
char *zsym_quote(const char *name);

/* CRC-32 of data[off, off+len) with the bytes marked in mask (may be NULL)
 * taken as zero: how a statement's bytes are compared (relocated fields are
 * masked because the binder changes them). */
uint32_t zsym_crc(const uint8_t *data, size_t datalen, const uint8_t *mask, int64_t off, int64_t len);

#endif
