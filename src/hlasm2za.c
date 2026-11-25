/* hlasm2za - convert HLASM source to zosasm source. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "convert.h"
#include "util.h"
#include "version.h"

static void usage(FILE *f) {
    fprintf(f,
            "usage: hlasm2za [options] input.asm\n"
            "  -o FILE         output file (default: the input with the extension .za; - = stdout)\n"
            "  -ictl B,E,C     source columns when the file has no ICTL (default 1,71,16)\n"
            "  -relaxed        leave register numbers as they are (default: %%r1 notation for\n"
            "                  -strict-registers)\n"
            "  -w N            break operand lists after a comma beyond column N (default 100, 0 = never)\n"
            "  -inc-ext EXT    extension of the files COPY becomes #include of (default .inc)\n"
            "  -m              the input is a macro library member: write only the macro definitions\n"
            "                  (default output extension .zmac)\n"
            "  -macro-dir DIR  convert every member DIR/*.mac like -m; -o names the output directory\n"
            "                  (default: next to the members); prints a summary\n"
            "  -copy-dir DIR   macros: COPY inside a macro definition reads DIR/MEMBER.mac (default: the\n"
            "                  -macro-dir directory)\n"
            "  -macro-path DIR write #pragma macro_path \"DIR\" at the start of the output (repeatable)\n"
            "  -flat           macros: #goto/#label for every sequence symbol, no structure recovery\n"
            "  -keep-goto      macros: where the flow cannot be structured, write #goto/#label instead of\n"
            "                  a dispatch loop (a switch on the next block)\n"
            "  -comment-macros leave macro definitions as comments (no conversion)\n"
            "  -q              no warnings\n"
            "  -version        print the version\n"
            "Input - is standard input. Warnings (what could not be converted) go to standard error.\n");
}

static const char *OUT_EXT = ".za";

/* prefix the converted source with #pragma macro_path lines, quoting the path */
static char *with_macro_paths(const char *src, char **paths, size_t npaths) {
    Buf b = {0};
    size_t i;
    for (i = 0; i < npaths; i++) {
        const char *p = paths[i];
        buf_puts(&b, "#pragma macro_path \"");
        for (; *p; p++) {
            if (*p == '"' || *p == '\\') buf_putc(&b, '\\');
            buf_putc(&b, *p);
        }
        buf_puts(&b, "\"\n");
    }
    buf_puts(&b, src);
    {
        char *r = xstrdup(buf_cstr(&b));
        free(b.p);
        return r;
    }
}

static char *default_output(const char *in) {
    const char *slash = strrchr(in, '/'), *dot = strrchr(in, '.');
    size_t n = strlen(in);
    if (dot && (!slash || dot > slash) && dot != in && (!slash || dot > slash + 1)) n = (size_t)(dot - in);
    return xasprintf("%.*s%s", (int)n, in, OUT_EXT);
}

static char *read_stdin(size_t *len) {
    Buf b = {0};
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, stdin)) > 0) buf_write(&b, tmp, n);
    *len = b.len;
    return xstrdup(buf_cstr(&b));
}

static int has_ext(const char *n, const char *ext) {
    size_t l = strlen(n), e = strlen(ext);
    return l > e && za_streqi(n + l - e, ext);
}

static void print_names(const char *what, size_t n, char **v) {
    size_t k;
    if (!n) return;
    fprintf(stderr, "hlasm2za:   %s (%lu):", what, (unsigned long)n);
    for (k = 0; k < n; k++) fprintf(stderr, " %s", v[k]);
    fprintf(stderr, "\n");
}

/* every member DIR/NAME.mac -> OUTDIR/NAME.zmac; a summary of what could not be converted */
static int macro_dir(const char *dir, const char *outdir, const ConvOptions *o, int quiet) {
    DIR *d = opendir(dir);
    struct dirent *de;
    Vec(char *) names = {0};
    size_t k, nmem = 0, nmac = 0, nfail = 0, nwarn = 0;
    int bad = 0; /* a member could not be read or its .zmac not written */
    Vec(char *) notasm = {0}, nomacro = {0}, failed = {0}; /* member names for the summary */
    if (!d) {
        fprintf(stderr, "hlasm2za: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    while ((de = readdir(d))) {
        if (has_ext(de->d_name, ".mac")) vec_push(&names, xstrdup(de->d_name));
    }
    closedir(d);
    if (outdir && mkdir(outdir, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "hlasm2za: %s: %s\n", outdir, strerror(errno));
        return 1;
    }
    for (k = 0; k < names.n; k++) {
        size_t len;
        char *path = xasprintf("%s/%s", dir, names.v[k]);
        char *text = read_file(path, &len), *res, *line;
        FILE *w = tmpfile();
        int nw = 0;
        long wl;
        char *wbuf;
        if (!text) {
            fprintf(stderr, "hlasm2za: %s: %s\n", path, strerror(errno));
            bad = 1;
            continue;
        }
        if (!w) {
            fprintf(stderr, "hlasm2za: cannot create a temporary file\n");
            bad = 1;
            continue;
        }
        res = hlasm_convert(text, len, path, o, w, &nw);
        nmem++;
        wl = ftell(w);
        rewind(w);
        wbuf = xcalloc((size_t)wl + 1);
        if (wl > 0 && fread(wbuf, 1, (size_t)wl, w) != (size_t)wl) wl = 0;
        wbuf[wl > 0 ? wl : 0] = 0;
        fclose(w);
        {
            int is_fail = 0, is_notasm = 0;
            char *base = xstrndup(names.v[k], strlen(names.v[k]) - 4);
            for (line = wbuf; *line;) {
                char *e = strchr(line, '\n');
                size_t n = e ? (size_t)(e - line) : strlen(line);
                char *ln = xstrndup(line, n);
                if (strstr(ln, "macro definition commented out")) is_fail = 1;
                if (strstr(ln, "not an assembler macro")) is_notasm = 1;
                free(ln);
                if (!e) break;
                line = e + 1;
            }
            if (is_fail) {
                nfail++;
                vec_push(&failed, base);
            } else if (is_notasm) {
                vec_push(&notasm, base);
            } else if (!strstr(res, "#macro ")) {
                vec_push(&nomacro, base);
            } else {
                free(base);
            }
        }
        for (line = wbuf; *line;) {
            char *e = strchr(line, '\n');
            size_t n = e ? (size_t)(e - line) : strlen(line);
            if (!quiet) fprintf(stderr, "%.*s\n", (int)n, line);
            if (!e) break;
            line = e + 1;
        }
        nwarn += (size_t)nw;
        if (strstr(res, "#macro ")) {
            const char *p = res;
            while ((p = strstr(p, "#macro ")) != NULL) {
                nmac++;
                p += 7;
            }
        }
        if (res[0]) {
            size_t n = strlen(names.v[k]) - 4;
            char *op = xasprintf("%s/%.*s.zmac", outdir ? outdir : dir, (int)n, names.v[k]);
            {
                char *tmp = xasprintf("%s.tmp.%d", op, (int)getpid());
                FILE *f = fopen(tmp, "wb");
                size_t rlen = strlen(res);
                int ok = f != NULL;
                if (ok && rlen && fwrite(res, 1, rlen, f) != rlen) ok = 0;
                if (f && fclose(f) != 0) ok = 0;
                if (ok && rename(tmp, op) != 0) ok = 0;
                if (!ok) {
                    fprintf(stderr, "hlasm2za: %s: %s\n", op, strerror(errno));
                    remove(tmp);
                    bad = 1;
                }
                free(tmp);
            }
            free(op);
        }
        free(res);
        free(wbuf);
        free(path);
    }
    fprintf(stderr, "hlasm2za: %lu members, %lu macros converted, %lu members failed, %lu warnings\n",
            (unsigned long)nmem, (unsigned long)nmac, (unsigned long)nfail, (unsigned long)nwarn);
    print_names("not assembler macros (PL/X)", notasm.n, notasm.v);
    print_names("no macro definition", nomacro.n, nomacro.v);
    print_names("FAILED", failed.n, failed.v);
    for (k = 0; k < names.n; k++) free(names.v[k]);
    for (k = 0; k < notasm.n; k++) free(notasm.v[k]);
    for (k = 0; k < nomacro.n; k++) free(nomacro.v[k]);
    for (k = 0; k < failed.n; k++) free(failed.v[k]);
    free(names.v); free(notasm.v); free(nomacro.v); free(failed.v);
    return bad || nfail > 0;
}

int main(int argc, char **argv) {
    ConvOptions o;
    const char *in = NULL, *out = NULL, *mdir = NULL;
    Vec(char *) macro_paths = {0};
    int i, quiet = 0, nwarn = 0;
    char *text, *res;
    size_t len;
    FILE *f;
    conv_options_default(&o);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-') a++;
        if (!strcmp(a, "-h") || !strcmp(a, "-help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-version")) {
            printf("hlasm2za %s\n", ZASM_VERSION_STR);
            return 0;
        } else if (!strcmp(a, "-o") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(a, "-ictl") && i + 1 < argc) {
            int b, e, c = 16, n = sscanf(argv[++i], "%d,%d,%d", &b, &e, &c);
            int ok = n >= 2 && b >= 1 && b <= 40 && e >= 41 && e <= 80 && e >= b + 5;
            if (ok) ok = n == 3 ? (c >= 2 && c <= 40 && c > b && c < e) : (b < 16 && e > 16);
            if (!ok) {
                fprintf(stderr, "hlasm2za: bad -ictl value\n");
                return 2;
            }
            o.begin = b;
            o.end = e;
            o.cont = e == 80 ? 0 : c;
        } else if (!strcmp(a, "-relaxed")) {
            o.strict = 0;
        } else if (!strcmp(a, "-w") && i + 1 < argc) {
            if (!za_parse_int(argv[++i], &o.width)) {
                fprintf(stderr, "hlasm2za: bad value for -w\n");
                return 2;
            }
        } else if (!strcmp(a, "-inc-ext") && i + 1 < argc) {
            o.inc_ext = argv[++i];
        } else if (!strcmp(a, "-m")) {
            o.member = 1;
            OUT_EXT = ".zmac";
        } else if (!strcmp(a, "-macro-dir") && i + 1 < argc) {
            mdir = argv[++i];
            o.member = 1;
            OUT_EXT = ".zmac";
        } else if (!strcmp(a, "-copy-dir") && i + 1 < argc) {
            o.copy_dir = argv[++i];
        } else if (!strcmp(a, "-macro-path") && i + 1 < argc) {
            vec_push(&macro_paths, argv[++i]);
        } else if (!strcmp(a, "-keep-goto")) {
            o.keep_goto = 1;
        } else if (!strcmp(a, "-flat")) {
            o.flat = 1;
        } else if (!strcmp(a, "-comment-macros")) {
            o.comment_macros = 1;
        } else if (!strcmp(a, "-q")) {
            quiet = 1;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "hlasm2za: unknown option %s\n", argv[i]);
            usage(stderr);
            return 2;
        } else if (!in) {
            in = argv[i];
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (mdir) {
        if (in && !quiet)
            fprintf(stderr, "hlasm2za: -macro-dir converts the whole directory; ignoring the positional input %s\n", in);
        if (!o.copy_dir) o.copy_dir = mdir;
        if (macro_paths.n && !quiet)
            fprintf(stderr, "hlasm2za: -macro-path has no effect with -macro-dir (the .zmac files are written without a main .za file)\n");
        return macro_dir(mdir, out, &o, quiet);
    }
    if (!in) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(in, "-")) {
        text = read_stdin(&len);
        in = "(stdin)";
        if (!out) out = "-";
    } else {
        text = read_file(in, &len);
        if (!text) {
            fprintf(stderr, "hlasm2za: %s: %s\n", in, strerror(errno));
            return 1;
        }
    }
    res = hlasm_convert(text, len, in, &o, quiet ? NULL : stderr, &nwarn);
    if (!out) out = default_output(in);
    {
        char *with = with_macro_paths(res, macro_paths.v, macro_paths.n);
        free(res);
        res = with;
    }
    if (!strcmp(out, "-")) {
        f = stdout;
    } else {
        {
            char *tmp = xasprintf("%s.tmp.%d", out, (int)getpid());
            f = fopen(tmp, "wb");
            if (!f) {
                fprintf(stderr, "hlasm2za: %s: %s\n", out, strerror(errno));
                free(tmp);
                return 1;
            }
            {
                size_t rlen = strlen(res);
                int ok = rlen == 0 || fwrite(res, 1, rlen, f) == rlen;
                if (fclose(f) != 0) ok = 0;
                if (!ok) {
                    fprintf(stderr, "hlasm2za: %s: %s\n", out, strerror(errno));
                    remove(tmp);
                    free(tmp);
                    return 1;
                }
            }
            if (rename(tmp, out) != 0) {
                fprintf(stderr, "hlasm2za: %s: %s\n", out, strerror(errno));
                remove(tmp);
                free(tmp);
                return 1;
            }
            free(tmp);
            f = NULL;
        }
    }
    if (f && (fwrite(res, 1, strlen(res), f) != strlen(res))) {
        fprintf(stderr, "hlasm2za: %s: %s\n", out, strerror(errno));
        return 1;
    }
    free(macro_paths.v);
    return 0;
}
