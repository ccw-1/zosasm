/* reader.c - source text to statements.
 *
 * Source format (a variant of HLASM without the column rules): the name
 * field starts in column 1 (optional trailing ':'), the operation follows
 * after blanks, the operands end at the first blank outside quotes and
 * parentheses, the rest of the line is a remark. A comma at the end of the
 * operand field continues it on the next line. '*' or ';' in column 1
 * starts a comment line. */
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "ebcdic.h"

int is_blank(int c) { return c == ' ' || c == '\t'; }

const char *caret_rest(const char *s) {
    const char *p = s;
    while (is_blank((unsigned char)*p)) p++;
    return *p == '^' ? p + 1 : s;
}

int is_sym_start(int c) { return za_isalpha(c) || c == '@' || c == '#' || c == '$' || c == '_'; }

int is_sym_char(int c) { return is_sym_start(c) || za_isdigit(c); }

int is_symbol_like(const char *s) {
    if (!*s || !is_sym_start((unsigned char)*s)) return 0;
    for (; *s; s++)
        if (!is_sym_char((unsigned char)*s)) return 0;
    return 1;
}

long close_dquote(const char *s, size_t i) {
    for (i++; s[i]; i++) {
        if (s[i] == '\\') {
            if (!s[i + 1]) return -1;
            i++;
        } else if (s[i] == '"') {
            return (long)i;
        }
    }
    return -1;
}

static int hexdig(int c) { return za_isdigit(c) ? c - '0' : za_toupper(c) - 'A' + 10; }

char *sym_decode(const char *s, size_t n, const char **err) {
    Buf b = {0};
    size_t i;
    if (n == 0) {
        *err = "a quoted symbol cannot be empty";
        return NULL;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') {
            if (i + 1 < n && (s[i + 1] == '"' || s[i + 1] == '\\')) {
                buf_putc(&b, s[++i]);
            } else if (i + 3 < n && s[i + 1] == 'x' && za_isxdigit((unsigned char)s[i + 2]) &&
                       za_isxdigit((unsigned char)s[i + 3])) {
                int v = hexdig((unsigned char)s[i + 2]) * 16 + hexdig((unsigned char)s[i + 3]);
                if (v == 0) {
                    *err = "\\x00 cannot be part of a name";
                    return NULL;
                }
                if (v < 0x80) {
                    buf_putc(&b, v);
                } else {
                    buf_putc(&b, 0xC0 | (v >> 6));
                    buf_putc(&b, 0x80 | (v & 0x3F));
                }
                i += 3;
            } else {
                *err = "unknown escape; use \\\", \\\\ or \\xHH";
                return NULL;
            }
        } else if (c < 0x20 || c == 0x7F) {
            *err = "control characters must be written \\xHH";
            return NULL;
        } else {
            buf_putc(&b, c);
        }
    }
    if (b.len > 32767) {
        *err = "the name is longer than 32767 bytes";
        return NULL;
    }
    return xstrdup(buf_cstr(&b));
}

char *sym_text(const char *tok, const char **err) {
    size_t n = strlen(tok);
    if (tok[0] == '"') {
        long j = close_dquote(tok, 0);
        if (j < 0) {
            *err = "the quoted symbol is not closed";
            return NULL;
        }
        if ((size_t)j != n - 1) {
            *err = "unexpected text after the closing quote";
            return NULL;
        }
        return sym_decode(tok + 1, n - 2, err);
    }
    if (n == 0 || n > 32767 || !is_symbol_like(tok)) {
        *err = "symbols start with a letter or one of @ # $ _ and continue with letters, digits "
               "or those characters; any other name is written in double quotes";
        return NULL;
    }
    return xstrdup(tok);
}

char *sym_source(const unsigned char *eb, size_t n) {
    Buf b = {0}, q = {0};
    size_t i;
    if (n == 0) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = ZA_A(eb[i]);
        if (c == 0) return NULL;
        buf_putc(&b, c);
    }
    if (n <= 32767 && is_symbol_like(buf_cstr(&b))) return xstrdup(buf_cstr(&b));
    buf_putc(&q, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = b.p[i];
        if (c == '"' || c == '\\') {
            buf_putc(&q, '\\');
            buf_putc(&q, c);
        } else if (c >= 0x20 && c < 0x7F) {
            buf_putc(&q, c);
        } else {
            buf_printf(&q, "\\x%02X", c);
        }
    }
    buf_putc(&q, '"');
    return xstrdup(buf_cstr(&q));
}

int is_attr_quote(const char *s, size_t i) {
    int c, n;
    if (i == 0) return 0;
    c = (unsigned char)s[i - 1];
    if (c == 0 || !strchr("LTSIDOKNltsidokn", c)) return 0;
    if (i >= 2) {
        int pc = (unsigned char)s[i - 2];
        if (is_sym_char(pc) || pc == '\'') return 0;
    }
    n = (unsigned char)s[i + 1];
    return n != 0 && (is_sym_start(n) || n == '*' || n == '"' || n == '&');
}

long match_paren(const char *s, size_t i) {
    int depth = 0, in_q = 0;
    for (; s[i]; i++) {
        char c = s[i];
        if (in_q) {
            if (c == '\'') {
                if (s[i + 1] == '\'')
                    i++;
                else
                    in_q = 0;
            }
            continue;
        }
        switch (c) {
        case '\'':
            if (!is_attr_quote(s, i)) in_q = 1;
            break;
        case '"': {
            long j = close_dquote(s, i);
            if (j < 0) return -1;
            i = (size_t)j;
            break;
        }
        case '(':
            depth++;
            break;
        case ')':
            depth--;
            if (depth == 0) return (long)i;
            break;
        }
    }
    return -1;
}

long close_quote(const char *s, size_t i) {
    size_t j;
    for (j = i + 1; s[j]; j++) {
        if (s[j] == '\'') {
            if (s[j + 1] == '\'') {
                j++;
                continue;
            }
            return (long)j;
        }
    }
    return -1;
}

/* ---- block comments ----------------------------------------------------- */

int bc_line(char *s, int *in) {
    int touched = *in;
    size_t i = 0;
    if (!*in) { /* a comment line (indented too: macro bodies) is not scanned */
        const char *p = caret_rest(s);
        while (is_blank((unsigned char)*p)) p++;
        if (*p == '*' || *p == ';') return 0;
    }
    while (s[i]) {
        if (*in) {
            if (s[i] == '*' && s[i + 1] == '/') {
                s[i] = s[i + 1] = ' ';
                i += 2;
                *in = 0;
            } else {
                s[i++] = ' ';
            }
            continue;
        }
        if (s[i] == '"') {
            long j = close_dquote(s, i);
            if (j < 0) break;
            i = (size_t)j + 1;
        } else if (s[i] == '\'') {
            long j;
            if (is_attr_quote(s, i)) {
                i++;
                continue;
            }
            j = close_quote(s, i);
            if (j < 0) break; /* an open quote: the rest is not scanned */
            i = (size_t)j + 1;
        } else if (s[i] == '/' && s[i + 1] == '/') {
            break;
        } else if (s[i] == '/' && s[i + 1] == '*') {
            s[i] = s[i + 1] = ' ';
            i += 2;
            *in = 1;
            touched = 1;
        } else {
            i++;
        }
    }
    return touched;
}

/* ---- text to lines ------------------------------------------------------ */

void src_blank_comments(SrcVec *lines, Reporter *rep) {
    size_t j, open_line = 0;
    int in = 0, open_col = 0;
    for (j = 0; j < lines->n; j++) {
        const char *txt = lines->v[j].text;
        char *copy;
        int was = in;
        if (!in && !strstr(txt, "/*")) continue;
        copy = xstrdup(txt);
        if (!bc_line(copy, &in)) {
            free(copy);
            continue;
        }
        if (!was && in) {
            const char *q, *last = NULL;
            for (q = strstr(txt, "/*"); q; q = strstr(q + 1, "/*")) last = q;
            open_line = j;
            open_col = last ? (int)(last - txt) + 1 : 1;
        }
        lines->v[j].orig = txt;
        lines->v[j].text = copy;
    }
    if (in)
        diag_help(rep_errorf(rep, "ZA4036",
                             span_make(lines->v[open_line].file, lines->v[open_line].line, open_col, 2),
                             "this comment is not closed before the end of the file"),
                  "a comment that starts with /* ends at the next */; a /* in a remark that is not "
                  "in quotes starts one too");
}

void src_split(SrcVec *out, const char *file, const char *text, size_t len) {
    char *buf;
    size_t i, start = 0, n = 0;
    int line = 1;
    if (utf8_valid(text, len)) {
        buf = xmalloc(len + 1);
        memcpy(buf, text, len);
        n = len;
    } else { /* ISO-8859-1 */
        buf = xmalloc(len * 2 + 1);
        for (i = 0; i < len; i++) {
            unsigned char c = (unsigned char)text[i];
            if (c < 0x80) {
                buf[n++] = (char)c;
            } else {
                buf[n++] = (char)(0xC0 | (c >> 6));
                buf[n++] = (char)(0x80 | (c & 0x3F));
            }
        }
    }
    buf[n] = 0;
    for (i = 0; i <= n; i++) {
        if (i == n || buf[i] == '\n') {
            SrcLine l;
            if (i < n && i > start && buf[i - 1] == '\r') buf[i - 1] = 0;
            buf[i] = 0;
            l.file = file;
            l.line = line++;
            l.text = buf + start;
            l.orig = NULL;
            l.plines = NULL;
            l.poffs = NULL;
            l.nplines = 0;
            vec_push(out, l);
            start = i + 1;
        }
    }
}

/* ---- statements --------------------------------------------------------- */

Span stmt_operand_span(const Stmt *s, int off, int n) {
    Span sp = span_make(s->file, s->line, 1, n);
    int i;
    for (i = s->nsegs - 1; i >= 0; i--) {
        if (off >= s->segs[i].off) {
            sp.pos.line = s->segs[i].line;
            sp.pos.col = s->segs[i].col + (off - s->segs[i].off);
            break;
        }
    }
    if (sp.len < 1) sp.len = 1;
    return sp;
}

Span stmt_span(const Stmt *s) {
    if (s->op[0]) return s->op_span;
    return span_make(s->file, s->line, 1, 1);
}

/* End of the operand field starting at start: first blank outside quotes
 * and parentheses. Returns 0 if a quote or parenthesis is left open. */
int scan_operand_end(const char *s, size_t start, size_t *end) {
    int depth = 0, in_q = 0;
    size_t i;
    for (i = start; s[i]; i++) {
        char c = s[i];
        if (in_q) {
            if (c == '\'') {
                if (s[i + 1] == '\'') {
                    i++;
                    continue;
                }
                in_q = 0;
            }
            continue;
        }
        switch (c) {
        case '\'':
            if (is_attr_quote(s, i)) continue;
            in_q = 1;
            break;
        case '"': {
            long j = close_dquote(s, i);
            if (j < 0) {
                *end = strlen(s);
                return 0;
            }
            i = (size_t)j;
            break;
        }
        case '(':
            depth++;
            break;
        case ')':
            depth--;
            break;
        case ' ':
        case '\t':
            if (depth <= 0) {
                *end = i;
                return 1;
            }
            break;
        }
    }
    *end = i;
    return !in_q && depth == 0;
}

static int blank_line(const char *s) {
    for (; *s; s++)
        if (!za_isspace((unsigned char)*s)) return 0;
    return 1;
}

void read_statements(StmtVec *out, const SrcLine *lines, size_t n, Reporter *rep) {
    size_t i;
    for (i = 0; i < n; i++) {
        const char *line = lines[i].text;
        const char *file = lines[i].file;
        int ln = lines[i].line;
        const int *plines = lines[i].plines;
        size_t nplines = lines[i].nplines;
        Stmt *st;
        size_t p = 0, q;
        const char *cur;
        int cur_ln;
        size_t cur_pseg = 0;
        Buf ops = {0};
        Vec(Seg) segs = {0};
        int co; /* columns before the text read as column 1 (the '^' anchor) */
        {
            const char *rest = caret_rest(line);
            co = (int)(rest - line);
            line = rest;
        }
        if (blank_line(line) || line[0] == '*' || line[0] == ';') continue;
        st = xcalloc(sizeof *st);
        st->file = file;
        st->line = ln;
        st->li = i;
        st->nli = 1;
        st->name = st->op = st->operands = st->remark = "";
        /* name field */
        if (!is_blank((unsigned char)line[0])) {
            size_t nl;
            q = p;
            if (line[q] == '"') { /* quoted symbol: may hold blanks */
                long j = close_dquote(line, q);
                if (j >= 0) q = (size_t)j + 1;
            }
            while (line[q] && !is_blank((unsigned char)line[q])) q++;
            nl = q - p;
            st->name_span = span_make(file, ln, (int)p + 1 + co, (int)nl);
            if (nl > 0 && line[q - 1] == ':') nl--;
            if (line[p] == '"') {
                const char *why = NULL;
                char *t = xstrndup(line + p, nl);
                st->name = sym_text(t, &why);
                if (!st->name) {
                    diag_help(rep_errorf(rep, "ZA0004", st->name_span, "invalid quoted symbol: %s", why),
                              "write \"name\" with \\\" for a quote, \\\\ for a backslash and \\xHH for "
                              "other characters");
                    st->name = xstrdup("");
                    st->bad = 1;
                }
                st->name_quoted = 1;
                free(t);
            } else {
                st->name = xstrndup(line + p, nl);
            }
            p = q;
        }
        while (line[p] && is_blank((unsigned char)line[p])) p++;
        /* operation field */
        q = p;
        while (line[q] && !is_blank((unsigned char)line[q])) q++;
        st->op = xstrndup(line + p, q - p);
        st->op_span = span_make(file, ln, (int)p + 1 + co, (int)(q - p));
        if (!st->op[0]) {
            diag_help(rep_errorf(rep, "ZA0001", span_make(file, ln, 1 + co, (int)strlen(st->name)),
                                 "statement has a name but no operation"),
                      "an operation (instruction or directive) must follow the name; labels need "
                      "to start in column 1 and the operation must be separated by blanks");
            st->bad = 1;
            vec_push(out, st);
            continue;
        }
        p = q;
        while (line[p] && is_blank((unsigned char)line[p])) p++;
        /* operand field (may continue on following lines) */
        cur = line;
        cur_ln = ln;
        cur_pseg = 0;
        for (;;) {
            size_t end;
            const char *rest, *nl;
            Seg sg;
            char *seg = NULL;
            size_t slen, base_off = 0;
            const size_t *poffs = lines[i].poffs;
            if (plines && poffs && cur_pseg < nplines) {
                size_t next = (cur_pseg + 1 < nplines) ? poffs[cur_pseg + 1] : strlen(lines[i].text);
                base_off = poffs[cur_pseg];
                slen = next - base_off;
                seg = xstrndup(lines[i].text + base_off, slen);
                cur_ln = plines[cur_pseg];
            } else {
                nl = strchr(cur, '\n');
                slen = nl ? (size_t)(nl - cur) : strlen(cur);
                seg = xstrndup(cur, slen);
                if (plines && cur_pseg < nplines) cur_ln = plines[cur_pseg];
            }
            if (!scan_operand_end(seg, p, &end)) {
                diag_help(rep_errorf(rep, "ZA0002", span_make(file, cur_ln, (int)p + 1 + co, (int)(slen - p)),
                                     "unterminated quoted string or unbalanced parenthesis in the "
                                     "operand field"),
                          "a quote (') must be closed on the same line; write two quotes ('') for a "
                          "quote inside a string");
                st->bad = 1;
                end = slen;
            }
            sg.off = (int)ops.len;
            sg.line = cur_ln;
            sg.col = (int)p + 1 + co;
            vec_push(&segs, sg);
            buf_write(&ops, seg + p, end - p);
            rest = seg + end;
            while (*rest == ' ' || *rest == '\t') rest++;
            if (end > p && seg[end - 1] == ',') {
                if (plines && poffs && cur_pseg + 1 < nplines) {
                    free(seg);
                    cur_pseg++;
                    cur_ln = plines[cur_pseg];
                    p = 0; co = 0;
                    {
                        const char *ns = lines[i].text + poffs[cur_pseg];
                        size_t nlen = ((cur_pseg + 1 < nplines) ? poffs[cur_pseg + 1] : strlen(lines[i].text)) - poffs[cur_pseg];
                        while (p < nlen && is_blank((unsigned char)ns[p])) p++;
                        if (p >= nlen) {
                            rep_errorf(rep, "ZA0003", span_make(file, cur_ln, 1, 1),
                                       "the previous line ends with a comma but this continuation line is empty");
                            st->bad = 1; break;
                        }
                    }
                    continue;
                }
                nl = strchr(cur, '\n');
                if (nl) {
                    free(seg);
                    cur = nl + 1;
                    cur_pseg++;
                    cur_ln = (plines && cur_pseg < nplines) ? plines[cur_pseg] : ln;
                    p = 0; co = 0;
                    while (cur[p] && cur[p] != '\n' && is_blank((unsigned char)cur[p])) p++;
                    if (!cur[p] || cur[p] == '\n') {
                        rep_errorf(rep, "ZA0003", span_make(file, cur_ln, 1, 1),
                                   "the previous line ends with a comma but this continuation line is empty");
                        st->bad = 1; break;
                    }
                    continue;
                }
                if (!st->bad && i + 1 < n) {
                    free(seg);
                    i++;
                    cur = lines[i].text;
                    cur_ln = lines[i].line;
                    plines = lines[i].plines;
                    nplines = lines[i].nplines;
                    cur_pseg = 0;
                    {
                        const char *cr = caret_rest(cur);
                        co = (int)(cr - cur);
                    }
                    p = co;
                    while (cur[p] && is_blank((unsigned char)cur[p])) p++;
                    if (!cur[p]) {
                        rep_errorf(rep, "ZA0003", span_make(file, cur_ln, 1, 1),
                                   "the previous line ends with a comma but this continuation line is empty");
                        st->bad = 1; break;
                    }
                    continue;
                }
            }
            st->remark = xstrdup(rest);
            free(seg);
            break;
        }
        st->operands = xstrdup(buf_cstr(&ops));
        st->segs = segs.v;
        st->nsegs = (int)segs.n;
        /* REPRO: the record that follows is data, not a statement */
        if (za_streqi(st->op, "REPRO") && i + 1 < n) i++;
        st->nli = (int)(i - st->li + 1);
        vec_push(out, st);
    }
}

void split_operands(OperandVec *out, const char *s) {
    int depth = 0, in_q = 0;
    size_t i, start = 0;
    Operand o;
    if (!*s) return;
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if (in_q) {
            if (c == '\'') {
                if (s[i + 1] == '\'')
                    i++;
                else
                    in_q = 0;
            }
            continue;
        }
        switch (c) {
        case '\'':
            if (!is_attr_quote(s, i)) in_q = 1;
            break;
        case '"': {
            long j = close_dquote(s, i);
            if (j >= 0) i = (size_t)j;
            break;
        }
        case '(':
            depth++;
            break;
        case ')':
            depth--;
            break;
        case ',':
            if (depth == 0) {
                o.text = xstrndup(s + start, i - start);
                o.off = (int)start;
                vec_push(out, o);
                start = i + 1;
            }
            break;
        }
    }
    o.text = xstrdup(s + start);
    o.off = (int)start;
    vec_push(out, o);
}
