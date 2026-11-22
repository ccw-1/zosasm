/* listing.c - the assembly listing. See listing.h.
 *
 * The statements come from Module.stmts, the bytes of each from Module.zrecs
 * (class offsets of the final layout), literals from Module.lits, USINGs from
 * Module.usings, symbol references from Symbol.refs. The source lines come
 * from the preprocessor's events, so lines that were removed (#if) or turned
 * into directives, and lines changed by macro expansion, can be shown as such. */
#include "listing.h"

#include <stdlib.h>
#include <string.h>

#include "goff.h"
#include "version.h"

typedef struct {
    const Module *m;
    const ListOptions *o;
    FILE *f;
    int lw;            /* width of the location column */
    int paging;
    int page_no, used; /* current page, lines used on it */
    int pending;       /* a new page (the first one: the header) starts with the next line */
    char *title;
    const char *colhead;
    int printing, gen, data; /* PRINT ON/OFF, GEN/NOGEN, DATA/NODATA */
    int force;         /* show the rows of this event although printing is off (it has diagnostics) */
    int hide;          /* CEJECT, PUSH/POP ...,NOPRINT: the statement itself is not shown */
    struct {
        int printing, gen, data;
    } pstack[64];      /* PUSH PRINT */
    int npstack;
    Vec(const char *) files;
    /* statements and lines */
    const SrcVec *lines;
    long *st_at;       /* line index -> index in m->stmts of the statement that starts there, or -1 */
    char *cont;        /* line index -> a continuation line of a statement */
    long *zi;          /* statement index -> index in m->zrecs, or -1 */
    long *lit_head, *lit_next; /* literals of each zrec, in order (by zrec index; by literal index) */
} L;

/* ---- low level output ------------------------------------------------- */

static int file_idx(L *l, const char *file) {
    size_t i;
    if (!file) file = "";
    for (i = 0; i < l->files.n; i++)
        if (!strcmp(l->files.v[i], file)) return (int)i;
    vec_push(&l->files, file);
    return (int)l->files.n - 1;
}

/* the main file has a blank id, the others 1-9, A-Z */
static char file_char(int idx) {
    if (idx == 0) return ' ';
    if (idx <= 9) return (char)('0' + idx);
    if (idx <= 35) return (char)('A' + idx - 10);
    return '*';
}

/* the header is written when the first line of the page comes, so a TITLE
 * before it can still go into it */
static void start_page(L *l) { l->pending = 1; }

static void new_page(L *l) {
    if (l->page_no > 0) {
        if (l->paging) fputc('\f', l->f);
        else fputc('\n', l->f);
    }
    l->page_no++;
    l->pending = 0;
    fprintf(l->f, "zosasm %s  %s", ZASM_VERSION_STR, l->m->name);
    if (l->paging) fprintf(l->f, "    Page %d", l->page_no);
    fputc('\n', l->f);
    fprintf(l->f, "%s\n", l->title ? l->title : "");
    fprintf(l->f, "%s\n", l->colhead);
    l->used = 3;
}

/* one line of output, without trailing blanks */
static void put(L *l, const char *s) {
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') n--;
    if (n == 0 && l->paging && !l->pending && (l->used >= l->o->page || l->used <= 3)) return; /* no blank line at the top of a page */
    if (l->pending || (l->paging && l->used >= l->o->page)) new_page(l);
    fwrite(s, 1, n, l->f);
    fputc('\n', l->f);
    l->used++;
}

static void rowhead(L *l, char *out, size_t n) {
    snprintf(out, n, "%-*s  %-19s%6s   %s", l->lw, "Loc", "Object", "Line", "Source");
}

/* one listing row; loc, obj and src may be NULL; line 0: no number */
static void row(L *l, const char *loc, const char *obj, int more, int fc, int line, int mark,
                const char *src) {
    Buf b = {0};
    if ((!l->printing && !l->force) || l->hide) return;
    buf_printf(&b, "%-*s  %-17s%c ", l->lw, loc ? loc : "", obj ? obj : "", more ? '+' : ' ');
    if (line > 0) buf_printf(&b, "%c%5d", fc, line);
    else buf_puts(&b, "      ");
    buf_printf(&b, " %c %s", mark ? mark : ' ', src ? src : "");
    put(l, buf_cstr(&b));
    free(b.p);
}

static char *hexobj(const uint8_t *p, int64_t n) {
    Buf b = {0};
    int64_t i;
    for (i = 0; i < n; i++) {
        if (i > 0 && i % 4 == 0) buf_putc(&b, ' ');
        buf_printf(&b, "%02X", p[i]);
    }
    return xstrdup(buf_cstr(&b));
}

static char *hexloc(L *l, int64_t v) { return xasprintf("%0*llX", l->lw, (unsigned long long)v); }

/* the first row carries the line; with data on, further rows follow with the rest of the bytes */
static void obj_rows(L *l, int64_t off, int have_loc, const uint8_t *b, int64_t nb, int fc, int line,
                     int mark, const char *src) {
    int64_t done = 0;
    int first = 1;
    do {
        int64_t k = nb - done;
        int more = 0;
        char *loc = NULL, *obj;
        if (k > 8) {
            k = 8;
            more = !l->data;
        }
        obj = hexobj(b ? b + done : NULL, b ? k : 0);
        if (have_loc && (first || l->data)) loc = hexloc(l, off + done);
        if (first) row(l, loc, obj, more, fc, line, mark, src);
        else row(l, loc, obj, 0, fc, 0, ' ', NULL);
        first = 0;
        done += 8;
        if (!l->data) break;
    } while (done < nb);
}

/* ---- print control (TITLE, EJECT, SPACE, PRINT and the #pragma forms) --- */

static const char *skip_blank(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* the text of 'a title' or "a title" */
static char *unquote(const char *s) {
    size_t n;
    char q = s[0];
    Buf b = {0};
    size_t i;
    s = skip_blank(s);
    q = s[0];
    if (q != '\'' && q != '"') return xstrdup(s);
    n = strlen(s);
    while (n > 1 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (n < 2 || s[n - 1] != q) return xstrdup(s + 1);
    for (i = 1; i + 1 < n; i++) {
        if (q == '\'' && s[i] == '\'' && s[i + 1] == '\'' && i + 1 < n - 1) i++;
        buf_putc(&b, s[i]);
    }
    return xstrdup(buf_cstr(&b));
}

/* word is TITLE, EJECT, SPACE, PRINT, CEJECT, PUSH or POP (any case); returns 0 for any other word */
static int ctl_word(const char *w) {
    if (za_streqi(w, "TITLE")) return 1;
    if (za_streqi(w, "EJECT")) return 2;
    if (za_streqi(w, "SPACE")) return 3;
    if (za_streqi(w, "PRINT")) return 4;
    if (za_streqi(w, "CEJECT")) return 5;
    if (za_streqi(w, "PUSH")) return 6;
    if (za_streqi(w, "POP")) return 7;
    return 0;
}

static void ctl_print(L *l, const char *arg) {
    char *a = za_strlower(arg), *p = a;
    while (*p) {
        size_t n = 0;
        char *w;
        while (*p == ' ' || *p == ',') p++;
        while (p[n] && p[n] != ' ' && p[n] != ',') n++;
        if (!n) break;
        w = xstrndup(p, n);
        if (!strcmp(w, "on")) l->printing = 1;
        else if (!strcmp(w, "off")) l->printing = 0;
        else if (!strcmp(w, "gen")) l->gen = 1;
        else if (!strcmp(w, "nogen")) l->gen = 0;
        else if (!strcmp(w, "data")) l->data = 1;
        else if (!strcmp(w, "nodata")) l->data = l->o->data;
        p += n;
    }
}

/* before the row of the statement: TITLE starts a page with the new title,
 * PRINT takes effect (the row of PRINT ON / OFF itself is shown either way) */
/* does the operand list of PUSH/POP contain the keyword? */
static int has_word(const char *arg, const char *kw) {
    char *a = za_strlower(arg), *p = a;
    size_t n = strlen(kw);
    while (*p) {
        size_t k = 0;
        while (*p == ' ' || *p == ',') p++;
        while (p[k] && p[k] != ' ' && p[k] != ',') k++;
        if (k == n && !strncmp(p, kw, n)) return 1;
        p += k;
    }
    return 0;
}

static void ctl_before(L *l, int w, const char *arg) {
    if (w == 5) {
        l->hide = 1;
    } else if (w == 6 || w == 7) {
        int was = l->printing;
        if (has_word(arg, "print")) {
            if (w == 6) {
                if (l->npstack < 64) {
                    l->pstack[l->npstack].printing = l->printing;
                    l->pstack[l->npstack].gen = l->gen;
                    l->pstack[l->npstack].data = l->data;
                    l->npstack++;
                }
            } else if (l->npstack > 0) {
                l->npstack--;
                l->printing = l->pstack[l->npstack].printing;
                l->gen = l->pstack[l->npstack].gen;
                l->data = l->pstack[l->npstack].data;
            }
        }
        if (has_word(arg, "noprint")) l->hide = 1;
        else l->force = l->force || was || l->printing;
    } else if (w == 1) {
        l->title = unquote(arg);
        if (l->paging && l->used > 3) start_page(l);
    } else if (w == 4) {
        int was = l->printing;
        ctl_print(l, arg);
        l->force = l->force || was || l->printing;
    }
}

static void ctl_after(L *l, int w, const char *arg) {
    l->hide = 0;
    if (w == 5) {
        const char *p = skip_blank(arg);
        int num = *p != 0;
        long n = 0;
        const char *q;
        for (q = p; *q && *q != ' ' && *q != '\t'; q++)
            if (*q < '0' || *q > '9') num = 0;
        if (*p && num) n = strtol(p, NULL, 10);
        if (l->paging && l->used > 3) {
            if (!*p || (num && (n == 0 || l->o->page - l->used < n))) start_page(l);
        }
    } else if (w == 2) {
        if (l->paging && l->used > 3) start_page(l);
    } else if (w == 3) {
        long n = *skip_blank(arg) ? strtol(skip_blank(arg), NULL, 10) : 1;
        if (!l->printing) return;
        if (n < 1) n = 1;
        if (n > 50) n = 50;
        while (n-- > 0) put(l, "");
    }
}

/* ---- diagnostics ------------------------------------------------------ */

static const char *sev_name(Severity s) {
    switch (s) {
    case SEV_NOTE: return "note";
    case SEV_WARNING: return "warning";
    case SEV_ERROR: return "error";
    default: return "fatal error";
    }
}

/* the carets under the source text of the row above (src = the text shown in the row,
 * NULL when the columns of the diagnostic do not refer to it) */
static void put_carets(L *l, const Diag *d, const char *src) {
    Buf b = {0};
    size_t col, tl, n, i;
    if (!src || d->span.pos.line < 1 || d->span.pos.col < 1) return;
    tl = strlen(src);
    col = (size_t)d->span.pos.col - 1;
    if (col > tl) col = tl;
    n = d->span.len < 1 ? 1 : (size_t)d->span.len;
    if (col + n > tl && tl > col) n = tl - col;
    for (i = 0; i < (size_t)l->lw + 30; i++) buf_putc(&b, ' ');
    {
        size_t bi = 0;
        while (bi < col && bi < tl) {
            unsigned char c = (unsigned char)src[bi];
            size_t sl = 1;
            if (c == '\t') { buf_putc(&b, '\t'); bi++; continue; }
            if (c < 0x80) sl = 1;
            else if (c < 0xE0) sl = 2;
            else if (c < 0xF0) sl = 3;
            else sl = 4;
            if (bi + sl > tl) sl = tl - bi;
            buf_putc(&b, ' ');
            bi += sl;
        }
    }
    for (i = 0; i < (col + n <= tl ? utf8_count(src + col, n) : n); i++) buf_putc(&b, '^');
    if (d->label && *d->label) buf_printf(&b, " %s", d->label);
    put(l, buf_cstr(&b));
    free(b.p);
}

static void put_diag(L *l, const Diag *d, const char *src) {
    int i;
    char *s;
    put_carets(l, d, src);
    s = xasprintf("** %s %s: %s", d->code, sev_name(d->sev), d->msg);
    put(l, s);
    for (i = 0; i < d->nnotes; i++) {
        const Pos *p = &d->notes[i].span.pos;
        if (p->line > 0)
            s = xasprintf("**   note: %s (%s:%d)", d->notes[i].msg, p->file ? p->file : "", p->line);
        else
            s = xasprintf("**   note: %s", d->notes[i].msg);
        put(l, s);
    }
    for (i = 0; i < d->nhelp; i++) {
        s = xasprintf("**   help: %s", d->help[i]);
        put(l, s);
    }
}

/* the source line as written (block comments are blanked in text) */
static const char *ltext(const SrcLine *s) { return s->orig ? s->orig : s->text; }

/* ---- statement rows --------------------------------------------------- */

static int op_is(const Stmt *st, const char *name) { return st->op && za_streqi(st->op, name); }

static void lit_rows(L *l, long zrec) {
    long k;
    if (zrec < 0) return;
    for (k = l->lit_head[zrec]; k >= 0; k = l->lit_next[k]) {
        const LitRec *r = &l->m->lits.v[k];
        const uint8_t *b = r->cls && r->cls->data && r->off + r->len <= r->cls->len ? r->cls->data + r->off : NULL;
        obj_rows(l, r->off, 1, b, b ? r->len : 0, ' ', 0, ' ', r->key);
    }
}

/* the row (or rows) of source line k; mark is its marker, info: the statement
 * that starts there gives location and object */
static void line_row(L *l, size_t k, int mark, int info) {
    const SrcLine *sl = &l->lines->v[k];
    int fc = file_char(file_idx(l, sl->file));
    long si = info ? l->st_at[k] : -1;
    const Stmt *st;
    long zi;
    const ZRec *z;
    int is_pool, no_bytes, showloc, w = 0;
    int64_t loc = 0, nb = 0;
    const uint8_t *bytes = NULL;
    const char *op;
    if (si < 0) {
        row(l, NULL, NULL, 0, fc, sl->line, mark, ltext(sl));
        return;
    }
    st = l->m->stmts.v[si];
    zi = l->zi[si];
    z = zi >= 0 ? &l->m->zrecs.v[zi] : NULL;
    op = st->op ? st->op : "";
    w = ctl_word(op);
    if (w) ctl_before(l, w, st->operands ? st->operands : "");
    is_pool = op_is(st, "LTORG") || op_is(st, "END");
    no_bytes = is_pool || op_is(st, "DS") || op_is(st, "ORG");
    showloc = z && z->cls && (z->len > 0 || op_is(st, "DC") || op_is(st, "DS") || op_is(st, "LTORG") ||
                              op_is(st, "CNOP")) && !op_is(st, "ORG");
    if (showloc) {
        loc = z->off + z->pad;
        nb = z->len - z->pad;
        if (!no_bytes && nb > 0 && z->cls->data && z->off + z->len <= z->cls->len) bytes = z->cls->data + loc;
        else nb = 0;
    } else if (op_is(st, "EQU") && st->name && *st->name) {
        const Symbol *s = module_symbol(l->m, st->name);
        if (s && s->kind == SYM_ABS && value_is_abs(&s->val) && s->val.abs >= 0 && s->val.abs <= 0xFFFFFFFFLL) {
            loc = s->val.abs;
            showloc = 1;
        }
    }
    obj_rows(l, loc, showloc, bytes, bytes ? nb : 0, fc, sl->line, mark, ltext(sl));
    lit_rows(l, zi);
    if (w) ctl_after(l, w, st->operands ? st->operands : "");
}

/* #pragma title|eject|space|print in a directive line */
static void pragma_effect(L *l, const char *text, int before) {
    const char *p = skip_blank(text);
    size_t n = 0;
    char *word;
    int w;
    if (*p != '#') return;
    p = skip_blank(p + 1);
    if (strncmp(p, "pragma", 6) != 0 || (p[6] != ' ' && p[6] != '\t')) return;
    p = skip_blank(p + 6);
    while (p[n] && p[n] != ' ' && p[n] != '\t') n++;
    word = xstrndup(p, n);
    w = ctl_word(word);
    if (!w) return;
    p = skip_blank(p + n);
    if (before) ctl_before(l, w, p);
    else ctl_after(l, w, p);
}

/* ---- tables ----------------------------------------------------------- */

/* "12" in the main file, "1:12" in another file */
static void posstr(char *buf, size_t n, int fi, int line) {
    if (fi == 0) snprintf(buf, n, "%d", line);
    else snprintf(buf, n, "%c:%d", file_char(fi), line);
}

static void table_head(L *l, const char *title, const char *head) {
    l->colhead = head;
    if (l->paging) {
        free(l->title);
        l->title = xstrdup(title);
        start_page(l);
    } else {
        put(l, "");
        put(l, title);
        put(l, head);
    }
}

static const char *amode_name(int a) {
    switch (a) {
    case GOFF_AMODE_24: return "24";
    case GOFF_AMODE_31: return "31";
    case GOFF_AMODE_ANY: return "ANY";
    case GOFF_AMODE_64: return "64";
    case GOFF_AMODE_MIN: return "MIN";
    default: return "-";
    }
}

static const char *rmode_name(int r) {
    switch (r) {
    case GOFF_RMODE_24: return "24";
    case GOFF_RMODE_31: return "31";
    case GOFF_RMODE_64: return "64";
    default: return "-";
    }
}

static const char *sect_name(const Section *s) { return s && s->name && *s->name ? s->name : "(private)"; }

static void table_files(L *l) {
    size_t i;
    table_head(l, "Source files", "Id  File");
    for (i = 0; i < l->files.n; i++) put(l, xasprintf("%c   %s%s", file_char((int)i), l->files.v[i], i == 0 ? "  (main)" : ""));
}

static void table_sections(L *l) {
    size_t i, j, w = 7;
    Buf b = {0};
    const Module *m = l->m;
    for (i = 0; i < m->sections.n; i++)
        if (strlen(sect_name(m->sections.v[i])) > w) w = strlen(sect_name(m->sections.v[i]));
    if (m->sections.n == 0) return;
    buf_printf(&b, "%-*s  %-6s  %-10s  %-*s  %-5s  %s", (int)w, "Section", "Kind", "Class", l->lw, "Length", "AMODE", "RMODE");
    table_head(l, "Sections", buf_cstr(&b));
    for (i = 0; i < m->sections.n; i++) {
        const Section *s = m->sections.v[i];
        for (j = 0; j < s->classes.n; j++) {
            const Class *c = s->classes.v[j];
            char *len = hexloc(l, c->len);
            put(l, xasprintf("%-*s  %-6s  %-10s  %-*s  %-5s  %s", (int)w, j == 0 ? sect_name(s) : "",
                             j == 0 ? sectkind_name(s->kind) : "", c->name ? c->name : "", l->lw, len,
                             j == 0 ? amode_name(s->amode) : "", j == 0 ? rmode_name(s->rmode) : ""));
        }
    }
}

static const char *using_base(const UsingRec *u) {
    switch (u->key.kind) {
    case B_CLASS: return sect_name(((Class *)u->key.p)->sect);
    case B_PIECE: return sect_name(((Piece *)u->key.p)->cls->sect);
    case B_SYM: return ((Symbol *)u->key.p)->name;
    default: return "(absolute)";
    }
}

static void table_usings(L *l) {
    size_t i;
    if (l->m->usings.n == 0) return;
    table_head(l, "USING table", "Line      Label       Reg  Base                    Offset    Range              Dropped");
    for (i = 0; i < l->m->usings.n; i++) {
        const UsingRec *u = &l->m->usings.v[i];
        char *range, *drop;
        char at[32], dr[32];
        posstr(at, sizeof at, file_idx(l, u->sp.pos.file), u->sp.pos.line);
        if (u->lo == INT64_MIN && u->hi == INT64_MAX) range = xstrdup("");
        else if (u->hi == INT64_MAX) range = xasprintf("%lld..", (long long)u->lo);
        else range = xasprintf("%lld..%lld", (long long)u->lo, (long long)u->hi - 1);
        if (u->dropped.file) {
            posstr(dr, sizeof dr, file_idx(l, u->dropped.file), u->dropped.line);
            drop = dr;
        } else {
            drop = "";
        }
        put(l, xasprintf("%-8s  %-10s  r%-2d  %-22s  %-8llX  %-17s  %s", at, u->label ? u->label : "", u->reg,
                         using_base(u), (unsigned long long)u->off, u->dependent ? "(dependent)" : range, drop));
    }
}

typedef struct {
    int fi, line;
} RefKey;

static int cmp_ref(const void *a, const void *b) {
    const RefKey *x = a, *y = b;
    if (x->fi != y->fi) return x->fi < y->fi ? -1 : 1;
    return x->line < y->line ? -1 : x->line > y->line;
}

static int cmp_sym(const void *a, const void *b) {
    return strcmp((*(Symbol *const *)a)->name, (*(Symbol *const *)b)->name);
}

static const char *kind_name(const Symbol *s) {
    switch (s->kind) {
    case SYM_LABEL: return s->entry ? "entry" : "label";
    case SYM_ABS: return "equ";
    case SYM_REL: return "equ rel";
    case SYM_EXT: return s->weak ? "wxtrn" : "extrn";
    case SYM_SECT: return "section";
    default: return "dxd";
    }
}

static void table_symbols(L *l) {
    const Module *m = l->m;
    size_t i, n = 0, k;
    Symbol **v = xmalloc((m->sym_order.n + 1) * sizeof *v);
    for (i = 0; i < m->sym_order.n; i++)
        if (strncmp(m->sym_order.v[i]->name, "$LT", 3) != 0) v[n++] = m->sym_order.v[i];
    if (n == 0) return;
    qsort(v, n, sizeof *v, cmp_sym);
    table_head(l, "Symbol cross reference",
               "Symbol                    Kind     Length  Value     Section    Defined  References");
    for (i = 0; i < n; i++) {
        const Symbol *s = v[i];
        char *val = xstrdup(""), def[32] = "", *pre;
        const char *sec = "";
        size_t nr = 0, plen;
        RefKey *rk = xmalloc((s->refs.n + 1) * sizeof *rk);
        Buf b = {0};
        int col;
        if (s->kind == SYM_LABEL && s->piece) {
            val = xasprintf("%llX", (unsigned long long)(s->piece->start + s->off));
            sec = s->piece->cls ? sect_name(s->piece->cls->sect) : "";
        } else if (s->kind == SYM_ABS && value_is_abs(&s->val)) {
            val = xasprintf("%llX", (unsigned long long)s->val.abs);
        } else if (s->kind == SYM_SECT && s->sect) {
            sec = sect_name(s->sect);
        }
        if (s->def.pos.line > 0) {
            posstr(def, sizeof def, file_idx(l, s->def.pos.file), s->def.pos.line);
        }
        for (k = 0; k < s->refs.n; k++) {
            if (s->refs.v[k].line <= 0) continue;
            rk[nr].fi = file_idx(l, s->refs.v[k].file);
            rk[nr].line = s->refs.v[k].line;
            nr++;
        }
        qsort(rk, nr, sizeof *rk, cmp_ref);
        {
            const char *nm = s->name;
            char *shown = is_symbol_like(nm) ? xstrdup(nm) : xasprintf("\"%s\"", nm);
            pre = xasprintf("%-24s  %-7s  %-6lld  %-8s  %-9s  %-7s  ", shown, kind_name(s), (long long)s->len, val, sec, def);
        }
        plen = strlen(pre);
        buf_puts(&b, pre);
        col = (int)plen;
        for (k = 0; k < nr; k++) {
            char item[40];
            if (k > 0 && rk[k].fi == rk[k - 1].fi && rk[k].line == rk[k - 1].line) continue;
            posstr(item, sizeof item, rk[k].fi, rk[k].line);
            if (col + (int)strlen(item) + 1 > 110 && col > (int)plen) {
                put(l, buf_cstr(&b));
                b.len = 0;
                buf_printf(&b, "%*s", (int)plen, "");
                col = (int)plen;
            }
            buf_printf(&b, "%s ", item);
            col += (int)strlen(item) + 1;
        }
        put(l, buf_cstr(&b));
        free(b.p);
    }
}

/* ---- the listing ------------------------------------------------------ */

static long *index_array(size_t n, long v) {
    long *a = xmalloc((n + 1) * sizeof *a);
    size_t i;
    for (i = 0; i < n; i++) a[i] = v;
    return a;
}

void listing_write(FILE *f, const Module *m, const SrcVec *lines, const PpEventVec *events,
                   const Reporter *rep, const ListOptions *opt) {
    L ll, *l = &ll;
    size_t i, j, nev, stmt_total = 0;
    PpEventVec own = {0};
    const PpEventVec *ev = events;
    char head[160];
    StrMap evmap;
    Vec(const Diag *) *evd;
    Vec(const Diag *) stray = {0};
    const Diag **sorted;
    size_t nd;

    memset(l, 0, sizeof *l);
    l->m = m;
    l->o = opt;
    l->f = f;
    l->lines = lines;
    l->paging = opt->page > 0;
    l->printing = 1;
    l->gen = 1;
    l->data = opt->data;
    l->lw = 6;
    for (i = 0; i < m->sections.n; i++)
        for (j = 0; j < m->sections.v[i]->classes.n; j++)
            if (m->sections.v[i]->classes.v[j]->len > 0xFFFFFF) l->lw = 8;
    file_idx(l, m->name);
    if (l->paging && opt->page < 8) {
        ListOptions *o2 = xmalloc(sizeof *o2);
        *o2 = *opt;
        o2->page = 8;
        l->o = o2;
    }

    /* events: when the preprocessor did not record any, each line is one */
    if (!ev) {
        for (i = 0; i < lines->n; i++) {
            PpEvent e;
            memset(&e, 0, sizeof e);
            e.file = lines->v[i].file;
            e.line = e.line_end = lines->v[i].line;
            e.kind = PE_LINE;
            e.text = ltext(&lines->v[i]);
            e.first = i;
            e.n = 1;
            vec_push(&own, e);
        }
        ev = &own;
    }
    nev = ev->n;

    /* statements <-> lines <-> object records */
    l->st_at = index_array(lines->n, -1);
    l->cont = xcalloc(lines->n + 1);
    for (i = 0; i < m->stmts.n; i++) {
        const Stmt *st = m->stmts.v[i];
        if (st->li < lines->n) {
            l->st_at[st->li] = (long)i;
            for (j = 1; j < (size_t)st->nli && st->li + j < lines->n; j++) l->cont[st->li + j] = 1;
        }
    }
    l->zi = index_array(m->stmts.n, -1);
    j = 0;
    for (i = 0; i < m->stmts.n; i++)
        if (j < m->zrecs.n && m->zrecs.v[j].st == m->stmts.v[i]) l->zi[i] = (long)j++;
    stmt_total = m->stmts.n;
    l->lit_head = index_array(m->zrecs.n, -1);
    l->lit_next = index_array(m->lits.n, -1);
    for (i = m->lits.n; i-- > 0;) {
        size_t o = m->lits.v[i].owner;
        if (o < m->zrecs.n) {
            l->lit_next[i] = l->lit_head[o];
            l->lit_head[o] = (long)i;
        }
    }

    /* diagnostics go after the last row of the event of their line */
    memset(&evmap, 0, sizeof evmap);
    for (i = 0; i < nev; i++) {
        int ln;
        for (ln = ev->v[i].line; ln <= ev->v[i].line_end; ln++) {
            char *key = xasprintf("%s:%d", ev->v[i].file ? ev->v[i].file : "", ln);
            if (!strmap_get(&evmap, key)) strmap_put(&evmap, key, (void *)(i + 1));
        }
    }
    evd = xcalloc((nev + 1) * sizeof *evd);
    sorted = rep_sorted(rep, &nd);
    for (i = 0; i < nd; i++) {
        const Diag *d = sorted[i];
        size_t e = 0;
        if (d->span.pos.line > 0) {
            char *key = xasprintf("%s:%d", d->span.pos.file ? d->span.pos.file : "", d->span.pos.line);
            e = (size_t)strmap_get(&evmap, key);
        }
        if (e) vec_push(&evd[e - 1], d);
        else vec_push(&stray, d);
    }

    l->colhead = head;
    rowhead(l, head, sizeof head);
    start_page(l);

    for (i = 0; i < nev; i++) {
        const PpEvent *e = &ev->v[i];
        int fc = file_char(file_idx(l, e->file));
        l->force = evd[i].n > 0;
        if (e->kind == PE_DIRECTIVE || e->kind == PE_SKIPPED) {
            if (e->kind == PE_DIRECTIVE) pragma_effect(l, e->text, 1);
            row(l, NULL, NULL, 0, fc, e->line, e->kind == PE_DIRECTIVE ? '#' : '-', e->text);
            if (e->kind == PE_DIRECTIVE) pragma_effect(l, e->text, 0);
        } else if (e->n == 1 && !strcmp(e->text, ltext(&lines->v[e->first]))) {
            line_row(l, e->first, ' ', 1);
        } else {
            row(l, NULL, NULL, 0, fc, e->line, ' ', e->text);
            if (l->gen || l->force)
                for (j = 0; j < e->n; j++) line_row(l, e->first + j, '+', 1);
        }
        for (j = 0; j < evd[i].n; j++) {
            int was = l->printing;
            l->printing = 1;
            put_diag(l, evd[i].v[j],
                     (e->kind == PE_DIRECTIVE || e->kind == PE_SKIPPED ||
                      (e->n == 1 && !strcmp(e->text, ltext(&lines->v[e->first]))))
                         ? e->text
                         : NULL);
            l->printing = was;
        }
    }
    l->force = 0;

    /* literals of an END that the source does not have */
    if (m->zrecs.n > 0 && (m->stmts.n == 0 || m->zrecs.v[m->zrecs.n - 1].st != m->stmts.v[m->stmts.n - 1]) &&
        l->lit_head[m->zrecs.n - 1] >= 0) {
        int lastz = 1;
        for (i = 0; i < m->stmts.n; i++)
            if (l->zi[i] == (long)m->zrecs.n - 1) lastz = 0;
        if (lastz) {
            row(l, NULL, NULL, 0, ' ', 0, ' ', "(implied END: literal pool)");
            lit_rows(l, (long)m->zrecs.n - 1);
        }
    }
    l->printing = 1;
    if (stray.n) {
        put(l, "");
        put(l, "Diagnostics that do not belong to a source line:");
        for (j = 0; j < stray.n; j++) put_diag(l, stray.v[j], NULL);
    }

    if (!opt->brief) {
        table_files(l);
        table_sections(l);
        table_usings(l);
        table_symbols(l);
    }
    {
        char *s = rep_summary(rep);
        put(l, "");
        put(l, xasprintf("%zu source lines, %zu statements; %s", lines->n, stmt_total, s));
    }
}
