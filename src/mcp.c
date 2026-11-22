/* mcp.c - zosasm-mcp: a Model Context Protocol server (stdio) that lets AI
 * agents run zosasm and read its user's guide.  See DESIGN.md section 18.
 *
 * Transport: one JSON-RPC 2.0 message per line on standard input and output
 * (MCP "stdio"); diagnostics of the server itself go to standard error.
 *
 * The tools run the command line programs `zosasm` and `hlasm2za` that sit in
 * the same directory (or ZOSASM_BIN_DIR) as separate processes, so the server
 * has no assembler state of its own.  This file uses POSIX (popen, mkdtemp),
 * the rest of zosasm is plain C99. */
#define _XOPEN_SOURCE 700
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"
#include "version.h"

#define PROTOCOL_LATEST "2025-06-18"
#define MAX_TEXT (1024 * 1024) /* longest text returned in one result */
#define MAX_READ (8 * 1024 * 1024) /* max file read into MCP memory */
#define MAX_REQUEST (16 * 1024 * 1024) /* longest JSON-RPC line read */

/* ====================================================================
 * JSON
 * ==================================================================== */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct J J;
struct J {
    JType t;
    int b;
    double num;
    char *s; /* J_STR */
    size_t n; /* items of J_ARR / J_OBJ */
    J **v;
    char **k; /* J_OBJ */
};

typedef struct {
    const char *p, *end;
    const char *err;
    int depth;
} JP;

static void jp_ws(JP *p) {
    while (p->p < p->end && (*p->p == ' ' || *p->p == '\t' || *p->p == '\n' || *p->p == '\r')) p->p++;
}

static int hex4(JP *p, unsigned *out) {
    unsigned v = 0;
    int i;
    if (p->end - p->p < 4) return 0;
    for (i = 0; i < 4; i++) {
        int c = (unsigned char)p->p[i], d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return 0;
        v = v * 16 + (unsigned)d;
    }
    p->p += 4;
    *out = v;
    return 1;
}

static void put_utf8(Buf *b, unsigned cp) {
    if (cp < 0x80) {
        buf_putc(b, (int)cp);
    } else if (cp < 0x800) {
        buf_putc(b, (int)(0xC0 | (cp >> 6)));
        buf_putc(b, (int)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_putc(b, (int)(0xE0 | (cp >> 12)));
        buf_putc(b, (int)(0x80 | ((cp >> 6) & 0x3F)));
        buf_putc(b, (int)(0x80 | (cp & 0x3F)));
    } else {
        buf_putc(b, (int)(0xF0 | (cp >> 18)));
        buf_putc(b, (int)(0x80 | ((cp >> 12) & 0x3F)));
        buf_putc(b, (int)(0x80 | ((cp >> 6) & 0x3F)));
        buf_putc(b, (int)(0x80 | (cp & 0x3F)));
    }
}

/* p->p is after the opening quote */
static char *jp_string(JP *p, size_t *len) {
    Buf b = {0};
    while (p->p < p->end) {
        unsigned char c = (unsigned char)*p->p++;
        if (c == '"') {
            char *r;
            if (len) *len = b.len;
            buf_putc(&b, 0);
            r = xmalloc(b.len);
            memcpy(r, b.p, b.len);
            free(b.p);
            return r;
        }
        if (c < 0x20) {
            p->err = "control character in string";
            free(b.p);
            return NULL;
        }
        if (c != '\\') {
            buf_putc(&b, c);
            continue;
        }
        if (p->p >= p->end) break;
        c = (unsigned char)*p->p++;
        switch (c) {
        case '"': buf_putc(&b, '"'); break;
        case '\\': buf_putc(&b, '\\'); break;
        case '/': buf_putc(&b, '/'); break;
        case 'b': buf_putc(&b, '\b'); break;
        case 'f': buf_putc(&b, '\f'); break;
        case 'n': buf_putc(&b, '\n'); break;
        case 'r': buf_putc(&b, '\r'); break;
        case 't': buf_putc(&b, '\t'); break;
        case 'u': {
            unsigned u, lo;
            if (!hex4(p, &u)) {
                p->err = "bad \\u escape";
                free(b.p);
                return NULL;
            }
            if (u >= 0xD800 && u < 0xDC00 && p->end - p->p >= 6 && p->p[0] == '\\' && p->p[1] == 'u') {
                const char *save = p->p;
                p->p += 2;
                if (hex4(p, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                } else {
                    p->p = save;
                }
            }
            if (u >= 0xD800 && u < 0xE000) u = 0xFFFD;
            if (u == 0) {
                p->err = "string contains NUL";
                free(b.p);
                return NULL;
            }
            put_utf8(&b, u);
            break;
        }
        default:
            p->err = "bad escape in string";
            free(b.p);
            return NULL;
        }
    }
    p->err = "unterminated string";
    free(b.p);
    return NULL;
}

static J *jnew(JType t) {
    J *j = xcalloc(sizeof *j);
    j->t = t;
    return j;
}

static J *jp_value(JP *p);
static void jfree(J *j);

static J *jp_value(JP *p) {
    J *j;
    jp_ws(p);
    if (p->p >= p->end) {
        p->err = "unexpected end of input";
        return NULL;
    }
    if (p->depth > 64) {
        p->err = "nesting too deep";
        return NULL;
    }
    switch (*p->p) {
    case '{':
    case '[': {
        char close = *p->p == '{' ? '}' : ']';
        int obj = close == '}';
        size_t cap = 0;
        p->p++;
        p->depth++;
        j = jnew(obj ? J_OBJ : J_ARR);
        jp_ws(p);
        if (p->p < p->end && *p->p == close) {
            p->p++;
            p->depth--;
            return j;
        }
        for (;;) {
            char *key = NULL;
            J *v;
            jp_ws(p);
            if (obj) {
                if (p->p >= p->end || *p->p != '"') {
                    p->err = "object key expected";
                    goto fail;
                }
                p->p++;
                key = jp_string(p, NULL);
                if (!key) goto fail;
                jp_ws(p);
                if (p->p >= p->end || *p->p != ':') {
                    p->err = "':' expected";
                    goto fail;
                }
                p->p++;
            }
            v = jp_value(p);
            if (!v) goto fail;
            if (j->n == cap) {
                cap = cap ? cap * 2 : 8;
                j->v = xrealloc(j->v, cap * sizeof *j->v);
                if (obj) j->k = xrealloc(j->k, cap * sizeof *j->k);
            }
            j->v[j->n] = v;
            if (obj) j->k[j->n] = key;
            key = NULL; /* now owned by j */
            j->n++;
            jp_ws(p);
            if (p->p < p->end && *p->p == ',') {
                p->p++;
                continue;
            }
            if (p->p < p->end && *p->p == close) {
                p->p++;
                p->depth--;
                return j;
            }
            p->err = "',' or closing bracket expected";
        fail:
            free(key);
            jfree(j);
            return NULL;
        }
    }
    case '"':
        p->p++;
        j = jnew(J_STR);
        j->s = jp_string(p, &j->n);
        if (!j->s) { free(j); return NULL; }
        return j;
    case 't':
        if (p->end - p->p >= 4 && !strncmp(p->p, "true", 4)) {
            p->p += 4;
            j = jnew(J_BOOL);
            j->b = 1;
            return j;
        }
        break;
    case 'f':
        if (p->end - p->p >= 5 && !strncmp(p->p, "false", 5)) {
            p->p += 5;
            return jnew(J_BOOL);
        }
        break;
    case 'n':
        if (p->end - p->p >= 4 && !strncmp(p->p, "null", 4)) {
            p->p += 4;
            return jnew(J_NULL);
        }
        break;
    default:
        if (*p->p == '-' || za_isdigit(*p->p)) {
            const char *q = p->p;
            char *tmp, *e;
            if (*q == '-') q++;
            while (q < p->end && (za_isdigit(*q) || *q == '.' || *q == 'e' || *q == 'E' || *q == '+' || *q == '-')) q++;
            tmp = xstrndup(p->p, (size_t)(q - p->p));
            j = jnew(J_NUM);
            j->num = strtod(tmp, &e);
            if (*e) { /* e points into tmp: read it before the free */
                p->err = "bad number";
                free(tmp);
                jfree(j);
                return NULL;
            }
            free(tmp);
            p->p = q;
            return j;
        }
    }
    p->err = "unexpected character";
    return NULL;
}

/* frees a parse tree (a whole request, or a partial one of a failed parse) */
static void jfree(J *j) {
    size_t i;
    if (!j) return;
    if (j->t == J_ARR || j->t == J_OBJ) { /* only these have children in v */
        for (i = 0; i < j->n; i++) {
            jfree(j->v[i]);
            if (j->k) free(j->k[i]);
        }
    }
    free(j->v);
    free(j->k);
    free(j->s);
    free(j);
}

static J *json_parse(const char *s, size_t n, const char **err) {
    JP p;
    J *j;
    p.p = s;
    p.end = s + n;
    p.err = NULL;
    p.depth = 0;
    j = jp_value(&p);
    if (j) {
        jp_ws(&p);
        if (p.p != p.end) {
            p.err = "trailing characters";
            jfree(j);
            j = NULL;
        }
    }
    if (!j) *err = p.err ? p.err : "invalid JSON";
    return j;
}

static J *jget(const J *o, const char *key) {
    size_t i;
    if (!o || o->t != J_OBJ) return NULL;
    for (i = 0; i < o->n; i++)
        if (!strcmp(o->k[i], key)) return o->v[i];
    return NULL;
}

static const char *jstr(const J *o, const char *key) {
    J *v = jget(o, key);
    return v && v->t == J_STR ? v->s : NULL;
}

static int jbool(const J *o, const char *key, int def) {
    J *v = jget(o, key);
    return v && v->t == J_BOOL ? v->b : def;
}

static long jint(const J *o, const char *key, long def) {
    J *v = jget(o, key);
    return v && v->t == J_NUM ? (long)v->num : def;
}

/* ---- JSON output ---- */

/* the length of the valid UTF-8 sequence at s[i], or 0 */
static size_t json_utf8_len(const unsigned char *s, size_t i, size_t n) {
    unsigned char c = s[i];
    size_t len, k;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF)
        len = 2;
    else if (c >= 0xE0 && c <= 0xEF)
        len = 3;
    else if (c >= 0xF0 && c <= 0xF4)
        len = 4;
    else
        return 0;
    if (i + len > n) return 0;
    for (k = 1; k < len; k++)
        if ((s[i + k] & 0xC0) != 0x80) return 0;
    if (c == 0xE0 && s[i + 1] < 0xA0) return 0;
    if (c == 0xED && s[i + 1] >= 0xA0) return 0;
    if (c == 0xF0 && s[i + 1] < 0x90) return 0;
    if (c == 0xF4 && s[i + 1] >= 0x90) return 0;
    return len;
}

/* a JSON string; bytes that are not valid UTF-8 are taken as ISO-8859-1 */
static void w_str(Buf *b, const char *str, size_t n) {
    const unsigned char *s = (const unsigned char *)str;
    size_t i;
    buf_putc(b, '"');
    for (i = 0; i < n;) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\') {
            buf_putc(b, '\\');
            buf_putc(b, c);
            i++;
        } else if (c == '\n') {
            buf_puts(b, "\\n");
            i++;
        } else if (c == '\r') {
            buf_puts(b, "\\r");
            i++;
        } else if (c == '\t') {
            buf_puts(b, "\\t");
            i++;
        } else if (c < 0x20 || c == 0x7F) {
            buf_printf(b, "\\u%04x", c);
            i++;
        } else if (c < 0x80) {
            buf_putc(b, c);
            i++;
        } else {
            size_t l = json_utf8_len(s, i, n);
            if (l) {
                buf_write(b, s + i, l);
                i += l;
            } else {
                put_utf8(b, c);
                i++;
            }
        }
    }
    buf_putc(b, '"');
}

static void w_cstr(Buf *b, const char *s) { w_str(b, s, s ? strlen(s) : 0); }

static void w_key(Buf *b, int *first, const char *key) {
    if (!*first) buf_putc(b, ',');
    *first = 0;
    w_cstr(b, key);
    buf_putc(b, ':');
}

static void w_json(Buf *b, const J *j) {
    size_t i;
    switch (j->t) {
    case J_NULL: buf_puts(b, "null"); break;
    case J_BOOL: buf_puts(b, j->b ? "true" : "false"); break;
    case J_NUM:
        if (j->num == (double)(long long)j->num)
            buf_printf(b, "%lld", (long long)j->num);
        else
            buf_printf(b, "%.17g", j->num);
        break;
    case J_STR: w_str(b, j->s, j->n); break;
    case J_ARR:
        buf_putc(b, '[');
        for (i = 0; i < j->n; i++) {
            if (i) buf_putc(b, ',');
            w_json(b, j->v[i]);
        }
        buf_putc(b, ']');
        break;
    case J_OBJ:
        buf_putc(b, '{');
        for (i = 0; i < j->n; i++) {
            if (i) buf_putc(b, ',');
            w_cstr(b, j->k[i]);
            buf_putc(b, ':');
            w_json(b, j->v[i]);
        }
        buf_putc(b, '}');
        break;
    }
}

/* ====================================================================
 * Files, processes, base64
 * ==================================================================== */

static char *read_whole(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    Buf b = {0};
    char tmp[8192];
    size_t n;
    if (!f) return NULL;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_write(&b, tmp, n);
    fclose(f);
    if (len) *len = b.len;
    {
        char *r = xstrdup(buf_cstr(&b)); /* NUL terminated, may hold NULs inside (len tells) */
        free(b.p);
        return r;
    }
}

/* like read_whole but exact binary: copies b.len bytes */
static unsigned char *read_binary(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    Buf b = {0};
    char tmp[8192];
    size_t n;
    unsigned char *r;
    if (!f) return NULL;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_write(&b, tmp, n);
    fclose(f);
    r = xmalloc(b.len + 1);
    if (b.len) memcpy(r, b.p, b.len);
    *len = b.len;
    free(b.p);
    return r;
}

static int write_whole(const char *path, const void *data, size_t n) {
    char *tmp = xasprintf("%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(tmp); return 0; }
    if (n && fwrite(data, 1, n, f) != n) {
        fclose(f);
        remove(tmp);
        free(tmp);
        return 0;
    }
    if (fclose(f) != 0) { remove(tmp); free(tmp); return 0; }
    if (rename(tmp, path) != 0) { remove(tmp); free(tmp); return 0; }
    free(tmp);
    return 1;
}

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(Buf *out, const unsigned char *s, size_t n) {
    size_t i;
    for (i = 0; i < n; i += 3) {
        unsigned v = (unsigned)s[i] << 16;
        if (i + 1 < n) v |= (unsigned)s[i + 1] << 8;
        if (i + 2 < n) v |= s[i + 2];
        buf_putc(out, b64chars[(v >> 18) & 63]);
        buf_putc(out, b64chars[(v >> 12) & 63]);
        buf_putc(out, i + 1 < n ? b64chars[(v >> 6) & 63] : '=');
        buf_putc(out, i + 2 < n ? b64chars[v & 63] : '=');
    }
}

/* returns a malloc'ed buffer or NULL for bad input */
static unsigned char *b64_decode(const char *s, size_t n, size_t *len) {
    unsigned char *out = xmalloc(n / 4 * 3 + 4);
    size_t i, o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (i = 0; i < n; i++) {
        const char *q;
        unsigned c = (unsigned char)s[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        q = strchr(b64chars, (int)c);
        if (!q || !c) {
            free(out);
            return NULL;
        }
        acc = (acc << 6) | (unsigned)(q - b64chars);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    *len = o;
    return out;
}

/* a temporary directory for one tool call */
typedef struct {
    char *dir;
    Vec(char *) files;
} Tmp;

static int tmp_open(Tmp *t) {
    const char *base = getenv("TMPDIR");
    char *tpl;
    memset(t, 0, sizeof *t);
    if (!base || !*base) base = "/tmp";
    tpl = xasprintf("%s/zosasm-mcp-XXXXXX", base);
    t->dir = mkdtemp(tpl);
    if (t->dir) {
        t->dir = tpl;
        return 1;
    }
    free(tpl);
    t->dir = NULL;
    return 0;
}

static char *tmp_path(Tmp *t, const char *name) {
    char *p = xasprintf("%s/%s", t->dir, name);
    vec_push(&t->files, p);
    return p;
}

static void tmp_close(Tmp *t) {
    size_t i;
    if (!t->dir) return;
    for (i = 0; i < t->files.n; i++) {
        remove(t->files.v[i]);
        free(t->files.v[i]);
    }
    free(t->files.v);
    rmdir(t->dir);
    free(t->dir);
    t->dir = NULL;
    t->files.v = NULL;
    t->files.n = 0;
}

/* a file name without a directory part (for names the caller chooses) */
static int plain_name(const char *s) {
    size_t i;
    if (!s || !*s || !strcmp(s, ".") || !strcmp(s, "..")) return 0;
    for (i = 0; s[i]; i++)
        if (s[i] == '/' || s[i] == '\\' || (unsigned char)s[i] < 0x20) return 0;
    return 1;
}

static void shq(Buf *b, const char *s) {
    buf_putc(b, '\'');
    for (; *s; s++) {
        if (*s == '\'')
            buf_puts(b, "'\\''");
        else
            buf_putc(b, *s);
    }
    buf_putc(b, '\'');
}

typedef struct {
    Buf out, err;
    int status; /* exit status; 127 when it could not be run */
} Run;

/* Run prog with args; standard output and error are captured. */
static void run_prog(Tmp *t, const char *prog, char *const *args, size_t nargs, Run *r) {
    Buf cmd = {0};
    char *errfile = tmp_path(t, ".stderr");
    FILE *p;
    char tmp[8192];
    size_t n, i;
    int st;
    memset(r, 0, sizeof *r);
    buf_puts(&cmd, "exec </dev/null ");
    shq(&cmd, prog);
    for (i = 0; i < nargs; i++) {
        buf_putc(&cmd, ' ');
        shq(&cmd, args[i]);
    }
    buf_puts(&cmd, " 2>");
    shq(&cmd, errfile);
    p = popen(buf_cstr(&cmd), "r");
    if (!p) {
        buf_printf(&r->err, "cannot start %s: %s\n", prog, strerror(errno));
        r->status = 127;
        free(cmd.p);
        return;
    }
    while ((n = fread(tmp, 1, sizeof tmp, p)) > 0) {
        if (r->out.len < MAX_TEXT) {
            size_t take = n;
            if (r->out.len + take > MAX_TEXT) take = MAX_TEXT - r->out.len;
            buf_write(&r->out, tmp, take);
        }
    }
    st = pclose(p);
    r->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    {
        size_t el = 0;
        char *e = read_whole(errfile, &el);
        if (e) {
            size_t take = el;
            if (take > MAX_TEXT) take = MAX_TEXT;
            buf_write(&r->err, e, take);
            free(e);
        }
    }
    free(cmd.p);
}

/* replace every occurrence of the temporary directory in b */
static void scrub(Buf *b, const Tmp *t) {
    Buf o = {0};
    char *pat = xasprintf("%s/", t->dir);
    size_t pl = strlen(pat), i = 0;
    const char *s = buf_cstr(b);
    (void)pl; /* used in the loop below */
    while (i < b->len) {
        if (i + pl <= b->len && !memcmp(s + i, pat, pl)) {
            i += pl;
        } else {
            buf_putc(&o, s[i++]);
        }
    }
    free(b->p);
    *b = o;
    free(pat);
}

/* ====================================================================
 * Where things are
 * ==================================================================== */

static char *g_bindir;  /* directory of zosasm and hlasm2za */
static char *g_guide;   /* path of GUIDE.md; process-lifetime, not a per-request leak */
static char *g_docdir;  /* its directory (examples/ is below it) */
static char *g_server_dir; /* the directory of the server binary (the workspace) */
static char *g_tools_json; /* process-lifetime tools/list cache */
static char *g_workspace;

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return xstrdup(".");
    if (slash == path) return xstrdup("/");
    return xstrndup(path, (size_t)(slash - path));
}

typedef struct Guide Guide;
static Guide *load_guide_cache(void);
static void guide_free(Guide *g);
extern int g_guide_tried; /* defined with guide cache */
static void locate_reset(void) {
    Guide *g = load_guide_cache();
    guide_free(g);
    g_guide_tried = 0;
    free(g_bindir); g_bindir = NULL;
    free(g_guide); g_guide = NULL;
    free(g_docdir); g_docdir = NULL;
    free(g_server_dir); g_server_dir = NULL;
    free(g_tools_json); g_tools_json = NULL;
    free(g_workspace); g_workspace = NULL;
}

static void locate(const char *argv0) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    char *exe = NULL;
    const char *env;
    locate_reset();
    if (n > 0) {
        buf[n] = 0;
        exe = xstrdup(buf);
    } else if (strchr(argv0, '/')) {
        exe = xstrdup(argv0);
    }
    /* the workspace is the server's own directory, before ZOSASM_BIN_DIR
     * moves the programs elsewhere */
    g_server_dir = exe ? dir_of(exe) : xstrdup(".");
    env = getenv("ZOSASM_BIN_DIR");
    if (env && *env)
        g_bindir = xstrdup(env);
    else if (exe)
        g_bindir = dir_of(exe);
    else
        g_bindir = xstrdup(".");
    free(exe);
    env = getenv("ZOSASM_GUIDE");
    if (env && *env) {
        g_guide = xstrdup(env);
    } else {
        char *cand[3];
        size_t i;
        cand[0] = xasprintf("%s/../docs/GUIDE.md", g_bindir);
        cand[1] = xasprintf("%s/docs/GUIDE.md", g_bindir);
        cand[2] = xasprintf("%s/../share/zosasm/GUIDE.md", g_bindir);
        for (i = 0; i < 3; i++) {
            if (!g_guide && file_exists(cand[i]))
                g_guide = cand[i];
            else
                free(cand[i]);
        }
        if (!g_guide) g_guide = xstrdup("docs/GUIDE.md");
    }
    {
        char *real = realpath(g_guide, NULL);
        if (real) {
            free(g_guide);
            g_guide = real;
        }
    }
    {
        char *rb = realpath(g_bindir, NULL);
        if (rb) {
            free(g_bindir);
            g_bindir = rb;
        }
    }
    g_docdir = dir_of(g_guide);
}

/* ---- the workspace --------------------------------------------------------
 * By default every file name a tool call gives (path, output, output_dir,
 * macro_dir, include_dirs, macro_dirs, copy_dir) must resolve to below the
 * directory of the server binary: relative names are taken relative to it,
 * absolute names must be inside it, and symlinks or ".." cannot escape it.
 * ZOSASM_UNRESTRICTED (set to a non-empty value) lifts the limit: any file
 * name is allowed, relative to the working directory, as before. */

static int g_unrestricted;
static char g_ws_err[512]; /* the error of the last ws_resolve failure; the
                            * server is serial, tool_fail copies it at once */

static int inside_workspace(const char *real) {
    size_t wl = strlen(g_workspace);
    return !strcmp(real, g_workspace) ||
           (!strncmp(real, g_workspace, wl) && real[wl] == '/');
}

/* Resolve p against the workspace. Returns a canonical path, or NULL with
 * *msg set to an error message for the caller. A path that does not exist
 * yet (an output, an output directory) is resolved through its deepest
 * existing directory, and the rest is joined on textually with "." and ".."
 * resolved; the result must still be inside the workspace. */
static char *ws_resolve(const char *p, const char *what, const char **msg) {
    char *full, *real = NULL;
    struct stat st;
    *msg = NULL;
    if (!p || !*p) {
        snprintf(g_ws_err, sizeof g_ws_err, "%s: the file name is empty", what);
        *msg = g_ws_err;
        return NULL;
    }
    if (g_unrestricted) return xstrdup(p);
    full = p[0] == '/' ? xstrdup(p) : xasprintf("%s/%s", g_workspace, p);
    if (stat(full, &st) == 0) {
        real = realpath(full, NULL);
        if (!real) {
            snprintf(g_ws_err, sizeof g_ws_err, "%s \"%s\": %s", what, p, strerror(errno));
            *msg = g_ws_err;
        }
    } else {
        char *dir = xstrdup(full), *rd = NULL, *slash;
        Buf b = {0};
        size_t pl, i;
        pl = strlen(dir);
        while (pl > 1 && dir[pl - 1] == '/') dir[--pl] = 0; /* trailing slashes */
        while ((slash = strrchr(dir + 1, '/')) != NULL) {
            *slash = 0;
            if (stat(dir, &st) == 0) {
                rd = realpath(dir, NULL);
                break;
            }
        }
        if (!rd && stat(dir, &st) == 0) rd = realpath(dir, NULL);
        if (!rd) {
            snprintf(g_ws_err, sizeof g_ws_err, "%s \"%s\": the directory does not exist", what, p);
            *msg = g_ws_err;
            free(b.p);
        } else {
            buf_puts(&b, rd);
            free(rd);
            for (i = slash ? (size_t)(slash - dir) : strlen(dir); full[i]; ) {
                size_t j = i;
                while (full[j] && full[j] != '/') j++;
                if (j > i + 1 || (j == i + 1 && full[i] != '.')) { /* not "." or empty */
                    if (j - i == 2 && full[i] == '.' && full[i + 1] == '.') {
                        char *last = strrchr(buf_cstr(&b), '/'); /* b starts with '/' */
                        if (last && last != b.p) {
                            b.len = (size_t)(last - b.p);
                            b.p[b.len] = 0;
                        }
                    } else {
                        buf_putc(&b, '/');
                        buf_write(&b, full + i, j - i);
                    }
                }
                i = j;
                if (full[i] == '/') i++; /* skip the separator, not the end of the string */
            }
            real = xstrdup(buf_cstr(&b));
            free(b.p);
        }
        free(dir);
    }
    if (real && !inside_workspace(real)) {
        snprintf(g_ws_err, sizeof g_ws_err,
                 "%s \"%s\" is outside the workspace %s (relative names are relative to the "
                 "workspace; the operator can lift the limit with ZOSASM_UNRESTRICTED)",
                 what, p, g_workspace);
        *msg = g_ws_err;
        free(real);
        real = NULL;
    }
    free(full);
    return real;
}

/* ====================================================================
 * The user's guide
 * ==================================================================== */

typedef struct {
    int level;     /* 1 = #, 2 = ##, 3 = ### */
    size_t line;   /* 0 based line number of the heading */
    char *text;    /* the heading without the #s */
} Heading;

struct Guide {
    char *text;
    size_t len;
    Vec(char *) lines; /* pointers into a copy, NUL terminated lines */
    Vec(Heading) heads;
};

static Guide g_guide_cache;
int g_guide_tried;
static Guide *load_guide_cache(void) { return &g_guide_cache; }
static void guide_free(Guide *g) {
    size_t i;
    if (!g) return;
    for (i = 0; i < g->heads.n; i++) free(g->heads.v[i].text);
    free(g->heads.v);
    free(g->lines.v);
    free(g->text);
    memset(g, 0, sizeof *g);
}

/* Process-lifetime cache: the guide is loaded once on demand and retained for
 * the server lifetime. This is intentional, not a per-request leak. */
static Guide *load_guide(void) {
    Guide *g = &g_guide_cache;
    size_t n, i;
    char *t, *start;
    int in_code = 0;
    if (g_guide_tried) return g->text ? g : NULL;
    g_guide_tried = 1;
    t = read_whole(g_guide, &n);
    if (!t) return NULL;
    g->text = t;
    g->len = n;
    start = t;
    for (i = 0; i <= n; i++) {
        if (i == n || t[i] == '\n') {
            t[i] = 0;
            vec_push(&g->lines, start);
            start = t + i + 1;
        }
    }
    for (i = 0; i < g->lines.n; i++) {
        const char *l = g->lines.v[i];
        int lv = 0;
        if (!strncmp(l, "```", 3)) in_code = !in_code;
        if (in_code) continue;
        while (l[lv] == '#') lv++;
        if (lv >= 1 && lv <= 3 && l[lv] == ' ') {
            Heading h;
            h.level = lv;
            h.line = i;
            h.text = xstrdup(l + lv + 1);
            vec_push(&g->heads, h);
        }
    }
    return g;
}

/* the heading a query names: "6", "6.1", "17", or part of a title */
static int find_heading(const Guide *g, const char *q, int *ambiguous) {
    size_t i, ql = strlen(q);
    int found = -1, count = 0;
    *ambiguous = 0;
    while (ql && (q[ql - 1] == '.' || q[ql - 1] == ' ')) ql--;
    if (ql && za_isdigit(q[0])) {
        for (i = 0; i < g->heads.n; i++) {
            const char *h = g->heads.v[i].text;
            if (!strncmp(h, q, ql) && (h[ql] == '.' || h[ql] == ' ') && g->heads.v[i].level >= 2) return (int)i;
        }
        return -1;
    }
    for (i = 0; i < g->heads.n; i++) {
        const char *h = g->heads.v[i].text;
        size_t hl = strlen(h), k;
        for (k = 0; k + ql <= hl; k++)
            if (za_strncmpi(h + k, q, ql) == 0) {
                if (found < 0) found = (int)i;
                count++;
                break;
            }
    }
    if (count > 1) *ambiguous = 1;
    return found;
}

static void guide_section(const Guide *g, int hi, Buf *out) {
    size_t i, end = g->lines.n;
    int lv = g->heads.v[hi].level;
    size_t k;
    for (k = (size_t)hi + 1; k < g->heads.n; k++)
        if (g->heads.v[k].level <= lv) {
            end = g->heads.v[k].line;
            break;
        }
    for (i = g->heads.v[hi].line; i < end; i++) {
        if (out->len + strlen(g->lines.v[i]) + 1 > MAX_TEXT) {
            buf_printf(out, "\n[... section truncated at %zu bytes ...]\n", (size_t)MAX_TEXT);
            break;
        }
        buf_puts(out, g->lines.v[i]);
        buf_putc(out, '\n');
    }
}

static void guide_contents(const Guide *g, Buf *out) {
    size_t i;
    for (i = 0; i < g->heads.n; i++) {
        const Heading *h = &g->heads.v[i];
        if (h->level == 2)
            buf_printf(out, "%s\n", h->text);
        else if (h->level == 3)
            buf_printf(out, "    %s\n", h->text);
    }
}

/* the section of heading hi (0-based line) containing line ln */
static const char *section_of_line(const Guide *g, size_t ln) {
    size_t i;
    const char *best = "";
    for (i = 0; i < g->heads.n; i++) {
        if (g->heads.v[i].line > ln) break;
        best = g->heads.v[i].text;
    }
    return best;
}

/* ====================================================================
 * Tool results
 * ==================================================================== */

static void text_cap(Buf *b, const char *s, size_t n) {
    if (n > MAX_TEXT) {
        buf_write(b, s, MAX_TEXT);
        buf_printf(b, "\n[... %zu more bytes not shown ...]\n", n - MAX_TEXT);
    } else {
        buf_write(b, s, n);
    }
}

/* {"content":[{"type":"text","text":...}],"structuredContent":...,"isError":...}
 * structured may be NULL */
static void tool_result(Buf *out, const char *text, size_t tl, const char *structured, int is_error) {
    int first = 1;
    buf_putc(out, '{');
    w_key(out, &first, "content");
    buf_puts(out, "[{\"type\":\"text\",\"text\":");
    w_str(out, text, tl);
    buf_puts(out, "}]");
    if (structured) {
        w_key(out, &first, "structuredContent");
        buf_puts(out, structured);
    }
    w_key(out, &first, "isError");
    buf_puts(out, is_error ? "true" : "false");
    buf_putc(out, '}');
}

static void tool_fail(Buf *out, const char *msg) { tool_result(out, msg, strlen(msg), NULL, 1); }

/* ====================================================================
 * Tools
 * ==================================================================== */

static char *tool_path(const char *name) { return xasprintf("%s/%s", g_bindir, name); }

/* strings of an array argument; sets *bad when it is not an array of strings */
static size_t str_array(const J *args, const char *key, char ***out, int *bad) {
    J *a = jget(args, key);
    size_t i;
    *out = NULL;
    if (!a || a->t == J_NULL) return 0;
    if (a->t != J_ARR) {
        *bad = 1;
        return 0;
    }
    *out = xmalloc((a->n + 1) * sizeof **out);
    for (i = 0; i < a->n; i++) {
        if (a->v[i]->t != J_STR) {
            free(*out);
            *out = NULL;
            *bad = 1;
            return 0;
        }
        (*out)[i] = a->v[i]->s;
    }
    return a->n;
}

/* Write the 'files' object (name -> text) into the temp dir. Returns an
 * error message or NULL. */
static const char *write_files(Tmp *t, const J *args) {
    J *f = jget(args, "files");
    size_t i;
    if (!f || f->t == J_NULL) return NULL;
    if (f->t != J_OBJ) return "'files' must be an object that maps file names to their text";
    for (i = 0; i < f->n; i++) {
        char *p;
        if (!plain_name(f->k[i])) return "'files' keys must be plain file names without a directory";
        if (f->v[i]->t != J_STR) return "'files' values must be strings";
        if (f->k[i][0] == '.' && !strcmp(f->k[i], ".stderr")) return "reserved file name in 'files'";
        p = tmp_path(t, f->k[i]);
        if (!write_whole(p, f->v[i]->s, f->v[i]->n)) return "cannot write a file of 'files'";
    }
    return NULL;
}

/* The input program: 'source' (written into the temp dir as name) or 'path'.
 * Returns the path to hand to the program, or NULL with *msg set. */
static char *input_file(Tmp *t, const J *args, const char *defname, const char **msg) {
    const char *src = jstr(args, "source"), *path = jstr(args, "path"), *name = jstr(args, "name");
    if ((src != NULL) == (path != NULL)) {
        *msg = "give exactly one of 'source' (the program text) or 'path' (a file name)";
        return NULL;
    }
    if (path) {
        char *rp;
        const char *wm = NULL;
        if (!*path) {
            *msg = "'path' is empty";
            return NULL;
        }
        rp = ws_resolve(path, "'path'", &wm);
        if (!rp) {
            *msg = wm;
            return NULL;
        }
        return rp;
    }
    if (!name) name = defname;
    if (!plain_name(name) || !strcmp(name, ".stderr")) {
        *msg = "'name' must be a plain file name such as prog.za";
        return NULL;
    }
    {
        char *p = tmp_path(t, name);
        if (!write_whole(p, src, jget(args, "source")->n)) {
            *msg = "cannot write the temporary source file";
            return NULL;
        }
        return xstrdup(p); /* the caller frees; the Tmp copy is freed by tmp_close */
    }
}

typedef Vec(char *) ArgVec;
static void add_arg(ArgVec *v, const char *s) { vec_push(v, xstrdup(s)); }
static void free_args(ArgVec *v) {
    size_t i;
    for (i = 0; i < v->n; i++) free(v->v[i]);
    free(v->v);
}

/* common to assemble and preprocess: -D -I -elf -charset -strict-registers */
static const char *common_options(ArgVec *a, const J *args, Tmp *t, int *is_elf) {
    char **list;
    size_t n, i;
    int bad = 0;
    const char *fmt = jstr(args, "format"), *cs = jstr(args, "charset");
    *is_elf = 0;
    if (fmt) {
        if (!strcmp(fmt, "elf")) {
            add_arg(a, "-elf");
            *is_elf = 1;
        } else if (strcmp(fmt, "goff") != 0) {
            return "'format' must be \"goff\" or \"elf\"";
        }
    }
    if (cs) {
        if (strcmp(cs, "ascii") != 0 && strcmp(cs, "ebcdic") != 0) return "'charset' must be \"ascii\" or \"ebcdic\"";
        add_arg(a, "-charset");
        add_arg(a, cs);
    }
    if (jbool(args, "strict_registers", 0)) add_arg(a, "-strict-registers");
    n = str_array(args, "defines", &list, &bad);
    if (bad) return "'defines' must be an array of strings such as [\"NAME\",\"DEBUG=1\"]";
    for (i = 0; i < n; i++) vec_push(a, xasprintf("-D%s", list[i]));
    free(list);
    n = str_array(args, "include_dirs", &list, &bad);
    if (bad) return "'include_dirs' must be an array of directory names";
    for (i = 0; i < n; i++) {
        const char *wm = NULL;
        char *rp = ws_resolve(list[i], "'include_dirs'", &wm);
        if (!rp) { free(list); return wm; }
        vec_push(a, xasprintf("-I%s", rp));
        free(rp);
    }
    free(list);
    if (jget(args, "files")) vec_push(a, xasprintf("-I%s", t->dir));
    n = str_array(args, "macro_dirs", &list, &bad);
    if (bad) return "'macro_dirs' must be an array of directory names";
    for (i = 0; i < n; i++) {
        const char *wm = NULL;
        char *rp = ws_resolve(list[i], "'macro_dirs'", &wm);
        if (!rp) { free(list); return wm; }
        vec_push(a, xasprintf("-M%s", rp));
        free(rp);
    }
    free(list);
    if (jget(args, "files")) vec_push(a, xasprintf("-M%s", t->dir));
    if (jstr(args, "sysparm")) {
        add_arg(a, "-sysparm");
        add_arg(a, jstr(args, "sysparm"));
    }
    if (jget(args, "macro_steps")) {
        long ms = jint(args, "macro_steps", -1);
        if (ms < 1) return "'macro_steps' must be a positive integer";
        add_arg(a, "-macro-steps");
        vec_push(a, xasprintf("%ld", ms));
    }
    return NULL;
}

static void diag_text(Buf *text, const char *title, Buf *b) {
    if (b->len == 0) return;
    buf_printf(text, "--- %s ---\n", title);
    text_cap(text, b->p, b->len);
    if (b->p[b->len - 1] != '\n') buf_putc(text, '\n');
}

static void tool_assemble(const J *args, Buf *out) {
    Tmp t;
    ArgVec a = {0};
    const char *msg = NULL, *output = jstr(args, "output");
    char *in = NULL, *out_path = NULL, *dw_path = NULL, *objfile, *lstfile = NULL;
    Buf lbuf = {0};
    Run r;
    int is_elf = 0, listing = jbool(args, "listing", 0), want_b64 = jbool(args, "object_base64", 0), gz;
    Buf text = {0}, st = {0};
    int first = 1;
    unsigned char *obj = NULL, *dbg = NULL;
    size_t objlen = 0, dbglen = 0;
    long mx;
    const char *e;
    const char *dwarf_out = jstr(args, "dwarf_output");
    char *dbgfile = NULL;
    int want_dwarf = jbool(args, "dwarf", 0);
    memset(&r, 0, sizeof r);

    if (!tmp_open(&t)) { tool_fail(out, "cannot create a temporary directory"); return; }
    if ((e = write_files(&t, args)) != NULL) {
        tool_fail(out, e);
        goto done;
    }
    in = input_file(&t, args, "input.za", &msg);
    if (!in) {
        tool_fail(out, msg);
        goto done;
    }
    { /* outputs go inside the workspace */
        char *rp;
        const char *wm = NULL;
        if (output && *output) {
            rp = ws_resolve(output, "'output'", &wm);
            if (!rp) { tool_fail(out, wm); goto done; }
            output = out_path = rp;
        }
        if (dwarf_out && *dwarf_out) {
            rp = ws_resolve(dwarf_out, "'dwarf_output'", &wm);
            if (!rp) { tool_fail(out, wm); goto done; }
            dwarf_out = dw_path = rp;
        }
    }
    objfile = tmp_path(&t, ".object");
    add_arg(&a, "-o");
    add_arg(&a, objfile);
    if ((msg = common_options(&a, args, &t, &is_elf)) != NULL) {
        tool_fail(out, msg);
        goto done;
    }
    gz = (int)jint(args, "debug_info", 1);
    if (jget(args, "debug_info")) {
        if (gz < 0 || gz > 2) {
            tool_fail(out, "'debug_info' must be 0, 1 or 2");
            goto done;
        }
        vec_push(&a, xasprintf("-gz%d", gz));
    }
    if (want_dwarf) {
        char **maps;
        size_t nm, k;
        int bad = 0;
        if (is_elf && (jstr(args, "dwarf_output") || jstr(args, "dwarf_dbgpath") || jbool(args, "dwarf_base64", 0))) {
            tool_fail(out, "'dwarf_output', 'dwarf_base64' and 'dwarf_dbgpath' are for the .dbg file of GOFF; "
                           "with format \"elf\" the DWARF is in the object");
            goto done;
        }
        /* with format elf the DWARF sections are in the object and no .dbg file is written */
        dbgfile = is_elf ? NULL : tmp_path(&t, ".dbg");
        add_arg(&a, "-g");
        if (dbgfile) {
            add_arg(&a, "-gdwarf");
            add_arg(&a, dbgfile);
        }
        if (jstr(args, "dwarf_dir")) {
            add_arg(&a, "-gdir");
            add_arg(&a, jstr(args, "dwarf_dir"));
        }
        if (jstr(args, "dwarf_dbgpath")) {
            add_arg(&a, "-gdbgpath");
            add_arg(&a, jstr(args, "dwarf_dbgpath"));
        }
        nm = str_array(args, "dwarf_map", &maps, &bad);
        if (bad) {
            free(maps);
            tool_fail(out, "'dwarf_map' must be an array of \"FROM=TO\" strings");
            goto done;
        }
        for (k = 0; k < nm; k++) {
            add_arg(&a, "-gmap");
            add_arg(&a, maps[k]);
        }
        free(maps);
    }
    if (jbool(args, "werror", 0)) add_arg(&a, "-Werror");
    if (jbool(args, "reproducible", 0)) add_arg(&a, "-reproducible");
    if (jget(args, "max_errors")) {
        if (jget(args, "max_errors")->t != J_NUM) { tool_fail(out, "'max_errors' must be an integer"); goto done; }
        mx = jint(args, "max_errors", -1);
        if (mx < 0) { tool_fail(out, "'max_errors' must be a non-negative integer"); goto done; }
        add_arg(&a, "-max-errors");
        vec_push(&a, xasprintf("%ld", mx));
    }
    if (listing) {
        lstfile = tmp_path(&t, ".listing");
        add_arg(&a, "-l");
        add_arg(&a, lstfile);
        if (jbool(args, "listing_all_bytes", 0)) add_arg(&a, "-ldata");
        if (jbool(args, "listing_brief", 0)) add_arg(&a, "-lbrief");
    }
    add_arg(&a, in);
    {
        char *prog = tool_path("zosasm");
        run_prog(&t, prog, a.v, a.n, &r);
        free(prog);
    }
    scrub(&r.err, &t);
    scrub(&r.out, &t);

    obj = r.status == 0 ? read_binary(objfile, &objlen) : NULL;
    if (obj && dbgfile) {
        dbg = read_binary(dbgfile, &dbglen);
        if (dbg && dwarf_out && *dwarf_out && !write_whole(dwarf_out, dbg, dbglen)) {
            buf_printf(&text, "zosasm succeeded but the debug file could not be written to %s: %s\n", dwarf_out,
                       strerror(errno));
            dwarf_out = NULL;
            r.status = 1;
        }
    }
    if (obj && output && *output) {
        if (!write_whole(output, obj, objlen)) {
            buf_printf(&text, "zosasm succeeded but the object could not be written to %s: %s\n", output, strerror(errno));
            output = NULL;
            r.status = 1;
        }
    }
    buf_printf(&text, "zosasm %s (exit status %d)\n", r.status == 0 ? "succeeded" : "failed", r.status);
    if (obj) {
        buf_printf(&text, "object: %zu bytes, %s%s%s\n", objlen, is_elf ? "ELF64 s390x relocatable" : "z/OS GOFF",
                   output && *output ? ", written to " : "", output && *output ? output : "");
        if (!output || !*output) {
            if (!want_b64) buf_puts(&text, "(the object was not kept: give 'output' to write it to a file or 'object_base64' to get it here)\n");
        }
    }
    if (dbg) {
        buf_printf(&text, "debug file: %zu bytes (DWARF)%s%s\n", dbglen, dwarf_out && *dwarf_out ? ", written to " : "",
                   dwarf_out && *dwarf_out ? dwarf_out : "");
        if ((!dwarf_out || !*dwarf_out) && !jbool(args, "dwarf_base64", 0))
            buf_puts(&text, "(the debug file was not kept: give 'dwarf_output' or 'dwarf_base64')\n");
    }
    diag_text(&text, "diagnostics (stderr)", &r.err);
    diag_text(&text, "standard output", &r.out);
    if (r.status == 0 && r.err.len == 0) buf_puts(&text, "no diagnostics\n");
    if (listing) {
        size_t ll = 0;
        char *lst = read_whole(lstfile, &ll);
        if (lst) {
            buf_write(&lbuf, lst, ll);
            free(lst);
            scrub(&lbuf, &t);
            buf_puts(&text, "--- listing ---\n");
            text_cap(&text, lbuf.p ? lbuf.p : "", lbuf.len);
        }
    }

    buf_putc(&st, '{');
    w_key(&st, &first, "success");
    buf_puts(&st, r.status == 0 ? "true" : "false");
    w_key(&st, &first, "exit_code");
    buf_printf(&st, "%d", r.status);
    w_key(&st, &first, "format");
    w_cstr(&st, is_elf ? "elf" : "goff");
    w_key(&st, &first, "diagnostics");
    { Buf tcap={0}; text_cap(&tcap, r.err.p ? r.err.p : "", r.err.len); w_str(&st, tcap.p ? tcap.p : "", tcap.len); free(tcap.p); }
    if (obj) {
        w_key(&st, &first, "object_size");
        buf_printf(&st, "%zu", objlen);
        if (output && *output) {
            w_key(&st, &first, "output");
            w_cstr(&st, output);
        }
        if (want_b64) {
            Buf b64 = {0};
            b64_encode(&b64, obj, objlen);
            w_key(&st, &first, "object_base64");
            { Buf bcap={0}; text_cap(&bcap, b64.p ? b64.p : "", b64.len); w_str(&st, bcap.p ? bcap.p : "", bcap.len); free(bcap.p); }
            free(b64.p);
        }
    }
    if (dbg) {
        w_key(&st, &first, "dwarf_size");
        buf_printf(&st, "%zu", dbglen);
        if (dwarf_out && *dwarf_out) {
            w_key(&st, &first, "dwarf_output");
            w_cstr(&st, dwarf_out);
        }
        if (jbool(args, "dwarf_base64", 0)) {
            Buf b64 = {0}, bcap = {0};
            b64_encode(&b64, dbg, dbglen);
            text_cap(&bcap, b64.p ? b64.p : "", b64.len);
            w_key(&st, &first, "dwarf_base64");
            w_str(&st, bcap.p ? bcap.p : "", bcap.len);
            free(bcap.p);
            free(b64.p);
        }
    }
    if (listing) {
        w_key(&st, &first, "listing");
        { Buf lcap={0}; text_cap(&lcap, lbuf.p ? lbuf.p : "", lbuf.len); w_str(&st, lcap.p ? lcap.p : "", lcap.len); free(lcap.p); }
    }
    buf_putc(&st, '}');
    tool_result(out, buf_cstr(&text), text.len, buf_cstr(&st), r.status != 0);
done:
    free(in);
    free(out_path);
    free(dw_path);
    free_args(&a);
    free(text.p);
    free(st.p);
    free(lbuf.p);
    free(r.out.p);
    free(r.err.p);
    free(obj);
    free(dbg);
    tmp_close(&t);
}

static void tool_preprocess(const J *args, Buf *out) {
    Tmp t;
    ArgVec a = {0};
    const char *msg = NULL, *e;
    char *in = NULL;
    Run r;
    int is_elf;
    Buf text = {0}, st = {0};
    int first = 1;
    memset(&r, 0, sizeof r);
    if (!tmp_open(&t)) { tool_fail(out, "cannot create a temporary directory"); return; }
    if ((e = write_files(&t, args)) != NULL) {
        tool_fail(out, e);
        goto done;
    }
    in = input_file(&t, args, "input.za", &msg);
    if (!in) {
        tool_fail(out, msg);
        goto done;
    }
    add_arg(&a, "-E");
    if ((msg = common_options(&a, args, &t, &is_elf)) != NULL) {
        tool_fail(out, msg);
        goto done;
    }
    add_arg(&a, in);
    {
        char *prog = tool_path("zosasm");
        run_prog(&t, prog, a.v, a.n, &r);
        free(prog);
    }
    scrub(&r.err, &t);
    buf_printf(&text, "zosasm -E %s (exit status %d)\n", r.status == 0 ? "succeeded" : "failed", r.status);
    diag_text(&text, "diagnostics (stderr)", &r.err);
    buf_puts(&text, "--- preprocessed source ---\n");
    text_cap(&text, r.out.p ? r.out.p : "", r.out.len);
    buf_putc(&st, '{');
    w_key(&st, &first, "success");
    buf_puts(&st, r.status == 0 ? "true" : "false");
    w_key(&st, &first, "exit_code");
    buf_printf(&st, "%d", r.status);
    w_key(&st, &first, "preprocessed");
    { Buf ocap={0}; text_cap(&ocap, r.out.p ? r.out.p : "", r.out.len); w_str(&st, ocap.p ? ocap.p : "", ocap.len); free(ocap.p); }
    w_key(&st, &first, "diagnostics");
    { Buf tcap={0}; text_cap(&tcap, r.err.p ? r.err.p : "", r.err.len); w_str(&st, tcap.p ? tcap.p : "", tcap.len); free(tcap.p); }
    buf_putc(&st, '}');
    tool_result(out, buf_cstr(&text), text.len, buf_cstr(&st), r.status != 0);
done:
    free(in);
    free_args(&a);
    free(text.p);
    free(st.p);
    free(r.out.p);
    free(r.err.p);
    tmp_close(&t);
}

/* the object of dump and disasm: 'path' or 'object_base64' -> file name */
static char *object_file(Tmp *t, const J *args, const char **msg) {
    const char *path = jstr(args, "path"), *b64 = jstr(args, "object_base64");
    if ((path != NULL) == (b64 != NULL)) {
        *msg = "give exactly one of 'path' (an object file) or 'object_base64' (its bytes, base64 encoded)";
        return NULL;
    }
    if (path) {
        char *rp;
        const char *wm = NULL;
        if (!*path) {
            *msg = "'path' is empty";
            return NULL;
        }
        rp = ws_resolve(path, "'path'", &wm);
        if (!rp) {
            *msg = wm;
            return NULL;
        }
        return rp;
    }
    {
        size_t n;
        unsigned char *d;
        char *p;
        if (strlen(b64) > (size_t)MAX_TEXT * 2) {
            *msg = "'object_base64' is too large";
            return NULL;
        }
        d = b64_decode(b64, strlen(b64), &n);
        if (!d) {
            *msg = "'object_base64' is not valid base64";
            return NULL;
        }
        if (n > (size_t)MAX_TEXT * 2) {
            free(d);
            *msg = "'object_base64' decodes to an object that is too large";
            return NULL;
        }
        p = tmp_path(t, "input.o");
        if (!write_whole(p, d, n)) {
            free(d);
            *msg = "cannot write the temporary object file";
            return NULL;
        }
        free(d);
        return xstrdup(p); /* the caller frees; the Tmp copy is freed by tmp_close */
    }
}

static void simple_result(Buf *out, const char *what, Run *r, const char *key, Tmp *t) {
    Buf text = {0}, st = {0};
    int first = 1;
    scrub(&r->err, t);
    buf_printf(&text, "%s %s (exit status %d)\n", what, r->status == 0 ? "succeeded" : "failed", r->status);
    diag_text(&text, "diagnostics (stderr)", &r->err);
    buf_printf(&text, "--- %s ---\n", key);
    text_cap(&text, r->out.p ? r->out.p : "", r->out.len);
    buf_putc(&st, '{');
    w_key(&st, &first, "success");
    buf_puts(&st, r->status == 0 ? "true" : "false");
    w_key(&st, &first, "exit_code");
    buf_printf(&st, "%d", r->status);
    w_key(&st, &first, key);
    { Buf ocap={0}; text_cap(&ocap, r->out.p ? r->out.p : "", r->out.len); w_str(&st, ocap.p ? ocap.p : "", ocap.len); free(ocap.p); }
    w_key(&st, &first, "diagnostics");
    { Buf ecap={0}; text_cap(&ecap, r->err.p ? r->err.p : "", r->err.len); w_str(&st, ecap.p ? ecap.p : "", ecap.len); free(ecap.p); }
    buf_putc(&st, '}');
    tool_result(out, buf_cstr(&text), text.len, buf_cstr(&st), r->status != 0);
    free(text.p);
    free(st.p);
}

static void tool_dump(const J *args, Buf *out) {
    Tmp t;
    const char *msg = NULL;
    char *f = NULL, *av[2];
    Run r;
    memset(&r, 0, sizeof r);
    if (!tmp_open(&t)) { tool_fail(out, "cannot create a temporary directory"); return; }
    f = object_file(&t, args, &msg);
    if (!f) {
        tool_fail(out, msg);
        goto done;
    }
    av[0] = (char *)"-dump";
    av[1] = f;
    {
        char *prog = tool_path("zosasm");
        run_prog(&t, prog, av, 2, &r);
        free(prog);
    }
    simple_result(out, "zosasm -dump", &r, "dump", &t);
done:
    free(f);
    free(r.out.p);
    free(r.err.p);
    tmp_close(&t);
}

static void tool_disasm(const J *args, Buf *out) {
    Tmp t;
    const char *msg = NULL;
    char *f = NULL;
    ArgVec a = {0};
    Run r;
    memset(&r, 0, sizeof r);
    if (!tmp_open(&t)) { tool_fail(out, "cannot create a temporary directory"); return; }
    f = object_file(&t, args, &msg);
    if (!f) {
        tool_fail(out, msg);
        goto done;
    }
    if (jbool(args, "generic", 0)) add_arg(&a, "-generic");
    if (jbool(args, "strict_registers", 0)) add_arg(&a, "-strict-registers");
    add_arg(&a, "-disasm");
    add_arg(&a, f);
    {
        char *prog = tool_path("zosasm");
        run_prog(&t, prog, a.v, a.n, &r);
        free(prog);
    }
    simple_result(out, "zosasm -disasm", &r, "source", &t);
done:
    free(f);
    free_args(&a);
    free(r.out.p);
    free(r.err.p);
    tmp_close(&t);
}

static void tool_convert(const J *args, Buf *out) {
    Tmp t;
    const char *msg = NULL, *ictl = jstr(args, "ictl"), *incext = jstr(args, "include_extension");
    const char *mdir = jstr(args, "macro_dir"), *odir = jstr(args, "output_dir"), *cdir = jstr(args, "copy_dir");
    char *in = NULL, *rm = NULL, *ro = NULL, *rc = NULL;
    ArgVec a = {0};
    Run r;
    long w;
    memset(&r, 0, sizeof r);
    if (!tmp_open(&t)) { tool_fail(out, "cannot create a temporary directory"); return; }
    if (mdir) {
        const char *wm = NULL;
        /* a whole library: every member of macro_dir (.mac) becomes a .zmac file in output_dir */
        if (!*mdir || !odir || !*odir) {
            tool_fail(out, "'macro_dir' needs 'output_dir', the directory that receives the converted .zmac files");
            goto done;
        }
        if (jstr(args, "source") || jstr(args, "path")) {
            tool_fail(out, "give 'macro_dir' or one of 'source' / 'path', not both");
            goto done;
        }
        rm = ws_resolve(mdir, "'macro_dir'", &wm);
        if (!rm) { tool_fail(out, wm); goto done; }
        ro = ws_resolve(odir, "'output_dir'", &wm);
        if (!ro) { tool_fail(out, wm); goto done; }
        add_arg(&a, "-macro-dir");
        add_arg(&a, rm);
        add_arg(&a, "-o");
        add_arg(&a, ro);
    } else {
        in = input_file(&t, args, jbool(args, "macros", 0) ? "input.mac" : "input.asm", &msg);
        if (!in) {
            tool_fail(out, msg);
            goto done;
        }
        add_arg(&a, "-o");
        add_arg(&a, "-");
        if (jbool(args, "macros", 0)) add_arg(&a, "-m");
    }
    if (jbool(args, "relaxed", 0)) add_arg(&a, "-relaxed");
    if (jbool(args, "flat", 0)) add_arg(&a, "-flat");
    if (jbool(args, "keep_goto", 0)) add_arg(&a, "-keep-goto");
    if (jbool(args, "comment_macros", 0)) add_arg(&a, "-comment-macros");
    if (cdir) {
        const char *wm = NULL;
        rc = ws_resolve(cdir, "'copy_dir'", &wm);
        if (!rc) { tool_fail(out, wm); goto done; }
        add_arg(&a, "-copy-dir");
        add_arg(&a, rc);
    }
    w = jint(args, "width", -1);
    if (w >= 0) {
        add_arg(&a, "-w");
        vec_push(&a, xasprintf("%ld", w));
    }
    if (ictl) {
        add_arg(&a, "-ictl");
        add_arg(&a, ictl);
    }
    if (incext) {
        add_arg(&a, "-inc-ext");
        add_arg(&a, incext);
    }
    if (in) add_arg(&a, in);
    {
        char *prog = tool_path("hlasm2za");
        run_prog(&t, prog, a.v, a.n, &r);
        free(prog);
    }
    simple_result(out, "hlasm2za", &r, mdir ? "output" : "source", &t);
done:
    free(in);
    free(rm);
    free(ro);
    free(rc);
    free_args(&a);
    free(r.out.p);
    free(r.err.p);
    tmp_close(&t);
}

static void tool_guide(const J *args, Buf *out) {
    const char *sec = jstr(args, "section"), *q = jstr(args, "search");
    Guide *g = load_guide();
    Buf text = {0}, st = {0};
    int first = 1;
    if (!g) {
        char *m = xasprintf("the user's guide %s cannot be read (set ZOSASM_GUIDE or start zosasm-mcp from the build directory of a zosasm checkout)", g_guide);
        tool_fail(out, m);
        free(m);
        return;
    }
    if (q && *q) {
        size_t i, hits = 0, ql = strlen(q);
        buf_printf(&text, "lines of %s that contain \"%s\" (case ignored); read one with the 'section' argument:\n", g_guide, q);
        for (i = 0; i < g->lines.n && hits < 40; i++) {
            const char *l = g->lines.v[i];
            size_t ll = strlen(l), k;
            for (k = 0; k + ql <= ll; k++)
                if (za_strncmpi(l + k, q, ql) == 0) {
                    const char *s = section_of_line(g, i);
                    buf_printf(&text, "line %zu [%s]: %.160s\n", i + 1, s, l);
                    hits++;
                    break;
                }
        }
        if (!hits) buf_puts(&text, "no match\n");
    } else if (sec && *sec) {
        if (!strcmp(sec, "all")) {
            size_t i;
            for (i = 0; i < g->lines.n; i++) {
                if (text.len + strlen(g->lines.v[i]) + 1 > MAX_TEXT) {
                    buf_printf(&text, "\n[... guide truncated at %zu bytes ...]\n", MAX_TEXT);
                    break;
                }
                buf_puts(&text, g->lines.v[i]);
                buf_putc(&text, '\n');
            }
        } else {
            int amb, hi = find_heading(g, sec, &amb);
            if (hi < 0) {
                Buf shown = {0};
                buf_printf(&text, "no section \"%s\". Contents:\n", sec);
                guide_contents(g, &text);
                text_cap(&shown, text.p ? text.p : "", text.len);
                tool_result(out, buf_cstr(&shown), shown.len, NULL, 1);
                free(shown.p);
                goto done;
            }
            if (amb) buf_printf(&text, "[%s matches several headings; showing the first, give a section number to be exact]\n\n", sec);
            guide_section(g, hi, &text);
        }
    } else {
        buf_printf(&text,
                   "zosasm user's guide: %s\n\n"
                   "How to use it: call this tool with section=\"6\" (a chapter), \"6.1\" (a subsection), a part of a\n"
                   "heading (\"listing\"), or section=\"all\" for the whole guide; search=\"word\" finds lines.\n"
                   "The same text is the MCP resource zosasm://guide. Start with chapters 1 (getting started),\n"
                   "2 (the source language), 6 (instructions and registers), 13 (command reference: every option),\n"
                   "15 (recipes), 17 (Linux on Z / ELF). Example programs: resources zosasm://examples/NAME.\n\n"
                   "Contents:\n",
                   g_guide);
        guide_contents(g, &text);
    }
    buf_putc(&st, '{');
    w_key(&st, &first, "guide_path");
    w_cstr(&st, g_guide);
    {
        Buf shown = {0};
        text_cap(&shown, text.p ? text.p : "", text.len);
        w_key(&st, &first, "text");
        w_str(&st, shown.p ? shown.p : "", shown.len);
        buf_putc(&st, '}');
        tool_result(out, buf_cstr(&shown), shown.len, buf_cstr(&st), 0);
        free(shown.p);
    }
done:
    free(text.p);
    free(st.p);
}

/* ---- tool list ---- */

#define P_SRC                                                                                     \
    "\"source\":{\"type\":\"string\",\"description\":\"The program text, passed inline. Give " \
    "either 'source' or 'path', not both.\"},"                                                    \
    "\"path\":{\"type\":\"string\",\"description\":\"Name of a file on the machine that runs this " \
    "server. File names are limited to the workspace, the directory of the server binary " \
    "(relative names are relative to it); the operator can lift the limit with the " \
    "ZOSASM_UNRESTRICTED environment variable. Give either 'path' or 'source'.\"},"       \
    "\"name\":{\"type\":\"string\",\"description\":\"With 'source': the file name shown in " \
    "diagnostics and listings (default input.za; a plain name, no directory).\"}"

#define P_PP                                                                                       \
    "\"defines\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Preprocessor " \
    "macros as for -D: \\\"NAME\\\" (value 1) or \\\"NAME=value\\\". Example: [\\\"DEBUG\\\",\\\"LEVEL=3\\\"].\"},"  \
    "\"include_dirs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Directories " \
    "searched by #include (-I), inside the workspace.\"},"                                        \
    "\"files\":{\"type\":\"object\",\"additionalProperties\":{\"type\":\"string\"},\"description\":" \
    "\"Extra files for #include and macro calls, as {\\\"regs.inc\\\":\\\"text...\\\"}. Written to a temporary " \
    "directory that is searched by #include and by macro calls (NAME.zmac); use plain file names.\"},"                            \
    "\"format\":{\"type\":\"string\",\"enum\":[\"goff\",\"elf\"],\"description\":\"Object format: " \
    "goff = z/OS GOFF object (default); elf = relocatable ELF64 s390x object for Linux on Z (implies " \
    "charset ascii; the preprocessor defines __ELF__ instead of __GOFF__).\"},"                   \
    "\"charset\":{\"type\":\"string\",\"enum\":[\"ascii\",\"ebcdic\"],\"description\":\"Character " \
    "set of C'..' constants (default ebcdic IBM-1047; ascii ISO-8859-1 with format elf). CA'..' and " \
    "CE'..' always mean ASCII and EBCDIC.\"},"                                                     \
    "\"strict_registers\":{\"type\":\"boolean\",\"description\":\"Accept only %r1 style registers " \
    "(or names made with NAME EQU %r1) in register positions; numbers are errors. Default false.\"}," \
    "\"macro_dirs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Macro " \
    "libraries (-M): directories searched for NAME.zmac or NAME.mac when a macro is called but not " \
    "defined, inside the workspace. Macros given in 'files' are found too. Guide chapter 19.\"}," \
    "\"sysparm\":{\"type\":\"string\",\"description\":\"The text that sys.parm (HLASM &SYSPARM) has in " \
    "macros (-sysparm).\"}," \
    "\"macro_steps\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Stop a macro expansion " \
    "after this many statements (-macro-steps, default 1000000).\"}"

/* Process-lifetime cache: built once, intentionally kept for the server
 * lifetime. This is not a per-request leak; zosasm-mcp is short/medium lived
 * and the data is small and immutable for the process. */
static const char *tools_json(void) {
    const char *g;
    Guide *gd;
    Buf b = {0}, o = {0}, gp = {0};
    char *tmp;
    const char *p;
    size_t gl;
    if (g_tools_json) return g_tools_json;
    g = g_guide;
    gd = load_guide();
    (void)gd;
    buf_puts(&b, "[");
    /* zosasm_assemble */
    buf_puts(&b,
        "{\"name\":\"zosasm_assemble\",\"title\":\"Assemble zosasm source\","
        "\"description\":\"Assemble a zosasm program (an HLASM-like assembly language for IBM Z, source "
        "extension .za, C-style preprocessor, registers written %r1 %f0, quoted symbols) into an object "
        "file. INPUT: exactly one of 'source' (the text) or 'path' (a file). OUTPUT: an object in z/OS GOFF "
        "format by default, or a relocatable ELF64 s390x object for Linux on Z with format 'elf'. The object "
        "is written to the file named in 'output'; with 'object_base64' true it is also returned in the "
        "result. The result always carries 'diagnostics' (messages ZAnnnn with the source line and often a "
        "help text; read them and fix the source), 'success', 'exit_code', and with 'listing' true the "
        "assembly listing (location, object bytes, source). Nothing is written when there are errors. "
        "The language and every option are described in the user's guide, @GUIDE@: read it with the tool "
        "zosasm_guide (chapters 1, 2, 6, 13) or the resource zosasm://guide. No z/OS system is needed or "
        "used; objects are not run or linked here.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{" P_SRC "," P_PP ",");
    buf_puts(&b,
        "\"output\":{\"type\":\"string\",\"description\":\"File name to write the object to, inside "
        "the workspace (default: the object is not kept; the call only checks the program).\"},"
        "\"object_base64\":{\"type\":\"boolean\",\"description\":\"Also return the object, base64 encoded, "
        "in the 'object_base64' field (default false).\"},"
        "\"listing\":{\"type\":\"boolean\",\"description\":\"Return the assembly listing in the 'listing' "
        "field (default false). The listing is produced even when there are errors.\"},"
        "\"listing_all_bytes\":{\"type\":\"boolean\",\"description\":\"Listing: show all object bytes of a "
        "statement, not the first 8 (-ldata).\"},"
        "\"listing_brief\":{\"type\":\"boolean\",\"description\":\"Listing: leave out the tables at the end "
        "(-lbrief).\"},"
        "\"debug_info\":{\"type\":\"integer\",\"enum\":[0,1,2],\"description\":\"Symbolic information in a "
        "GOFF object (-gz): 0 none, 1 symbols and statements (default), 2 also the original sources. "
        "Ignored for ELF.\"},"
        "\"dwarf\":{\"type\":\"boolean\",\"description\":\"Also make the debug information of z/OS "
        "`as -g`: DWARF for dbx and LLDB, in the object (link module, class C_ADATAE9C3) and in a separate .dbg file (-gside). "
        "Give 'dwarf_output' or 'dwarf_base64' to get the .dbg. Use 'dwarf_dir' to say where the "
        "source and the .dbg will be ON Z/OS. With format elf the DWARF sections are in the object itself (no .dbg; "
        "dwarf_dir and dwarf_map apply). Guide chapter 21.\"},"
        "\"dwarf_output\":{\"type\":\"string\",\"description\":\"File name to write the .dbg to, "
        "inside the workspace.\"},"
        "\"dwarf_base64\":{\"type\":\"boolean\",\"description\":\"Also return the .dbg base64 encoded in "
        "'dwarf_base64'.\"},"
        "\"dwarf_dir\":{\"type\":\"string\",\"description\":\"The z/OS directory that corresponds to the "
        "current directory (-gdir); the .dbg path recorded in the object is made from it.\"},"
        "\"dwarf_map\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Path "
        "prefixes FROM=TO of this machine and the z/OS (-gmap).\"},"
        "\"dwarf_dbgpath\":{\"type\":\"string\",\"description\":\"The z/OS path of the .dbg, at most 200 "
        "bytes (-gdbgpath).\"},"
        "\"werror\":{\"type\":\"boolean\",\"description\":\"Treat warnings as errors (-Werror).\"},"
        "\"reproducible\":{\"type\":\"boolean\",\"description\":\"Fail on anything that makes the object "
        "differ between runs (-reproducible).\"},"
        "\"max_errors\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"Stop after this many errors "
        "(default 50, 0 = no limit).\"}"
        "}},\"annotations\":{\"readOnlyHint\":false,\"destructiveHint\":false,\"idempotentHint\":true,"
        "\"openWorldHint\":false}},");
    /* zosasm_preprocess */
    buf_puts(&b,
        "{\"name\":\"zosasm_preprocess\",\"title\":\"Preprocess zosasm source\","
        "\"description\":\"Run only the C-style preprocessor (#define, #include, #if, #error, "
        "__LINE__ ...) and return the resulting source text (zosasm -E). Use it to see what macros "
        "expand to. INPUT: 'source' or 'path' plus the preprocessor options. OUTPUT: 'preprocessed' (the "
        "lines the assembler would read) and 'diagnostics'. Preprocessor chapter: guide section 3 (@GUIDE@).\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{" P_SRC "," P_PP "}},"
        "\"annotations\":{\"readOnlyHint\":true,\"openWorldHint\":false}},");
    /* zosasm_dump */
    buf_puts(&b,
        "{\"name\":\"zosasm_dump\",\"title\":\"Print the structure of a GOFF object\","
        "\"description\":\"Print the records of a z/OS GOFF object file (zosasm -dump): ESD items "
        "(sections, classes, labels, external references) with their attributes, text, relocations and "
        "the END record. INPUT: exactly one of 'path' (an object file) or 'object_base64' (the bytes of "
        "an object, for example from zosasm_assemble). OUTPUT: 'dump' (text). Only GOFF is read, not ELF "
        "(use readelf for that). Guide section 12.1 (@GUIDE@).\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"Object file name. Give either 'path' or "
        "'object_base64'.\"},"
        "\"object_base64\":{\"type\":\"string\",\"description\":\"The object, base64 encoded.\"}}},"
        "\"annotations\":{\"readOnlyHint\":true,\"openWorldHint\":false}},");
    /* zosasm_disasm */
    buf_puts(&b,
        "{\"name\":\"zosasm_disasm\",\"title\":\"Convert a GOFF object back to source\","
        "\"description\":\"Turn a GOFF object file into zosasm source (zosasm -disasm). With ZSYM "
        "information (assembled with -gz1 or -gz2, the default) the result is close to the original "
        "source: labels, DC/DS, EQU; without it the bytes are decoded as instructions and data. INPUT: "
        "exactly one of 'path' or 'object_base64'. OUTPUT: 'source' (the .za text) and 'diagnostics'. "
        "Guide section 12.3 (@GUIDE@).\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"Object file name. Give either 'path' or "
        "'object_base64'.\"},"
        "\"object_base64\":{\"type\":\"string\",\"description\":\"The object, base64 encoded.\"},"
        "\"generic\":{\"type\":\"boolean\",\"description\":\"Ignore the ZSYM information and decode the "
        "bytes generically (-generic).\"},"
        "\"strict_registers\":{\"type\":\"boolean\",\"description\":\"Write register operands as %r1 "
        "style (-strict-registers).\"}}},"
        "\"annotations\":{\"readOnlyHint\":true,\"openWorldHint\":false}},");
    /* zosasm_convert_hlasm */
    buf_puts(&b,
        "{\"name\":\"zosasm_convert_hlasm\",\"title\":\"Convert IBM HLASM source to zosasm\","
        "\"description\":\"Convert High Level Assembler source (fixed card columns 1-71, continuation in "
        "column 72, upper case, COPY) to zosasm source (hlasm2za). INPUT: exactly one of 'source' (HLASM "
        "text) or 'path'. OUTPUT: 'source' (the converted .za text) and 'diagnostics', which list what "
        "could not be converted and was left or commented out: conditional assembly outside macros (AIF, "
        "&variables), OPSYN, ALIAS and macro calls such as SAVE or WTO; they must "
        "be rewritten by hand (use the C-style preprocessor, guide section 3). Registers become %r1 style "
        "so that the result assembles with strict_registers. MACRO..MEND definitions are converted to "
        "zosasm macros (#macro, guide chapter 19) with their control flow rewritten as if/while/switch; "
        "with 'macros' true the input is a macro library member and only the definitions are returned, "
        "with 'macro_dir' a whole library is converted to a directory. Assemble with the converted macros "
        "through zosasm_assemble 'macro_dirs' or 'files'. After converting, run zosasm_assemble on the "
        "result. Guide section 16 (@GUIDE@).\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{" P_SRC ","
        "\"relaxed\":{\"type\":\"boolean\",\"description\":\"Keep register numbers as they are instead of "
        "writing %r1 (default false).\"},"
        "\"width\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"Break operand lists after a comma "
        "beyond this column (default 100, 0 = never).\"},"
        "\"ictl\":{\"type\":\"string\",\"description\":\"Source columns \\\"begin,end,continue\\\" when the "
        "file has no ICTL (default \\\"1,71,16\\\").\"},"
        "\"include_extension\":{\"type\":\"string\",\"description\":\"Extension of the files that COPY "
        "becomes #include of (default .inc).\"},"
        "\"macros\":{\"type\":\"boolean\",\"description\":\"The input is a macro library member (MACRO..MEND "
        "definitions): they are converted to #macro and only the definitions are returned (-m). Guide 16.4.\"},"
        "\"flat\":{\"type\":\"boolean\",\"description\":\"Macros: write #goto/#label for every sequence "
        "symbol, no structure recovery (-flat).\"},"
        "\"keep_goto\":{\"type\":\"boolean\",\"description\":\"Macros: where the flow cannot be "
        "structured write #goto/#label instead of a dispatch loop (-keep-goto).\"},"
        "\"comment_macros\":{\"type\":\"boolean\",\"description\":\"Leave macro definitions as comments "
        "instead of converting them (-comment-macros).\"},"
        "\"copy_dir\":{\"type\":\"string\",\"description\":\"Macros: COPY inside a macro definition reads "
        "DIR/MEMBER.mac (-copy-dir; with macro_dir the library directory is the default).\"},"
        "\"macro_dir\":{\"type\":\"string\",\"description\":\"Convert every member DIR/*.mac of a macro "
        "library (-macro-dir) and write the .zmac files to 'output_dir'. Use instead of 'source'/'path'. "
        "The result lists the counts and what was not converted in 'diagnostics'.\"},"
        "\"output_dir\":{\"type\":\"string\",\"description\":\"With macro_dir: the directory that "
        "receives the converted files, inside the workspace.\"}}},"
        "\"annotations\":{\"readOnlyHint\":false,\"destructiveHint\":false,\"idempotentHint\":true,"
        "\"openWorldHint\":false}},");
    /* zosasm_guide */
    buf_puts(&b,
        "{\"name\":\"zosasm_guide\",\"title\":\"Read the zosasm user's guide\","
        "\"description\":\"Read the zosasm user's guide (@GUIDE@): the language, every command line option "
        "(chapter 13), diagnostics (11), recipes (15), HLASM conversion (16) and ELF output (17). With no "
        "arguments it returns how to use the guide and its table of contents. 'section' returns one "
        "chapter or subsection (\\\"6\\\", \\\"6.1\\\", \\\"listing\\\", or \\\"all\\\" for everything); "
        "'search' lists the lines that contain a word, with their section. Call this before writing "
        "zosasm source if you do not know the language: it is close to IBM HLASM but has no card columns, "
        "uses a C-style preprocessor instead of macros, and writes registers as %r1.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
        "\"section\":{\"type\":\"string\",\"description\":\"A chapter number (\\\"13\\\"), a subsection "
        "(\\\"12.3\\\"), part of a heading (\\\"listing\\\") or \\\"all\\\".\"},"
        "\"search\":{\"type\":\"string\",\"description\":\"Case-insensitive text to look for; returns "
        "matching lines with line numbers and sections (at most 40).\"}}},"
        "\"annotations\":{\"readOnlyHint\":true,\"openWorldHint\":false}}");
    buf_puts(&b, "]");
    tmp = xstrdup(buf_cstr(&b));
    free(b.p);
    /* @GUIDE@ -> the guide's path, escaped for JSON */
    w_cstr(&gp, g);
    /* w_cstr adds the quotes; strip them */
    gl = gp.len >= 2 ? gp.len - 2 : 0;
    p = tmp;
    while (*p) {
        if (!strncmp(p, "@GUIDE@", 7)) {
            buf_write(&o, gp.p + 1, gl);
            p += 7;
        } else {
            buf_putc(&o, *p++);
        }
    }
    free(gp.p);
    free(tmp);
    g_tools_json = xstrdup(buf_cstr(&o));
    free(o.p);
    return g_tools_json;
}

static void tool_call(const char *name, const J *args, Buf *out) {
    if (!strcmp(name, "zosasm_assemble"))
        tool_assemble(args, out);
    else if (!strcmp(name, "zosasm_preprocess"))
        tool_preprocess(args, out);
    else if (!strcmp(name, "zosasm_dump"))
        tool_dump(args, out);
    else if (!strcmp(name, "zosasm_disasm"))
        tool_disasm(args, out);
    else if (!strcmp(name, "zosasm_convert_hlasm"))
        tool_convert(args, out);
    else if (!strcmp(name, "zosasm_guide"))
        tool_guide(args, out);
    else
        tool_fail(out, "unknown tool");
}

/* ====================================================================
 * Resources
 * ==================================================================== */

static int list_examples(ArgVec *names) {
    char *dir = xasprintf("%s/examples", g_docdir);
    DIR *d = opendir(dir);
    struct dirent *e;
    size_t i, j;
    if (!d) { free(dir); return 0; }
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.') vec_push(names, xstrdup(e->d_name));
    closedir(d);
    free(dir);
    for (i = 1; i < names->n; i++) /* insertion sort */
        for (j = i; j > 0 && strcmp(names->v[j - 1], names->v[j]) > 0; j--) {
            char *t = names->v[j];
            names->v[j] = names->v[j - 1];
            names->v[j - 1] = t;
        }
    return 1;
}

static void resources_list(Buf *out) {
    ArgVec ex = {0};
    size_t i;
    buf_puts(out, "{\"resources\":[");
    buf_puts(out,
             "{\"uri\":\"zosasm://guide\",\"name\":\"guide\",\"title\":\"zosasm user's guide\","
             "\"description\":\"The complete user's guide (Markdown): language, options, diagnostics, "
             "recipes, HLASM conversion, ELF output.\",\"mimeType\":\"text/markdown\"}");
    list_examples(&ex);
    for (i = 0; i < ex.n; i++) {
        char *uri = xasprintf("zosasm://examples/%s", ex.v[i]);
        int first = 1;
        buf_putc(out, ',');
        buf_putc(out, '{');
        w_key(out, &first, "uri");
        w_cstr(out, uri);
        w_key(out, &first, "name");
        w_cstr(out, ex.v[i]);
        w_key(out, &first, "description");
        w_cstr(out, "An example program from the user's guide (every example is assembled by the test suite).");
        w_key(out, &first, "mimeType");
        w_cstr(out, "text/plain");
        buf_putc(out, '}');
        free(uri);
        free(ex.v[i]);
    }
    free(ex.v);
    buf_puts(out, "]}");
}

static void resources_templates(Buf *out) {
    buf_puts(out,
             "{\"resourceTemplates\":[{\"uriTemplate\":\"zosasm://guide/section/{section}\","
             "\"name\":\"guide-section\",\"title\":\"One section of the user's guide\","
             "\"description\":\"A chapter (\\\"13\\\"), subsection (\\\"12.3\\\") or part of a heading "
             "(\\\"listing\\\") of the user's guide.\",\"mimeType\":\"text/markdown\"}]}");
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *url_decode(const char *s) {
    Buf b = {0};
    char *r;
    for (; *s; s++) {
        if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            buf_putc(&b, hexval(s[1]) * 16 + hexval(s[2]));
            s += 2;
        } else if (*s == '+') {
            buf_putc(&b, ' ');
        } else {
            buf_putc(&b, *s);
        }
    }
    r = xstrdup(buf_cstr(&b));
    free(b.p);
    return r;
}

/* returns 1 and fills the result, or 0 when the uri is unknown */
static int resource_read(const char *uri, Buf *out) {
    Buf text = {0};
    const char *mime = "text/plain";
    Guide *g;
    int first = 1;
    if (!strcmp(uri, "zosasm://guide")) {
        size_t i;
        g = load_guide();
        if (!g) { free(text.p); return 0; }
        for (i = 0; i < g->lines.n; i++) {
            if (text.len + strlen(g->lines.v[i]) + 1 > MAX_TEXT) {
                buf_printf(&text, "\n[... guide truncated at %zu bytes ...]\n", (size_t)MAX_TEXT);
                break;
            }
            buf_puts(&text, g->lines.v[i]);
            buf_putc(&text, '\n');
        }
        mime = "text/markdown";
    } else if (!strncmp(uri, "zosasm://guide/section/", 23)) {
        char *q = url_decode(uri + 23);
        int amb, hi;
        g = load_guide();
        if (!g) { free(q); free(text.p); return 0; }
        hi = find_heading(g, q, &amb);
        free(q);
        if (hi < 0) { free(text.p); return 0; }
        {
            size_t i, end = g->lines.n;
            int lv = g->heads.v[hi].level;
            size_t k;
            for (k = (size_t)hi + 1; k < g->heads.n; k++)
                if (g->heads.v[k].level <= lv) { end = g->heads.v[k].line; break; }
            for (i = g->heads.v[hi].line; i < end; i++) {
                if (text.len + strlen(g->lines.v[i]) + 1 > MAX_TEXT) {
                    buf_printf(&text, "\n[... section truncated at %zu bytes ...]\n", (size_t)MAX_TEXT);
                    break;
                }
                buf_puts(&text, g->lines.v[i]);
                buf_putc(&text, '\n');
            }
        }
        mime = "text/markdown";
    } else if (!strncmp(uri, "zosasm://examples/", 18)) {
        const char *name = uri + 18;
        char *path, *t;
        size_t n;
        if (!plain_name(name)) { free(text.p); return 0; }
        path = xasprintf("%s/examples/%s", g_docdir, name);
        t = read_whole(path, &n);
        free(path);
        if (!t) { free(text.p); return 0; }
        if (n > MAX_TEXT) n = MAX_TEXT;
        buf_write(&text, t, n);
        if (n == (size_t)MAX_TEXT) buf_printf(&text, "\n[... example truncated at %zu bytes ...]\n", (size_t)MAX_TEXT);
        free(t);
    } else {
        free(text.p);
        return 0;
    }
    buf_puts(out, "{\"contents\":[{");
    w_key(out, &first, "uri");
    w_cstr(out, uri);
    w_key(out, &first, "mimeType");
    w_cstr(out, mime);
    w_key(out, &first, "text");
    {
        Buf shown = {0};
        text_cap(&shown, text.p ? text.p : "", text.len);
        w_str(out, shown.p ? shown.p : "", shown.len);
        free(shown.p);
    }
    buf_puts(out, "}]}");
    free(text.p);
    return 1;
}

/* ====================================================================
 * JSON-RPC
 * ==================================================================== */

#define INSTRUCTIONS                                                                                       \
    "zosasm is a cross assembler for an HLASM-like language for IBM Z. It writes z/OS GOFF object files "  \
    "(or ELF64 s390x objects for Linux on Z with format=elf). Source files have the extension .za. "        \
    "Workflow: (1) if you do not know the language, call zosasm_guide (no arguments for the contents, "     \
    "section=\"6\" for a chapter, search=\"word\" to find text); the full guide is the file @GUIDE@ and "   \
    "the resource zosasm://guide. (2) Write the program and call zosasm_assemble with source (inline text) "\
    "or path; give output to keep the object, listing=true to see addresses and bytes. Read 'diagnostics': "\
    "every message has a code ZAnnnn, the source line and usually a help line. Fix and repeat until "       \
    "success is true. (3) zosasm_preprocess shows macro expansion, zosasm_dump and zosasm_disasm inspect "  \
    "a GOFF object (path or object_base64), zosasm_convert_hlasm converts IBM HLASM source (then assemble "  \
    "the result). Inputs are either text in the arguments or file names; file names are limited "     \
    "to the workspace (the directory of this server binary; relative names are relative to it; "     \
    "the operator can lift the limit with ZOSASM_UNRESTRICTED). Outputs "   \
    "are in the result fields and, for objects, the 'output' file. Nothing is executed: objects are not "    \
    "linked or run, and no z/OS system is involved."

static void send_msg(const char *s, size_t n) {
    fwrite(s, 1, n, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void reply_result(const J *id, const char *result) {
    Buf b = {0};
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    w_json(&b, id);
    buf_puts(&b, ",\"result\":");
    buf_puts(&b, result);
    buf_putc(&b, '}');
    send_msg(b.p, b.len);
    free(b.p);
}

static void reply_error(const J *id, int code, const char *msg) {
    Buf b = {0};
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id)
        w_json(&b, id);
    else
        buf_puts(&b, "null");
    buf_printf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
    w_cstr(&b, msg);
    buf_puts(&b, "}}");
    send_msg(b.p, b.len);
    free(b.p);
}

/* one request: the reply is written and the result buffer freed */
static void dispatch(const J *msg, Buf *r) {
    const char *method = jstr(msg, "method");
    J *id = jget(msg, "id");
    J *params = jget(msg, "params");
    if (!method) {
        if (id) reply_error(id, -32600, "invalid request: no method");
        return; /* a response or garbage: ignore */
    }
    if (!id) return; /* notification (initialized, cancelled ...) */
    if (!strcmp(method, "initialize")) {
        const char *want = params ? jstr(params, "protocolVersion") : NULL;
        static const char *known[] = {"2025-06-18", "2025-03-26", "2024-11-05", NULL};
        const char *use = PROTOCOL_LATEST;
        size_t i;
        for (i = 0; want && known[i]; i++)
            if (!strcmp(want, known[i])) use = known[i];
        buf_printf(r, "{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{\"listChanged\":false},"
                       "\"resources\":{\"subscribe\":false,\"listChanged\":false}},"
                       "\"serverInfo\":{\"name\":\"zosasm-mcp\",\"title\":\"zosasm assembler\",\"version\":\"%s\"},"
                       "\"instructions\":",
                   use, ZASM_VERSION_STR);
        {
            Buf ins = {0};
            const char *t = INSTRUCTIONS, *p;
            size_t gl = strlen(g_guide);
            for (p = t; *p;) {
                if (!strncmp(p, "@GUIDE@", 7)) {
                    buf_write(&ins, g_guide, gl);
                    p += 7;
                } else {
                    buf_putc(&ins, *p++);
                }
            }
            w_str(r, ins.p, ins.len);
            free(ins.p);
        }
        buf_putc(r, '}');
        if (r->len > MAX_TEXT) { r->len = 0; reply_error(id, -32603, "initialize result too large"); }
        else reply_result(id, buf_cstr(r));
    } else if (!strcmp(method, "ping")) {
        reply_result(id, "{}");
    } else if (!strcmp(method, "tools/list")) {
        {
            const char *tj = tools_json();
            if (strlen(tj) > MAX_TEXT) reply_error(id, -32603, "tool list too large");
            else { buf_printf(r, "{\"tools\":%s}", tj); reply_result(id, buf_cstr(r)); }
        }
    } else if (!strcmp(method, "tools/call")) {
        const char *name = params ? jstr(params, "name") : NULL;
        J *args = params ? jget(params, "arguments") : NULL;
        if (!name) {
            reply_error(id, -32602, "tools/call needs a tool name");
            return;
        }
        if (args && args->t != J_OBJ) {
            reply_error(id, -32602, "'arguments' must be an object");
            return;
        }
        if (!strcmp(name, "zosasm_assemble") || !strcmp(name, "zosasm_preprocess") ||
            !strcmp(name, "zosasm_dump") || !strcmp(name, "zosasm_disasm") ||
            !strcmp(name, "zosasm_convert_hlasm") || !strcmp(name, "zosasm_guide")) {
            tool_call(name, args, r);
            if (r->len > MAX_TEXT) { r->len = 0; reply_error(id, -32603, "tool result too large"); }
            else reply_result(id, buf_cstr(r));
        } else {
            reply_error(id, -32602, "unknown tool");
        }
    } else if (!strcmp(method, "resources/list")) {
        resources_list(r);
        if (r->len > MAX_TEXT) { r->len = 0; reply_error(id, -32603, "resource list too large"); }
        else reply_result(id, buf_cstr(r));
    } else if (!strcmp(method, "resources/templates/list")) {
        resources_templates(r);
        if (r->len > MAX_TEXT) { r->len = 0; reply_error(id, -32603, "resource templates too large"); }
        else reply_result(id, buf_cstr(r));
    } else if (!strcmp(method, "resources/read")) {
        const char *uri = params ? jstr(params, "uri") : NULL;
        if (!uri) {
            reply_error(id, -32602, "resources/read needs a uri");
        } else if (!resource_read(uri, r)) {
            reply_error(id, -32002, "resource not found");
        } else if (r->len > MAX_TEXT) {
            r->len = 0;
            reply_error(id, -32603, "resource too large");
        } else {
            reply_result(id, buf_cstr(r));
        }
    } else if (!strcmp(method, "prompts/list")) {
        reply_result(id, "{\"prompts\":[]}");
    } else {
        reply_error(id, -32601, "method not found");
    }
}

/* one request: dispatch it and free the reply buffer (the server is a long
 * lived process, so per-request memory is given back) */
static void handle(const J *msg) {
    Buf r = {0};
    dispatch(msg, &r);
    free(r.p);
}

#ifndef ZA_MCP_NO_MAIN
static void usage(void) {
    fprintf(stderr,
            "usage: zosasm-mcp [--guide FILE] [--bin-dir DIR]\n"
            "  A Model Context Protocol server (stdio) for AI agents: tools to assemble, preprocess,\n"
            "  dump and disassemble, convert HLASM source and read the user's guide.\n"
            "  --guide FILE    the user's guide (default: ../docs/GUIDE.md next to the program,\n"
            "                  or environment ZOSASM_GUIDE)\n"
            "  --bin-dir DIR   where zosasm and hlasm2za are (default: the program's directory,\n"
            "                  or environment ZOSASM_BIN_DIR)\n"
            "File names are limited to the program's directory (the workspace), unless\n"
            "  the environment variable ZOSASM_UNRESTRICTED is set to a non-empty value.\n"
            "Register it in an MCP client as the command build/zosasm-mcp (see README.md).\n");
}

int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--guide") && i + 1 < argc) {
            setenv("ZOSASM_GUIDE", argv[++i], 1);
        } else if (!strcmp(argv[i], "--bin-dir") && i + 1 < argc) {
            setenv("ZOSASM_BIN_DIR", argv[++i], 1);
        } else if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-version")) {
            printf("zosasm-mcp %s\n", ZASM_VERSION_STR);
            return 0;
        } else {
            usage();
            return !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") ? 0 : 2;
        }
    }
    locate(argv[0]);
    {
        const char *env = getenv("ZOSASM_UNRESTRICTED");
        if (env && *env)
            g_unrestricted = 1; /* the operator lifted the file name limit */
        else
            g_workspace = xstrdup(g_server_dir); /* the directory of the server binary */
    }
    for (;;) {
        Buf req = {0};
        int c;
        while ((c = fgetc(stdin)) != EOF) {
            if (c == '\n') break;
            if (req.len < MAX_REQUEST + 1) buf_putc(&req, (char)c);
        }
        if (c == EOF && req.len == 0) { free(req.p); break; }
        while (req.len > 0 && (req.p[req.len - 1] == '\n' || req.p[req.len - 1] == '\r')) req.len--;
        if (req.len == 0) { free(req.p); continue; }
        if (req.len > MAX_REQUEST) {
            reply_error(NULL, -32600, "request too large");
            free(req.p);
            /* if line was longer than buffered, drain to newline */
            if (c != EOF && c != '\n') while ((c = fgetc(stdin)) != EOF && c != '\n') {}
            continue;
        }
        {
            const char *err = NULL;
            J *msg = json_parse(req.p ? req.p : "", req.len, &err);
            if (!msg) {
                reply_error(NULL, -32700, err);
            } else if (msg->t != J_OBJ) {
                reply_error(NULL, -32600, "invalid request: not an object");
                jfree(msg);
            } else {
                handle(msg);
                jfree(msg);
            }
        }
        free(req.p);
        if (c == EOF) break;
    }
    return 0;
}
#endif /* ZA_MCP_NO_MAIN */
