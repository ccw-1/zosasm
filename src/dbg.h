/* dbg.h - the debug information of -g: a DWARF file (.dbg, an ELF32 big endian
 * relocatable object, as z/OS /bin/as -g writes it) and the small GOFF module
 * that is appended to the object and links it to that file.  See DESIGN.md 21. */
#ifndef ZA_DBG_H
#define ZA_DBG_H

#include "asm.h"

/* the class of module 2 that holds the .dbg ELF (NOLOAD, framed copies); reader: CDADwarf.cpp */
#define DBG_CLASS "C_ADATAE9C3"

typedef struct {
    const char *cwd;      /* the local current directory (absolute) */
    const char *gdir;     /* z/OS directory that corresponds to cwd, or NULL */
    Vec(const char *) map_from, map_to; /* -gmap FROM=TO, applied in order */
    const char *main_file; /* the source file as given on the command line */
    const char *dbg_path;  /* path of the .dbg on the z/OS host (recorded in the object) */
    const char *producer;  /* DW_AT_producer */
} DbgOptions;

typedef struct {
    uint8_t *elf;   /* the .dbg file */
    size_t elf_len;
    uint8_t *mod2;  /* the GOFF module for the object file (appended to it) */
    size_t mod2_len;
} DbgResult;

/* The path as recorded in the debug information: made absolute from cwd,
 * normalized and mapped (-gdir, -gmap); malloc'ed. */
char *dbg_host_path(const DbgOptions *o, const char *path);

/* Builds both parts after module_emit (obj is the object module_emit wrote).
 * Returns 1, or 0 and describes the problem in err. */
int dbg_build(Module *m, const uint8_t *obj, size_t objlen, const DbgOptions *o, DbgResult *r, char *err,
              size_t errsz);

/* ELF form (-elf -g): the DWARF sections, to be added to the ELF object as sections of their own.
 * A relocation refers to the section of a class (cls) or to another DWARF section (sec, an index
 * into the list); off is in the section, the in-place value is the addend. */
typedef struct {
    uint32_t off;
    uint32_t type; /* R_390_32 or R_390_64 */
    Class *cls;
    int sec;
} DbgElfRel;

typedef struct {
    const char *name; /* .debug_info ... */
    uint8_t *data;
    size_t len;
    DbgElfRel *rels;
    size_t nrels;
} DbgElfSec;

/* Makes the DWARF sections for the ELF object; the array is malloc'ed. Returns 1, or 0 with err. */
int dbg_build_elf(Module *m, const DbgOptions *o, DbgElfSec **secs, size_t *nsecs, char *err, size_t errsz);

/* module_emit_elf with the DWARF sections added (elf.c). */
uint8_t *module_emit_elf_dbg(Module *m, const DbgElfSec *dbg, size_t ndbg, size_t *len, char *err, size_t errsz);

#endif
