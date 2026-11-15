/* goff.h - writer and reader for z/OS GOFF (Generalized Object File
 * Format) objects: fixed 80-byte records HDR, ESD, TXT, RLD, LEN, END.
 *
 * Reference: z/OS MVS Program Management: Advanced Facilities, appendix
 * "Generalized object file format (GOFF)"; see also
 * _context/goff-format-knowledge.md in the go-dev workspace.
 *
 * Functions that can fail return NULL on success or a static error text. */
#ifndef ZA_GOFF_H
#define ZA_GOFF_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "util.h"

#define GOFF_RECORD_LEN 80
#define GOFF_MAX_TEXT 32760
#define GOFF_DEFERRED_LENGTH 0xFFFFFFFFu

/* record types */
enum { GOFF_T_ESD = 0x0, GOFF_T_TXT = 0x1, GOFF_T_RLD = 0x2, GOFF_T_LEN = 0x3, GOFF_T_END = 0x4, GOFF_T_HDR = 0xF };

typedef enum { GOFF_SD = 0, GOFF_ED = 1, GOFF_LD = 2, GOFF_PR = 3, GOFF_ER = 4 } GoffESDType;

enum { GOFF_NS_NORMAL = 1, GOFF_NS_PSEUDO = 2, GOFF_NS_PARTS3 = 3 };
enum { GOFF_AMODE_NONE = 0, GOFF_AMODE_24 = 1, GOFF_AMODE_31 = 2, GOFF_AMODE_ANY = 3, GOFF_AMODE_64 = 4, GOFF_AMODE_MIN = 0x10 };
enum { GOFF_RMODE_NONE = 0, GOFF_RMODE_24 = 1, GOFF_RMODE_31 = 3, GOFF_RMODE_64 = 4 };
enum { GOFF_STYLE_BYTE = 0, GOFF_STYLE_STRUCTURED = 1, GOFF_STYLE_UNSTRUCTURED = 2 };
enum { GOFF_BIND_CAT = 0, GOFF_BIND_MERGE = 1 };
enum { GOFF_TASK_UNSPEC = 0, GOFF_TASK_NONREUS = 1, GOFF_TASK_REUS = 2, GOFF_TASK_RENT = 3 };
enum { GOFF_EXEC_UNSPEC = 0, GOFF_EXEC_DATA = 1, GOFF_EXEC_CODE = 2 };
enum { GOFF_STRENGTH_STRONG = 0, GOFF_STRENGTH_WEAK = 1 };
enum { GOFF_LOAD_NOW = 0, GOFF_LOAD_DEFERRED = 1, GOFF_NOLOAD = 2 };
enum { GOFF_SCOPE_UNSPEC = 0, GOFF_SCOPE_SECTION = 1, GOFF_SCOPE_MODULE = 2, GOFF_SCOPE_LIBRARY = 3, GOFF_SCOPE_IMPORT_EXPORT = 4 };
enum { GOFF_LINKAGE_OS = 0, GOFF_LINKAGE_XPLINK = 1 };
/* alignment = log2 of the boundary */
enum { GOFF_ALIGN_BYTE = 0, GOFF_ALIGN_HALF = 1, GOFF_ALIGN_WORD = 2, GOFF_ALIGN_DBL = 3, GOFF_ALIGN_QUAD = 4, GOFF_ALIGN_PAGE = 12 };

/* relocation reference / referent types and actions */
enum { GOFF_REF_ADDRESS = 0, GOFF_REF_OFFSET = 1, GOFF_REF_LENGTH = 2, GOFF_REF_RELIMM = 6, GOFF_REF_RTYPE = 7, GOFF_REF_LONGDISP = 9 };
enum { GOFF_REFERENT_LABEL = 0, GOFF_REFERENT_ELEMENT = 1, GOFF_REFERENT_CLASS = 2, GOFF_REFERENT_PART = 3 };
enum { GOFF_ACTION_ADD = 0, GOFF_ACTION_SUB = 1 };

/* the ten behavioral attribute bytes of an ESD record */
typedef struct {
    uint8_t amode, rmode;
    uint8_t text_style;    /* ED/PR, 4 bits */
    uint8_t bind_algorithm; /* 4 bits */
    uint8_t tasking;       /* 3 bits */
    int read_only;
    uint8_t executable;    /* 3 bits */
    uint8_t dup_severity;  /* 2 bits, PR */
    uint8_t bind_strength; /* 4 bits */
    uint8_t loading;       /* 2 bits */
    int common, indirect;
    uint8_t scope;     /* 4 bits */
    uint8_t linkage;   /* 1 bit */
    uint8_t alignment; /* 5 bits */
} GoffBehavior;

void goff_behavior_pack(const GoffBehavior *b, uint8_t o[10]);
void goff_behavior_unpack(const uint8_t o[10], GoffBehavior *b);

/* One external symbol definition; name is raw EBCDIC. */
typedef struct {
    GoffESDType type;
    uint32_t id; /* assigned by the writer */
    uint32_t parent, offset, length;
    uint32_t ext_attr_id, ext_attr_off;
    uint8_t name_space;
    int has_fill;
    uint8_t fill;
    int mangled, renameable, removable;
    uint8_t reserved_qwords; /* 3 bits */
    uint32_t ada_id, priority;
    GoffBehavior attr;
    uint8_t *name;
    size_t name_len;
} GoffESD;

/* One RLD data item: patch length bytes at (pid, offset) using rid. */
typedef struct {
    uint32_t rid, pid, offset;
    uint8_t reference, referent, action;
    int store;
    uint8_t length, bit_length, bit_offset;
    int cond_seq, amode_sens;
} GoffReloc;

typedef struct {
    uint32_t esdid, length;
} GoffDeferredLen;

typedef enum { GOFF_ENTRY_NONE = 0, GOFF_ENTRY_BYID = 1, GOFF_ENTRY_BYNAME = 2 } GoffEntryKind;

typedef struct {
    GoffEntryKind kind;
    uint8_t amode;
    uint32_t esdid, offset;
    uint8_t *name;
    size_t name_len;
} GoffEnd;

/* ---- writer ---------------------------------------------------------- */

typedef struct {
    Buf buf;
    uint32_t logical, nesd;
    int ended;
} GoffWriter;

GoffWriter *goff_writer_new(uint32_t arch_level);
/* assigns e->id */
const char *goff_write_esd(GoffWriter *w, GoffESD *e);
const char *goff_write_text(GoffWriter *w, uint32_t esdid, uint32_t offset, uint8_t style,
                            const uint8_t *data, size_t n);
const char *goff_write_rld(GoffWriter *w, const GoffReloc *items, size_t n);
const char *goff_write_len(GoffWriter *w, const GoffDeferredLen *items, size_t n);
/* finishes the object; the result is owned by the writer */
const char *goff_write_end(GoffWriter *w, const GoffEnd *e, const uint8_t **out, size_t *len);

/* ---- reader ---------------------------------------------------------- */

typedef struct {
    uint32_t esdid, offset;
    uint8_t style;
    uint16_t encoding;
    uint32_t true_len;
    uint8_t *data;
    size_t len;
} GoffText;

typedef struct {
    uint32_t arch_level;
    Vec(GoffESD *) esds;
    Vec(GoffText) texts;
    Vec(GoffReloc) relocs;
    Vec(GoffDeferredLen) lens;
    GoffEnd end;
    uint32_t end_count; /* record count stored in END */
    int logical;        /* logical records seen */
} GoffModule;

/* Parse an object; returns NULL and sets *err on failure. */
GoffModule *goff_parse(const uint8_t *b, size_t n, const char **err);
const GoffESD *goff_module_esd(const GoffModule *m, uint32_t id);
/* Same ESDs, text, relocations, lengths and END? The ESD item skip_a of a
 * (0 = none) and its text are left out of the comparison. */
int goff_equal(const GoffModule *a, uint32_t skip_a, const GoffModule *b);
/* readable listing */
void goff_dump(const GoffModule *m, FILE *w);

#endif
