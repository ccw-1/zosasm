/* zsrc.h - the original sources, kept in the object at -gz2 (ZSYM.md, tier 2).
 *
 * The class C_ADATAE9C2 (NOLOAD, unstructured) holds the files the assembler
 * read before the preprocessor touched them: the main file and every
 * #include, byte for byte, so the preprocessor structure (#define, #include,
 * #if) is kept as written. A reader can show the source of an object, or
 * unpack the files and assemble them again. The stream uses the same chunks
 * as ZSYM (zsym.h, zio.h) with its own kinds:
 *
 *   kind 1 HEAD: producer:str level:u8 ndefs:uv defs:str* nfiles:uv
 *   kind 2 FILE: name:str length:uv crc32:u32 method:u8 data...
 *                method 0 = stored, 1 = LZ4 block (lz.h); crc32 is the CRC of
 *                the original bytes
 *   kind 3 END:  nfiles:uv
 *
 * A block is HEAD FILE* END; the binder concatenates the classes of several
 * modules, so a reader finds several blocks. */
#ifndef ZA_ZSRC_H
#define ZA_ZSRC_H

#include <stdio.h>

#include "asm.h"
#include "goff.h"

#define ZSRC_CLASS "C_ADATAE9C2"

enum { ZR_HEAD = 1, ZR_FILE = 2, ZR_END = 3 };
enum { ZR_STORED = 0, ZR_LZ = 1 };

typedef struct {
    char *name;
    uint8_t *data; /* the original bytes; NULL when damaged */
    size_t len;
    size_t packed; /* size of the stored form */
    int method;
    uint32_t crc;
} ZSrcFile;

typedef struct {
    char *producer;
    int level;
    Vec(char *) defs;
    Vec(ZSrcFile) files;
    uint64_t want;
    int complete, damaged;
} ZSrcBlock;

typedef struct {
    Vec(ZSrcBlock *) blocks;
    int bad_chunks;
} ZSrc;

/* The chunks for the module's sources; NULL when m->zlevel < 2. */
uint8_t *zsrc_build(Module *m, size_t *len);

/* Parse a stream of chunks. Never fails on content: damaged chunks and files
 * whose data does not decompress or does not match its checksum are counted
 * (bad_chunks / damaged) and left out. */
ZSrc *zsrc_parse(const uint8_t *b, size_t n);
/* the concatenated text of the class C_ADATAE9C2, or NULL when absent */
uint8_t *zsrc_extract(const GoffModule *m, size_t *len);
void zsrc_dump(const ZSrc *z, FILE *f);
/* Write the files of block 0 (or all blocks, the later ones in subdirectories
 * "2/", "3/"...) under dir, keeping their relative paths ("/" and ".." are
 * made harmless). Returns the number of files written, -1 on an I/O error
 * (reported on stderr). Each file name is listed on out when out is not NULL. */
int zsrc_unpack(const ZSrc *z, const char *dir, FILE *out);

#endif
