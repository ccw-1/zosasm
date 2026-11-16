/* hlasm2za - convert HLASM source to zosasm source. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
            "  -flat           macros: #goto/#label for every sequence symbol, no structure recovery\n"
            "  -comment-macros leave macro definitions as comments (no conversion)\n"
            "  -q              no warnings\n"
            "  -version        print the version\n"
            "Input - is standard input. Warnings (what could not be converted) go to standard error.\n");
}

static const char *OUT_EXT = ".za";

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

/* every member DIR/NAME.mac -> OUTDIR/NAME.zmac; a summary of what could not be converted */
static int macro_dir(const char *dir, const char *outdir, const ConvOptions *o, int quiet) {
    DIR *d = opendir(dir);
    struct dirent *de;
    Vec(char *) names = {0};
    size_t k, nmem = 0, nmac = 0, nfail = 0, nwarn = 0;
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
        if (!text || !w) continue;
        res = hlasm_convert(text, len, path, o, w, &nw);
        nmem++;
        wl = ftell(w);
        rewind(w);
        wbuf = xcalloc((size_t)wl + 1);
        if (wl > 0 && fread(wbuf, 1, (size_t)wl, w) != (size_t)wl) wl = 0;
        wbuf[wl > 0 ? wl : 0] = 0;
        fclose(w);
        for (line = wbuf; *line;) {
            char *e = strchr(line, '\n');
            size_t n = e ? (size_t)(e - line) : strlen(line);
            if (strstr(line, "macro definition commented out") && (!e || strstr(line, "commented out") < e)) nfail++;
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
            FILE *f = fopen(op, "wb");
            if (!f || fwrite(res, 1, strlen(res), f) != strlen(res) || fclose(f) != 0)
                fprintf(stderr, "hlasm2za: %s: write error\n", op);
        }
    }
    fprintf(stderr, "hlasm2za: %lu members, %lu macros converted, %lu not converted, %lu warnings\n",
            (unsigned long)nmem, (unsigned long)nmac, (unsigned long)nfail, (unsigned long)nwarn);
    return 0;
}

int main(int argc, char **argv) {
    ConvOptions o;
    const char *in = NULL, *out = NULL, *mdir = NULL;
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
            if (n < 2 || b < 1 || e <= b || e > 80) {
                fprintf(stderr, "hlasm2za: bad -ictl value\n");
                return 2;
            }
            o.begin = b;
            o.end = e;
            o.cont = e == 80 ? 0 : c;
        } else if (!strcmp(a, "-relaxed")) {
            o.strict = 0;
        } else if (!strcmp(a, "-w") && i + 1 < argc) {
            o.width = atoi(argv[++i]);
        } else if (!strcmp(a, "-inc-ext") && i + 1 < argc) {
            o.inc_ext = argv[++i];
        } else if (!strcmp(a, "-m")) {
            o.member = 1;
            OUT_EXT = ".zmac";
        } else if (!strcmp(a, "-macro-dir") && i + 1 < argc) {
            mdir = argv[++i];
            o.member = 1;
            OUT_EXT = ".zmac";
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
    if (mdir) return macro_dir(mdir, out, &o, quiet);
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
    if (!strcmp(out, "-")) {
        f = stdout;
    } else {
        f = fopen(out, "wb");
        if (!f) {
            fprintf(stderr, "hlasm2za: %s: %s\n", out, strerror(errno));
            return 1;
        }
    }
    if (fwrite(res, 1, strlen(res), f) != strlen(res) || (f != stdout && fclose(f) != 0)) {
        fprintf(stderr, "hlasm2za: %s: write error\n", out);
        return 1;
    }
    return 0;
}
