/* hlasm2za - convert HLASM source to zosasm source. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
            "  -q              no warnings\n"
            "  -version        print the version\n"
            "Input - is standard input. Warnings (what could not be converted) go to standard error.\n");
}

static char *default_output(const char *in) {
    const char *slash = strrchr(in, '/'), *dot = strrchr(in, '.');
    size_t n = strlen(in);
    if (dot && (!slash || dot > slash) && dot != in && (!slash || dot > slash + 1)) n = (size_t)(dot - in);
    return xasprintf("%.*s.za", (int)n, in);
}

static char *read_stdin(size_t *len) {
    Buf b = {0};
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, stdin)) > 0) buf_write(&b, tmp, n);
    *len = b.len;
    return xstrdup(buf_cstr(&b));
}

int main(int argc, char **argv) {
    ConvOptions o;
    const char *in = NULL, *out = NULL;
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
