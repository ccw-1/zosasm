/* obj.h - reader for the older z/OS object module (OBJ) and extended object
 * module (XOBJ) formats, and the OBJ -> GOFF converter.
 *
 * The OBJ record layouts are from High Level Assembler Programmer's Guide
 * (SC26-4941), Appendix C, and the mapping rules are from z/OS MVS Program
 * Management: Advanced Facilities, Appendix C. */
#ifndef ZA_OBJ_H
#define ZA_OBJ_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

typedef struct {
    uint16_t id;          /* OBJ ESDID; 0 for an LD item (it has no own ID) */
    uint8_t type;         /* 0 SD, 1 LD, 2 ER, 4 PC, 5 CM, 6 PR, 0xA WX,
                             or the quad-aligned variants 0xD/0xE/0xF */
    uint8_t flags;        /* AMODE/RMODE/RSECT for SD/PC/CM, alignment for PR */
    int name_len;
    char *name;           /* EBCDIC bytes, not NUL terminated */
    uint32_t addr;
    uint32_t len;         /* length, LDID, or 0 when the field is spaces */
    int has_len;
    int from_xsd;
} ObjEsd;

typedef struct {
    uint16_t esdid;
    uint32_t addr;
    uint8_t *data;
    size_t len;
} ObjText;

typedef struct {
    uint16_t rid, pid;
    uint32_t addr;
    uint8_t flags;
} ObjRld;

typedef struct {
    uint8_t *data;
    size_t len;
} ObjSym;

typedef struct {
    int kind;             /* 0 none, 1 by ESDID+offset, 2 by name */
    uint8_t amode;
    uint16_t esdid;
    uint32_t offset;
    int name_len;
    char *name;
    int has_len;
    uint32_t section_len;
    Vec(char *) idrs;     /* 19-byte IDR payloads, copied */
} ObjEnd;

typedef struct {
    Vec(ObjEsd) esds;
    Vec(ObjText) texts;
    Vec(ObjRld) rlds;
    Vec(ObjSym) syms;
    ObjEnd end;
    int has_end;
} ObjModule;

typedef struct {
    Vec(ObjModule *) modules;
} ObjFile;

/* Parse an OBJ/XOBJ file. Returns NULL or an error text. */
const char *obj_parse(const uint8_t *data, size_t len, ObjFile *out);
void obj_free(ObjFile *f);

/* Convert the parsed object to GOFF bytes appended to out. keep_sym preserves
 * SYM records in the unloaded class C_OBJSYM. Returns NULL or an error. */
const char *obj_to_goff(const ObjFile *f, Buf *out, int keep_sym, int dwarf, char *err, size_t errsz);
/* Best-effort DWARF from OBJ ESD labels. Appends the GOFF link module to mod2
 * and returns the .dbg ELF in elf. */
const char *obj_to_dwarf(const ObjFile *f, Buf *mod2, Buf *elf, const char *dbg_path,
                         const char *input, char *err, size_t errsz);

#endif
