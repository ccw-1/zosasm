/* zosasm - a cross assembler for an HLASM-like language that writes z/OS
 * GOFF object files. */
#ifndef _XOPEN_SOURCE
#  define _XOPEN_SOURCE 700
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "asm.h"
#include "dbg.h"
#include "disasm.h"
#include "goff.h"
#include "listing.h"
#include "pp.h"
#include "version.h"
#include "ebcdic.h"
#include "zsrc.h"
#include "zsym.h"

static void usage(FILE *f) {
    fprintf(f, "usage: zosasm [-o out.o] [-elf] [-ascii|-charset ascii|ebcdic] [-gz0|-gz1|-gz2] [-g [-gside] [-gdwarf FILE] [-gdir DIR] [-gmap FROM=TO] [-gdbgpath PATH]] [-DNAME[=val]] [-Idir] [-Mdir] [-sysparm STR] [-macro-steps N] [-E] [-reproducible] [-strict-registers] [-l FILE [-ldata] [-lpage N] [-lbrief]] [-diag text|json] [-color] [-Werror] [-max-errors N] source.za (or - for standard input)\n"
               "       zosasm -dump file.o\n"
               "       zosasm -dump-readable file.o [-insn]\n"
               "       zosasm [-o out.za] -disasm file.o\n"
               "       zosasm [-o dir] -unpack file.o\n"
               "  -o FILE         output object file (default: input with .o)\n"
               "  -elf            write a relocatable ELF64 object for Linux on Z (s390x) instead of GOFF;\n"
               "                  implies -ascii, ignores -gz\n"
               "  -ascii          same as -charset ascii\n"
               "  -charset NAME   character set of C'..' constants and the type attribute: ascii\n"
               "                  (ISO-8859-1) or ebcdic (IBM-1047); default ebcdic, ascii with -elf\n"
               "                  (the preprocessor defines __ELF__ or __GOFF__, __ASCII__ or __EBCDIC__)\n"
               "  -DNAME[=VAL]    define a preprocessor macro (default value 1)\n"
               "  -Idir           search dir for #include files\n"
               "  -Mdir           search dir for library macros (NAME.zmac, NAME.mac); after the\n"
               "                  source and #pragma macro_path directories, before ZOSASM_MACLIB\n"
               "                  and the -I directories\n"
               "  -sysparm STR    the value of sys.parm in macros (HLASM &SYSPARM)\n"
               "  -macro-steps N  stop a macro expansion after N statements (default 1000000)\n"
               "  -E              print the preprocessed source and stop\n"
               "  -reproducible   fail on anything that changes between runs (__DATE__ and __TIME__\n"
               "                  need SOURCE_DATE_EPOCH)\n"
               "  -version        print the version\n"
               "  -dump FILE      print the machine-readable structure of a GOFF object and exit\n"
               "  -dump-readable FILE  print a readable GOFF listing and exit\n"
               "  -insn           with -dump/-dump-readable: disassemble the text records\n"
               "  -disasm FILE    convert a GOFF object back to source (stdout or -o)\n"
               "  -unpack FILE    write the original sources kept in an object made with -gz2 below\n"
               "                  the directory given with -o (default .)\n"
               "  -gz0|-gz1|-gz2  symbolic object information (ZSYM) in the object: none | symbols and\n"
               "                  statements (default) | also the original source files\n"
               "  -g              the DWARF (as z/OS as -g) goes into the object, in the link module\n"
               "                  appended to it (class C_ADATAE9C3, NOLOAD); the paths in the debug\n"
               "                  information are those of the z/OS host (chapter 21). With -elf the\n"
               "                  DWARF sections (.debug_*) are in the ELF object itself\n"
               "  -gside          also write FILE.dbg with the same DWARF (for dbx); implies -g\n"
               "  -gdwarf FILE    write the .dbg file here (default: the object name with .dbg); implies -gside\n"
               "  -gdir DIR       the z/OS directory that corresponds to the current directory\n"
               "  -gmap FROM=TO   a path prefix of this machine and its z/OS counterpart (repeatable)\n"
               "  -gdbgpath PATH  where the .dbg will be on z/OS (default: its path made with -gdir/-gmap)\n"
               "  -strict-registers  in a register position accept only %%r1 (any case; %%f %%a %%c %%v too)\n"
               "                  or a name made with NAME EQU %%r1, not plain numbers or other symbols;\n"
               "                  with -disasm the source is written that way\n"
               "  -l FILE         write an assembly listing to FILE (- = standard output), also when\n"
               "                  the assembly has errors\n"
               "  -ldata          listing: show all object bytes of a statement (default: 8)\n"
               "  -lpage N        listing: N lines per page with a header (default 0: one page)\n"
               "  -lbrief         listing: no tables at the end\n"
               "  -generic        with -disasm: ignore the ZSYM information\n"
               "  -diag text|json diagnostics as text (default) or as one JSON document on stderr\n"
               "  -color          colorize diagnostics\n"
               "  -max-errors N   stop after this many errors (default 50, 0 = no limit)\n"
               "  -Werror         treat warnings as errors\n");
}

/* Write a whole file atomically. "-" is standard output and "/dev/null" is
 * opened directly (a temporary file in /dev cannot be created). */
static int out_write(const char *path, const void *data, size_t n) {
    if (!strcmp(path, "-")) {
        if (n && fwrite(data, 1, n, stdout) != n) {
            fprintf(stderr, "zosasm: stdout: %s\n", strerror(errno));
            return 0;
        }
        return 1;
    }
    if (!strcmp(path, "/dev/null")) {
        FILE *f = fopen(path, "wb");
        int ok = f != NULL;
        if (ok && n && fwrite(data, 1, n, f) != n) ok = 0;
        if (f && fclose(f) != 0) ok = 0;
        if (!ok) fprintf(stderr, "zosasm: %s: %s\n", path, strerror(errno));
        return ok;
    }
    {
        char *tmp = xasprintf("%s.tmp.%d", path, (int)getpid());
        FILE *f = fopen(tmp, "wb");
        int ok = f != NULL && (n == 0 || fwrite(data, 1, n, f) == n);
        if (f && fclose(f) != 0) ok = 0;
        if (!ok) {
            fprintf(stderr, "zosasm: %s: %s\n", path, strerror(errno));
            if (f) remove(tmp);
            free(tmp);
            return 0;
        }
        if (rename(tmp, path) != 0) {
            fprintf(stderr, "zosasm: %s: %s\n", path, strerror(errno));
            remove(tmp);
            free(tmp);
            return 0;
        }
        free(tmp);
    }
    return 1;
}

/* The path made absolute without requiring the final file to exist: the
 * directory is resolved with realpath(), the base name is appended. */
static char *canon_path(const char *p) {
    const char *slash = strrchr(p, '/');
    char *dir, *base, *rd, *r;
    if (!slash) {
        dir = xstrdup(".");
        base = xstrdup(p);
    } else if (slash == p) {
        dir = xstrdup("/");
        base = xstrdup(slash + 1);
    } else {
        dir = xstrndup(p, (size_t)(slash - p));
        base = xstrdup(slash + 1);
    }
    rd = realpath(dir, NULL);
    if (!rd) r = xstrdup(p);
    else if (!strcmp(rd, "/")) r = xasprintf("/%s", base);
    else r = xasprintf("%s/%s", rd, base);
    free(rd);
    free(dir);
    free(base);
    return r;
}

static int same_path(const char *a, const char *b) {
    char *ca, *cb;
    int eq;
    if (!a || !b) return 0;
    if (!strcmp(a, "-") || !strcmp(b, "-") || !strcmp(a, "/dev/null") || !strcmp(b, "/dev/null"))
        return !strcmp(a, b);
    ca = canon_path(a);
    cb = canon_path(b);
    eq = !strcmp(ca, cb);
    free(ca);
    free(cb);
    return eq;
}

static char *dbg_name_for(const char *outname, const char *gdwarf) {
    const char *slash, *dot;
    size_t k;
    if (gdwarf) return xstrdup(gdwarf);
    slash = strrchr(outname, '/');
    dot = strrchr(outname, '.');
    k = strlen(outname);
    if (dot && (!slash || dot > slash)) k = (size_t)(dot - outname);
    return xasprintf("%.*s.dbg", (int)k, outname);
}

static int do_dump(const char *path, int readable, int insn) {
    size_t n;
    const char *err = NULL;
    GoffModule *m;
    char *b = read_file(path, &n);
    if (!b) {
        fprintf(stderr, "zosasm: %s: %s\n", path, strerror(errno));
        return 1;
    }
    m = goff_parse((const uint8_t *)b, n, &err);
    if (!m) {
        fprintf(stderr, "zosasm: %s: %s\n", path, err);
        return 1;
    }
    if (readable) goff_dump_readable(m, stdout, insn);
    else goff_dump(m, stdout);
    {
        /* further modules (the debug link module written by -g) */
        size_t at = m->bytes;
        int k = 2;
        while (at < n) {
            GoffModule *m2 = goff_parse((const uint8_t *)b + at, n - at, &err);
            if (!m2) {
                fprintf(stderr, "zosasm: %s: module %d: %s\n", path, k, err);
    return 1;
            }
            printf("\n=== module %d at byte %lu ===\n", k++, (unsigned long)at);
            if (readable) goff_dump_readable(m2, stdout, insn);
            else goff_dump(m2, stdout);
            at += m2->bytes;
            goff_free(m2);
        }
    }
    {
        size_t zlen;
        uint8_t *z = zsym_extract(m, &zlen);
        if (z) {
            Zsym *y = zsym_parse(z, zlen);
            putchar('\n');
            zsym_dump(y, stdout);
            zsym_free(y);
            free(z);
        }
        z = zsrc_extract(m, &zlen);
        if (z) {
            ZSrc *y = zsrc_parse(z, zlen);
            putchar('\n');
            zsrc_dump(y, stdout);
            zsrc_free(y);
            free(z);
        }
    }
    goff_free(m);
    free(b);
    return 0;
}

/* Assemble the generated source (without ZSYM) and compare it with the
 * object: 0 equal, 1 does not assemble, 2 differs. */
static int check_text(const GoffModule *m, const char *orig, size_t n, uint32_t skip_z, uint32_t nskip, const char *text,
                      Reporter **rep_out, size_t *diff_at) {
    size_t olen;
    char err[256];
    Reporter *rep = rep_new(50);
    Module *mod = asm_assemble_text("(disassembly)", text, strlen(text), rep); /* strict with -strict-registers */
    uint8_t *obj = mod ? module_emit(mod, &olen, err, sizeof err) : NULL; /* zlevel 0: no ZSYM */
    *rep_out = rep;
    *diff_at = 0;
    if (!obj) { return 1; }
    if (skip_z) {
        const char *e2 = NULL;
        GoffModule *m2 = goff_parse(obj, olen, &e2);
        int r = m2 && goff_equal(m, skip_z, nskip, m2) ? 0 : 2;
        goff_free(m2);
        free(obj);
        return r;
    }
    if (olen != n || memcmp(obj, orig, n) != 0) {
        size_t i = 0;
        while (i < n && i < olen && obj[i] == (uint8_t)orig[i]) i++;
        *diff_at = i;
        free(obj);
        return 2;
    }
    free(obj);
    return 0;
}

/* Convert the object to source, then assemble that source again and say
 * whether it gives the same object. An object with ZSYM is converted from it;
 * if that source does not give the object back, the generic converter
 * is used instead. */
/* Write the original sources of an object made with -gz2. */
static int do_unpack(const char *path, const char *dir) {
    size_t n, zlen;
    const char *err = NULL;
    GoffModule *m;
    uint8_t *z;
    ZSrc *s;
    int k;
    char *b = read_file(path, &n);
    if (!b) {
        fprintf(stderr, "zosasm: %s: %s\n", path, strerror(errno));
        return 1;
    }
    m = goff_parse((const uint8_t *)b, n, &err);
    if (!m) {
        fprintf(stderr, "zosasm: %s: %s\n", path, err);
        return 1;
    }
    z = zsrc_extract(m, &zlen);
    if (!z) {
        fprintf(stderr, "zosasm: %s: no original sources in the object (assemble with -gz2)\n", path);
        return 1;
    }
    s = zsrc_parse(z, zlen);
    k = zsrc_unpack(s, dir ? dir : ".", stdout);
    if (k < 0) { zsrc_free(s); free(z); goff_free(m); free(b); return 1; }
    if (s->bad_chunks || k == 0)
        fprintf(stderr, "zosasm: %s: warning: the sources are damaged or incomplete\n", path);
    else {
        size_t i;
        for (i = 0; i < s->blocks.n; i++)
            if (!s->blocks.v[i]->complete) {
                fprintf(stderr, "zosasm: %s: warning: block %zu is damaged or incomplete\n", path, i + 1);
                break;
            }
    }
    zsrc_free(s);
    free(z);
    goff_free(m);
    free(b);
    return 0;
}

static int do_disasm(const char *path, const char *out, int generic) {
    size_t n, tlen, k, at;
    if (out && same_path(out, path)) {
        fprintf(stderr, "zosasm: -o names the input object file: %s\n", path);
        return 2;
    }
    const char *perr = NULL;
    GoffModule *m;
    char *b = read_file(path, &n), *text, err[256];
    int nwarn = 0, smart = 0, rc;
    uint32_t skip_z = 0, nskip = 0;
    Reporter *rep;
    if (!b) {
        fprintf(stderr, "zosasm: %s: %s\n", path, strerror(errno));
        return 1;
    }
    m = goff_parse((const uint8_t *)b, n, &perr);
    if (!m) {
        fprintf(stderr, "zosasm: %s: %s\n", path, perr);
        return 1;
    }
    for (k = 0; k < m->esds.n; k++) {
        const GoffESD *e = m->esds.v[k];
        char *nm = e->type == GOFF_ED ? ebcdic_decode(e->name, e->name_len) : NULL;
        if (nm && (!strcmp(nm, ZSYM_CLASS) || !strcmp(nm, ZSRC_CLASS))) {
            if (!skip_z) skip_z = (uint32_t)k + 1;
            nskip++;
        }
    }
    text = disasm_ex(m, (generic ? DISASM_GENERIC : 0) | (za_strict_registers ? DISASM_STRICT : 0), stderr, &nwarn, &smart, err, sizeof err);
    if (!text) {
        fprintf(stderr, "zosasm: %s: %s\n", path, err);
        return 1;
    }
    rc = check_text(m, b, n, skip_z, nskip, text, &rep, &at);
    if (smart && rc) {
        fprintf(stderr, "zosasm: warning: the source made from the ZSYM information %s; using the generic "
                        "converter\n", rc == 1 ? "does not assemble" : "does not give the object back");
        if (rc == 1) rep_print(rep, stderr, 0);
        rep_free(rep);
        free(text);
        text = disasm_ex(m, DISASM_GENERIC, NULL, &nwarn, NULL, err, sizeof err);
        if (!text) {
            fprintf(stderr, "zosasm: %s: %s\n", path, err);
    return 1;
        }
        rc = check_text(m, b, n, skip_z, nskip, text, &rep, &at);
    }
    tlen = strlen(text);
    if (!out_write(out ? out : "-", text, tlen)) return 1;
    if (rc == 1) {
        fprintf(stderr, "zosasm: warning: the generated source does not assemble\n");
        rep_print(rep, stderr, 0);
    } else if (rc == 2 && skip_z) {
        fprintf(stderr, "zosasm: warning: the generated source assembles to a different object\n");
    } else if (rc == 2) {
        fprintf(stderr,
                "zosasm: warning: the generated source assembles to a different object (first "
                "difference at byte %zu, record %zu)\n",
                at, at / GOFF_RECORD_LEN + 1);
    }
    rep_free(rep);
    goff_free(m);
    free(b);
    free(text);
    return 0;
}

static char *default_output(const char *in) {
    const char *slash = strrchr(in, '/');
    const char *dot = strrchr(in, '.');
    size_t n = strlen(in);
    if (!strcmp(in, "(stdin)")) return xstrdup("a.o");
    if (dot && (!slash || dot > slash)) n = (size_t)(dot - in);
    return xasprintf("%.*s.o", (int)n, in);
}

int main(int argc, char **argv) {
    const char *out = NULL, *in = NULL, *disasm = NULL, *unpack = NULL, *dump = NULL, *dump_readable = NULL;
    int color = 0, json = 0, werror = 0, max_err = 50, i, only_pp = 0, zlevel = 1, generic = 0, elf = 0, gflag = 0, insn = 0;
    const char *gdwarf = NULL, *gdbgpath = NULL;
    int gside = 0;
    DbgOptions dopt;
    const char *charset = NULL;
    PpOptions ppo;
    SrcVec lines = {0};
    size_t n, olen;
    char *text, err[256], *outname = NULL, *dbgname = NULL;
    Reporter *rep;
    Module *mod = NULL;
    uint8_t *obj;
    FILE *f;
    const char *listfile = NULL;
    ListOptions lo;
    PpEventVec events = {0};
    CBlockDebugVec cbdbg = {0};

    memset(&ppo, 0, sizeof ppo);
    memset(&lo, 0, sizeof lo);
    memset(&dopt, 0, sizeof dopt);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-') a++; /* accept --flag too */
        if (!strcmp(a, "-o") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(a, "-dump")) {
            dump = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "";
        } else if (!strcmp(a, "-dump-readable")) {
            dump_readable = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "";
        } else if (!strcmp(a, "-insn")) {
            insn = 1;
        } else if (!strcmp(a, "-disasm") && i + 1 < argc) {
            disasm = argv[++i];
        } else if (!strcmp(a, "-unpack") && i + 1 < argc) {
            unpack = argv[++i];
        } else if (!strncmp(a, "-D", 2) && (a[2] || i + 1 < argc)) {
            vec_push(&ppo.defines, a[2] ? a + 2 : argv[++i]);
        } else if (!strncmp(a, "-I", 2) && (a[2] || i + 1 < argc)) {
            vec_push(&ppo.include_dirs, a[2] ? a + 2 : argv[++i]);
        } else if (!strncmp(a, "-M", 2) && a[2] && strcmp(a, "-macro-steps") != 0) {
            vec_push(&ppo.macro_dirs, a + 2);
        } else if (!strcmp(a, "-M") && i + 1 < argc) {
            vec_push(&ppo.macro_dirs, argv[++i]);
        } else if (!strcmp(a, "-sysparm") && i + 1 < argc) {
            ppo.sysparm = argv[++i];
        } else if (!strcmp(a, "-macro-steps") && i + 1 < argc) {
            int v;
            if (!za_parse_int(argv[++i], &v)) {
                fprintf(stderr, "zosasm: bad value for -macro-steps: %s\n", argv[i]);
                return 2;
            }
            ppo.macro_steps = v;
        } else if (!strcmp(a, "-E")) {
            only_pp = 1;
        } else if (!strcmp(a, "-reproducible")) {
            ppo.reproducible = 1;
        } else if (!strcmp(a, "-version")) {
            printf("zosasm %s\n", ZASM_VERSION_STR);
    return 0;
        } else if (!strcmp(a, "-strict-registers")) {
            za_strict_registers = 1;
        } else if (!strcmp(a, "-l") && i + 1 < argc) {
            listfile = argv[++i];
        } else if (!strcmp(a, "-ldata")) {
            lo.data = 1;
        } else if (!strcmp(a, "-lbrief")) {
            lo.brief = 1;
        } else if (!strcmp(a, "-lpage") && i + 1 < argc) {
            int v;
            if (!za_parse_int(argv[++i], &v)) {
                fprintf(stderr, "zosasm: bad value for -lpage: %s\n", argv[i]);
                return 2;
            }
            lo.page = v;
        } else if (!strcmp(a, "-generic")) {
            generic = 1;
        } else if (!strcmp(a, "-elf")) {
            elf = 1;
        } else if (!strcmp(a, "-ascii")) {
            charset = "ascii";
        } else if (!strcmp(a, "-charset") && i + 1 < argc) {
            charset = argv[++i];
        } else if (!strcmp(a, "-g")) {
            gflag = 1;
        } else if (!strcmp(a, "-gside")) {
            gside = 1;
            gflag = 1;
        } else if (!strcmp(a, "-gdwarf") && i + 1 < argc) {
            gdwarf = argv[++i];
            gside = 1;
            gflag = 1;
        } else if (!strcmp(a, "-gdir") && i + 1 < argc) {
            dopt.gdir = argv[++i];
            gflag = 1;
        } else if (!strcmp(a, "-gdbgpath") && i + 1 < argc) {
            gdbgpath = argv[++i];
            gflag = 1;
        } else if (!strcmp(a, "-gmap") && i + 1 < argc) {
            char *m = xstrdup(argv[++i]), *eq = strchr(m, '=');
            if (!eq || eq == m || !eq[1]) {
                fprintf(stderr, "zosasm: -gmap takes FROM=TO\n");
    return 2;
            }
            *eq = 0;
            vec_push(&dopt.map_from, m);
            vec_push(&dopt.map_to, eq + 1);
            gflag = 1;
        } else if (!strcmp(a, "-gz0")) {
            zlevel = 0;
        } else if (!strcmp(a, "-gz1")) {
            zlevel = 1;
        } else if (!strcmp(a, "-gz2")) {
            zlevel = 2;
        } else if (!strcmp(a, "-color")) {
            color = 1;
        } else if (!strcmp(a, "-diag") && i + 1 < argc) {
            const char *f = argv[++i];
            if (!strcmp(f, "json")) {
                json = 1;
            } else if (!strcmp(f, "text")) {
                json = 0;
            } else {
                fprintf(stderr, "zosasm: -diag takes text or json, not %s\n", f);
    return 2;
            }
        } else if (!strcmp(a, "-Werror")) {
            werror = 1;
        } else if (!strcmp(a, "-max-errors") && i + 1 < argc) {
            int v;
            if (!za_parse_int(argv[++i], &v)) {
                fprintf(stderr, "zosasm: bad value for -max-errors: %s\n", argv[i]);
                return 2;
            }
            max_err = v;
        } else if (!strcmp(a, "-h") || !strcmp(a, "-help")) {
            usage(stdout);
    return 0;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "zosasm: unknown or incomplete option %s\n", argv[i]);
            usage(stderr);
    return 2;
        } else if (!in) {
            in = argv[i];
        } else {
            usage(stderr);
    return 2;
        }
    }
    if (only_pp && listfile) {
        fprintf(stderr, "zosasm: -E does not write a listing; do not combine it with -l\n");
        return 2;
    }
    if (charset && strcmp(charset, "ascii") != 0 && strcmp(charset, "ebcdic") != 0) {
        fprintf(stderr, "zosasm: -charset takes ascii or ebcdic, not %s\n", charset);
        return 2;
    }
    if (gflag && elf && (gside || gdbgpath)) {
        fprintf(stderr, "zosasm: -gside, -gdwarf and -gdbgpath write a .dbg file, which -elf does not use "
                        "(with -elf the DWARF is in the object)\n");
        return 2;
    }
    za_ascii = charset ? !strcmp(charset, "ascii") : elf;
    ppo.elf = elf;
    if ((dump != NULL ? 1 : 0) + (dump_readable != NULL ? 1 : 0) + (unpack ? 1 : 0) +
        (disasm ? 1 : 0) + (only_pp ? 1 : 0) > 1) {
        fprintf(stderr, "zosasm: choose only one of -E, -dump, -dump-readable, -disasm or -unpack\n");
        return 2;
    }
    if (dump != NULL || dump_readable != NULL) {
        const char *path = dump && *dump ? dump : dump_readable && *dump_readable ? dump_readable : in;
        for (i = 1; i < argc; i++) {
            const char *a = argv[i];
            if (a[0] == '-' && a[1] == '-') a++;
            if (!strcmp(a, "-dump") || !strcmp(a, "-dump-readable")) {
                if (i + 1 < argc && argv[i + 1][0] != '-') i++;
                continue;
            }
            if (!strcmp(a, "-insn")) continue;
            if (a[0] != '-') continue;
            fprintf(stderr, "zosasm: -dump takes only -insn as another option\n");
            return 2;
        }
        if (!path) {
            fprintf(stderr, "zosasm: -dump needs an object file\n");
            return 2;
        }
        return do_dump(path, dump_readable != NULL || insn, insn);
    }
    if (insn) {
        fprintf(stderr, "zosasm: -insn needs -dump or -dump-readable\n");
        return 2;
    }
    if (unpack) return do_unpack(unpack, out);
    if (disasm) return do_disasm(disasm, out, generic);
    if (!in) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(in, "-")) {
        Buf b = {0};
        char tmp[8192];
        size_t k;
        in = "(stdin)";
        while ((k = fread(tmp, 1, sizeof tmp, stdin)) > 0) buf_write(&b, tmp, k);
        text = xstrdup(buf_cstr(&b));
        n = b.len;
        free(b.p);
    } else {
        text = read_file(in, &n);
        if (!text) {
            fprintf(stderr, "zosasm: %s: %s\n", in, strerror(errno));
            return 1;
        }
    }
    outname = out ? xstrdup(out) : default_output(in);
    if (!only_pp) {
        if (gflag && !elf) dbgname = dbg_name_for(outname, gdwarf);
        if (gflag && !elf && gside) {
            if (!gdwarf && (!strcmp(outname, "-") || !strcmp(outname, "/dev/null"))) {
                fprintf(stderr, "zosasm: -gside needs a real object file name (use -gdwarf FILE with -o %s)\n", outname);
                return 2;
            }
            if (!strcmp(dbgname, "-")) {
                fprintf(stderr, "zosasm: -gdwarf - would put the .dbg file on standard output\n");
                return 2;
            }
            if (same_path(outname, dbgname)) {
                fprintf(stderr, "zosasm: -o and -gside output name the same file: %s\n", outname);
                return 2;
            }
        }
        if (strcmp(in, "(stdin)") && same_path(outname, in)) {
            fprintf(stderr, "zosasm: -o names the input source file: %s\n", in);
            return 2;
        }
        if (listfile && strcmp(in, "(stdin)") && same_path(listfile, in)) {
            fprintf(stderr, "zosasm: -l names the input source file: %s\n", in);
            return 2;
        }
        if (listfile && same_path(outname, listfile)) {
            fprintf(stderr, "zosasm: -o and -l name the same file: %s\n", outname);
            return 2;
        }
        if (listfile && gflag && !elf && gside && same_path(listfile, dbgname)) {
            fprintf(stderr, "zosasm: -l and -gside output name the same file: %s\n", listfile);
            return 2;
        }
    }
    rep = rep_new(max_err);
    rep_set_warn_error(rep, werror);
    rep_add_source(rep, in, text, n);
    if (listfile) ppo.events = &events;
    if (gflag) ppo.cblock_debug = &cbdbg;
    pp_run(in, text, n, &ppo, rep, &lines);
    if (only_pp) {
        Buf b = {0};
        size_t k;
        int ok;
        for (k = 0; k < lines.n; k++) {
            buf_puts(&b, lines.v[k].text);
            buf_putc(&b, '\n');
        }
        ok = out_write(out ? out : "-", b.p ? b.p : "", b.len);
        free(b.p);
        if (json) rep_print_json(rep, stderr);
        else rep_print(rep, stderr, color);
        return ok ? (rep_error_count(rep) > 0) : 1;
    }
    if (listfile) {
        /* the listing is written whatever happened */
        mod = asm_assemble_all(lines.v, lines.n, in, rep);
        if (!strcmp(listfile, "-")) {
            listing_write(stdout, mod, &lines, &events, rep, &lo);
            fflush(stdout);
        } else if (!strcmp(listfile, "/dev/null")) {
            f = fopen(listfile, "wb");
            if (!f) { fprintf(stderr, "zosasm: %s: %s\n", listfile, strerror(errno)); return 1; }
            listing_write(f, mod, &lines, &events, rep, &lo);
            if (fclose(f) != 0) { fprintf(stderr, "zosasm: %s: %s\n", listfile, strerror(errno)); return 1; }
        } else {
            char *tmp = xasprintf("%s.tmp.%d", listfile, (int)getpid());
            int ok;
            f = fopen(tmp, "wb");
            if (!f) { fprintf(stderr, "zosasm: %s: %s\n", listfile, strerror(errno)); free(tmp); return 1; }
            listing_write(f, mod, &lines, &events, rep, &lo);
            ok = fclose(f) == 0;
            if (!ok) { fprintf(stderr, "zosasm: %s: %s\n", listfile, strerror(errno)); remove(tmp); free(tmp); return 1; }
            if (rename(tmp, listfile) != 0) { fprintf(stderr, "zosasm: %s: %s\n", listfile, strerror(errno)); remove(tmp); free(tmp); return 1; }
            free(tmp);
        }
    } else {
        mod = rep_error_count(rep) > 0 ? NULL : asm_assemble(lines.v, lines.n, in, rep);
    }
    if (json) rep_print_json(rep, stderr);
    else rep_print(rep, stderr, color);
    if (!mod || rep_error_count(rep) > 0) {
        cblock_debug_free(&cbdbg);
        return 1;
    }
    mod->cblocks = cbdbg;
    memset(&cbdbg, 0, sizeof cbdbg);
    mod->zlevel = zlevel;
    if (zlevel >= 2) {
        const char *sn, *st;
        size_t sl, k;
        for (k = 0; rep_get_source(rep, k, &sn, &st, &sl); k++) {
            ZSrcIn f;
            f.name = xstrdup(sn);
            f.text = xstrndup(st, sl);
            f.len = sl;
            vec_push(&mod->zsrc, f);
        }
        for (k = 0; k < ppo.defines.n; k++) vec_push(&mod->zdefs, xstrdup(ppo.defines.v[k]));
    }
    if (gflag && elf) {
        DbgElfSec *dsecs = NULL;
        size_t nd = 0;
        char cwd[4096];
        dopt.cwd = getcwd(cwd, sizeof cwd) ? cwd : "";
        dopt.main_file = in;
        dopt.producer = xasprintf("zosasm %s", ZASM_VERSION_STR);
        dopt.dbg_path = "";
        if (!dbg_build_elf(mod, &dopt, &dsecs, &nd, err, sizeof err)) {
            fprintf(stderr, "zosasm: %s: %s\n", in, err);
            return 1;
        }
        obj = module_emit_elf_dbg(mod, dsecs, nd, &olen, err, sizeof err);
        for (i = 0; i < (int)nd; i++) free(dsecs[i].rels);
        free(dsecs);
    } else {
        obj = elf ? module_emit_elf(mod, &olen, err, sizeof err) : module_emit(mod, &olen, err, sizeof err);
    }
    if (!obj) {
        fprintf(stderr, "zosasm: %s: cannot write object: %s\n", in, err);
        return 1;
    }
    if (gflag && !elf) {
        DbgResult dr;
        char cwd[4096];
        Buf ob = {0};
        dopt.cwd = getcwd(cwd, sizeof cwd) ? cwd : "";
        dopt.main_file = in;
        dopt.producer = xasprintf("zosasm %s", ZASM_VERSION_STR);
        dopt.dbg_path = gdbgpath ? gdbgpath : dbg_host_path(&dopt, dbgname);
        if (!dbg_build(mod, obj, olen, &dopt, &dr, err, sizeof err)) {
            fprintf(stderr, "zosasm: %s: %s\n", in, err);
            return 1;
        }
        buf_write(&ob, obj, olen);
        buf_write(&ob, dr.mod2, dr.mod2_len);
        free(dr.mod2);
        if (!out_write(outname, ob.p, ob.len)) {
            free(ob.p);
            free(dr.elf);
            return 1;
        }
        free(ob.p);
        if (gside && !out_write(dbgname, dr.elf, dr.elf_len)) {
            free(dr.elf);
            return 1;
        }
        free(dr.elf);
    } else {
        if (!out_write(outname, obj, olen)) return 1;
    }
    asm_free(mod);
    rep_free(rep);
    free(obj);
    {
        size_t k;
        for (k = 0; k < lines.n; k++) {
            free((char *)lines.v[k].text);
            free((char *)lines.v[k].orig);
            free((int *)lines.v[k].plines);
            free((size_t *)lines.v[k].poffs);
        }
        free(lines.v);
    }
    free(text);
    free(outname);
    free(dbgname);
    free(ppo.defines.v);
    free(ppo.include_dirs.v);
    free(ppo.macro_dirs.v);
    {
        size_t k;
        for (k = 0; k < events.n; k++) free((char *)events.v[k].text);
    }
    free(events.v);
    return 0;
}
