/* zafmt - format zosasm (.za) source. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "fmt.h"
#include "util.h"
#include "version.h"

static void usage(FILE *f) {
    fprintf(f,
            "usage: zafmt [options] [file ...]\n"
            "  -i            rewrite the files in place (default: write the result to standard output)\n"
            "  -o FILE       write the result of the one input file to FILE\n"
            "  -check        change nothing; list the files that are not formatted; exit 1 if any\n"
            "  -op N         column of the operation field (default 10)\n"
            "  -remark N     column of the remarks (default 35)\n"
            "  -indent N     indentation per block level of the macro language (default 4)\n"
            "  -blank N      at most N blank lines in a row (default 2)\n"
            "  -version      print the version\n"
            "Without a file, or with -, the source comes from standard input.\n"
            "Only blanks and line ends change; text inside quotes is kept.\n");
}

static char *read_stdin(size_t *len) {
    Buf b = {0};
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, stdin)) > 0) buf_write(&b, tmp, n);
    buf_putc(&b, 0);
    *len = b.len - 1;
    {
        char *r = xmalloc(b.len);
        memcpy(r, b.p, b.len);
        free(b.p);
        return r;
    }
}

static int write_out(const char *path, const char *s, size_t n) {
    if (!strcmp(path, "-")) {
        if (fwrite(s, 1, n, stdout) != n) {
            fprintf(stderr, "zafmt: stdout: write error\n");
            return 0;
        }
        return 1;
    }
    {
        char *tmp = xasprintf("%s.tmp.%d", path, (int)getpid());
        FILE *f = fopen(tmp, "wb");
        if (!f) {
            fprintf(stderr, "zafmt: %s: cannot write\n", path);
            free(tmp);
            return 0;
        }
        {
            int ok = n == 0 || fwrite(s, 1, n, f) == n;
            if (fclose(f) != 0) ok = 0;
            if (!ok) {
                fprintf(stderr, "zafmt: %s: %s\n", path, strerror(errno));
                remove(tmp);
                free(tmp);
                return 0;
            }
        }
        if (rename(tmp, path) != 0) {
            fprintf(stderr, "zafmt: %s: %s\n", path, strerror(errno));
            remove(tmp);
            free(tmp);
            return 0;
        }
        free(tmp);
    }
    return 1;
}

int main(int argc, char **argv) {
    FmtOptions o;
    const char *out = NULL;
    Vec(const char *) in = {0};
    int i, inplace = 0, check = 0, rc = 0;
    size_t k;
    fmt_options_default(&o);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-') a++;
        if (!strcmp(a, "-h") || !strcmp(a, "-help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-version")) {
            printf("zafmt %s\n", ZASM_VERSION_STR);
            return 0;
        } else if (!strcmp(a, "-i")) {
            inplace = 1;
        } else if (!strcmp(a, "-check")) {
            check = 1;
        } else if (!strcmp(a, "-o") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(a, "-op") && i + 1 < argc) {
            o.op_col = atoi(argv[++i]);
        } else if (!strcmp(a, "-remark") && i + 1 < argc) {
            o.remark_col = atoi(argv[++i]);
        } else if (!strcmp(a, "-indent") && i + 1 < argc) {
            o.indent = atoi(argv[++i]);
        } else if (!strcmp(a, "-blank") && i + 1 < argc) {
            o.max_blank = atoi(argv[++i]);
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "zafmt: unknown option %s\n", argv[i]);
            usage(stderr);
            return 2;
        } else {
            vec_push(&in, argv[i]);
        }
    }
    if (o.op_col < 2 || o.op_col > 40 || o.remark_col < o.op_col + 7 || o.remark_col > 200 || o.indent < 0 ||
        o.indent > 16 || o.max_blank < 0) {
        fprintf(stderr, "zafmt: bad column option (-op 2..40, -remark at least -op + 7)\n");
        return 2;
    }
    if (in.n == 0) vec_push(&in, "-");
    if ((out && in.n > 1) || (inplace && (out || (in.n == 1 && !strcmp(in.v[0], "-")))) || (inplace && check)) {
        fprintf(stderr, "zafmt: -o takes one input file; -i needs files and cannot be combined with -o or -check\n");
        return 2;
    }
    for (k = 0; k < in.n; k++) {
        size_t len;
        char *text, *res;
        int same;
        if (!strcmp(in.v[k], "-")) {
            text = read_stdin(&len);
        } else {
            text = read_file(in.v[k], &len);
            if (!text) {
                fprintf(stderr, "zafmt: %s: cannot read\n", in.v[k]);
                rc = 2;
                continue;
            }
        }
        res = za_format(text, len, &o);
        same = strlen(res) == len && !memcmp(res, text, len);
        if (check) {
            if (!same) {
                printf("%s\n", in.v[k]);
                rc = rc ? rc : 1;
            }
        } else if (inplace) {
            if (!same && !write_out(in.v[k], res, strlen(res))) rc = 2;
        } else if (!write_out(out ? out : "-", res, strlen(res))) {
            rc = 2;
        }
    }
    return rc;
}
