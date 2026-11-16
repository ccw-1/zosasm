/* zosasm - a cross assembler for an HLASM-like language that writes z/OS
 * GOFF object files. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "disasm.h"
#include "goff.h"
#include "listing.h"
#include "pp.h"
#include "version.h"
#include "ebcdic.h"
#include "zsrc.h"
#include "zsym.h"

static void usage(FILE *f) {
    fprintf(f, "usage: zosasm [-o out.o] [-elf] [-ascii|-charset ascii|ebcdic] [-gz0|-gz1|-gz2] [-DNAME[=val]] [-Idir] [-Mdir] [-macro-steps N] [-E] [-reproducible] [-strict-registers] [-l FILE [-ldata] [-lpage N] [-lbrief]] [-diag text|json] [-color] [-Werror] [-max-errors N] source.za\n"
               "       zosasm -dump file.o\n"
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
               "  -Mdir           search dir for library macros (NAME.zmac, NAME.mac); the -I\n"
               "                  directories are searched after it\n"
               "  -macro-steps N  stop a macro expansion after N statements (default 1000000)\n"
               "  -E              print the preprocessed source and stop\n"
               "  -reproducible   fail on anything that changes between runs (__DATE__ and __TIME__\n"
               "                  need SOURCE_DATE_EPOCH)\n"
               "  -version        print the version\n"
               "  -dump FILE      print the structure of a GOFF object file and exit\n"
               "  -disasm FILE    convert a GOFF object back to source (stdout or -o)\n"
               "  -unpack FILE    write the original sources kept in an object made with -gz2 below\n"
               "                  the directory given with -o (default .)\n"
               "  -gz0|-gz1|-gz2  symbolic object information (ZSYM) in the object: none | symbols and\n"
               "                  statements (default) | also the original source files\n"
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

static int do_dump(const char *path) {
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
    goff_dump(m, stdout);
    {
        size_t zlen;
        uint8_t *z = zsym_extract(m, &zlen);
        if (z) {
            putchar('\n');
            zsym_dump(zsym_parse(z, zlen), stdout);
        }
        z = zsrc_extract(m, &zlen);
        if (z) {
            putchar('\n');
            zsrc_dump(zsrc_parse(z, zlen), stdout);
        }
    }
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
    if (!obj) return 1;
    if (skip_z) {
        const char *e2 = NULL;
        GoffModule *m2 = goff_parse(obj, olen, &e2);
        return m2 && goff_equal(m, skip_z, nskip, m2) ? 0 : 2;
    }
    if (olen != n || memcmp(obj, orig, n) != 0) {
        size_t i = 0;
        while (i < n && i < olen && obj[i] == (uint8_t)orig[i]) i++;
        *diff_at = i;
        return 2;
    }
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
    if (k < 0) return 1;
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
    return 0;
}

static int do_disasm(const char *path, const char *out, int generic) {
    size_t n, tlen, k, at;
    const char *perr = NULL;
    GoffModule *m;
    char *b = read_file(path, &n), *text, err[256];
    int nwarn = 0, smart = 0, rc;
    uint32_t skip_z = 0, nskip = 0;
    Reporter *rep;
    FILE *f;
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
        text = disasm_ex(m, DISASM_GENERIC, NULL, &nwarn, NULL, err, sizeof err);
        if (!text) {
            fprintf(stderr, "zosasm: %s: %s\n", path, err);
            return 1;
        }
        rc = check_text(m, b, n, skip_z, nskip, text, &rep, &at);
    }
    tlen = strlen(text);
    f = out ? fopen(out, "wb") : stdout;
    if (!f || fwrite(text, 1, tlen, f) != tlen || (out && fclose(f) != 0)) {
        fprintf(stderr, "zosasm: %s: %s\n", out ? out : "stdout", strerror(errno));
        return 1;
    }
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
    return 0;
}

static char *default_output(const char *in) {
    const char *slash = strrchr(in, '/');
    const char *dot = strrchr(in, '.');
    size_t n = strlen(in);
    if (dot && (!slash || dot > slash)) n = (size_t)(dot - in);
    return xasprintf("%.*s.o", (int)n, in);
}

int main(int argc, char **argv) {
    const char *out = NULL, *in = NULL, *disasm = NULL, *unpack = NULL;
    int color = 0, json = 0, werror = 0, max_err = 50, i, only_pp = 0, zlevel = 1, generic = 0, elf = 0;
    const char *charset = NULL;
    PpOptions ppo;
    SrcVec lines = {0};
    size_t n, olen;
    char *text, err[256], *outname;
    Reporter *rep;
    Module *mod;
    uint8_t *obj;
    FILE *f;
    const char *listfile = NULL;
    ListOptions lo;
    PpEventVec events = {0};

    memset(&ppo, 0, sizeof ppo);
    memset(&lo, 0, sizeof lo);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-') a++; /* accept --flag too */
        if (!strcmp(a, "-o") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(a, "-dump") && i + 1 < argc) {
            return do_dump(argv[i + 1]);
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
        } else if (!strcmp(a, "-macro-steps") && i + 1 < argc) {
            ppo.macro_steps = atol(argv[++i]);
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
            lo.page = atoi(argv[++i]);
            if (lo.page < 0) lo.page = 0;
        } else if (!strcmp(a, "-generic")) {
            generic = 1;
        } else if (!strcmp(a, "-elf")) {
            elf = 1;
        } else if (!strcmp(a, "-ascii")) {
            charset = "ascii";
        } else if (!strcmp(a, "-charset") && i + 1 < argc) {
            charset = argv[++i];
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
            max_err = atoi(argv[++i]);
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
    if (charset && strcmp(charset, "ascii") != 0 && strcmp(charset, "ebcdic") != 0) {
        fprintf(stderr, "zosasm: -charset takes ascii or ebcdic, not %s\n", charset);
        return 2;
    }
    za_ascii = charset ? !strcmp(charset, "ascii") : elf;
    ppo.elf = elf;
    if (unpack) return do_unpack(unpack, out);
    if (disasm) return do_disasm(disasm, out, generic);
    if (!in) {
        usage(stderr);
        return 2;
    }
    text = read_file(in, &n);
    if (!text) {
        fprintf(stderr, "zosasm: %s: %s\n", in, strerror(errno));
        return 1;
    }
    rep = rep_new(max_err);
    rep_set_warn_error(rep, werror);
    rep_add_source(rep, in, text, n);
    if (listfile) ppo.events = &events;
    pp_run(in, text, n, &ppo, rep, &lines);
    if (only_pp) {
        size_t k;
        for (k = 0; k < lines.n; k++) puts(lines.v[k].text);
        if (json) rep_print_json(rep, stderr);
        else rep_print(rep, stderr, color);
        return rep_error_count(rep) > 0;
    }
    if (listfile) {
        /* the listing is written whatever happened */
        mod = asm_assemble_all(lines.v, lines.n, in, rep);
        f = !strcmp(listfile, "-") ? stdout : fopen(listfile, "wb");
        if (!f) {
            fprintf(stderr, "zosasm: %s: %s\n", listfile, strerror(errno));
            return 1;
        }
        listing_write(f, mod, &lines, &events, rep, &lo);
        if (f != stdout && fclose(f) != 0) {
            fprintf(stderr, "zosasm: %s: %s\n", listfile, strerror(errno));
            return 1;
        }
        if (f == stdout) fflush(f);
    } else {
        mod = rep_error_count(rep) > 0 ? NULL : asm_assemble(lines.v, lines.n, in, rep);
    }
    if (json) rep_print_json(rep, stderr);
    else rep_print(rep, stderr, color);
    if (!mod || rep_error_count(rep) > 0) return 1;
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
    obj = elf ? module_emit_elf(mod, &olen, err, sizeof err) : module_emit(mod, &olen, err, sizeof err);
    if (!obj) {
        fprintf(stderr, "zosasm: %s: cannot write object: %s\n", in, err);
        return 1;
    }
    outname = out ? xstrdup(out) : default_output(in);
    f = fopen(outname, "wb");
    if (!f || fwrite(obj, 1, olen, f) != olen || fclose(f) != 0) {
        fprintf(stderr, "zosasm: %s: %s\n", outname, strerror(errno));
        return 1;
    }
    return 0;
}
