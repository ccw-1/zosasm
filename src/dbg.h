/* dbg.h - the debug information of -g: a DWARF file (.dbg, an ELF32 big endian
 * relocatable object, as z/OS /bin/as -g writes it) and the small GOFF module
 * that is appended to the object and links it to that file.  See DESIGN.md 21. */
#ifndef ZA_DBG_H
#define ZA_DBG_H

#include "asm.h"

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

#endif
