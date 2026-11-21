/* fmt.c - the .za source formatter (zafmt).
 *
 * Layout (all columns are options):
 *   name in column 1, operation in column 10, operands 6 columns after the operation
 *   (or one blank after a longer operation), remarks aligned at column 35 (never closer
 *   than one blank to the operands), a continuation line starts at the operand column.
 *   Lines of the macro language (#...) start in column 1 outside blocks; inside a
 *   braced block they are indented by 4 per level (#case/#default one level out, like a C
 *   switch), and so are the model statements there, which have no fixed columns (a label
 *   is written NAME:).
 *
 * The line structure is read the way the assembler and the macro collector read it
 * (reader.c scan_operand_end, macro.c code_scan/code_delta/model_fields): only blanks
 * between fields and at the start and end of lines change. */
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "fmt.h"

void fmt_options_default(FmtOptions *o) {
    o->op_col = 10;
    o->remark_col = 35;
    o->indent = 4;
    o->max_blank = 2;
}

/* ---- scanning (as in macro.c) --------------------------------------------------------- */

static int is_idc(int c) { return za_isalnum(c) || c == '_'; }

static const char *skipb(const char *s) {
    while (is_blank((unsigned char)*s)) s++;
    return s;
}

/* is s[i] the start of an X'..' B'..' C'..' constant? returns the index of its closing quote */
static long const_end(const char *s, size_t i) {
    size_t j;
    int c = za_toupper((unsigned char)s[i]);
    if (!(c == 'X' || c == 'B' || c == 'C') || s[i + 1] != '\'') return -1;
    if (i > 0 && is_idc((unsigned char)s[i - 1])) return -1;
    for (j = i + 2; s[j]; j++) {
        if (s[j] == '\'') {
            if (s[j + 1] == '\'')
                j++;
            else
                return (long)j;
        }
    }
    return -1;
}

typedef struct {
    size_t end;
    int open;
    int in_string;
    int backslash;
} CodeScan;

static CodeScan code_scan(const char *s) {
    CodeScan r = {0, 0, 0, 0};
    size_t i;
    for (i = 0; s[i]; i++) {
        long ce;
        if (s[i] == '"') {
            size_t j = i + 1;
            while (s[j] && s[j] != '"') {
                if (s[j] == '\\' && s[j + 1]) j++;
                j++;
            }
            if (!s[j]) {
                r.in_string = 1;
                i = j - 1;
                break;
            }
            i = j;
        } else if ((ce = const_end(s, i)) >= 0) {
            i = (size_t)ce;
        } else if (s[i] == '/' && s[i + 1] == '/') {
            break;
        } else if (s[i] == '(' || s[i] == '[') {
            r.open++;
        } else if (s[i] == ')' || s[i] == ']') {
            r.open--;
        }
    }
    r.end = i;
    while (r.end > 0 && is_blank((unsigned char)s[r.end - 1])) r.end--;
    r.backslash = r.end > 0 && s[r.end - 1] == '\\';
    return r;
}

/* the text with the inside of every &{...} replaced by 'x' (blanks and quotes there are not fields) */
static char *mask_braces(const char *s) {
    char *m = xstrdup(s);
    size_t i;
    for (i = 0; s[i]; i++) {
        if (s[i] == '&' && s[i + 1] == '{') {
            size_t j = i + 2;
            int depth = 1;
            while (s[j]) {
                if (s[j] == '"') {
                    size_t k = j + 1;
                    while (s[k] && s[k] != '"') {
                        if (s[k] == '\\' && s[k + 1]) k++;
                        k++;
                    }
                    if (!s[k]) break;
                    j = k;
                } else if (s[j] == '{') {
                    depth++;
                } else if (s[j] == '}' && --depth == 0) {
                    break;
                }
                j++;
            }
            if (s[j] == '}') {
                size_t k;
                for (k = i + 2; k < j; k++) m[k] = 'x';
                i = j;
            }
        } else if (s[i] == '&' && s[i + 1] == '&') {
            i++;
        }
    }
    return m;
}

/* ---- state ------------------------------------------------------------------------------ */

typedef struct {
    const FmtOptions *o;
    Vec(char *) L;
    size_t i;
    Buf out;
    int blanks;
    Vec(char *) gh, gr; /* pending statement lines with a remark: head and remark */
    Vec(char) stk; /* open blocks: 1 = a switch */
    Vec(char *) B; /* the lines with their block comments blanked */
    Vec(char) T;   /* 1 = the line touches a block comment */
} F;

static size_t ulen(const char *s, size_t n) {
    size_t i, c = 0;
    for (i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

static char *rtrim_dup(const char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    return xstrndup(s, n);
}

static int is_blank_line(const char *s) { return *skipb(s) == 0; }

static void pad_to(Buf *b, size_t col, int min_one) {
    size_t w = ulen(b->p, b->len);
    if (min_one && w >= col) {
        buf_putc(b, ' ');
        return;
    }
    while (w < col) {
        buf_putc(b, ' ');
        w++;
    }
}

static void flush_group(F *f) {
    size_t k, rc = (size_t)(f->o->remark_col - 1), over = rc;
    /* lines that reach the remark column are aligned with each other, the others use the column */
    for (k = 0; k < f->gh.n; k++) {
        size_t w = ulen(f->gh.v[k], strlen(f->gh.v[k])) + 1;
        if (w >= rc && w > over) over = w;
    }
    for (k = 0; k < f->gh.n; k++) {
        Buf b = {0};
        buf_puts(&b, f->gh.v[k]);
        pad_to(&b, ulen(f->gh.v[k], strlen(f->gh.v[k])) + 1 >= rc ? over : rc, 1);
        buf_puts(&b, f->gr.v[k]);
        buf_puts(&f->out, buf_cstr(&b));
        buf_putc(&f->out, '\n');
    }
    f->gh.n = f->gr.n = 0;
}

static void emit_blanks(F *f) {
    int k, n = f->blanks < f->o->max_blank ? f->blanks : f->o->max_blank;
    for (k = 0; k < n; k++) buf_putc(&f->out, '\n');
    f->blanks = 0;
}

static void put_line(F *f, const char *s) {
    if (f->gh.n) flush_group(f);
    if (!*s) {
        f->blanks++;
        return;
    }
    emit_blanks(f);
    buf_puts(&f->out, s);
    buf_putc(&f->out, '\n');
}

/* a statement line with a remark: the remarks of consecutive such lines are aligned together */
static void put_stmt(F *f, const char *head, const char *rem) {
    if (!*rem) {
        put_line(f, head);
        return;
    }
    if (!f->gh.n) emit_blanks(f);
    vec_push(&f->gh, xstrdup(head));
    vec_push(&f->gr, xstrdup(rem));
}

/* ---- statements ------------------------------------------------------------------------- */

typedef struct {
    char *name, *op, *opd, *rem;
} Fld;

static char *sub(const char *s, size_t a, size_t b) { return xstrndup(s + a, b - a); }

/* Split a statement line (or, with cont, a continuation line). In a block the first token is
 * the operation unless it ends in a colon (macro.c model_fields). 0 when it cannot be split. */
static size_t vcol(const char *s, size_t n) {
    size_t i, c = 0;
    for (i = 0; i < n; i++) {
        if (s[i] == '\t')
            c = (c / 8 + 1) * 8;
        else if (((unsigned char)s[i] & 0xC0) != 0x80)
            c++;
    }
    return c;
}

static int split_stmt(const char *line, int block, int cont, int rc, Fld *f) {
    char *m = block ? mask_braces(line) : xstrdup(line);
    size_t p = 0, q, end;
    f->name = f->op = f->opd = f->rem = "";
    if (!cont) {
        if (block) {
            while (is_blank((unsigned char)m[p])) p++;
            q = p;
            while (m[q] && !is_blank((unsigned char)m[q])) q++;
            if (q - p >= 2 && m[q - 1] == ':') {
                f->name = sub(line, p, q);
                p = q;
            }
        } else if (!is_blank((unsigned char)line[0])) {
            q = 0;
            if (line[0] == '"') {
                long j = close_dquote(line, 0);
                if (j >= 0) q = (size_t)j + 1;
            }
            while (line[q] && !is_blank((unsigned char)line[q])) q++;
            f->name = sub(line, 0, q);
            p = q;
        }
        while (is_blank((unsigned char)m[p])) p++;
        q = p;
        while (m[q] && !is_blank((unsigned char)m[q])) q++;
        if (q == p) return block && f->name[0]; /* a label alone */
        f->op = sub(line, p, q);
        p = q;
    }
    while (is_blank((unsigned char)m[p])) p++;
    if (m[p]) {
        if (!scan_operand_end(m, p, &end)) return 0;
        f->opd = sub(line, p, end);
        /* text that starts in the remark column is a remark, also after an operation that takes no operands */
        if (!cont && !block && vcol(line, p) >= (size_t)rc && f->opd[strlen(f->opd) - 1] != ',') {
            f->rem = rtrim_dup(line + p);
            f->opd = "";
            return 1;
        }
        p = end;
        while (is_blank((unsigned char)m[p])) p++;
        f->rem = rtrim_dup(line + p);
    }
    return 1;
}

static char *layout(const FmtOptions *o, int indent, int block, const Fld *f, int cont, size_t opd_col,
                    size_t *opd_col_out) {
    Buf b = {0};
    if (cont) {
        pad_to(&b, opd_col, 0);
    } else {
        size_t opstart, k;
        for (k = 0; k < (size_t)indent; k++) buf_putc(&b, ' ');
        if (f->name[0]) buf_puts(&b, f->name);
        if (block) {
            if (f->name[0] && f->op[0]) buf_putc(&b, ' ');
        } else {
            pad_to(&b, (size_t)(o->op_col - 1), f->name[0] != 0);
        }
        opstart = ulen(b.p, b.len);
        buf_puts(&b, f->op);
        if (f->opd[0]) {
            size_t ol = ulen(f->op, strlen(f->op));
            opd_col = opstart + (ol + 1 > 6 ? ol + 1 : 6);
            if (!block) {
                opd_col = (size_t)(o->op_col - 1 + 6);
                if (opd_col < opstart + ol + 1) opd_col = opstart + ol + 1;
            }
            pad_to(&b, opd_col, 0);
        }
    }
    if (f->opd[0]) buf_puts(&b, f->opd);
    if (opd_col_out) *opd_col_out = opd_col;
    return xstrdup(buf_cstr(&b));
}

static void do_stmt(F *f, int block) {
    char *line = f->L.v[f->i];
    Fld fl;
    size_t opd_col = 0;
    int indent = block ? (int)f->stk.n * f->o->indent : 0;
    if (!split_stmt(line, block, 0, f->o->remark_col - 1, &fl)) {
        put_line(f, rtrim_dup(line));
        f->i++;
        return;
    }
    put_stmt(f, layout(f->o, indent, block, &fl, 0, 0, &opd_col), fl.rem);
    f->i++;
    while (fl.opd[0] && fl.opd[strlen(fl.opd) - 1] == ',' && f->i < f->L.n) {
        Fld c;
        char *next = f->L.v[f->i];
        if (is_blank_line(next) || f->T.v[f->i] || !split_stmt(next, block, 1, 0, &c) || !c.opd[0]) break;
        put_stmt(f, layout(f->o, indent, block, &c, 1, opd_col, NULL), c.rem);
        f->i++;
        fl = c;
    }
    if (!block && za_streqi(fl.op, "REPRO") && f->i < f->L.n) { /* the record that follows is data */
        char *d = f->L.v[f->i++];
        if (!*d) { /* an empty data record must not count as a blank line */
            f->blanks = 0;
            buf_putc(&f->out, '\n');
        } else {
            put_line(f, d);
        }
    }
}

/* ---- macro language lines ----------------------------------------------------------------- */

static int word_is(const char *p, const char *w) {
    size_t n = strlen(w);
    return !strncmp(p, w, n) && !is_idc((unsigned char)p[n]);
}

static int is_directive_word(const char *p) {
    static const char *const dirs[] = {"define", "elif",    "else",   "endif", "error",  "ifdef",
                                       "ifndef", "include", "pragma", "undef", "warning"};
    size_t k;
    for (k = 0; k < sizeof dirs / sizeof dirs[0]; k++)
        if (word_is(p, dirs[k])) return 1;
    return 0;
}

static void shift_line(Buf *b, const char *s, int delta) {
    if (is_blank_line(s)) return;
    if (delta >= 0) {
        int k;
        for (k = 0; k < delta; k++) buf_putc(b, ' ');
        buf_puts(b, s);
    } else {
        int k = 0;
        while (k < -delta && is_blank((unsigned char)*s)) {
            s++;
            k++;
        }
        buf_puts(b, s);
    }
}

static void do_hash(F *f) {
    char *line = f->L.v[f->i];
    const char *p = skipb(line), *q, *kw;
    int oldind = (int)(p - line), newind, lead, trail, d, depth0 = f->stk.n == 0;
    char *acc = xstrdup(p);
    Vec(char *) conts = {0};
    CodeScan sc;
    size_t k = f->i + 1;
    for (;;) {
        sc = code_scan(skipb(acc + 1));
        if ((sc.backslash || sc.open > 0 || sc.in_string) && k < f->L.n) {
            acc = xasprintf("%s %s", acc, skipb(f->L.v[k]));
            vec_push(&conts, f->L.v[k]);
            k++;
        } else {
            break;
        }
    }
    q = skipb(acc + 1);
    lead = *q == '}';
    trail = sc.end > 0 && q[sc.end - 1] == '{';
    kw = skipb(q + lead);
    if (depth0) { /* outside a block only a line that opens one counts; #else { and the like are directives */
        lead = 0;
        if (is_directive_word(kw)) trail = 0;
    }
    if (lead && f->stk.n > 0) f->stk.n--;
    d = (int)f->stk.n;
    if ((word_is(kw, "case") || word_is(kw, "default")) && f->stk.n > 0 && f->stk.v[f->stk.n - 1]) d--;
    if (d < 0) d = 0;
    newind = depth0 ? 0 : d * f->o->indent;
    {
        Buf b = {0};
        size_t j;
        int delta = newind - oldind;
        char *t = rtrim_dup(p);
        int kk;
        for (kk = 0; kk < newind; kk++) buf_putc(&b, ' ');
        buf_puts(&b, t);
        put_line(f, buf_cstr(&b));
        for (j = 0; j < conts.n; j++) {
            Buf c = {0};
            char *r = rtrim_dup(conts.v[j]);
            shift_line(&c, r, delta);
            put_line(f, buf_cstr(&c));
        }
    }
    if (trail) vec_push(&f->stk, (char)word_is(kw, "switch"));
    f->i = k;
}

/* ---- anchored lines ----------------------------------------------------------------------- */

/* does the operand field of the statement text (name field first, unless cont) end in a comma? */
static int ends_in_comma(const char *s, int cont) {
    size_t p = 0, end;
    if (!cont) {
        while (s[p] && !is_blank((unsigned char)s[p])) p++; /* name */
        while (is_blank((unsigned char)s[p])) p++;
        while (s[p] && !is_blank((unsigned char)s[p])) p++; /* operation */
    }
    while (is_blank((unsigned char)s[p])) p++;
    if (!s[p] || !scan_operand_end(s, p, &end)) return 0;
    return end > p && s[end - 1] == ',';
}

/* A line that starts with the column anchor '^' ("    ^LB1  DC  AD(0)") or touches a block
 * comment is kept as written, with its continuation lines: the author chose its layout.
 * stmt is the text of the statement on the first line (without comments). */
static void do_verbatim(F *f, const char *stmt) {
    int more = ends_in_comma(stmt, 0);
    put_line(f, rtrim_dup(f->L.v[f->i]));
    f->i++;
    while (more && f->i < f->L.n && !is_blank_line(f->B.v[f->i]) && !f->T.v[f->i]) {
        more = ends_in_comma(f->B.v[f->i], 1);
        put_line(f, rtrim_dup(f->L.v[f->i]));
        f->i++;
    }
}

/* ---- driver ------------------------------------------------------------------------------- */

char *za_format(const char *text, size_t len, const FmtOptions *o) {
    F f;
    size_t i = 0;
    memset(&f, 0, sizeof f);
    f.o = o;
    while (i < len) {
        size_t s = i, e;
        while (i < len && text[i] != '\n') i++;
        e = i;
        if (e > s && text[e - 1] == '\r') e--;
        vec_push(&f.L, xstrndup(text + s, e - s));
        if (i < len) i++;
    }
    {
        int in = 0;
        size_t k;
        for (k = 0; k < f.L.n; k++) {
            char *b = xstrdup(f.L.v[k]);
            vec_push(&f.T, (char)bc_line(b, &in));
            vec_push(&f.B, b);
        }
    }
    while (f.i < f.L.n) {
        char *line = f.L.v[f.i];
        const char *p = skipb(line);
        int block = f.stk.n > 0;
        if (f.T.v[f.i]) {
            do_verbatim(&f, caret_rest(f.B.v[f.i]));
        } else if (*p == 0) {
            put_line(&f, "");
            f.i++;
        } else if (!block && (line[0] == '*' || line[0] == ';' || (line[0] == '/' && line[1] == '/'))) {
            put_line(&f, rtrim_dup(line));
            f.i++;
        } else if (block && *p == '*') {
            put_line(&f, rtrim_dup(line));
            f.i++;
        } else if (block && p[0] == '/' && p[1] == '/') {
            put_line(&f, xasprintf("%*s%s", (int)f.stk.n * o->indent, "", rtrim_dup(p)));
            f.i++;
        } else if (*p == '^') {
            do_verbatim(&f, caret_rest(line));
        } else if (*p == '#' && (block || line[0] == '#')) {
            do_hash(&f);
        } else {
            do_stmt(&f, block);
        }
    }
    flush_group(&f);
    return xstrdup(buf_cstr(&f.out));
}
