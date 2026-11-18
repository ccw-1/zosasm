/* assembler.c - the two-pass driver, symbols, expression evaluation,
 * location counter handling and layout. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "ebcdic.h"
#include "goff.h"

const char *sectkind_name(SectKind k) {
    static const char *const n[] = {"CSECT", "RSECT", "DSECT", "COM"};
    return n[k];
}

int64_t piece_len(const Piece *p) { return p->loc > p->max ? p->loc : p->max; }

Symbol *module_symbol(const Module *m, const char *name) { return strmap_get(&m->symbols, name); }

char *str_trim(const char *s) {
    size_t n;
    while (*s && za_isspace((unsigned char)*s)) s++;
    n = strlen(s);
    while (n > 0 && za_isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

/* ---- directive table --------------------------------------------------- */

typedef struct {
    const char *name;
    Handler fn;
} DirEntry;

static const DirEntry directive_tab[] = {
    {"ACONTROL", dir_acontrol}, {"AMODE", dir_mode},   {"ASSERT", dir_assert}, {"CATTR", dir_cattr},
    {"CCW", dir_ccw},      {"CCW0", dir_ccw},     {"CCW1", dir_ccw},     {"CEJECT", dir_ceject},
    {"CNOP", dir_cnop},    {"COM", dir_section},  {"COPY", dir_copy},    {"CSECT", dir_section},
    {"CXD", dir_cxd},      {"DC", dir_dc},        {"DROP", dir_drop},    {"DS", dir_dc},
    {"DSECT", dir_section}, {"DXD", dir_dxd},     {"EJECT", dir_ignore}, {"END", dir_end},
    {"ENTRY", dir_entry},  {"EQU", dir_equ},      {"EXITCTL", dir_exitctl}, {"EXTRN", dir_extrn},
    {"ICTL", dir_ignore},  {"ISEQ", dir_ignore},  {"LOCTR", dir_loctr},  {"LTORG", dir_ltorg},
    {"OPSYN", dir_opsyn},  {"ORG", dir_org},      {"POP", dir_pushpop},  {"PRINT", dir_ignore},
    {"PUNCH", dir_punch},
    {"PUSH", dir_pushpop}, {"REPRO", dir_repro},  {"RMODE", dir_mode},   {"RSECT", dir_section},
    {"SPACE", dir_ignore}, {"START", dir_section}, {"TITLE", dir_ignore}, {"USING", dir_using},
    {"WXTRN", dir_extrn},  {"XATTR", dir_xattr},
};

#define NDIRS (sizeof directive_tab / sizeof directive_tab[0])

size_t asm_directive_names(const char *const **names) {
    static const char *list[NDIRS];
    size_t i;
    for (i = 0; i < NDIRS; i++) list[i] = directive_tab[i].name;
    *names = list;
    return NDIRS;
}

static Handler find_directive(const char *upper_op) {
    size_t lo = 0, hi = NDIRS;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(upper_op, directive_tab[mid].name);
        if (c == 0) return directive_tab[mid].fn;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

void dir_ignore(Asm *a, Stmt *st) {
    (void)a;
    (void)st;
}

void dir_copy(Asm *a, Stmt *st) {
    if (a->pass == 1) {
        diag_help(rep_errorf(a->rep, "ZA9001", st->op_span, "COPY is not supported"),
                  "use the preprocessor: #include \"file\"");
        st->bad = 1;
    }
}

/* ---- statements -------------------------------------------------------- */

int asm_is_directive(const char *upper_op) { return find_directive(upper_op) != NULL; }

int asm_is_op(const char *upper_op) {
    size_t n;
    if (find_directive(upper_op)) return 1;
    isa_lookup(upper_op, &n);
    return n > 0;
}

static void unknown_op(Asm *a, Stmt *st) {
    const char *const *dn, *const *mn;
    const char **cands;
    size_t nd, nm, i, n = 0;
    Diag *d;
    const char *s;
    char *up;
    if (a->pass != 1) return;
    nd = asm_directive_names(&dn);
    mn = isa_mnemonics(&nm);
    cands = xmalloc((nd + nm) * sizeof *cands);
    for (i = 0; i < nd; i++) cands[n++] = dn[i];
    for (i = 0; i < nm; i++) cands[n++] = mn[i];
    d = diag_label(rep_errorf(a->rep, "ZA2001", st->op_span, "unknown operation \"%s\"", st->op),
                   "not an instruction or directive");
    s = diag_closest(st->op, cands, n);
    if (s) {
        up = za_strupper(s);
        diag_help(d, "did you mean \"%s\"?", up);
    } else if (!st->name[0] && st->op[0] == '&') {
        diag_help(d, "the macro/conditional-assembly language is not supported");
    }
    if (!st->name[0] && st->op_span.pos.col > 1 && is_symbol_like(st->op))
        diag_help(d,
                  "labels must start in column 1; if \"%s\" was meant as a label, move it to "
                  "column 1",
                  st->op);
    free(cands);
    st->bad = 1;
}

void asm_stmt(Asm *a, Stmt *st) {
    size_t n;
    char *up = za_strupper(st->op);
    char *orig = NULL;
    Handler h;
    if (a->opsyn.n > 0) { /* an operation code made by OPSYN */
        const char *al = strmap_get(&a->opsyn, up);
        if (al) {
            if (!*al) {
                unknown_op(a, st);
                return;
            }
            orig = st->op;
            st->op = xstrdup(al);
            up = za_strupper(st->op);
        }
    }
    h = find_directive(up);
    if (h) {
        h(a, st);
    } else {
        isa_lookup(st->op, &n);
        if (n > 0)
            asm_instr(a, st);
        else
            unknown_op(a, st);
    }
    if (orig) st->op = orig;
}

/* ---- symbols ----------------------------------------------------------- */

Symbol *asm_lookup(Asm *a, const char *name) { return module_symbol(a->mod, name); }

Symbol *asm_define(Asm *a, Stmt *st, const char *name, Span sp, int *created) {
    Symbol *old = asm_lookup(a, name), *s;
    if (old) {
        if (a->pass == 1) {
            diag_note(rep_errorf(a->rep, "ZA1020", sp, "symbol \"%s\" is already defined", name),
                      old->def, "\"%s\" was first defined here", name);
            st->bad = 1;
        }
        if (created) *created = 0;
        return old;
    }
    s = xcalloc(sizeof *s);
    s->name = xstrdup(name);
    s->def = sp;
    s->def_idx = a->cur_idx;
    strmap_put(&a->mod->symbols, name, s);
    vec_push(&a->mod->sym_order, s);
    if (created) *created = 1;
    return s;
}

int asm_valid_name(const char *n) {
    size_t len = strlen(n);
    return len > 0 && len <= 32767 && is_symbol_like(n);
}

int asm_valid_stmt_name(const Stmt *st) {
    size_t len = strlen(st->name);
    if (st->name_quoted) return len > 0 && len <= 32767;
    return asm_valid_name(st->name);
}

int asm_need_section(Asm *a, Stmt *st) {
    char *up;
    if (a->cur) return 1;
    if (a->pass == 1) {
        up = za_strupper(st->op);
        diag_help(rep_errorf(a->rep, "ZA3001", stmt_span(st), "no active section for %s", up),
                  "start the code with a CSECT, RSECT or START statement, or a DSECT for a dummy "
                  "section");
        st->bad = 1;
    }
    return 0;
}

/* Define the statement's name field as an address in the current location
 * counter. */
Symbol *asm_define_label(Asm *a, Stmt *st, int64_t length, char typ) {
    Piece *piece;
    int64_t off;
    int created;
    Symbol *s;
    if (!st->name[0] || a->pass != 1) return NULL;
    if (!asm_need_section(a, st)) return NULL;
    if (!asm_valid_stmt_name(st)) {
        diag_help(rep_errorf(a->rep, "ZA1021", st->name_span, "\"%s\" is not a valid symbol name",
                             st->name),
                  "symbols start with a letter or one of @ # $ _ and continue with letters, digits "
                  "or those characters; any other name is written in double quotes");
        st->bad = 1;
        return NULL;
    }
    piece = a->cur;
    off = a->cur->loc;
    s = asm_define(a, st, st->name, st->name_span, &created);
    if (created) {
        s->kind = SYM_LABEL;
        s->piece = piece;
        s->off = off;
        s->len = length;
        s->type = typ;
        s->sect = a->sect;
        s->count = 1;
        s->align = 1;
        return s;
    }
    return NULL;
}

/* ---- operands and expressions ------------------------------------------ */

int asm_ops(Asm *a, Stmt *st, int min, int max, OperandVec *out) {
    int n;
    OperandVec tmp = {0};
    if (!out) out = &tmp;
    split_operands(out, st->operands);
    n = (int)out->n;
    if (n >= min && (max < 0 || n <= max)) return 1;
    if (a->pass == 1) {
        Span sp = st->op_span;
        char *want, *up = za_strupper(st->op);
        if (n > 0) sp = stmt_operand_span(st, 0, (int)strlen(st->operands));
        if (max < 0)
            want = xasprintf("at least %d", min);
        else if (min == max)
            want = xasprintf("%d", min);
        else
            want = xasprintf("%d to %d", min, max);
        rep_errorf(a->rep, "ZA0010", sp, "%s needs %s operand(s), found %d", up, want, n);
        st->bad = 1;
    }
    return 0;
}

Expr *asm_expr(Asm *a, Stmt *st, Operand o) {
    Expr *e;
    if (a->pass != 1) /* pass 2 re-parses silently; errors were found in pass 1 */
        return parse_expr(st, o.text, o.off, a->scratch);
    e = parse_expr(st, o.text, o.off, a->rep);
    if (!e) st->bad = 1;
    return e;
}

Span asm_op_span(Stmt *st, Operand o) {
    return stmt_operand_span(st, o.off, (int)strlen(o.text));
}

void asm_expr_err(Asm *a, Stmt *st, Span sp, const char *code, const char *fmt, ...) {
    if (a->pass == 1 || a->laid) {
        va_list ap;
        char *msg;
        va_start(ap, fmt);
        msg = xvasprintf(fmt, ap);
        va_end(ap);
        rep_errorf(a->rep, code, sp, "%s", msg);
        free(msg);
        st->bad = 1;
    }
}

Value asm_addr_value(Asm *a, Piece *p, int64_t off) {
    Value v;
    v.defined = 1;
    v.nterms = 1;
    v.terms = xmalloc(sizeof(Term));
    v.terms[0].coef = 1;
    if (a->laid && p->cls->sect->kind != K_DSECT) {
        v.abs = p->start + off;
        v.terms[0].base = base_class(p->cls);
    } else {
        v.abs = off;
        v.terms[0].base = base_piece(p);
    }
    return v;
}

static Value undefined_value(void) {
    Value v = value_abs(0);
    v.defined = 0;
    return v;
}

/* a name that looks like a register (R1, F0, A2, C3, V31) but is not defined */
static void register_hint(Diag *d, const char *name) {
    size_t n = strlen(name), i;
    if (n < 2 || n > 3 || !strchr("RFACVrfacv", name[0])) return;
    for (i = 1; i < n; i++)
        if (!za_isdigit((unsigned char)name[i])) return;
    diag_help(d, "registers are written %%%c%s or as a plain number; a bare \"%s\" is an ordinary "
                 "symbol and must be defined first, for example `%s EQU %s`",
              za_tolower((unsigned char)name[0]), name + 1, name, name, name + 1);
}

/* x names a symbol that does not exist (yet) */
static int missing_symbol(Asm *a, Stmt *st, const Expr *x, int need, Value *out) {
    if (a->pass == 1 && !need) {
        *out = undefined_value();
        return 1;
    }
    if (a->pass == 1) {
        Diag *d0 = diag_label(rep_errorf(a->rep, "ZA1010", x->sp,
                                        "symbol \"%s\" must be defined before this statement",
                                        x->name),
                             "not defined yet");
        register_hint(d0, x->name);
        diag_help(d0,
                  "forward references are only allowed where the value is not needed to size "
                  "the statement (for example in A-type constants and branches)");
        st->bad = 1;
        return 0;
    }
    {
        Diag *d = rep_errorf(a->rep, "ZA1011", x->sp, "undefined symbol \"%s\"", x->name);
        size_t i;
        const char **names = xmalloc((a->mod->sym_order.n + 1) * sizeof *names);
        const char *c;
        for (i = 0; i < a->mod->sym_order.n; i++) names[i] = a->mod->sym_order.v[i]->name;
        c = diag_closest(x->name, names, a->mod->sym_order.n);
        register_hint(d, x->name);
        if (c) diag_help(d, "did you mean \"%s\"?", c);
        free(names);
    }
    st->bad = 1;
    return 0;
}

/* the listing's cross reference: a use of s in pass 2 */
void asm_note_ref(Asm *a, Symbol *s, Span sp) {
    if (a->pass == 2 && sp.pos.file) vec_push(&s->refs, sp.pos);
}

static int sym_value(Asm *a, Stmt *st, const Expr *x, int need, Value *out) {
    Symbol *s = asm_lookup(a, x->name);
    if (x->qual && a->laid && !asm_using_label_active(a, x->qual)) {
        diag_help(rep_errorf(a->rep, "ZA3068", x->sp, "\"%s\" is not the label of an active USING",
                             x->qual),
                  "a qualified symbol LABEL.NAME needs `LABEL USING ...` before it, and no DROP "
                  "of LABEL in between");
        st->bad = 1;
        return 0;
    }
    if (!s) return missing_symbol(a, st, x, need, out);
    s->used = 1;
    asm_note_ref(a, s, x->sp);
    if (s->regkind && a->strict) {
        diag_help(rep_errorf(a->rep, "ZA2068", x->sp, "\"%s\" is a register name and cannot be used as a number",
                             x->name),
                  "it was defined with EQU %%%c<n>; it is only valid where a register is expected",
                  za_tolower((unsigned char)s->regkind));
        st->bad = 1;
        return 0;
    }
    switch (s->kind) {
    case SYM_LABEL:
        *out = asm_addr_value(a, s->piece, s->off);
        return 1;
    case SYM_ABS:
    case SYM_REL:
        *out = s->val;
        return 1;
    case SYM_EXT:
        out->abs = 0;
        out->defined = 1;
        out->nterms = 1;
        out->terms = xmalloc(sizeof(Term));
        out->terms[0].base = base_sym(s);
        out->terms[0].coef = 1;
        return 1;
    case SYM_SECT:
        *out = asm_addr_value(a, s->piece, 0);
        return 1;
    case SYM_DXD:
        diag_help(rep_errorf(a->rep, "ZA2062", x->sp,
                             "\"%s\" is an external dummy section and has no address",
                             x->name),
                  "use it in a Q-type constant, Q(%s), to get its offset, or J(%s) for its length",
                  x->name, x->name);
        st->bad = 1;
        return 0;
    }
    return 0;
}

/* Integer attribute I' (DESIGN.md 7): the number of digits (bits for fixed
 * point) left of the decimal point; 0 for types without a scale. */
static int64_t integer_attr(const Symbol *s) {
    int64_t l = s->len > 0 ? s->len : 1, sc = s->scale > 0 ? s->scale : 0;
    switch (s->type) {
    case 'F':
    case 'H':
        return 8 * l - sc - 1;
    case 'P':
        return 2 * l - sc - 1;
    case 'Z':
        return l - sc;
    case 'E':
    case 'D':
    case 'L':
        return 2 * (l - 1) - sc;
    }
    return 0;
}

/* Type attribute T' as a letter. */
static char type_attr(const Symbol *s) {
    switch (s->kind) {
    case SYM_EXT:
        return 'T';
    case SYM_SECT:
        return 'J';
    default:
        return s->type == 'G' ? '@' : s->type ? s->type : 'U';
    }
}

static int attr_value(Asm *a, Stmt *st, const Expr *x, int need, Value *out) {
    Symbol *s;
    int64_t n = 0;
    if (x->attr == 'K' || x->attr == 'N') {
        diag_help(rep_errorf(a->rep, "ZA1032", x->sp, "the %c' attribute belongs to the macro language",
                             x->attr),
                  "zosasm has no macro language; use the preprocessor (#define) instead");
        st->bad = 1;
        return 0;
    }
    if (x->attr == 'O') { /* operation code attribute of a name */
        size_t cnt = 0;
        char *up = za_strupper(x->name);
        char c = 'U';
        isa_lookup(x->name, &cnt);
        if (find_directive(up))
            c = 'A';
        else if (cnt > 0)
            c = 'O';
        free(up);
        *out = value_abs(ZA_E(c));
        return 1;
    }
    if (x->loc) {
        if (x->attr != 'L' || a->star_len <= 0) {
            diag_help(rep_errorf(a->rep, "ZA1031", x->sp, "%c'* is not available here", x->attr),
                      "L'* is the length of the current machine instruction or of a DC/DS whose "
                      "first operand has an explicit length or a fixed-length type");
            st->bad = 1;
            return 0;
        }
        *out = value_abs(a->star_len);
        return 1;
    }
    s = asm_lookup(a, x->name);
    if (x->attr == 'D') { /* defined before this statement */
        int d = s && (a->pass == 1 || s->def_idx < a->cur_idx);
        *out = value_abs(d ? 1 : 0);
        return 1;
    }
    if (!s) {
        if (x->attr == 'T') { /* an unknown name has type U */
            *out = value_abs(ZA_E('U'));
            return 1;
        }
        if (a->pass == 1 && !need) {
            *out = undefined_value();
            return 1;
        }
        asm_expr_err(a, st, x->sp, "ZA1011", "undefined symbol \"%s\" in attribute reference",
                     x->name);
        return 0;
    }
    asm_note_ref(a, s, x->sp);
    switch (x->attr) {
    case 'L':
        n = s->len ? s->len : 1;
        break;
    case 'S':
        n = s->scale;
        break;
    case 'I':
        n = integer_attr(s);
        break;
    case 'T':
        n = ZA_E(type_attr(s));
        break;
    }
    *out = value_abs(n);
    return 1;
}

static const char *op_text(char op) {
    switch (op) {
    case 'e': return "==";
    case 'n': return "!=";
    case 'l': return "<=";
    case 'g': return ">=";
    case '&': return "&&";
    case '|': return "||";
    case '*': return "*";
    case '/': return "/";
    case '<': return "<";
    case '>': return ">";
    }
    return "?";
}

/* ---- sizeof, offsetof, alignof, countof (DESIGN.md 12) ------------------ */

/* the symbol is of a kind the function cannot handle */
static int func_kind_err(Asm *a, Stmt *st, const Expr *x, const Expr *arg, const char *want) {
    asm_expr_err(a, st, arg->sp, "ZA1041", "%s needs %s, but \"%s\" is something else", x->name, want,
                 arg->name);
    return 0;
}

/* The length of a section is known after the first pass; that of a DSECT is
 * taken from what has been read so far and checked at the end of pass 1.
 * Returns 1 when it is known, 0 with a placeholder in *out when it is not
 * and is not needed, -1 after an error (it is needed to lay out the
 * statement). */
static int func_late(Asm *a, Stmt *st, const Expr *x, int need, Value *out) {
    if (a->laid) return 1;
    *out = undefined_value();
    if (!need) return 0;
    diag_help(rep_errorf(a->rep, "ZA1042", x->sp,
                         "%s of a section is not known yet where its value sizes this statement",
                         x->name),
              "the size of a CSECT is known only after the whole source has been read; use it "
              "where the value is not needed to lay out storage (A-type constants, "
              "instructions), or use the size of a labelled DS or a DSECT instead");
    st->bad = 1;
    return -1;
}

static int func_value(Asm *a, Stmt *st, const Expr *x, int need, Value *out) {
    Symbol *s = asm_lookup(a, x->l->name);
    int64_t n = 0;
    int late;
    if (!s) return missing_symbol(a, st, x->l, need, out);
    s->used = 1;
    asm_note_ref(a, s, x->l->sp);
    if (x->op == 'o') {
        Symbol *f = asm_lookup(a, x->r->name);
        Class *c;
        if (s->kind != SYM_SECT || !s->sect)
            return func_kind_err(a, st, x, x->l, "a section name as its first argument");
        if (!f) return missing_symbol(a, st, x->r, need, out);
        f->used = 1;
        asm_note_ref(a, f, x->r->sp);
        if (f->kind != SYM_LABEL || !f->piece || f->piece->cls->sect != s->sect ||
            f->piece->cls != s->sect->classes.v[0]) {
            asm_expr_err(a, st, x->r->sp, "ZA1043", "\"%s\" is not a field of \"%s\"", f->name,
                         s->name);
            return 0;
        }
        c = f->piece->cls;
        if (c->sect->kind == K_DSECT) {
            n = f->off;
        } else {
            late = func_late(a, st, x, need, out);
            if (late <= 0) return late == 0;
            n = f->piece->start + f->off;
        }
        *out = value_abs(n);
        return 1;
    }
    switch (s->kind) {
    case SYM_LABEL:
        if (x->op == 's')
            n = (s->len ? s->len : 1) * s->count;
        else if (x->op == 'a')
            n = s->align;
        else
            n = s->count;
        break;
    case SYM_SECT:
        if (x->op == 'c') return func_kind_err(a, st, x, x->l, "a label of a DC or DS");
        if (!a->laid && s->sect->kind == K_DSECT) { /* provisional, checked after pass 1 */
            Piece *last = s->sect->classes.v[0]->pieces.v[s->sect->classes.v[0]->pieces.n - 1];
            FuncUse u;
            n = x->op == 's' ? piece_len(last) : (s->sect->max_align > 1 ? s->sect->max_align : 1);
            u.sect = s->sect;
            u.fn = x->op;
            u.val = n;
            u.sp = x->sp;
            vec_push(&a->fuses, u);
            break;
        }
        late = func_late(a, st, x, need, out);
        if (late <= 0) return late == 0;
        if (x->op == 's')
            n = s->sect->classes.v[0]->len;
        else
            n = s->sect->max_align > 1 ? s->sect->max_align : 1;
        break;
    case SYM_DXD:
        if (x->op == 'c') return func_kind_err(a, st, x, x->l, "a label of a DC or DS");
        n = x->op == 's' ? s->xd_len : s->xd_align;
        break;
    default:
        return func_kind_err(a, st, x, x->l,
                             x->op == 'c' ? "a label of a DC or DS"
                                          : "a label, a section or a DXD name");
    }
    *out = value_abs(n);
    return 1;
}

int asm_eval(Asm *a, Stmt *st, const Expr *e, int need, Value *out) {
    switch (e->kind) {
    case EX_NUM:
        *out = value_abs(e->num);
        return 1;
    case EX_LOC:
        if (!asm_need_section(a, st)) return 0;
        *out = asm_addr_value(a, a->cur, a->cur->loc);
        return 1;
    case EX_SYM:
        return sym_value(a, st, e, need, out);
    case EX_ATTR:
        return attr_value(a, st, e, need, out);
    case EX_FUNC:
        return func_value(a, st, e, need, out);
    case EX_UN: {
        Value v;
        if (!asm_eval(a, st, e->l, need, &v)) return 0;
        if (e->op == '!') {
            if (!value_is_abs(&v)) {
                asm_expr_err(a, st, e->sp, "ZA1012", "the operand of \"!\" must be an absolute value");
                return 0;
            }
            *out = value_abs(!v.abs);
            out->defined = v.defined;
            return 1;
        }
        *out = e->op == '-' ? value_neg(v) : v;
        return 1;
    }
    case EX_BIN: {
        Value l, r;
        int ok1 = asm_eval(a, st, e->l, need, &l);
        int ok2 = asm_eval(a, st, e->r, need, &r);
        if (!ok1 || !ok2) return 0;
        if (e->op == '+') {
            *out = value_add(l, r, 1);
            return 1;
        }
        if (e->op == '-') {
            *out = value_add(l, r, -1);
            return 1;
        }
        if (!value_is_abs(&l) || !value_is_abs(&r)) {
            asm_expr_err(a, st, e->sp, "ZA1012", "operands of \"%s\" must be absolute values",
                         op_text(e->op));
            return 0;
        }
        if (strchr("enlg<>&|", e->op)) {
            int64_t x = l.abs, y = r.abs, z = 0;
            switch (e->op) {
            case 'e': z = x == y; break;
            case 'n': z = x != y; break;
            case 'l': z = x <= y; break;
            case 'g': z = x >= y; break;
            case '<': z = x < y; break;
            case '>': z = x > y; break;
            case '&': z = x != 0 && y != 0; break;
            case '|': z = x != 0 || y != 0; break;
            }
            *out = value_abs(z);
            out->defined = l.defined && r.defined;
            return 1;
        }
        if (e->op == '*') {
            *out = value_abs(l.abs * r.abs);
            out->defined = l.defined && r.defined;
            return 1;
        }
        if (r.abs == 0) {
            if (!r.defined) {
                *out = undefined_value();
                return 1;
            }
            asm_expr_err(a, st, e->sp, "ZA1014", "division by zero");
            return 0;
        }
        *out = value_abs(l.abs / r.abs);
        out->defined = l.defined && r.defined;
        return 1;
    }
    }
    return 0;
}

int asm_abs_expr(Asm *a, Stmt *st, Operand o, const char *what, int64_t *out) {
    Expr *e = asm_expr(a, st, o);
    Value v;
    if (!e) return 0;
    if (!asm_eval(a, st, e, 1, &v)) return 0;
    if (!value_is_abs(&v)) {
        if (a->pass == 1 || a->laid) {
            diag_label(rep_errorf(a->rep, "ZA1013", e->sp, "%s must be an absolute value", what),
                       "this is a relocatable address");
            st->bad = 1;
        }
        return 0;
    }
    *out = v.abs;
    return 1;
}

/* ---- location counter and storage -------------------------------------- */

void asm_advance(Asm *a, int64_t n) {
    if (a->in_align) {
        if (a->lead_open) a->lead_pad += n;
    } else if (n > 0) {
        a->lead_open = 0;
    }
    a->cur->loc += n;
    if (a->cur->loc > a->cur->max) a->cur->max = a->cur->loc;
}

void asm_align(Asm *a, int64_t n) {
    if (!a->cur || n <= 1) return;
    if (n > a->cur->cls->sect->max_align) a->cur->cls->sect->max_align = (int)n;
    a->in_align = 1;
    asm_advance(a, (n - (a->cur->hint + a->cur->loc) % n) % n);
    a->in_align = 0;
}

void asm_put(Asm *a, const uint8_t *b, size_t n) {
    if (a->pass == 2 && a->cur->cls->sect->kind != K_DSECT) {
        Class *c = a->cur->cls;
        int64_t at = a->cur->start + a->cur->loc;
        if (at >= 0 && at + (int64_t)n <= c->len) memcpy(c->data + at, b, n);
    }
    asm_advance(a, (int64_t)n);
}

void asm_reloc(Asm *a, int n, Symbol *target, Class *cls, RelocKind kind, int sub) {
    Class *c;
    Reloc r;
    if (a->pass != 2 || a->cur->cls->sect->kind == K_DSECT) return;
    c = a->cur->cls;
    memset(&r, 0, sizeof r);
    r.off = a->cur->start + a->cur->loc;
    r.len = n;
    r.target = target;
    r.cls = cls;
    r.kind = kind;
    r.sub = sub;
    vec_push(&c->relocs, r);
}

/* the bytes of a constant with all its relocation items */
void asm_put_val(Asm *a, const DcVal *v) {
    int k;
    if (v->has_rel) {
        asm_reloc(a, (int)v->n, v->target, v->cls, v->kind, v->sub);
        for (k = 0; k < v->nmore; k++)
            asm_reloc(a, (int)v->n, v->more[k].target, v->more[k].cls, v->kind, v->more[k].sub);
    }
    asm_put(a, v->b, v->n);
}

/* Assign offsets to pieces and size the class data buffers. */
static Value relayout(Value v) {
    Value r = value_abs(v.abs);
    int i;
    r.defined = v.defined;
    for (i = 0; i < v.nterms; i++) {
        Term t = v.terms[i];
        Value one = value_abs(0);
        Term single;
        if (t.base.kind == B_PIECE) {
            Piece *p = t.base.p;
            if (p->cls->sect->kind != K_DSECT) {
                r.abs += (int64_t)t.coef * p->start;
                t.base = base_class(p->cls);
            }
        }
        single = t;
        one.terms = &single;
        one.nterms = 1;
        r = value_add(r, one, 1);
    }
    return r;
}

static void resolve_xd(Asm *a);

/* sizeof and alignof of a DSECT were answered in pass 1 from what was known
 * then; the DSECT must not have changed since */
static void check_func_uses(Asm *a) {
    size_t i;
    for (i = 0; i < a->fuses.n; i++) {
        FuncUse *u = &a->fuses.v[i];
        int64_t now = u->fn == 's' ? u->sect->classes.v[0]->len
                                   : (u->sect->max_align > 1 ? u->sect->max_align : 1);
        if (now != u->val) {
            diag_help(rep_errorf(a->rep, "ZA1044", u->sp,
                                 "%s of the DSECT \"%s\" was %lld here, but it is %lld at the "
                                 "end of the source",
                                 u->fn == 's' ? "sizeof" : "alignof", u->sect->name,
                                 (long long)u->val, (long long)now),
                      "the DSECT was extended after this use; use the value only after the DSECT "
                      "is complete");
        }
    }
}

static void layout(Asm *a) {
    size_t si, ci, pi;
    for (si = 0; si < a->mod->sections.n; si++) {
        Section *s = a->mod->sections.v[si];
        s->cur = s->text_cur = s->classes.v[0]->pieces.v[0];
        for (ci = 0; ci < s->classes.n; ci++) {
            Class *c = s->classes.v[ci];
            int64_t off = 0;
            for (pi = 0; pi < c->pieces.n; pi++) {
                Piece *p = c->pieces.v[pi];
                if (a->round8 && s->kind != K_DSECT && off % 8 != 0) off += 8 - off % 8;
                p->start = off;
                if (s->kind == K_DSECT) p->start = 0;
                off = p->start + piece_len(p);
            }
            c->len = off;
            if (s->kind != K_DSECT) {
                if (c->len > ZA_MAX_LOC) {
                    diag_help(rep_errorf(a->rep, "ZA3054", span_make(a->mod->name, 0, 0, 0),
                                         "the class %s of section \"%s\" is %lld bytes long; the "
                                         "limit for one section is %lld bytes",
                                         c->name ? c->name : "", s->name, (long long)c->len,
                                         (long long)ZA_MAX_LOC),
                              "GOFF stores class lengths and text offsets in 32 bits");
                    c->len = 0; /* do not allocate; the module is not emitted after an error */
                } else {
                    c->data = xcalloc((size_t)c->len + 1);
                    if (c->attr.fill >= 0) memset(c->data, c->attr.fill, (size_t)c->len);
                }
            }
            for (pi = 0; pi < c->pieces.n; pi++) {
                c->pieces.v[pi]->loc = 0;
                c->pieces.v[pi]->max = 0;
            }
        }
    }
    /* relocatable EQUs: piece bases become class bases */
    for (si = 0; si < a->mod->sym_order.n; si++) {
        Symbol *s = a->mod->sym_order.v[si];
        if (s->kind == SYM_REL) s->val = relayout(s->val);
    }
    resolve_xd(a);
    check_func_uses(a);
    /* ENTRY labels are known to pass 2 (R-type constants need them) */
    for (si = 0; si < a->entries.n; si++) {
        Symbol *s = asm_lookup(a, a->entries.v[si].name);
        if (s && (s->kind == SYM_LABEL || s->kind == SYM_SECT)) s->entry = 1;
    }
}

/* DSECTs named in Q and J constants become external dummy sections */
static void resolve_xd(Asm *a) {
    size_t i;
    for (i = 0; i < a->xdrefs.n; i++) {
        XdRef *r = &a->xdrefs.v[i];
        Symbol *s = asm_lookup(a, r->name);
        Section *d;
        if (!s || s->kind != SYM_SECT || !s->sect || s->sect->kind != K_DSECT) continue;
        if (!r->sect || r->sect->kind == K_DSECT) continue;
        d = s->sect;
        if (!s->xd) {
            s->xd = 1;
            s->xd_sect = d->prv_owner ? d->prv_owner : r->sect; /* IBM HLASM, probes st8 st9 */
            s->xd_len = d->classes.v[0]->len;
            s->xd_align = 8; /* doubleword whatever the DSECT contains (IBM HLASM, probes st8 st9) */
            s->xd_sect->prv = 1;
        }
    }
}

/* ---- the passes -------------------------------------------------------- */

/* ZSYM: where the statement put its bytes (class offsets of the final layout).
 * A statement that switches to another location counter belongs to the
 * counter it enters and covers no bytes. */
static void zsym_record(Asm *a, Stmt *st, Piece *before, int64_t loc_before) {
    ZRec r;
    r.st = st;
    r.pad = 0;
    if (a->cur && a->cur == before) {
        r.cls = a->cur->cls;
        r.off = a->cur->start + loc_before;
        r.len = a->cur->loc - loc_before;
        if (r.len < 0) r.len = 0;
        r.pad = a->lead_pad < r.len ? a->lead_pad : r.len;
    } else {
        r.cls = a->cur ? a->cur->cls : NULL;
        r.off = a->cur ? a->cur->start + a->cur->loc : 0;
        r.len = 0;
    }
    vec_push(&a->mod->zrecs, r);
}

/* the listing's USING table: copy new USINGs, note the line that ended one */
static void list_usings(Asm *a, Stmt *st) {
    size_t i, j;
    for (i = 0; i < a->mod->usings.n; i++) {
        UsingRec *r = &a->mod->usings.v[i];
        int live = 0;
        if (r->dropped.file) continue;
        for (j = 0; j < a->usings.n; j++)
            if (a->usings.v[j].id == r->id) live = 1;
        if (!live) {
            r->dropped.file = st->file;
            r->dropped.line = st->line;
            r->dropped.col = 1;
            r->zend = a->mod->zrecs.n - 1;
        }
    }
    for (j = 0; j < a->usings.n; j++) {
        const Using *u = &a->usings.v[j];
        UsingRec r;
        if (u->id <= a->using_logged) continue;
        memset(&r, 0, sizeof r);
        r.sp = u->sp;
        r.label = u->label;
        r.key = u->key;
        r.off = u->off;
        r.lo = u->lo;
        r.hi = u->hi;
        r.end = u->end;
        r.reg = u->reg;
        r.dependent = u->dependent;
        r.bias = u->bias;
        r.id = u->id;
        r.zbeg = a->mod->zrecs.n - 1;
        r.zend = (size_t)-1;
        vec_push(&a->mod->usings, r);
        if (u->id > a->using_logged) a->using_logged = u->id;
    }
}

static void each(Asm *a) {
    size_t i;
    a->using_logged = 0;
    a->cur = NULL;
    a->sect = NULL;
    a->ended = 0;
    a->usings.n = 0;
    a->using_id = 0;
    a->pool_no = 0;
    a->star_no = 0;
    a->strict = za_strict_registers;
    a->afpr = 1;
    a->print_depth = 0;
    a->using_stack.n = 0;
    a->ac_stack.n = 0;
    memset(&a->opsyn, 0, sizeof a->opsyn);
    for (i = 0; i < a->stmts.n; i++) {
        Stmt *st = a->stmts.v[i];
        Piece *before = a->cur;
        int64_t loc_before = before ? before->loc : 0;
        if (rep_too_many(a->rep)) return;
        if (a->ended) break;
        if (st->bad) continue;
        a->cur_idx = i;
        a->star_len = 0;
        a->lead_open = 1;
        a->lead_pad = 0;
        a->lit_owner = (size_t)-1;
        asm_stmt(a, st);
        if (a->pass == 2) {
            zsym_record(a, st, before, loc_before);
            list_usings(a, st);
        }
    }
    a->pool_piece = NULL;
    a->lead_open = 1;
    a->lead_pad = 0;
    a->lit_owner = (!a->ended || a->mod->zrecs.n == 0) ? a->mod->zrecs.n : a->mod->zrecs.n - 1;
    asm_end_pool(a);
    /* the literal pool written at the end belongs to the END statement (a
     * statement of its own when the source has none) */
    if (a->pass == 2 && a->pool_piece && a->stmts.n) {
        ZRec *r;
        if (!a->ended || a->mod->zrecs.n == 0) {
            Stmt *e = xcalloc(sizeof *e), *last = a->stmts.v[a->stmts.n - 1];
            ZRec z;
            e->file = last->file;
            e->line = last->line;
            e->op = xstrdup("END");
            e->operands = xstrdup("");
            e->name = xstrdup("");
            z.st = e;
            z.cls = NULL;
            z.off = z.len = 0;
            vec_push(&a->mod->zrecs, z);
        }
        r = &a->mod->zrecs.v[a->mod->zrecs.n - 1];
        r->cls = a->pool_piece->cls;
        r->off = a->pool_piece->start + a->pool_loc;
        r->len = a->pool_piece->start + a->pool_piece->loc - r->off;
        r->pad = a->lead_pad < r->len ? a->lead_pad : r->len;
    }
}

/* end-of-assembly checks */
static void finish(Asm *a) {
    size_t i;
    for (i = 0; i < a->entries.n; i++) {
        EntryRef *e = &a->entries.v[i];
        Symbol *s = asm_lookup(a, e->name);
        if (!s || (s->kind != SYM_LABEL && s->kind != SYM_SECT)) {
            rep_errorf(a->rep, "ZA3010", e->sp,
                       "ENTRY symbol \"%s\" is not defined as a label in this module", e->name);
            continue;
        }
        s->entry = 1;
        asm_note_ref(a, s, e->sp);
    }
    if (a->end_entry) {
        Symbol *s = asm_lookup(a, a->end_entry);
        if (!s || (s->kind != SYM_LABEL && s->kind != SYM_SECT && s->kind != SYM_EXT)) {
            rep_errorf(a->rep, "ZA3011", a->end_span,
                       "END entry point \"%s\" is not a label or external symbol", a->end_entry);
        } else if (s->sect && s->sect->kind == K_DSECT) {
            diag_help(rep_errorf(a->rep, "ZA3012", a->end_span,
                                 "END entry point \"%s\" belongs to the DSECT \"%s\"; the entry "
                                 "point must be in a control section",
                                 a->end_entry, s->sect->name),
                      "a DSECT has no text of its own; name a label of a CSECT or RSECT");
        } else {
            a->mod->entry = a->end_entry;
            a->mod->entry_def = a->end_span;
            asm_note_ref(a, s, a->end_span);
        }
    }
    for (i = 0; i < a->symmodes.n; i++) {
        SymMode *m = &a->symmodes.v[i];
        Symbol *s = asm_lookup(a, m->name);
        const char *up = m->is_amode ? "AMODE" : "RMODE";
        if (!s || (!s->entry && s->kind != SYM_SECT)) {
            diag_help(rep_errorf(a->rep, "ZA3040", m->sp,
                                 "%s names \"%s\", which is neither a section nor an ENTRY symbol",
                                 up, m->name),
                      "put the %s statement after the CSECT it applies to, or name an ENTRY label",
                      up);
            continue;
        }
        if (s->kind == SYM_SECT && s->sect) {
            if (m->is_amode) s->sect->amode = m->val; else s->sect->rmode = m->val;
        } else {
            if (m->is_amode) s->amode = m->val; else s->rmode = m->val;
        }
    }
    /* one-sided specifications (IBM HLASM, probe am1): RMODE 31/ANY implies AMODE 31,
     * RMODE 64 AMODE 64 and AMODE 64 RMODE 31 */
    for (i = 0; i < a->mod->sections.n; i++) {
        Section *sc = a->mod->sections.v[i];
        if (sc->kind == K_DSECT) continue;
        if (!sc->amode && sc->rmode == GOFF_RMODE_31) sc->amode = GOFF_AMODE_31;
        if (!sc->amode && sc->rmode == GOFF_RMODE_64) sc->amode = GOFF_AMODE_64;
        if (!sc->rmode && sc->amode == GOFF_AMODE_64) sc->rmode = GOFF_RMODE_31;
    }
    for (i = 0; i < a->xattrs.n; i++) {
        XattrStmt *x = a->xattrs.v[i];
        Symbol *s = asm_lookup(a, x->name);
        XAttr *xa;
        if (!s) {
            diag_help(rep_errorf(a->rep, "ZA3020", x->st->name_span,
                                 "XATTR names \"%s\", which is not defined", x->name),
                      "XATTR applies to an EXTRN/WXTRN symbol, an ENTRY label or a CATTR PART "
                      "name");
            continue;
        }
        asm_note_ref(a, s, x->st->name_span);
        xa = xmalloc(sizeof *xa);
        *xa = x->attr;
        s->xattr = xa;
    }
    if (a->mod->sections.n == 0 && rep_error_count(a->rep) == 0) {
        diag_help(rep_errorf(a->rep, "ZA3002", span_make(a->mod->name, 0, 0, 0),
                             "the source defines no control section"),
                  "add at least one CSECT");
    }
}

static void run(Asm *a) {
    a->pass = 1;
    each(a);
    if (rep_too_many(a->rep)) return;
    layout(a);
    a->pass = 2;
    a->laid = 1;
    each(a);
    finish(a);
}

int za_strict_registers;

static Asm *assemble_once(const SrcLine *lines, size_t n, const char *name, Reporter *rep,
                          StrMap *hints, int round8) {
    Asm *a = xcalloc(sizeof *a);
    a->rep = rep;
    a->scratch = rep_new(0);
    a->strict = za_strict_registers;
    a->hints = hints;
    a->round8 = round8;
    a->mod = xcalloc(sizeof *a->mod);
    a->mod->name = xstrdup(name);
    read_statements(&a->stmts, lines, n, rep);
    a->mod->stmts = a->stmts;
    run(a);
    return a;
}

/* Location counters (LOCTR) do not force alignment: a piece starts at the end of
 * the one before it and its items align to the final address (HLASM, checked with
 * the oracle probes lc1, lc2, st2, st5).  The start of a piece is only known
 * after the whole source was read, so the assembly is repeated with the starts of
 * the previous run as hints until they agree (modulo 4096, the largest alignment);
 * this is only needed when a class has several pieces. */
static StrMap *piece_starts(const Module *m, int *settled) {
    StrMap *h = xcalloc(sizeof *h);
    size_t si, ci, pi;
    *settled = 1;
    for (si = 0; si < m->sections.n; si++) {
        const Section *s = m->sections.v[si];
        for (ci = 0; ci < s->classes.n; ci++) {
            const Class *c = s->classes.v[ci];
            for (pi = 0; pi < c->pieces.n; pi++) {
                const Piece *p = c->pieces.v[pi];
                int64_t *v = xmalloc(sizeof *v);
                char *key = xasprintf("%s\x01%s\x01%s", s->name, c->name, p->name);
                *v = s->kind == K_DSECT ? 0 : p->start;
                strmap_put(h, key, v);
                if ((*v - p->hint) % 4096 != 0) *settled = 0;
            }
        }
    }
    return h;
}

Module *asm_assemble_all(const SrcLine *lines, size_t n, const char *name, Reporter *rep) {
    StrMap *hints = NULL;
    size_t mark = rep_mark(rep);
    int it;
    for (it = 0; it < 5; it++) {
        Asm *a = assemble_once(lines, n, name, rep, hints, 0);
        int settled;
        StrMap *h = piece_starts(a->mod, &settled);
        if (settled || rep_too_many(rep)) return a->mod;
        hints = h;
        rep_rewind(rep, mark);
    }
    return assemble_once(lines, n, name, rep, NULL, 1)->mod;
}

Module *asm_assemble(const SrcLine *lines, size_t n, const char *name, Reporter *rep) {
    Module *m = asm_assemble_all(lines, n, name, rep);
    return rep_error_count(rep) > 0 ? NULL : m;
}

Module *asm_assemble_text(const char *file, const char *text, size_t len, Reporter *rep) {
    SrcVec lines = {0};
    rep_add_source(rep, file, text, len);
    src_split(&lines, file, text, len);
    return asm_assemble(lines.v, lines.n, file, rep);
}

/* ---- goff attribute helpers --------------------------------------------- */

int amode_byte(const char *s, uint8_t *out) {
    char *u = za_strupper(s);
    int ok = 1;
    if (!strcmp(u, "24"))
        *out = GOFF_AMODE_24;
    else if (!strcmp(u, "31"))
        *out = GOFF_AMODE_31;
    else if (!strcmp(u, "64"))
        *out = GOFF_AMODE_64;
    else if (!strcmp(u, "ANY"))
        *out = GOFF_AMODE_ANY;
    else if (!strcmp(u, "MIN"))
        *out = GOFF_AMODE_MIN;
    else
        ok = 0;
    free(u);
    return ok;
}

int rmode_byte(const char *s, uint8_t *out) {
    char *u = za_strupper(s);
    int ok = 1;
    if (!strcmp(u, "24"))
        *out = GOFF_RMODE_24;
    else if (!strcmp(u, "31") || !strcmp(u, "ANY"))
        *out = GOFF_RMODE_31;
    else if (!strcmp(u, "64"))
        *out = GOFF_RMODE_64;
    else
        ok = 0;
    free(u);
    return ok;
}
