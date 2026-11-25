/* convert.c - HLASM source to zosasm source.
 *
 * Steps: (1) cut the card columns and join continuation lines into
 * statements, (2) split the fields and fold identifiers to upper case,
 * (3) find the EQU names that are registers and how they are used,
 * (4) write the statements again with registers as %r1 (strict) and the
 * zosasm layout. What cannot be converted (macros, conditional assembly) is
 * left as it is or commented out, and reported. */
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "convert.h"
#include "mconv.h"
#include "isa.h"
#include "regops.h"

void conv_options_default(ConvOptions *o) {
    o->begin = 1;
    o->end = 71;
    o->cont = 16;
    o->strict = 1;
    o->width = 100;
    o->inc_ext = ".inc";
    o->flat = 0;
    o->keep_goto = 0;
    o->comment_macros = 0;
    o->member = 0;
    o->copy_dir = NULL;
}

/* ---- data ----------------------------------------------------------------- */

enum { K_BLANK, K_COMMENT, K_STMT };

typedef struct {
    int kind;
    int line;   /* first physical line (1-based) */
    char *text; /* K_COMMENT: the whole line */
    char *name, *op, *operands, *remark; /* K_STMT, identifiers folded to upper case */
    int alias;  /* EQU of an already known register name */
} LS;

typedef struct Sym {
    char *name;
    int val;            /* the number it equates */
    struct Sym *parent; /* alias: the symbol it equates */
    int occ;            /* occurrences in operands (not counting alias EQUs) */
    int slot[5];        /* uses as a whole register operand, per class */
    int pref;           /* class suggested by the name (R5 = 5), or -1 */
    /* group results, valid at the root */
    int gslot[5];
    int gbad;  /* a member is used other than as a whole register operand */
    int gpref; /* class suggested by a member's name */
    int gclass; /* the class the EQU is written with; -1: it stays a number */
} Sym;

typedef struct {
    int line, seq;
    char *msg;
} Warn;

typedef struct {
    size_t lo, hi; /* line indexes (inclusive) consumed by a MACRO..MEND */
} MacRange;

typedef struct {
    ConvOptions o;
    Vec(char *) lines;
    Vec(LS) ls;
    Vec(Warn) warns;
    StrMap syms; /* name -> Sym * */
    Vec(Sym *) symlist;
    StrMap dirs;   /* known directives */
    StrMap macros; /* macros defined in this file: 1 commented out, 2 converted */
    int nconv;     /* macro definitions converted so far */
    StrMap noted;  /* keys already warned about */
    MHStmtVec open; /* open code from the first conditional assembly statement on */
    size_t open_first; /* its first line index */
    Vec(MacRange) open_macros; /* MACRO..MEND ranges inside it, already emitted */
    int wrapping;
    Buf out;
} Conv;

static const char CLS[] = "rfacv";

static void warnf(Conv *c, int line, const char *fmt, ...) {
    Warn w;
    va_list ap;
    va_start(ap, fmt);
    w.msg = xvasprintf(fmt, ap);
    va_end(ap);
    w.line = line;
    w.seq = (int)c->warns.n;
    vec_push(&c->warns, w);
}

static int once(Conv *c, const char *key) {
    if (strmap_get(&c->noted, key)) return 0;
    strmap_put(&c->noted, key, c);
    return 1;
}

static int is_digits(const char *s) {
    if (!*s) return 0;
    for (; *s; s++)
        if (!za_isdigit((unsigned char)*s)) return 0;
    return 1;
}

static int blank_str(const char *s) {
    for (; *s; s++)
        if (*s != ' ') return 0;
    return 1;
}

static char *rtrim_dup(const char *s, size_t n) {
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    return xstrndup(s, n);
}

/* ---- physical lines ---------------------------------------------------------- */

static void split_lines(Conv *c, const char *text, size_t len) {
    size_t i = 0;
    while (i < len) {
        Buf b = {0};
        int col = 0;
        while (i < len && text[i] != '\n') {
            unsigned char ch = (unsigned char)text[i++];
            if (ch == '\r' && (i >= len || text[i] == '\n')) continue;
            if (ch == '\t') {
                do {
                    buf_putc(&b, ' ');
                    col++;
                } while (col % 8);
            } else if (ch == '\f') {
                buf_putc(&b, ' ');
                col++;
            } else {
                buf_putc(&b, ch);
                if ((ch & 0xC0) != 0x80) col++;
            }
        }
        if (i < len) i++; /* the newline */
        vec_push(&c->lines, xstrdup(buf_cstr(&b)));
    }
}

/* ---- case folding ------------------------------------------------------------ */

/* upper case outside quoted strings; the digits of an X string are upper
 * cased too; attribute references (L'SYM) do not start a string */
char *conv_fold(const char *s) {
    Buf b = {0};
    int inq = 0, hex = 0;
    size_t i;
    for (i = 0; s[i]; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (inq) {
            if (ch == '\'') {
                if (s[i + 1] == '\'') {
                    buf_putc(&b, '\'');
                    buf_putc(&b, '\'');
                    i++;
                    continue;
                }
                inq = 0;
                buf_putc(&b, '\'');
                continue;
            }
            buf_putc(&b, hex ? za_toupper(ch) : ch);
            continue;
        }
        if (ch == '\'' && !is_attr_quote(s, i)) {
            size_t j = i;
            while (j > 0 && za_isalnum((unsigned char)s[j - 1])) j--;
            while (j < i && za_isdigit((unsigned char)s[j])) j++;
            hex = j < i && (s[j] == 'x' || s[j] == 'X');
            inq = 1;
            buf_putc(&b, '\'');
            continue;
        }
        buf_putc(&b, za_toupper(ch));
    }
    return xstrdup(buf_cstr(&b));
}

/* ---- fields ------------------------------------------------------------------ */

typedef struct {
    char *name, *op, *operands, *remark;
    int open_quote; /* the text ends inside a quoted string */
    int open_paren; /* the operand field ends inside parentheses */
} Fields;

/* a remark of the generated source must not hold the start of a block comment (a PL/X style
 * remark that opens one would swallow the lines after it): the slash and the star are
 * separated by a blank */
static char *no_block_comment(const char *r) {
    Buf b = {0};
    size_t i;
    for (i = 0; r[i]; i++) {
        buf_putc(&b, r[i]);
        if (r[i] == '/' && r[i + 1] == '*') buf_putc(&b, ' ');
    }
    return xstrdup(buf_cstr(&b));
}

static void split_fields(const char *s, Fields *f) {
    size_t p = 0, q;
    int depth = 0, inq = 0;
    memset(f, 0, sizeof *f);
    if (s[0] && !is_blank((unsigned char)s[0])) {
        while (s[p] && !is_blank((unsigned char)s[p])) p++;
        f->name = xstrndup(s, p);
    } else {
        f->name = xstrdup("");
    }
    while (is_blank((unsigned char)s[p])) p++;
    q = p;
    while (s[q] && !is_blank((unsigned char)s[q])) q++;
    f->op = xstrndup(s + p, q - p);
    p = q;
    while (is_blank((unsigned char)s[p])) p++;
    for (q = p; s[q]; q++) {
        char ch = s[q];
        if (inq) {
            if (ch == '\'') {
                if (s[q + 1] == '\'')
                    q++;
                else
                    inq = 0;
            }
            continue;
        }
        if (ch == '\'') {
            if (!is_attr_quote(s, q)) inq = 1;
        } else if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            depth--;
        } else if ((ch == ' ' || ch == '\t') && depth <= 0) {
            break;
        }
    }
    f->open_quote = inq;
    f->open_paren = depth > 0;
    f->operands = xstrndup(s + p, q - p);
    while (is_blank((unsigned char)s[q])) q++;
    f->remark = no_block_comment(s + q);
}

/* Columns count characters: the byte offset of column col (0-based) of a UTF-8 line */
static size_t u8off(const char *ln, size_t col) {
    size_t i = 0;
    while (ln[i]) {
        if (((unsigned char)ln[i] & 0xC0) != 0x80) {
            if (col == 0) return i;
            col--;
        }
        i++;
    }
    return i;
}

static size_t u8len(const char *s) {
    size_t n = 0, i;
    for (i = 0; s[i]; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

/* the source columns of a line from index from up to the end column */
static char *columns(const Conv *c, const char *ln, int from) {
    size_t e = u8off(ln, (size_t)c->o.end), b;
    if (from < 0) from = 0;
    b = u8off(ln, (size_t)from);
    if (b >= e) return xstrdup("");
    return xstrndup(ln + b, e - b);
}

static int has_cont_mark(const Conv *c, const char *ln) {
    size_t b;
    if (c->o.cont <= 0 || c->o.end >= 80 || u8len(ln) <= (size_t)c->o.end) return 0;
    b = u8off(ln, (size_t)c->o.end);
    return ln[b] != ' ';
}

/* ---- classification of operations ------------------------------------------- */

static const char *const OPERANDLESS[] = {"CSECT", "DSECT", "RSECT", "COM", "LOCTR", "LTORG", "EJECT", "MEND", "MEXIT", "ANOP", NULL};
static const char *const CASTMT[] = {"AIF",  "AGO",  "ANOP", "SETA", "SETB",  "SETC",    "LCLA",  "LCLB", "LCLC",
                                     "GBLA", "GBLB", "GBLC", "ACTR", "MNOTE", "MEXIT",   "AREAD", "AINSERT", "MHELP", NULL};
static const char *const CONV_UNSUPPORTED[] = {"OPSYN", "ADATA", "REPRO", "ALIAS", NULL};

static int in_list(const char *const *l, const char *s) {
    for (; *l; l++)
        if (!strcmp(*l, s)) return 1;
    return 0;
}

static int takes_no_operands(const char *op) {
    size_t n, i;
    const IsaInst *v;
    if (in_list(OPERANDLESS, op)) return 1;
    v = isa_lookup(op, &n);
    if (!v) return 0;
    for (i = 0; i < n; i++) {
        IsaSlots sl;
        isa_slots(&v[i], &sl);
        if (sl.n != 0) return 0;
    }
    return 1;
}

static int is_instruction(const char *op) {
    size_t n;
    return isa_lookup(op, &n) != NULL;
}

static int has_amp(const char *s) {
    for (; *s; s++) {
        if (*s == '&') {
            if (s[1] == '&') {
                s++;
                continue;
            }
            return 1;
        }
    }
    return 0;
}

/* operand forms that zosasm rejects or interprets differently; report them
 * here so the converted source does not fail with an unrelated diagnostic */
static void check_forms(Conv *c, const LS *l) {
    OperandVec ops = {0};
    split_operands(&ops, l->operands);
    if (!strcmp(l->op, "START") && ops.n > 0)
        warnf(c, l->line, "START address is ignored by zosasm (the binder assigns the load address)");
    if (!strcmp(l->op, "XATTR") && strstr(l->operands, "ATTRIBUTES"))
        warnf(c, l->line, "XATTR ATTRIBUTES is not supported by zosasm");
    operands_free(&ops); /* split_operands owns the operand text */
}

static void classify(Conv *c, const LS *l) {
    const char *op = l->op;
    int bad = 1;
    if (!*op) return;
    if (l->name[0] == '.' || l->name[0] == '&' || op[0] == '&') {
        warnf(c, l->line, "conditional assembly (sequence symbol or variable symbol): not converted");
    } else if (in_list(CASTMT, op)) {
        warnf(c, l->line, "%s is conditional assembly: not converted (use #if and #define)", op);
    } else if (in_list(CONV_UNSUPPORTED, op)) {
        warnf(c, l->line, "%s is not supported by zosasm%s", op,
              strcmp(op, "ALIAS") ? "" : " (name the symbol with a quoted symbol \"name\" instead)");
    } else if (strmap_get(&c->macros, op) == (void *)2) {
        bad = 0; /* a call of a macro that was converted */
    } else if (strmap_get(&c->macros, op)) {
        warnf(c, l->line, "call of %s, a macro defined in this file (the definition is commented out)", op);
    } else if (!strcmp(op, "COPY") || !strcmp(op, "ISEQ") || !strcmp(op, "ICTL")) {
        bad = 0; /* handled when writing */
    } else if (!is_instruction(op) && !strmap_get(&c->dirs, op)) {
        warnf(c, l->line, "%s is not an instruction or directive (a macro call?): left as it is", op);
    } else {
        bad = 0;
    }
    if (!bad && has_amp(l->operands)) warnf(c, l->line, "operand with a variable symbol (&): not converted");
}

/* ---- statements -------------------------------------------------------------- */

static void push_comment(Conv *c, int line, char *text) {
    LS l = {0};
    l.kind = K_COMMENT;
    l.line = line;
    l.text = text;
    vec_push(&c->ls, l);
}

static void push_stmt(Conv *c, int line, const char *name, const char *op, const char *operands, const char *remark) {
    LS l = {0};
    l.kind = K_STMT;
    l.line = line;
    l.name = xstrdup(name);
    l.op = xstrdup(op);
    l.operands = xstrdup(operands);
    l.remark = xstrdup(remark);
    vec_push(&c->ls, l);
}

/* a physical line as a comment: the source columns plus the continuation mark */
static char *raw_comment(const Conv *c, const char *ln) {
    size_t n = u8off(ln, (size_t)c->o.end + 1);
    char *r = rtrim_dup(ln, n);
    return xasprintf("* %s", r);
}

/* the statement that starts at physical line *i (joined with its continuation
 * lines); *i is left on its last line */
static char *join_statement(Conv *c, size_t *i) {
    Buf acc = {0};
    const char *ln = c->lines.v[*i];
    size_t pw = (size_t)(c->o.end - (c->o.begin - 1)), last;
    char *piece = columns(c, ln, c->o.begin - 1);
    buf_puts(&acc, piece);
    last = u8len(piece);
    while (has_cont_mark(c, ln)) {
        Fields f;
        char *t;
        int mode; /* 0: the next piece continues the text, 1: after a comma, 2: a remark */
        size_t tl;
        if (*i + 1 >= c->lines.n) {
            warnf(c, (int)*i + 1, "continuation mark on the last line");
            break;
        }
        for (; last < pw; last++) buf_putc(&acc, ' ');
        split_fields(buf_cstr(&acc), &f);
        if (f.open_quote) {
            mode = 0;
        } else {
            t = rtrim_dup(acc.p, acc.len);
            tl = strlen(t);
            split_fields(t, &f);
            if (f.open_paren) {
                mode = 0; /* inside parentheses the text goes on as written */
            } else if (tl > 0 && f.operands[0] && f.operands[strlen(f.operands) - 1] == ',') {
                mode = 1;
                if (*f.remark) t = xasprintf("%s %s %s", f.name, f.op, f.operands); /* the remark is dropped */
            } else if (acc.p[acc.len - 1] != ' ' && !*f.remark) {
                mode = 0;
            } else {
                mode = 2;
            }
            if (mode != 0) {
                acc.len = 0;
                buf_puts(&acc, t);
            }
        }
        (*i)++;
        ln = c->lines.v[*i];
        { /* defense: a continuation column outside begin..end cannot happen
           * after the ICTL check; clamp so the padding below stays bounded */
            int cc = c->o.cont;
            if (cc < c->o.begin || cc > c->o.end) cc = c->o.begin;
            piece = columns(c, ln, cc - 1);
            pw = (size_t)(c->o.end - (cc - 1));
        }
        last = u8len(piece);
        if (mode == 1) {
            char *p = piece;
            while (*p == ' ') p++;
            buf_puts(&acc, p);
        } else if (mode == 2) {
            char *p = piece;
            while (*p == ' ') p++;
            p = rtrim_dup(p, strlen(p));
            if (*p) {
                buf_putc(&acc, ' ');
                buf_puts(&acc, p);
            }
        } else {
            buf_puts(&acc, piece);
        }
        if (mode != 0) last = pw; /* nothing to pad */
    }
    return xstrdup(buf_cstr(&acc));
}

static void yregs(Conv *c, int line) {
    int k;
    for (k = 0; k < 16; k++) {
        char nm[8], num[8];
        sprintf(nm, "R%d", k);
        sprintf(num, "%d", k);
        push_stmt(c, line, nm, "EQU", num, "");
    }
}

static void macro_warn(void *user, int line, const char *msg) { warnf(user, line, "%s", msg); }

static void collect_stmts(Conv *c, size_t *pi, MHStmtVec *st, int is_main, int depth, int copy_line, int *ended);

/* HLASM continues a comment line (* or .* in column 1) over the following records while its
 * continuation column is not blank; those records are comment text whatever they contain.
 * Returns the text of the next such record (and moves *i onto it), or NULL. */
static char *comment_cont_next(Conv *c, size_t *i) {
    if (!has_cont_mark(c, c->lines.v[*i]) || *i + 1 >= c->lines.n) return NULL;
    (*i)++;
    {
        char *t = columns(c, c->lines.v[*i], c->o.begin - 1);
        return rtrim_dup(t, strlen(t));
    }
}


/* COPY member inside a macro: the statements of DIR/member.mac are read at this point (the
 * assembler does the same when it reads the macro definition). 1 when the member was found. */
static int copy_member(Conv *c, const char *name, MHStmtVec *st, int depth, int line) {
    static const char *const ext[] = {".mac", ".MAC", ""};
    char *base = str_trim(name), *text = NULL, *up, *lo;
    size_t len = 0, k;
    Conv sub;
    int ended = 0;
    if (!c->o.copy_dir || !*base || depth > 8) return 0;
    for (k = 0; base[k]; k++)
        if (base[k] == '/' || base[k] == '\\' || base[k] == '\'' || base[k] == ' ') return 0;
    up = za_strupper(base);
    lo = za_strlower(xstrdup(base));
    for (k = 0; k < 3 * 3 && !text; k++) {
        const char *nm = k / 3 == 0 ? up : k / 3 == 1 ? lo : base;
        text = read_file(xasprintf("%s/%s%s", c->o.copy_dir, nm, ext[k % 3]), &len);
    }
    if (!text) return 0;
    sub = *c;
    memset(&sub.lines, 0, sizeof sub.lines);
    memset(&sub.warns, 0, sizeof sub.warns);
    split_lines(&sub, text, len);
    k = 0;
    collect_stmts(&sub, &k, st, 0, depth, line, &ended);
    for (k = 0; k < sub.warns.n; k++) {
        Warn w = sub.warns.v[k];
        w.line = line;
        vec_push(&c->warns, w);
    }
    return 1;
}

/* the logical statements from line *pi on; the main text stops after MEND */
static void collect_stmts(Conv *c, size_t *pi, MHStmtVec *st, int is_main, int depth, int copy_line, int *ended) {
    size_t i;
    int nest = 0; /* MACRO statements inside the macro (their MEND does not end it) */
    for (i = *pi; i < c->lines.n && !(is_main && *ended); i++) {
        char *t = columns(c, c->lines.v[i], c->o.begin - 1);
        MHStmt s;
        memset(&s, 0, sizeof s);
        s.line = copy_line ? copy_line : (int)i + 1;
        if (blank_str(t)) continue;
        if (t[0] == '*' || (t[0] == '.' && t[1] == '*')) {
            char *more;
            s.kind = t[0] == '*' ? MH_ASMCOMMENT : MH_MACCOMMENT;
            s.text = rtrim_dup(t + (t[0] == '*' ? 1 : 2), strlen(t + (t[0] == '*' ? 1 : 2)));
            vec_push(st, s);
            while ((more = comment_cont_next(c, &i)) != NULL) {
                MHStmt k = s;
                k.line = copy_line ? copy_line : (int)i + 1;
                k.text = more;
                vec_push(st, k);
            }
            continue;
        } else {
            Fields f;
            char *joined = join_statement(c, &i);
            split_fields(joined, &f);
            s.kind = MH_STMT;
            s.name = f.name;
            s.op = f.op;
            s.operands = f.operands;
            s.remark = f.remark;
            if (is_main && za_streqi(f.op, "MACRO") && !f.name[0] && !f.operands[0]) nest++;
            if (is_main && za_streqi(f.op, "MEND") && nest-- == 0) *ended = 1;
            if (za_streqi(f.op, "COPY") && c->o.copy_dir && copy_member(c, f.operands, st, depth + 1, s.line)) {
                if (f.name[0]) { /* keep the sequence symbol of the COPY statement */
                    MHStmt an;
                    memset(&an, 0, sizeof an);
                    an.line = s.line;
                    an.kind = MH_STMT;
                    an.name = f.name;
                    an.op = xstrdup("ANOP");
                    an.operands = xstrdup("");
                    an.remark = xstrdup("");
                    vec_push(st, an);
                }
                continue;
            }
        }
        vec_push(st, s);
    }
    *pi = i;
}

/* A macro definition: the statements from the line after MACRO through MEND, converted to
 * a zosasm macro; when that is not possible, the lines become comments. *pi is left on MEND. */
static void macro_def(Conv *c, size_t macro_first, size_t *pi) {
    MHStmtVec st = {0};
    MConvOptions mo;
    size_t i, j, start = *pi;
    int ended = 0;
    char *res, *why = NULL;
    memset(&mo, 0, sizeof mo);
    mo.flat = c->o.flat;
    mo.keep_goto = c->o.keep_goto;
    mo.warn = macro_warn;
    mo.user = c;
    i = start + 1;
    collect_stmts(c, &i, &st, 1, 0, 0, &ended);
    i--; /* the last line consumed */
    if (!ended) {
        why = xstrdup("MEND is missing");
        res = NULL;
        i = c->lines.n - 1;
        if (c->o.member) {
            /* nothing after the last MEND of a library member is read by HLASM: a MACRO that is
             * not closed after a converted macro is the text of a PL/X or PL/I trailer */
            const char *first = c->lines.v[0];
            while (*first == ' ') first++;
            if (c->nconv > 0) {
                for (j = macro_first; j <= i && j < c->lines.n; j++) push_comment(c, (int)j + 1, raw_comment(c, c->lines.v[j]));
                warnf(c, (int)macro_first + 1, "text after the last MEND is ignored (a MACRO without MEND follows a converted macro)");
                *pi = i;
                return;
            }
            if (*first == '%' || strstr(c->lines.v[macro_first], "/*") || (macro_first + 1 < c->lines.n && strstr(c->lines.v[macro_first + 1], "/*"))) {
                push_comment(c, (int)macro_first + 1, xstrdup("* ---- not an assembler macro (PL/X source), left as comments ----"));
                for (j = macro_first; j <= i && j < c->lines.n; j++) push_comment(c, (int)j + 1, raw_comment(c, c->lines.v[j]));
                warnf(c, (int)macro_first + 1, "not an assembler macro: PL/X macro source, skipped");
                *pi = i;
                return;
            }
        }
    } else {
        res = c->o.comment_macros ? NULL : mconv_macro(&st, &mo, &why);
        if (c->o.comment_macros) why = xstrdup("-comment-macros");
    }
    if (res) {
        char *p = res;
        while (*p) {
            char *e = strchr(p, '\n');
            push_comment(c, (int)macro_first + 1, xstrndup(p, (size_t)(e - p)));
            p = e + 1;
        }
        for (j = 0; j < st.n; j++)
            if (st.v[j].kind == MH_STMT) {
                char *nm = za_strupper(st.v[j].op);
                strmap_put(&c->macros, nm, (void *)2);
                c->nconv++;
                break;
            }
    } else {
        push_comment(c, (int)macro_first + 1,
                     xstrdup("* ---- macro definition, not converted (see the warning) ----"));
        for (j = macro_first; j <= i && j < c->lines.n; j++) push_comment(c, (int)j + 1, raw_comment(c, c->lines.v[j]));
        for (j = 0; j < st.n; j++)
            if (st.v[j].kind == MH_STMT) {
                strmap_put(&c->macros, za_strupper(st.v[j].op), (void *)1);
                break;
            }
        warnf(c, (int)macro_first + 1, "macro definition commented out: %s", why ? why : "not converted");
    }
    *pi = i;
}

/* Open code conditional assembly: from the first statement that uses it to the end of the
 * source, the statements become the body of a macro OPENCODE (converted like any macro) that
 * is called in place. IBM HLASM substitutes variables in every statement of open code; the
 * macro language of zosasm does so only inside macros. */
static int ca_start(const char *fp, const Fields *f) {
    return in_list(CASTMT, fp) || f->name[0] == '.' || f->name[0] == '&' || f->op[0] == '&' ||
           has_amp(f->name) || has_amp(f->operands);
}

static void open_push(Conv *c, int line, int kind, const char *name, const char *op, const char *operands,
                      const char *remark, char *text) {
    MHStmt s;
    memset(&s, 0, sizeof s);
    s.line = line;
    s.kind = kind;
    s.name = (char *)name;
    s.op = (char *)op;
    s.operands = (char *)operands;
    s.remark = (char *)remark;
    s.text = text;
    vec_push(&c->open, s);
}

static void open_finish(Conv *c) {
    MConvOptions mo;
    MHStmt p, e;
    char *res, *why = NULL;
    size_t j;
    int line = c->open.n ? c->open.v[0].line : 1;
    memset(&mo, 0, sizeof mo);
    mo.flat = c->o.flat;
    mo.keep_goto = c->o.keep_goto;
    mo.warn = macro_warn;
    mo.user = c;
    memset(&p, 0, sizeof p);
    p.line = line;
    p.kind = MH_STMT;
    p.name = xstrdup("");
    p.op = xstrdup("OPENCODE");
    p.operands = xstrdup("");
    p.remark = xstrdup("");
    memset(&e, 0, sizeof e);
    e.line = line;
    e.kind = MH_STMT;
    e.name = xstrdup("");
    e.op = xstrdup("MEND");
    e.operands = xstrdup("");
    e.remark = xstrdup("");
    {
        MHStmtVec all = {0};
        vec_push(&all, p);
        for (j = 0; j < c->open.n; j++) vec_push(&all, c->open.v[j]);
        vec_push(&all, e);
        res = mconv_macro(&all, &mo, &why);
        free(all.v);
    }
    if (res) {
        char *q = res;
        push_comment(c, line, xstrdup("* ---- open code conditional assembly, converted to a macro called below ----"));
        while (*q) {
            char *nl = strchr(q, '\n');
            size_t len = nl ? (size_t)(nl - q) : strlen(q);
            push_comment(c, line, xstrndup(q, len));
            if (!nl) break;
            q = nl + 1;
        }
        push_comment(c, line, xstrdup("         OPENCODE"));
        free(res);
    } else {
        push_comment(c, line, xstrdup("* ---- open code conditional assembly, not converted (see the warning) ----"));
        for (j = c->open_first; j < c->lines.n; j++) {
            size_t r;
            int in_macro = 0;
            for (r = 0; r < c->open_macros.n; r++)
                if (j >= c->open_macros.v[r].lo && j <= c->open_macros.v[r].hi) in_macro = 1;
            if (in_macro) continue; /* the macro was already emitted above */
            push_comment(c, (int)j + 1, raw_comment(c, c->lines.v[j]));
        }
        warnf(c, line, "open code conditional assembly commented out: %s", why ? why : "not converted");
    }
    free(why);
    free(p.name); free(p.op); free(p.operands); free(p.remark);
    free(e.name); free(e.op); free(e.operands); free(e.remark);
}

static void parse(Conv *c) {
    size_t i;
    int seen = 0;
    for (i = 0; i < c->lines.n; i++) {
        const char *ln = c->lines.v[i];
        char *t = columns(c, ln, c->o.begin - 1);
        int line = (int)i + 1;
        size_t first;
        char *joined, *fo, *fn, *fp, *remark;
        Fields f;
        if (blank_str(t)) {
            LS l = {0};
            if (c->wrapping) continue;
            l.kind = K_BLANK;
            l.line = line;
            vec_push(&c->ls, l);
            continue;
        }
        if (c->wrapping && (t[0] == '*' || (t[0] == '.' && t[1] == '*'))) {
            char *more;
            int kind = t[0] == '*' ? MH_ASMCOMMENT : MH_MACCOMMENT;
            open_push(c, line, kind, NULL, NULL, NULL, NULL, rtrim_dup(t + (t[0] == '*' ? 1 : 2), strlen(t + (t[0] == '*' ? 1 : 2))));
            while ((more = comment_cont_next(c, &i)) != NULL) open_push(c, (int)i + 1, kind, NULL, NULL, NULL, NULL, more);
            continue;
        }
        if (t[0] == '*' || (t[0] == '.' && t[1] == '*')) {
            const char *body = t[0] == '*' ? t : t + 1;
            if (za_strncmpi(t, "*PROCESS", 8) == 0)
                warnf(c, line, "*PROCESS options are not converted: give the equivalent options to zosasm");
            if (c->o.member)
                push_comment(c, line, xasprintf("*%s", rtrim_dup(t[0] == '*' ? t + 1 : t + 2, strlen(t) - (t[0] == '*' ? 1 : 2))));
            else
                push_comment(c, line, rtrim_dup(body, strlen(body)));
            {
                char *more;
                while ((more = comment_cont_next(c, &i)) != NULL)
                    push_comment(c, (int)i + 1, xasprintf("*%s", more));
            }
            continue;
        }
        first = i;
        joined = join_statement(c, &i);
        split_fields(joined, &f);
        fo = conv_fold(f.operands);
        fn = conv_fold(f.name);
        fp = conv_fold(f.op);
        remark = f.remark;
        if (!seen && !strcmp(fp, "ICTL")) {
            int b = 1, e = 71, kk = 16, nn = sscanf(fo, "%d,%d,%d", &b, &e, &kk);
            if (nn >= 2 && b >= 1 && b <= 40 && e >= 41 && e <= 80 && e >= b + 5 &&
                (nn == 3 ? (kk >= 2 && kk <= 40 && kk > b && kk < e) : (b < 16 && e > 16))) {
                c->o.begin = b;
                c->o.end = e;
                c->o.cont = e == 80 ? 0 : (nn == 3 ? kk : 16);
            } else {
                warnf(c, line, "ICTL operands not understood: columns unchanged");
            }
            seen = 1;
            continue;
        }
        seen = 1;
        if (!strcmp(fp, "MACRO") && !*fn) {
            macro_def(c, first, &i);
            if (c->wrapping) {
                /* emitted above; the open_finish failure path must not repeat it */
                MacRange r;
                r.lo = first;
                r.hi = i;
                vec_push(&c->open_macros, r);
            }
            continue;
        }
        if (c->o.member) continue; /* a macro library member: only the definitions */
        if (!c->wrapping && ca_start(fp, &f)) {
            c->wrapping = 1;
            c->open_first = first;
        }
        if (c->wrapping) {
            if (!strcmp(fp, "YREGS") && !*fo) {
                int k;
                for (k = 0; k < 16; k++)
                    open_push(c, line, MH_STMT, xasprintf("R%d", k), "EQU", xasprintf("%d", k), "", NULL);
                continue;
            }
            open_push(c, line, MH_STMT, f.name, f.op, f.operands, f.remark, NULL);
            continue;
        }
        if (!strcmp(fp, "YREGS") && !*fo) {
            yregs(c, line);
            continue;
        }
        if (*fo && takes_no_operands(fp)) {
            /* the text is a remark; zosasm would read it as operands */
            push_comment(c, line, xasprintf("* %s%s%s", f.operands, *remark ? " " : "", remark));
            fo = xstrdup("");
            remark = xstrdup("");
        }
        push_stmt(c, line, fn, fp, fo, remark);
        classify(c, &c->ls.v[c->ls.n - 1]);
        check_forms(c, &c->ls.v[c->ls.n - 1]);
    }
    if (c->wrapping) open_finish(c);
}

/* ---- registers ----------------------------------------------------------------- */

static int cls_of(unsigned flags) { return (int)(strchr(CLS, regops_letter(flags)) - CLS); }

static Sym *root(Sym *s) {
    while (s->parent) s = s->parent;
    return s;
}

/* "R5", "FPR2", ... : the class and number a register name suggests */
static int reg_pattern(const char *n, int *cls, int *num) {
    static const struct {
        const char *pre;
        int cls;
    } P[] = {{"FPR", 1}, {"GR", 0}, {"FR", 1}, {"AR", 2}, {"CR", 3}, {"VR", 4}, {"R", 0}, {"F", 1}, {"V", 4}};
    size_t i;
    for (i = 0; i < sizeof P / sizeof P[0]; i++) {
        size_t l = strlen(P[i].pre);
        if (!strncmp(n, P[i].pre, l) && is_digits(n + l) && strlen(n + l) <= 2) {
            *cls = P[i].cls;
            *num = atoi(n + l);
            return 1;
        }
    }
    return 0;
}

static Sym *sym_new(Conv *c, const char *name, int val, Sym *parent) {
    Sym *s = xcalloc(sizeof *s);
    int cl, n;
    s->name = xstrdup(name);
    s->val = val;
    s->parent = parent;
    s->pref = -1;
    s->gpref = -1;
    s->gclass = -1;
    if (reg_pattern(name, &cl, &n) && n == val) s->pref = cl;
    strmap_put(&c->syms, name, s);
    vec_push(&c->symlist, s);
    return s;
}

/* EQU statements that equate a number or an already known register name */
static void collect_equs(Conv *c) {
    size_t i;
    for (i = 0; i < c->ls.n; i++) {
        LS *l = &c->ls.v[i];
        OperandVec ops = {0};
        char *t;
        if (l->kind != K_STMT || strcmp(l->op, "EQU") || !*l->name || strmap_get(&c->syms, l->name)) continue;
        split_operands(&ops, l->operands);
        if (ops.n != 1) continue;
        t = str_trim(ops.v[0].text);
        if (is_digits(t) && strlen(t) < 6) {
            sym_new(c, l->name, atoi(t), NULL);
        } else if (is_symbol_like(t)) {
            Sym *p = strmap_get(&c->syms, t);
            if (p) {
                sym_new(c, l->name, p->val, p);
                l->alias = 1;
            }
        }
    }
}

typedef struct {
    Conv *c;
    int line;
} RCtx;

static char *analyse_visit(void *ctx, const char *text, unsigned flags) {
    RCtx *rc = ctx;
    char *t = str_trim(text);
    Sym *s;
    if (is_symbol_like(t) && (s = strmap_get(&rc->c->syms, t)) != NULL) s->slot[cls_of(flags)]++;
    return NULL;
}

static char *literal(int cl, long n) { return xasprintf("%%%c%ld", CLS[cl], n); }

static char *rewrite_visit(void *ctx, const char *text, unsigned flags) {
    RCtx *rc = ctx;
    Conv *c = rc->c;
    char *t = str_trim(text);
    int cl = cls_of(flags), mx = cl == 4 ? 31 : 15;
    Sym *s;
    int pc, pn;
    if (!*t || t[0] == '%') return NULL;
    if (is_digits(t)) {
        long n = atol(t);
        if (n > mx) {
            warnf(c, rc->line, "register number %ld is out of range", n);
            return NULL;
        }
        return literal(cl, n);
    }
    if (is_symbol_like(t) && (s = strmap_get(&c->syms, t)) != NULL) {
        Sym *r = root(s);
        if (s->val > mx) {
            warnf(c, rc->line, "%s equates %d, which is out of range for a register here", t, s->val);
            return NULL;
        }
        if (!r->gbad && r->gclass == cl) return NULL; /* the EQU becomes %r<n>: keep the name */
        return literal(cl, s->val);
    }
    if (is_symbol_like(t) && reg_pattern(t, &pc, &pn) && pc == cl && pn <= mx) {
        if (once(c, t))
            warnf(c, rc->line, "%s is not defined in this file: taken as register %d (YREGS, or an EQU in a COPY member?)", t, pn);
        return literal(cl, pn);
    }
    warnf(c, rc->line, "operand \"%s\" is not a register number or an EQU name: left as it is", t);
    return NULL;
}

/* occurrences of the known EQU names in an operand text */
static void count_names(Conv *c, const char *s) {
    size_t i = 0;
    int inq = 0;
    while (s[i]) {
        unsigned char ch = (unsigned char)s[i];
        if (inq) {
            if (ch == '\'') {
                if (s[i + 1] == '\'')
                    i++;
                else
                    inq = 0;
            }
            i++;
            continue;
        }
        if (ch == '\'') {
            if (!is_attr_quote(s, i)) inq = 1;
            i++;
        } else if (is_sym_start(ch) && (i == 0 || s[i - 1] != '&')) {
            size_t j = i;
            char *tok;
            Sym *sy;
            while (is_sym_char((unsigned char)s[j])) j++;
            tok = xstrndup(s + i, j - i);
            sy = strmap_get(&c->syms, tok);
            if (sy) sy->occ++;
            i = j;
        } else if (za_isdigit(ch)) {
            while (is_sym_char((unsigned char)s[i])) i++; /* a number or hex digits: no names */
        } else {
            i++;
        }
    }
}

static int reg_kind(const char *op, RegOpsKind *k) {
    if (!strcmp(op, "USING")) {
        *k = RO_USING;
        return 1;
    }
    if (!strcmp(op, "DROP")) {
        *k = RO_DROP;
        return 1;
    }
    if (is_instruction(op)) {
        *k = RO_INSN;
        return 1;
    }
    return 0;
}

static void analyse_registers(Conv *c) {
    size_t i;
    RCtx rc;
    rc.c = c;
    collect_equs(c);
    for (i = 0; i < c->ls.n; i++) {
        LS *l = &c->ls.v[i];
        RegOpsKind k;
        if (l->kind != K_STMT) continue;
        rc.line = l->line;
        if (!l->alias) count_names(c, l->operands);
        if (reg_kind(l->op, &k)) regops_map(l->op, k, l->operands, analyse_visit, &rc);
    }
    for (i = 0; i < c->symlist.n; i++) {
        Sym *s = c->symlist.v[i], *r = root(s);
        int k, tot = 0;
        for (k = 0; k < 5; k++) {
            r->gslot[k] += s->slot[k];
            tot += s->slot[k];
        }
        if (s->occ != tot) r->gbad = 1;
        if (s->pref >= 0 && r->gpref < 0) r->gpref = s->pref;
    }
    for (i = 0; i < c->symlist.n; i++) {
        Sym *s = c->symlist.v[i];
        int k, n = 0, which = -1;
        if (s->parent) continue;
        for (k = 0; k < 5; k++)
            if (s->gslot[k]) {
                n++;
                which = k;
            }
        if (s->gbad)
            s->gclass = -1;
        else if (n == 1)
            s->gclass = which;
        else if (n == 0)
            s->gclass = s->gpref;
        else
            s->gclass = -1;
        if (s->gclass >= 0 && s->val > (s->gclass == 4 ? 31 : 15)) s->gclass = -1;
    }
}

/* ---- output --------------------------------------------------------------------- */

static void put_stmt(Conv *c, const char *name, const char *op, const char *operands, const char *remark) {
    Buf line = {0};
    OperandVec ops = {0};
    size_t i, k;
    const size_t indent = 15;
    buf_puts(&line, name);
    if (strlen(name) < 9) {
        for (k = strlen(name); k < 9; k++) buf_putc(&line, ' ');
    } else {
        buf_putc(&line, ' ');
    }
    buf_puts(&line, op);
    if (*operands) {
        for (k = strlen(op); k < 6; k++) buf_putc(&line, ' ');
        if (strlen(op) >= 6) buf_putc(&line, ' ');
        split_operands(&ops, operands);
        for (i = 0; i < ops.n; i++) {
            char *piece = xasprintf("%s%s", ops.v[i].text, i + 1 < ops.n ? "," : "");
            if (i > 0 && c->o.width > 0 && line.len + strlen(piece) > (size_t)c->o.width && line.len > indent) {
                buf_putc(&line, '\n');
                buf_puts(&c->out, buf_cstr(&line));
                line.len = 0;
                for (k = 0; k < indent; k++) buf_putc(&line, ' ');
            }
            buf_puts(&line, piece);
        }
    }
    if (*remark) {
        if (line.len < 35) {
            for (k = line.len; k < 35; k++) buf_putc(&line, ' ');
        } else {
            buf_puts(&line, "  ");
        }
        buf_puts(&line, remark);
    }
    {
        char *s = rtrim_dup(buf_cstr(&line), line.len);
        buf_puts(&c->out, s);
        buf_putc(&c->out, '\n');
    }
}

static void write_stmt(Conv *c, LS *l) {
    const char *ops = l->operands;
    RegOpsKind k;
    RCtx rc;
    rc.c = c;
    rc.line = l->line;
    if (!strcmp(l->op, "ISEQ")) return;
    if (!strcmp(l->op, "COPY") && !*l->name && *str_trim(l->operands)) {
        char *m = str_trim(l->operands), *lo = za_strlower(m);
        buf_printf(&c->out, "#include \"%s%s\"\n", lo, c->o.inc_ext);
        if (*l->remark) buf_printf(&c->out, "* %s\n", l->remark);
        if (once(c, lo))
            warnf(c, l->line, "COPY %s became #include \"%s%s\": convert that member too (hlasm2za -o %s%s)", m, lo, c->o.inc_ext, lo,
                  c->o.inc_ext);
        return;
    }
    if (c->o.strict && !strcmp(l->op, "EQU") && *l->name) {
        Sym *s = strmap_get(&c->syms, l->name);
        if (s) {
            Sym *r = root(s);
            OperandVec o = {0};
            split_operands(&o, l->operands);
            if (o.n == 1 && !r->gbad && r->gclass >= 0) ops = literal(r->gclass, s->val);
        }
    } else if (c->o.strict && reg_kind(l->op, &k) && *l->operands) {
        char *r = regops_map(l->op, k, l->operands, rewrite_visit, &rc);
        if (r)
            ops = r;
        else
            warnf(c, l->line, "no form of %s with this number of operands: registers not converted", l->op);
    }
    if (ops[0] && ops[strlen(ops) - 1] == ',')
        warnf(c, l->line, "the operand field ends with a comma: zosasm continues it on the next line");
    put_stmt(c, l->name, l->op, ops, l->remark);
}

static int warn_cmp(const void *a, const void *b) {
    const Warn *x = a, *y = b;
    if (x->line != y->line) return x->line < y->line ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

char *hlasm_convert(const char *text, size_t len, const char *file, const ConvOptions *opt, FILE *warn, int *nwarn) {
    Conv c;
    size_t i;
    memset(&c, 0, sizeof c);
    c.o = *opt;
    {
        const char *const *dn;
        size_t nd = asm_directive_names(&dn);
        for (i = 0; i < nd; i++) strmap_put(&c.dirs, dn[i], &c);
    }
    split_lines(&c, text, len);
    parse(&c);
    if (c.o.strict) analyse_registers(&c);
    for (i = 0; i < c.ls.n; i++) {
        LS *l = &c.ls.v[i];
        if (l->kind == K_BLANK)
            buf_putc(&c.out, '\n');
        else if (l->kind == K_COMMENT)
            buf_printf(&c.out, "%s\n", l->text);
        else
            write_stmt(&c, l);
    }
    if (c.warns.n) qsort(c.warns.v, c.warns.n, sizeof *c.warns.v, warn_cmp);
    for (i = 0; i < c.warns.n; i++)
        if (warn) fprintf(warn, "%s:%d: warning: %s\n", file, c.warns.v[i].line, c.warns.v[i].msg);
    if (nwarn) *nwarn = (int)c.warns.n;
    return xstrdup(buf_cstr(&c.out));
}
