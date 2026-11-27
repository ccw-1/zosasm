/* obj2goff.c - convert an older z/OS OBJ or extended OBJ (XOBJ) module to GOFF. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "obj.h"
#include "util.h"

static void usage(FILE *f) {
    fprintf(f, "usage: obj2goff [-o FILE] [-no-sym] FILE.obj\n"
               "  -o FILE       write the GOFF object to FILE\n"
               "  -no-sym       do not preserve OBJ SYM records as C_OBJSYM\n"
               "  -dwarf        write a best-effort .dbg (ESD labels) and link it\n");
}

int main(int argc, char **argv) {
    const char *in = NULL, *outname = NULL;
    int keep_sym = 1, dwarf = 0, i;
    size_t n = 0;
    char *data;
    ObjFile obj;
    Buf out = {0};
    char err[256];
    const char *e;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(stdout); return 0; }
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { outname = argv[++i]; continue; }
        if (!strcmp(argv[i], "-no-sym")) { keep_sym = 0; continue; }
        if (!strcmp(argv[i], "-dwarf")) { dwarf = 1; continue; }
        if (argv[i][0] == '-') { usage(stderr); return 2; }
        if (in) { usage(stderr); return 2; }
        in = argv[i];
    }
    if (!in) { usage(stderr); return 2; }
    if (!outname) outname = xasprintf("%s.goff.o", in);
    data = read_file(in, &n);
    if (!data) { fprintf(stderr, "obj2goff: cannot read %s\n", in); return 1; }
    e = obj_parse((const uint8_t *)data, n, &obj);
    if (e) { fprintf(stderr, "obj2goff: %s: %s\n", in, e); free(data); return 1; }
    e = obj_to_goff(&obj, &out, keep_sym, dwarf, err, sizeof err);
    if (e) { fprintf(stderr, "obj2goff: %s: %s\n", in, e); obj_free(&obj); free(data); return 1; }
    if (dwarf) {
        Buf mod2 = {0}, elf = {0};
        char *dbgname = xasprintf("%s.dbg", outname);
        e = obj_to_dwarf(&obj, &mod2, &elf, dbgname, in, err, sizeof err);
        if (e) {
            fprintf(stderr, "obj2goff: %s: DWARF: %s\n", in, e);
            free(dbgname); obj_free(&obj); free(out.p); free(data);
            free(mod2.p); free(elf.p); return 1;
        }
        buf_write(&out, mod2.p, mod2.len);
        {
            FILE *f = fopen(dbgname, "wb");
            if (!f || fwrite(elf.p, 1, elf.len, f) != elf.len) {
                fprintf(stderr, "obj2goff: cannot write %s\n", dbgname);
                if (f) fclose(f);
                free(dbgname); obj_free(&obj); free(out.p); free(data);
                free(mod2.p); free(elf.p); return 1;
            }
            fclose(f);
        }
        free(dbgname); free(mod2.p); free(elf.p);
    }
    {
        FILE *f = fopen(outname, "wb");
        if (!f) { fprintf(stderr, "obj2goff: cannot write %s\n", outname); obj_free(&obj); free(out.p); free(data); return 1; }
        if (fwrite(out.p, 1, out.len, f) != out.len) {
            fprintf(stderr, "obj2goff: write failed: %s\n", outname);
            fclose(f); obj_free(&obj); free(out.p); free(data); return 1;
        }
        fclose(f);
    }
    fprintf(stderr, "obj2goff: wrote %s (%lu bytes)\n", outname, (unsigned long)out.len);
    obj_free(&obj);
    free(out.p);
    free(data);
    return 0;
}
