/* instr.c - machine instructions: operand slots, base/displacement
 * resolution through USING, registers, relative branches and encoding. */
#include <stdlib.h>
#include <string.h>

#include "asm.h"


typedef IsaSlot Slot;
typedef IsaSlots Slots;
#define slots_of isa_slots

/* number of operands that must be written */
static int min_slots(const IsaInst *in, int n) {
    return n - in->opt;
}

static const char *reg_letter(unsigned f) {
    if (f & F_VR) return "V";
    if (f & F_FPR) return "F";
    if (f & F_AR) return "A";
    if (f & F_CR) return "C";
    return "R";
}

static char reg_prefix(unsigned f) { return reg_letter(f)[0]; }

static const char *reg_kind(unsigned f) {
    if (f & F_VR) return "vector";
    if (f & F_FPR) return "floating-point";
    if (f & F_AR) return "access";
    if (f & F_CR) return "control";
    return "general";
}

/* operand pattern of a variant, for diagnostics */
static char *syntax(const IsaInst *in) {
    Slots sl;
    Buf b = {0};
    int i;
    slots_of(in, &sl);
    for (i = 0; i < sl.n; i++) {
        const Slot *s = &sl.s[i];
        unsigned f = isa_op(in, (size_t)s->field)->flags;
        if (i) buf_putc(&b, ',');
        switch (s->kind) {
        case SK_ADDR:
            buf_putc(&b, 'D');
            if (s->mid >= 0 && s->base >= 0)
                buf_printf(&b, "(%c,B)", s->mid_kind);
            else if (s->base >= 0)
                buf_puts(&b, "(B)");
            break;
        case SK_REL:
            buf_puts(&b, "target");
            break;
        case SK_IMM:
            buf_puts(&b, (f & F_SIGNED) ? "I" : "U");
            break;
        default:
            buf_puts(&b, reg_letter(f));
        }
    }
    return xstrdup(buf_cstr(&b));
}

/* ---- USING ---------------------------------------------------------------- */

static Base base_key(Base b) {
    if (b.kind == B_PIECE) return base_class(((Piece *)b.p)->cls);
    return b;
}

/* a GNU style register operand (%r1): checked when the instruction is encoded */
static int is_pct_reg(Operand o) { return o.text[strspn(o.text, " \t")] == '%'; }

int asm_parse_pct(const char *t, char *kind, int *num) {
    size_t tl = strlen(t), i;
    if (tl < 3 || tl > 4 || t[0] != '%' || !strchr("rfacvRFACV", t[1])) return 0;
    for (i = 2; i < tl; i++)
        if (!za_isdigit((unsigned char)t[i])) return 0;
    *kind = (char)za_toupper((unsigned char)t[1]);
    *num = atoi(t + 2);
    return 1;
}

int asm_reg_max(char kind) { return kind == 'V' ? 31 : 15; }

static unsigned kind_flags(char kind) {
    return kind == 'V' ? F_VR : kind == 'F' ? F_FPR : kind == 'A' ? F_AR : kind == 'C' ? F_CR : F_GPR;
}

/* the symbol an operand names when it is a register name (NAME EQU %r1), else NULL */
static Symbol *reg_symbol(Asm *a, const char *t) {
    const char *err;
    char *n = sym_text(t, &err);
    Symbol *s = n ? asm_lookup(a, n) : NULL;
    return s && s->regkind ? s : NULL;
}

/* the diagnostic of strict registers: o is a number or an ordinary symbol where a register
 * is expected */
static void strict_register_error(Asm *a, Stmt *st, Operand o, const char *t, char kind) {
    Diag *d;
    char *fix = NULL;
    size_t i, n = strlen(t);
    int digits = n > 0 && n <= 2;
    for (i = 0; i < n; i++)
        if (!za_isdigit((unsigned char)t[i])) digits = 0;
    if (digits)
        fix = xasprintf("%%%c%s", za_tolower((unsigned char)kind), t);
    d = rep_errorf(a->rep, "ZA2067", asm_op_span(st, o), "\"%s\" is not written as a register", t);
    if (fix) diag_help(d, "write %s", fix);
    diag_help(d, "with -strict-registers a register is written %%%c<n>, or is a name defined "
                 "with `NAME EQU %%%c<n>`", za_tolower((unsigned char)kind),
              za_tolower((unsigned char)kind));
}

/* a register: %r1 (GNU style; the prefix says which kind), a name made with
 * EQU %r1, or, unless strict registers are on, an absolute expression: a
 * number or a name the program defined (R1 EQU 1). Bare R1 is NOT predefined:
 * it is an ordinary symbol, so it must be defined. */
static int asm_register(Asm *a, Stmt *st, Operand o, unsigned flags, int64_t *out) {
    char *t = str_trim(o.text);
    size_t lead = 0;
    Operand to;
    int ok;
    Symbol *rs;
    if (t[0] == '%') {
        /* GNU style register: %r1 %f0 %a2 %c3 %v31 */
        char kind;
        int num;
        if (!asm_parse_pct(t, &kind, &num)) {
            diag_help(rep_errorf(a->rep, "ZA2054", asm_op_span(st, o),
                                 "\"%s\" is not a register name", t),
                      "GNU style registers are %%r0-%%r15, %%f0-%%f15, %%a0-%%a15, %%c0-%%c15 and "
                      "%%v0-%%v31");
            free(t);
            return 0;
        }
        if (kind != reg_prefix(flags)) {
            diag_help(rep_errorf(a->rep, "ZA2055", asm_op_span(st, o),
                                 "%s is not a %s register", t, reg_kind(flags)),
                      "this operand takes a %s register (%%%c<n>)", reg_kind(flags),
                      (char)za_tolower((unsigned char)reg_prefix(flags)));
            free(t);
            return 0;
        }
        *out = num;
        free(t);
        return 1;
    }
    rs = reg_symbol(a, t);
    if (rs) { /* NAME EQU %r1 */
        if (rs->regkind != reg_prefix(flags)) {
            diag_help(rep_errorf(a->rep, "ZA2055", asm_op_span(st, o),
                                 "\"%s\" is a %s register name, but this operand takes a %s register",
                                 t, reg_kind(kind_flags(rs->regkind)), reg_kind(flags)),
                      "this operand takes a %s register (%%%c<n>)", reg_kind(flags),
                      (char)za_tolower((unsigned char)reg_prefix(flags)));
            free(t);
            return 0;
        }
        if (a->pass == 2) asm_note_ref(a, rs, asm_op_span(st, o));
        *out = rs->val.abs;
        free(t);
        return 1;
    }
    if (a->strict) {
        strict_register_error(a, st, o, t, reg_prefix(flags));
        free(t);
        return 0;
    }
    while (o.text[lead] && za_isspace((unsigned char)o.text[lead])) lead++;
    to.text = t;
    to.off = o.off + (int)lead;
    ok = asm_abs_expr(a, st, to, "a register number", out);
    return ok;
}

/* ---- USING and DROP (DESIGN.md 4.3) ---------------------------------------- */

#define DISP_LONG_MIN (-((int64_t)1 << 19))
#define DISP_LONG_MAX (((int64_t)1 << 19) - 1)

int asm_using_label_active(Asm *a, const char *name) {
    size_t i;
    for (i = 0; i < a->usings.n; i++)
        if (a->usings.v[i].label && strcmp(a->usings.v[i].label, name) == 0) return 1;
    return 0;
}

/* a register written as %r1: never an address */
static int is_register_name(Operand o) { return is_pct_reg(o); }

/* the qualifier of the first qualified symbol of e, or NULL */
static const char *expr_qual(const Expr *e) {
    const char *q;
    if (!e) return NULL;
    if (e->kind == EX_SYM) return e->qual;
    if (e->kind == EX_BIN || e->kind == EX_UN) {
        q = expr_qual(e->l);
        if (q) return q;
        return e->kind == EX_BIN ? expr_qual(e->r) : NULL;
    }
    return NULL;
}

typedef struct {
    int is_addr; /* a relocatable address (dependent USING, DROP address) */
    int64_t reg;
    Value v;
    const Expr *e;
} UsingOp;

/* classify a USING or DROP operand: a register number or an address; 0 after
 * an error (reported) */
static int using_operand(Asm *a, Stmt *st, Operand o, UsingOp *r) {
    memset(r, 0, sizeof *r);
    char *t = str_trim(o.text);
    int named = reg_symbol(a, t) != NULL;
    if (is_register_name(o) || named) {
        free(t);
        if (!asm_register(a, st, o, F_GPR, &r->reg)) return 0;
    } else {
        free(t);
        r->e = asm_expr(a, st, o);
        if (!r->e || !asm_eval(a, st, r->e, 1, &r->v)) return 0;
        if (!value_is_abs(&r->v)) {
            r->is_addr = 1;
            return 1;
        }
        if (a->strict) {
            char *t2 = str_trim(o.text);
            strict_register_error(a, st, o, t2, 'R');
            return 0;
        }
        r->reg = r->v.abs;
    }
    if (r->reg < 0 || r->reg > 15) {
        rep_errorf(a->rep, "ZA2041", asm_op_span(st, o), "register %lld is out of range 0 to 15",
                   (long long)r->reg);
        return 0;
    }
    return 1;
}

/* the key and offset of an address; 0 after an error */
static int using_address(Asm *a, const Expr *e, const Value *v, Base *key, int64_t *off) {
    Base b;
    memset(key, 0, sizeof *key);
    *off = v->abs;
    if (value_is_abs(v)) return 1;
    if (!value_simple(v, &b)) {
        rep_errorf(a->rep, "ZA3062", e->sp,
                   "USING operand must be a simple address in one section");
        return 0;
    }
    if (b.kind == B_SYM) {
        rep_errorf(a->rep, "ZA3061", e->sp,
                   "USING needs an address in this module, not an external symbol");
        return 0;
    }
    *key = base_key(b);
    return 1;
}

/* an end or limit operand of USING: in the section of the base (or absolute
 * when the base is); 0 after an error */
static int using_limit(Asm *a, Stmt *st, Operand o, Base key, int none_ok, int64_t *out,
                       int *none) {
    char *t = str_trim(o.text);
    Expr *e;
    Value v;
    Base k;
    int64_t off;
    int empty = t[0] == 0;
    free(t);
    *none = 0;
    if (empty) {
        *none = 1;
        return 1;
    }
    e = asm_expr(a, st, o);
    if (!e || !asm_eval(a, st, e, 1, &v)) return 0;
    if (none_ok && value_is_abs(&v) && v.abs == 0x80000000LL) { /* no limit */
        *none = 1;
        return 1;
    }
    if (!using_address(a, e, &v, &k, &off)) return 0;
    if (!base_eq(k, key)) {
        diag_help(rep_errorf(a->rep, "ZA3064", e->sp,
                             "the limit of a USING must be in the same section as its base"),
                  key.kind == B_NONE ? "the base is absolute, so the limit must be absolute"
                                     : "write it as an address, for example `BASE+4096`");
        return 0;
    }
    *out = off;
    return 1;
}

/* mark entry i and the USINGs that depend on it as ended */
static void using_kill(Asm *a, size_t i) {
    size_t j;
    Using *u = &a->usings.v[i];
    u->dead = 1;
    if (!u->dependent)
        for (j = 0; j < a->usings.n; j++)
            if (a->usings.v[j].root == u->id) a->usings.v[j].dead = 1;
}

static void using_compact(Asm *a) {
    size_t i, kept = 0;
    for (i = 0; i < a->usings.n; i++)
        if (!a->usings.v[i].dead) a->usings.v[kept++] = a->usings.v[i];
    a->usings.n = kept;
}

/* The USING that resolves offset t of key, or -1.  qual is the label of a
 * qualified symbol; an unqualified symbol only uses unlabeled USINGs.  The
 * smallest non-negative displacement wins (negative ones only as a last
 * resort); ranges that coincide use the higher register. */
static long find_using(Asm *a, Base key, int64_t t, const char *qual, int64_t lo, int64_t hi,
                       int64_t *disp) {
    long best = -1;
    int64_t brank = 0;
    int uns = lo >= 0;
    size_t i;
    for (i = a->usings.n; i-- > 0;) {
        const Using *u = &a->usings.v[i];
        int64_t d, rank;
        if (qual ? (!u->label || strcmp(u->label, qual) != 0) : u->label != NULL) continue;
        if (!base_eq(u->key, key)) continue;
        if (t < u->lo || t >= u->hi || (uns && t >= u->end)) continue;
        d = t - u->off + u->bias;
        if (d < lo || d > hi) continue;
        rank = d < 0 ? ((int64_t)1 << 40) - d : d;
        if (best < 0 || rank < brank || (rank == brank && u->reg > a->usings.v[best].reg)) {
            best = (long)i;
            brank = rank;
            *disp = d;
        }
    }
    return best;
}

/* explain a failed USING lookup */
static void no_addressability(Asm *a, const Expr *e, Value v, const char *qual, int64_t lo,
                              int64_t hi) {
    char *desc = expr_describe(e);
    Diag *d = diag_label(
        rep_errorf(a->rep, "ZA2030", e->sp, "no USING gives addressability to %s", desc),
        "this address is not covered");
    Base b, key;
    int near = 0, any = 0;
    size_t i;
    if (value_is_abs(&v)) {
        memset(&key, 0, sizeof key);
    } else if (value_simple(&v, &b)) {
        key = base_key(b);
    } else {
        diag_help(d, "the operand is not a simple address; give an explicit displacement and base "
                     "register, for example `0(%r12)`");
        return;
    }
    for (i = 0; i < a->usings.n; i++) {
        const Using *u = &a->usings.v[i];
        if (qual ? (!u->label || strcmp(u->label, qual) != 0) : u->label != NULL) continue;
        any = 1;
        if (base_eq(u->key, key)) {
            int64_t dd = v.abs - u->off + u->bias;
            near = 1;
            diag_note(d, u->sp,
                      "register %d is based here, but the displacement would be %lld (allowed %lld "
                      "to %lld)",
                      u->reg, (long long)dd, (long long)lo, (long long)hi);
        }
    }
    if (!near) {
        if (!value_is_abs(&v) && b.kind == B_SYM)
            diag_help(d,
                      "\"%s\" is external; load its address with a V- or A-type constant and use "
                      "it through a base register",
                      desc);
        else if (qual)
            diag_help(d, "no USING labeled \"%s\" covers the section of this address", qual);
        else if (!any)
            diag_help(d, "there is no active USING; add `USING *,R12` (or the register of your "
                         "choice) after loading the base register");
        else
            diag_help(d, "no active USING refers to the section/class of this address");
    }
}

void dir_using(Asm *a, Stmt *st) {
    OperandVec ops = {0}, rng = {0};
    Operand base_op;
    Expr *e;
    Value v;
    Base key, k2;
    int64_t off, lo = INT64_MIN, hi = INT64_MAX, end = INT64_MAX, parent_disp = 0;
    int none, dependent = 0, root = 0, parent_reg = 0;
    long parent = -1;
    char *t0;
    size_t i, nregs = 0;
    int regs[16];
    UsingOp o1;
    if (!asm_ops(a, st, 2, 17, &ops)) return;
    /* the first operand: BASE or (BASE,end,lower,upper) */
    t0 = str_trim(ops.v[0].text);
    if (t0[0] == '(' && match_paren(t0, 0) == (long)strlen(t0) - 1) {
        size_t lead = 0, k;
        char *inner = xstrndup(t0 + 1, strlen(t0) - 2);
        OperandVec tmp = {0};
        while (ops.v[0].text[lead] && za_isspace((unsigned char)ops.v[0].text[lead])) lead++;
        split_operands(&tmp, inner);
        for (k = 0; k < tmp.n; k++) {
            tmp.v[k].off += ops.v[0].off + (int)lead + 1;
            vec_push(&rng, tmp.v[k]);
        }
        if (rng.n < 1 || rng.n > 4) {
            if (a->pass == 1) {
                rep_errorf(a->rep, "ZA3063", asm_op_span(st, ops.v[0]),
                           "the first operand of USING takes 1 to 4 items: (base,end,lower,upper)");
                st->bad = 1;
            }
            return;
        }
    } else {
        vec_push(&rng, ops.v[0]);
    }
    base_op = rng.v[0];
    if (a->pass == 1) {
        if (st->name[0] && !asm_valid_stmt_name(st)) {
            diag_help(rep_errorf(a->rep, "ZA1021", st->name_span,
                                 "\"%s\" is not a valid symbol name", st->name),
                      "a USING label follows the rules of symbol names");
            st->bad = 1;
            return;
        }
        for (i = 0; i < rng.n; i++) {
            char *t = str_trim(rng.v[i].text);
            if (t[0]) asm_expr(a, st, rng.v[i]);
            free(t);
        }
        for (i = 1; i < ops.n; i++)
            if (!is_register_name(ops.v[i])) asm_expr(a, st, ops.v[i]);
        return;
    }
    /* pass 2 */
    e = asm_expr(a, st, base_op);
    if (!e || !asm_eval(a, st, e, 1, &v)) return;
    if (!using_address(a, e, &v, &key, &off)) return;
    for (i = 1; i < rng.n; i++) {
        int64_t x;
        if (!using_limit(a, st, rng.v[i], key, i >= 2, &x, &none)) return;
        if (none) continue;
        if (i == 1) {
            if (x <= off) {
                rep_errorf(a->rep, "ZA3065", asm_op_span(st, rng.v[i]),
                           "the end address must be greater than the base");
                return;
            }
            end = x;
        } else if (i == 2) {
            lo = x;
        } else {
            hi = x;
        }
    }
    if (lo != INT64_MIN && hi != INT64_MAX && hi <= lo) {
        rep_errorf(a->rep, "ZA3065", asm_op_span(st, rng.v[3]),
                   "the upper limit must be greater than the lower limit");
        return;
    }
    if (!using_operand(a, st, ops.v[1], &o1)) return;
    if (o1.is_addr) {
        /* dependent USING: based on the USING that resolves this address */
        int64_t t2;
        const char *q = expr_qual(o1.e);
        if (ops.n > 2) {
            rep_errorf(a->rep, "ZA3066", asm_op_span(st, ops.v[2]),
                       "a dependent USING takes one address, not several operands");
            return;
        }
        if (!using_address(a, o1.e, &o1.v, &k2, &t2)) return;
        parent = find_using(a, k2, t2, q, DISP_LONG_MIN, DISP_LONG_MAX, &parent_disp);
        if (parent < 0) {
            no_addressability(a, o1.e, o1.v, q, DISP_LONG_MIN, DISP_LONG_MAX);
            return;
        }
        dependent = 1;
        parent_reg = a->usings.v[parent].reg;
        root = a->usings.v[parent].root;
    } else {
        regs[nregs++] = (int)o1.reg;
        for (i = 2; i < ops.n; i++) {
            UsingOp o;
            if (!using_operand(a, st, ops.v[i], &o)) return;
            if (o.is_addr) {
                rep_errorf(a->rep, "ZA3066", asm_op_span(st, ops.v[i]),
                           "a base register is expected here, not an address");
                return;
            }
            regs[nregs++] = (int)o.reg;
        }
    }
    if (st->name[0]) {
        size_t j;
        if (asm_lookup(a, st->name)) {
            diag_help(rep_errorf(a->rep, "ZA3067", st->name_span,
                                 "\"%s\" is a USING label and also a symbol of the program",
                                 st->name),
                      "a USING label can be used only as a qualifier; choose another name");
            return;
        }
        for (j = 0; j < a->usings.n; j++) /* the same label again ends the prior USING */
            if (a->usings.v[j].label && strcmp(a->usings.v[j].label, st->name) == 0)
                using_kill(a, j);
        using_compact(a);
    }
    if (dependent) {
        Using u;
        memset(&u, 0, sizeof u);
        u.label = st->name[0] ? xstrdup(st->name) : NULL;
        u.key = key;
        u.off = off;
        u.lo = lo;
        u.hi = hi;
        u.end = end;
        u.reg = parent_reg;
        u.bias = parent_disp;
        u.dependent = 1;
        u.id = ++a->using_id;
        u.root = root;
        u.sp = asm_op_span(st, ops.v[0]);
        vec_push(&a->usings, u);
        return;
    }
    for (i = 0; i < nregs; i++) {
        Using u;
        memset(&u, 0, sizeof u);
        if (!st->name[0]) { /* a USING of the same register ends the prior one */
            size_t j;
            for (j = 0; j < a->usings.n; j++)
                if (!a->usings.v[j].label && !a->usings.v[j].dependent &&
                    a->usings.v[j].reg == regs[i])
                    using_kill(a, j);
            using_compact(a);
        }
        u.label = st->name[0] ? xstrdup(st->name) : NULL;
        u.key = key;
        u.off = off + (int64_t)i * 4096;
        u.lo = lo;
        u.hi = hi;
        u.end = end;
        u.reg = regs[i];
        u.id = ++a->using_id;
        u.root = u.id;
        u.sp = asm_op_span(st, ops.v[0]);
        vec_push(&a->usings, u);
    }
}

void dir_drop(Asm *a, Stmt *st) {
    OperandVec ops = {0};
    size_t i, j;
    if (a->pass != 2) return;
    split_operands(&ops, st->operands);
    if (ops.n == 0) {
        a->usings.n = 0;
        return;
    }
    for (i = 0; i < ops.n; i++) {
        UsingOp o;
        char *t = str_trim(ops.v[i].text);
        const char *err = NULL;
        char *name = t[0] ? sym_text(t, &err) : NULL;
        int found = 0;
        if (name && asm_using_label_active(a, name)) {
            for (j = 0; j < a->usings.n; j++)
                if (a->usings.v[j].label && strcmp(a->usings.v[j].label, name) == 0)
                    using_kill(a, j);
            using_compact(a);
            continue;
        }
        if (!using_operand(a, st, ops.v[i], &o)) continue;
        if (o.is_addr) {
            Base k;
            int64_t t2, d;
            long at;
            const char *q = expr_qual(o.e);
            if (!using_address(a, o.e, &o.v, &k, &t2)) continue;
            at = find_using(a, k, t2, q, DISP_LONG_MIN, DISP_LONG_MAX, &d);
            if (at < 0)
                rep_warnf(a->rep, "ZA3060", asm_op_span(st, ops.v[i]),
                          "DROP of an address that no active USING covers");
            else
                using_kill(a, (size_t)at);
        } else {
            for (j = 0; j < a->usings.n; j++) {
                const Using *u = &a->usings.v[j];
                if (!u->label && !u->dependent && u->reg == (int)o.reg) {
                    using_kill(a, j);
                    found = 1;
                }
            }
            if (!found)
                rep_warnf(a->rep, "ZA3060", asm_op_span(st, ops.v[i]),
                          "DROP of register %d, which has no active USING", (int)o.reg);
        }
        using_compact(a);
    }
}

/* ---- parsing of operand text ----------------------------------------------- */

AddrOp asm_split_addr(Operand o) {
    AddrOp r;
    const char *t = o.text;
    size_t tl = strlen(t), i, plen;
    int depth = 0, in_q = 0;
    long cand = -1;
    OperandVec in = {0};
    char *inner;
    memset(&r, 0, sizeof r);
    r.disp = o;
    if (!tl || t[tl - 1] != ')') return r;
    /* find the top-level '(' that closes at the end */
    for (i = 0; i < tl; i++) {
        char c = t[i];
        if (in_q) {
            if (c == '\'') {
                if (t[i + 1] == '\'')
                    i++;
                else
                    in_q = 0;
            }
            continue;
        }
        switch (c) {
        case '\'':
            if (!is_attr_quote(t, i)) in_q = 1;
            break;
        case '"': {
            long j = close_dquote(t, i);
            if (j >= 0) i = (size_t)j;
            break;
        }
        case '(': {
            size_t w = i;
            while (w > 0 && is_sym_char((unsigned char)t[w - 1])) w--;
            if (depth == 0 && !expr_is_func_name(t + w, i - w)) cand = (long)i;
            depth++;
            break;
        }
        case ')':
            depth--;
            break;
        }
    }
    if (cand <= 0 || match_paren(t, (size_t)cand) != (long)tl - 1) return r;
    plen = (size_t)cand;
    if (strchr("+-*/(", t[plen - 1]) && !(plen == 1 && t[0] == '*')) return r;
    r.disp.text = xstrndup(t, plen);
    r.disp.off = o.off;
    r.parens = 1;
    inner = xstrndup(t + cand + 1, tl - (size_t)cand - 2);
    split_operands(&in, inner);
    for (i = 0; i < in.n; i++) {
        in.v[i].off += o.off + (int)cand + 1;
        vec_push(&r.inner, in.v[i]);
    }
    free(inner);
    return r;
}

/* ---- the instruction statement ----------------------------------------------- */

/* pick the variant of an instruction that matches n operands */
const IsaInst *asm_choose_variant(const IsaInst *vars, size_t nv, int n) {
    const IsaInst *best = NULL;
    size_t k;
    for (k = 0; k < nv; k++) {
        const IsaInst *in = &vars[k];
        Slots si, sb;
        int ns;
        slots_of(in, &si);
        ns = si.n;
        if (n > ns || n < min_slots(in, ns)) continue;
        if (!best) {
            best = in;
            continue;
        }
        slots_of(best, &sb);
        if ((ns == n) != (sb.n == n)) {
            if (ns == n) best = in;
            continue;
        }
        if (!strncmp(in->format, "RXY", 3) || !strncmp(in->format, "RSY", 3)) best = in;
    }
    return best;
}

static void bad_arity(Asm *a, Stmt *st, const IsaInst *vars, size_t nv, int n) {
    Buf forms = {0};
    size_t k, j;
    Span sp = st->op_span;
    char *up;
    Diag *d;
    size_t nforms = 0;
    char **seen = xmalloc((nv + 1) * sizeof *seen);
    if (a->pass != 1) return;
    up = za_strupper(st->op);
    for (k = 0; k < nv; k++) {
        char *s = syntax(&vars[k]);
        int dup = 0;
        for (j = 0; j < nforms; j++)
            if (!strcmp(seen[j], s)) dup = 1;
        if (dup) continue;
        seen[nforms++] = s;
        if (forms.len) buf_puts(&forms, "   or   ");
        buf_printf(&forms, "%s %s", up, s);
    }
    if (n > 0) sp = stmt_operand_span(st, 0, (int)strlen(st->operands));
    d = rep_errorf(a->rep, "ZA2031", sp, "%s does not take %d operand(s)", up, n);
    diag_help(d, "syntax: %s", buf_cstr(&forms));
    st->bad = 1;
}

/* check the syntax of the operands in pass 1 */
static void parse_operands(Asm *a, Stmt *st, const IsaInst *in, const OperandVec *ops) {
    Slots sl;
    size_t i, k;
    char *up = za_strupper(st->op);
    slots_of(in, &sl);
    for (i = 0; i < ops->n; i++) {
        Operand o = ops->v[i];
        const Slot *s = &sl.s[i];
        AddrOp ad;
        int max_inner;
        char *tr = str_trim(o.text);
        if (!tr[0]) {
            char *syn = syntax(in);
            diag_help(rep_errorf(a->rep, "ZA2032", asm_op_span(st, o), "operand %d is empty",
                                 (int)i + 1),
                      "syntax: %s %s", up, syn);
            st->bad = 1;
            continue;
        }
        if (s->kind != SK_ADDR) {
            if (!is_pct_reg(o)) asm_expr(a, st, o);
            continue;
        }
        ad = asm_split_addr(o);
        if (!ad.disp.text[0]) {
            diag_help(rep_errorf(a->rep, "ZA2033", asm_op_span(st, o),
                                 "the displacement before the parenthesis is missing"),
                      "write for example `0(%r2)`");
            st->bad = 1;
            continue;
        }
        asm_expr(a, st, ad.disp);
        for (k = 0; k < ad.inner.n; k++) {
            char *t2 = str_trim(ad.inner.v[k].text);
            if (t2[0] && !is_pct_reg(ad.inner.v[k])) asm_expr(a, st, ad.inner.v[k]);
        }
        max_inner = (s->mid >= 0 && s->base >= 0) ? 2 : 1;
        if ((int)ad.inner.n > max_inner) {
            char *syn = syntax(in);
            diag_help(rep_errorf(a->rep, "ZA2034", asm_op_span(st, o),
                                 "too many registers in parentheses"),
                      "syntax: %s %s", up, syn);
            st->bad = 1;
        }
    }
}

/* length attribute of the leftmost symbol of e (1 if none) */
static int64_t expr_len(Asm *a, const Expr *e) {
    switch (e->kind) {
    case EX_SYM: {
        Symbol *s = asm_lookup(a, e->name);
        if (s && s->len > 0) return s->len;
        break;
    }
    case EX_BIN:
    case EX_UN:
        return expr_len(a, e->l);
    default:
        break;
    }
    return 1;
}

typedef struct {
    int byte_off;
    int n;
    Symbol *target;
    Class *cls;
} RelImm;

static int check_reg(Asm *a, Span sp, const IsaOperand *f, int64_t r, int64_t *vals, int idx) {
    int64_t max = 15;
    if (f->flags & F_VR) max = 31;
    if (r < 0 || r > max) {
        rep_errorf(a->rep, "ZA2041", sp, "register %lld is out of range 0 to %lld", (long long)r,
                   (long long)max);
        return 0;
    }
    if (!a->afpr && (f->flags & F_FPR) && !(f->flags & F_VR) && (r > 6 || (r & 1))) {
        diag_help(rep_errorf(a->rep, "ZA2071", sp, "floating-point register %lld is not available",
                             (long long)r),
                  "ACONTROL NOAFPR is on: only registers 0, 2, 4 and 6 can be used; ACONTROL AFPR "
                  "allows all 16");
        return 0;
    }
    vals[idx] = r;
    return 1;
}

static int reg_field(Asm *a, Stmt *st, Operand o, const IsaOperand *f, int64_t *vals, int idx) {
    int64_t r;
    if (!asm_register(a, st, o, f->flags, &r)) return 0;
    return check_reg(a, asm_op_span(st, o), f, r, vals, idx);
}

static int imm_field(Asm *a, Stmt *st, Operand o, const IsaOperand *f, int64_t *vals, int idx) {
    int64_t v, lo = 0, hi;
    unsigned bits = f->bits;
    if (!asm_abs_expr(a, st, o, "an immediate operand", &v)) return 0;
    hi = bits >= 63 ? INT64_MAX : ((int64_t)1 << bits) - 1;
    if (f->flags & F_SIGNED) lo = -((int64_t)1 << (bits - 1));
    if (v < lo || v > hi) {
        rep_errorf(a->rep, "ZA2043", asm_op_span(st, o),
                   "value %lld does not fit this %u-bit operand (allowed %lld to %lld)",
                   (long long)v, bits, (long long)lo, (long long)hi);
        return 0;
    }
    vals[idx] = v;
    return 1;
}

static int rel_field(Asm *a, Stmt *st, Operand o, const IsaOperand *f, int64_t *vals, int idx,
                     RelImm **ri_out) {
    Expr *e = asm_expr(a, st, o);
    Value v;
    int64_t here, lo, hi, diff;
    Base b;
    RelImm *r;
    *ri_out = NULL;
    if (!e) return 0;
    if (!asm_eval(a, st, e, 1, &v)) return 0;
    here = a->cur->start + a->cur->loc;
    lo = -((int64_t)1 << (f->bits - 1));
    hi = ((int64_t)1 << (f->bits - 1)) - 1;
    if (!value_simple(&v, &b)) {
        Diag *d = rep_errorf(a->rep, "ZA2044", e->sp, "a relative branch needs a simple address operand");
        if (value_is_abs(&v))
            diag_help(d, "write the target as a label or as `*+n` (n bytes from this instruction)");
        return 0;
    }
    if (base_eq(b, base_class(a->cur->cls))) {
        diff = v.abs - here;
        if (diff % 2 != 0) {
            diag_help(rep_errorf(a->rep, "ZA2045", e->sp,
                                 "the branch target is %lld bytes away, which is odd",
                                 (long long)diff),
                      "instructions are halfword aligned");
            return 0;
        }
        if (diff / 2 < lo || diff / 2 > hi) {
            diag_help(rep_errorf(a->rep, "ZA2046", e->sp,
                                 "the branch target is out of range (%lld halfwords, allowed %lld "
                                 "to %lld)",
                                 (long long)(diff / 2), (long long)lo, (long long)hi),
                      "use the long form (for example BRCL or JLU / BRASL) or a base register");
            return 0;
        }
        vals[idx] = diff / 2;
        return 1;
    }
    /* different class or external symbol: leave it to the binder */
    if (f->bits != 16 && f->bits != 32) {
        diag_help(rep_errorf(a->rep, "ZA2047", e->sp, "this branch operand cannot be relocated"),
                  "only 16 and 32-bit relative operands can refer to another class or an external "
                  "symbol");
        return 0;
    }
    if (b.kind == B_PIECE) {
        rep_errorf(a->rep, "ZA2048", e->sp, "cannot branch to an address in a dummy section");
        return 0;
    }
    r = xcalloc(sizeof *r);
    r->byte_off = f->shift / 8;
    r->n = f->bits / 8;
    if (b.kind == B_CLASS)
        r->cls = b.p;
    else if (b.kind == B_SYM)
        r->target = b.p;
    diff = v.abs - here;
    if (diff % 2 != 0) {
        rep_errorf(a->rep, "ZA2045", e->sp, "the branch target offset %lld is odd", (long long)diff);
        return 0;
    }
    vals[idx] = diff / 2;
    *ri_out = r;
    return 1;
}

/* evaluate one D(X,B)-style operand */
static int addr_field(Asm *a, Stmt *st, Operand o, const Slot *sl, int nfields, int64_t *vals,
                      const IsaInst *in) {
    AddrOp ad = asm_split_addr(o);
    const IsaOperand *dispf = isa_op(in, (size_t)sl->field);
    int64_t dmax = 4095, dmin = 0, mid = 0, base_reg = 0, disp;
    long abs_at;
    Base abs_key;
    Expr *e;
    Value dv;
    int has_mid = 0, explicit_base = 0;
    const Operand *mid_item = NULL, *base_item = NULL;
    (void)nfields;
    if (dispf->flags & F_SIGNED) {
        dmin = -((int64_t)1 << 19);
        dmax = ((int64_t)1 << 19) - 1;
    }
    e = asm_expr(a, st, ad.disp);
    if (!e) return 0;
    if (!asm_eval(a, st, e, 1, &dv)) return 0;
    if (ad.inner.n == 1) {
        if (sl->mid >= 0)
            mid_item = &ad.inner.v[0];
        else if (sl->base >= 0)
            base_item = &ad.inner.v[0];
    } else if (ad.inner.n == 2) {
        mid_item = &ad.inner.v[0];
        base_item = &ad.inner.v[1];
    }
    if (mid_item) {
        char *tr = str_trim(mid_item->text);
        if (tr[0]) {
            const IsaOperand *mf = isa_op(in, (size_t)sl->mid);
            has_mid = 1;
            if (sl->mid_kind == 'L') {
                int64_t n;
                if (!asm_abs_expr(a, st, *mid_item, "the length", &n)) return 0;
                mid = n;
            } else {
                int64_t r;
                if (!asm_register(a, st, *mid_item, mf->flags, &r)) return 0;
                vals[sl->mid] = 0;
                if (!check_reg(a, asm_op_span(st, *mid_item), mf, r, vals, sl->mid)) return 0;
                mid = r;
            }
        }
    }
    if (base_item) {
        int64_t r;
        char *tr = str_trim(base_item->text);
        if (!tr[0]) {
            rep_errorf(a->rep, "ZA2035", asm_op_span(st, *base_item), "the base register is empty");
            return 0;
        }
        if (!asm_register(a, st, *base_item, F_GPR, &r)) return 0;
        if (r < 0 || r > 15) {
            rep_errorf(a->rep, "ZA2041", asm_op_span(st, *base_item),
                       "register %lld is out of range 0 to 15", (long long)r);
            return 0;
        }
        base_reg = r;
        explicit_base = 1;
    }
    /* displacement and base */
    memset(&abs_key, 0, sizeof abs_key);
    if (value_is_abs(&dv) && !explicit_base && sl->base >= 0 &&
        (abs_at = find_using(a, abs_key, dv.abs, NULL, dmin, dmax, &disp)) >= 0) {
        /* an absolute address covered by a USING with an absolute base */
        base_reg = a->usings.v[abs_at].reg;
    } else if (value_is_abs(&dv)) {
        disp = dv.abs;
        if (disp < dmin || disp > dmax) {
            diag_help(rep_errorf(a->rep, "ZA2036", e->sp, "displacement %lld is out of range %lld to %lld",
                                 (long long)disp, (long long)dmin, (long long)dmax),
                      "use a smaller displacement or load a base register");
            return 0;
        }
    } else if (explicit_base) {
        diag_help(diag_label(rep_errorf(a->rep, "ZA2037", e->sp,
                                        "a displacement with an explicit base register must be "
                                        "absolute"),
                             "this is an address"),
                  "drop the base register to let USING supply it, or write the displacement as a "
                  "number");
        return 0;
    } else {
        Base b;
        long at = -1;
        const char *q = expr_qual(e);
        if (value_simple(&dv, &b)) at = find_using(a, base_key(b), dv.abs, q, dmin, dmax, &disp);
        if (at < 0) {
            no_addressability(a, e, dv, q, dmin, dmax);
            return 0;
        }
        base_reg = a->usings.v[at].reg;
        if (sl->base < 0) {
            rep_errorf(a->rep, "ZA2038", e->sp, "this operand has no base register field");
            return 0;
        }
    }
    /* default length for SS instructions */
    if (sl->mid_kind == 'L' && sl->mid >= 0 && !has_mid) mid = expr_len(a, e);
    vals[sl->field] = disp;
    if (sl->mid >= 0) {
        if (sl->mid_kind == 'L') {
            const IsaOperand *mf = isa_op(in, (size_t)sl->mid);
            int64_t max = (int64_t)1 << mf->bits;
            if (mid < 1 || mid > max) {
                Diag *d = rep_errorf(a->rep, "ZA2039", e->sp, "length %lld is out of range 1 to %lld",
                                     (long long)mid, (long long)max);
                if (!has_mid) {
                    char *desc = expr_describe(e);
                    diag_help(d,
                              "the length attribute of %s was used; give an explicit length, for "
                              "example `%s(8)`",
                              desc, ad.disp.text);
                }
                return 0;
            }
            vals[sl->mid] = mid - 1;
        } else {
            vals[sl->mid] = mid;
        }
    }
    if (sl->base >= 0) vals[sl->base] = base_reg;
    return 1;
}

/* The nominal value of an S or SY constant: either an address (the base
 * register and displacement come from the active USINGs) or
 * `displacement(base)` with two absolute expressions.  Returns 1 and the base
 * register and the displacement. */
int asm_base_disp(Asm *a, Stmt *st, Operand o, int longdisp, int64_t *base_out, int64_t *disp_out) {
    AddrOp ad = asm_split_addr(o);
    int64_t dmin = longdisp ? -((int64_t)1 << 19) : 0, dmax = longdisp ? ((int64_t)1 << 19) - 1 : 4095;
    int64_t disp = 0, base_reg = 0;
    Expr *e;
    Value dv;
    e = asm_expr(a, st, ad.disp);
    if (!e) return 0;
    if (!asm_eval(a, st, e, 1, &dv)) return 0;
    if (ad.inner.n > 1) {
        rep_errorf(a->rep, "ZA2038", e->sp, "write the constant as S(address) or S(displacement(base))");
        return 0;
    }
    if (ad.inner.n == 1) {
        int64_t r;
        if (!asm_register(a, st, ad.inner.v[0], F_GPR, &r)) return 0;
        if (r < 0 || r > 15) {
            rep_errorf(a->rep, "ZA2041", asm_op_span(st, ad.inner.v[0]),
                       "register %lld is out of range 0 to 15", (long long)r);
            return 0;
        }
        if (!value_is_abs(&dv)) {
            diag_help(rep_errorf(a->rep, "ZA2037", e->sp,
                                 "a displacement with an explicit base register must be absolute"),
                      "drop the base register to let USING supply it");
            return 0;
        }
        if (dv.abs < dmin || dv.abs > dmax) {
            rep_errorf(a->rep, "ZA2036", e->sp, "displacement %lld is out of range %lld to %lld",
                       (long long)dv.abs, (long long)dmin, (long long)dmax);
            return 0;
        }
        *base_out = r;
        *disp_out = dv.abs;
        return 1;
    }
    {
        Base b, key;
        long at = -1;
        const char *q = expr_qual(e);
        if (value_is_abs(&dv)) {
            memset(&key, 0, sizeof key);
            at = find_using(a, key, dv.abs, NULL, dmin, dmax, &disp);
            if (at < 0) {
                /* no USING covers the number: it is the displacement itself */
                if (dv.abs < dmin || dv.abs > dmax) {
                    rep_errorf(a->rep, "ZA2036", e->sp,
                               "displacement %lld is out of range %lld to %lld",
                               (long long)dv.abs, (long long)dmin, (long long)dmax);
                    return 0;
                }
                *base_out = 0;
                *disp_out = dv.abs;
                return 1;
            }
        } else if (value_simple(&dv, &b)) {
            at = find_using(a, base_key(b), dv.abs, q, dmin, dmax, &disp);
        }
        if (at < 0) {
            no_addressability(a, e, dv, q, dmin, dmax);
            return 0;
        }
        base_reg = a->usings.v[at].reg;
    }
    *base_out = base_reg;
    *disp_out = disp;
    return 1;
}

/* evaluate the operands and encode the instruction (pass 2); NULL on error */
static uint8_t *assemble_inst(Asm *a, Stmt *st, const IsaInst *in, const OperandVec *ops) {
    Slots sl;
    int64_t vals[8];
    size_t nf = isa_nops(in), i;
    int good = 1;
    RelImm *ri = NULL;
    uint8_t *out = xmalloc(8);
    char err[160];
    memset(vals, 0, sizeof vals);
    slots_of(in, &sl);
    for (i = 0; i < ops->n; i++) {
        const Slot *s = &sl.s[i];
        const IsaOperand *f = isa_op(in, (size_t)s->field);
        int ok = 0;
        switch (s->kind) {
        case SK_REG:
            ok = reg_field(a, st, ops->v[i], f, vals, s->field);
            break;
        case SK_IMM:
            ok = imm_field(a, st, ops->v[i], f, vals, s->field);
            break;
        case SK_REL: {
            RelImm *r;
            ok = rel_field(a, st, ops->v[i], f, vals, s->field, &r);
            if (r) ri = r;
            break;
        }
        case SK_ADDR:
            ok = addr_field(a, st, ops->v[i], s, (int)nf, vals, in);
            break;
        }
        if (!ok) good = 0;
    }
    if (!good) return NULL;
    if (isa_encode(in, vals, nf, out, err, sizeof err) != 0) {
        char *up = za_strupper(st->op);
        rep_errorf(a->rep, "ZA2040", st->op_span, "cannot encode %s: %s", up, err);
        return NULL;
    }
    if (ri && a->cur->cls->sect->kind != K_DSECT) {
        Reloc r;
        Class *c = a->cur->cls;
        memset(&r, 0, sizeof r);
        r.off = a->cur->start + a->cur->loc + ri->byte_off;
        r.len = ri->n;
        r.target = ri->target;
        r.cls = ri->cls;
        r.kind = REL_RI;
        vec_push(&c->relocs, r);
    }
    return out;
}

void asm_instr(Asm *a, Stmt *st) {
    size_t nv;
    const IsaInst *vars = isa_lookup(st->op, &nv);
    OperandVec ops = {0};
    const IsaInst *in;
    uint8_t *bytes;
    split_operands(&ops, st->operands);
    asm_literal_operands(a, st, &ops);
    in = asm_choose_variant(vars, nv, (int)ops.n);
    if (!in) {
        bad_arity(a, st, vars, nv, (int)ops.n);
        in = &vars[0];
    }
    if (!asm_need_section(a, st)) return;
    asm_align(a, 2);
    a->star_len = in->length;
    {
        Symbol *ls = asm_define_label(a, st, in->length, 'I');
        if (ls) ls->align = 2;
    }
    if (st->bad) {
        asm_advance(a, in->length);
        return;
    }
    if (a->pass == 1) {
        parse_operands(a, st, in, &ops);
        asm_advance(a, in->length);
        return;
    }
    bytes = assemble_inst(a, st, in, &ops);
    if (!bytes) bytes = xcalloc(8);
    asm_put(a, bytes, in->length);
    free(bytes);
}
