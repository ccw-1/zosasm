/* instr.c - machine instructions: operand slots, base/displacement
 * resolution through USING, registers, relative branches and encoding. */
#include <stdlib.h>
#include <string.h>

#include "asm.h"


/* ---- slots: the HLASM operands of an instruction variant ------------------ */

typedef enum { SK_REG, SK_IMM, SK_REL, SK_ADDR } SlotKind;

/* One HLASM operand; it covers one or more table fields. */
typedef struct {
    SlotKind kind;
    int field;    /* first field (the register, the immediate, or D) */
    int mid;      /* SK_ADDR: X/L/R field index or -1 */
    int base;     /* SK_ADDR: B field index or -1 */
    char mid_kind; /* 'X' (index), 'L' (length) or 'R' (register) */
} Slot;

typedef struct {
    Slot s[6];
    int n;
} Slots;

static void slots_of(const IsaInst *in, Slots *out) {
    size_t nops = isa_nops(in), i;
    out->n = 0;
    for (i = 0; i < nops; i++) {
        unsigned f = isa_op(in, i)->flags;
        Slot sl;
        sl.mid = sl.base = -1;
        sl.mid_kind = 0;
        sl.field = (int)i;
        if (f & F_DISP) {
            size_t j = i + 1;
            sl.kind = SK_ADDR;
            if (j < nops && !(isa_op(in, j)->flags & F_BASE)) {
                unsigned mf = isa_op(in, j)->flags;
                sl.mid = (int)j;
                sl.mid_kind = (mf & F_INDEX) ? 'X' : (mf & F_LENGTH) ? 'L' : 'R';
                j++;
            }
            if (j < nops && (isa_op(in, j)->flags & F_BASE)) {
                sl.base = (int)j;
                j++;
            }
            i = j - 1;
        } else if (f & F_PCREL) {
            sl.kind = SK_REL;
        } else if (f & (F_GPR | F_FPR | F_AR | F_CR | F_VR)) {
            sl.kind = SK_REG;
        } else {
            sl.kind = SK_IMM;
        }
        out->s[out->n++] = sl;
    }
}

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

/* a register number: the predefined names R0-R15 (F0.. A0.. C0.. V0..) are
 * used when the symbol is not defined by the program */
static int asm_register(Asm *a, Stmt *st, Operand o, unsigned flags, int64_t *out) {
    char *t = str_trim(o.text);
    size_t tl = strlen(t), lead = 0;
    Operand to;
    int ok;
    if (t[0] == '%') {
        /* GNU style register: %r1 %f0 %a2 %c3 %v31 */
        size_t i;
        int digits = tl >= 3 && tl <= 4;
        for (i = 2; i < tl; i++)
            if (!za_isdigit((unsigned char)t[i])) digits = 0;
        if (!digits || !strchr("rfacvRFACV", t[1])) {
            diag_help(rep_errorf(a->rep, "ZA2054", asm_op_span(st, o),
                                 "\"%s\" is not a register name", t),
                      "GNU style registers are %%r0-%%r15, %%f0-%%f15, %%a0-%%a15, %%c0-%%c15 and "
                      "%%v0-%%v31");
            free(t);
            return 0;
        }
        if (za_toupper((unsigned char)t[1]) != reg_prefix(flags)) {
            diag_help(rep_errorf(a->rep, "ZA2055", asm_op_span(st, o),
                                 "%s is not a %s register", t, reg_kind(flags)),
                      "this operand takes a %s register (%%%c<n>)", reg_kind(flags),
                      (char)za_tolower((unsigned char)reg_prefix(flags)));
            free(t);
            return 0;
        }
        *out = atoi(t + 2);
        free(t);
        return 1;
    }
    if (tl >= 2 && tl <= 3 && za_toupper((unsigned char)t[0]) == reg_prefix(flags) &&
        !asm_lookup(a, t)) {
        size_t i;
        int digits = 1;
        for (i = 1; i < tl; i++)
            if (!za_isdigit((unsigned char)t[i])) digits = 0;
        if (digits) {
            *out = atoi(t + 1);
            free(t);
            return 1;
        }
    }
    while (o.text[lead] && za_isspace((unsigned char)o.text[lead])) lead++;
    to.text = t;
    to.off = o.off + (int)lead;
    ok = asm_abs_expr(a, st, to, "a register number", out);
    return ok;
}

void dir_using(Asm *a, Stmt *st) {
    OperandVec ops = {0};
    Expr *e;
    Value v;
    Base key;
    size_t i;
    if (st->name[0] && a->pass == 1) {
        rep_errorf(a->rep, "ZA9002", st->name_span, "labeled USING is not supported yet");
        st->bad = 1;
        return;
    }
    if (!asm_ops(a, st, 2, 17, &ops)) return;
    e = asm_expr(a, st, ops.v[0]);
    if (a->pass == 1)
        for (i = 1; i < ops.n; i++)
            if (!is_pct_reg(ops.v[i])) asm_expr(a, st, ops.v[i]);
    if (a->pass != 2 || !e) return;
    if (!asm_eval(a, st, e, 1, &v)) return;
    memset(&key, 0, sizeof key);
    if (!value_is_abs(&v)) {
        Base b;
        if (value_simple(&v, &b)) {
            key = base_key(b);
            if (b.kind == B_SYM) {
                rep_errorf(a->rep, "ZA3061", e->sp,
                           "USING needs an address in this module, not an external symbol");
                return;
            }
        } else {
            rep_errorf(a->rep, "ZA3062", e->sp, "USING operand must be a simple address in one section");
            return;
        }
    }
    for (i = 1; i < ops.n; i++) {
        int64_t r;
        Using u;
        if (!asm_register(a, st, ops.v[i], F_GPR, &r)) continue;
        u.key = key;
        u.off = v.abs + (int64_t)(i - 1) * 4096;
        u.reg = (int)r;
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
        int64_t r;
        int found = 0;
        size_t kept = 0;
        if (!asm_register(a, st, ops.v[i], F_GPR, &r)) continue;
        for (j = 0; j < a->usings.n; j++) {
            if (a->usings.v[j].reg == (int)r) {
                found = 1;
                continue;
            }
            a->usings.v[kept++] = a->usings.v[j];
        }
        a->usings.n = kept;
        if (!found)
            rep_warnf(a->rep, "ZA3060", asm_op_span(st, ops.v[i]),
                      "DROP of register %d, which has no active USING", (int)r);
    }
}

/* find a base register and displacement for v */
static int resolve_using(Asm *a, Value v, int64_t lo, int64_t hi, int *reg, int64_t *disp) {
    Base b;
    Base key;
    int ok = 0;
    int64_t best = 0;
    size_t i;
    if (!value_simple(&v, &b)) return 0;
    key = base_key(b);
    for (i = a->usings.n; i-- > 0;) {
        const Using *u = &a->usings.v[i];
        int64_t d, rank;
        if (!base_eq(u->key, key)) continue;
        d = v.abs - u->off;
        if (d < lo || d > hi) continue;
        /* smaller is better; negatives only as a last resort */
        rank = d < 0 ? ((int64_t)1 << 40) - d : d;
        if (!ok || rank < best) {
            best = rank;
            *reg = u->reg;
            *disp = d;
            ok = 1;
        }
    }
    return ok;
}

/* explain a failed USING lookup */
static void no_addressability(Asm *a, const Expr *e, Value v, int64_t lo, int64_t hi) {
    char *desc = expr_describe(e);
    Diag *d = diag_label(
        rep_errorf(a->rep, "ZA2030", e->sp, "no USING gives addressability to %s", desc),
        "this address is not covered");
    Base b, key;
    int near = 0;
    size_t i;
    if (!value_simple(&v, &b)) {
        diag_help(d, "the operand is not a simple address; give an explicit displacement and base "
                     "register, for example `0(R12)`");
        return;
    }
    key = base_key(b);
    for (i = 0; i < a->usings.n; i++) {
        const Using *u = &a->usings.v[i];
        if (base_eq(u->key, key)) {
            int64_t dd = v.abs - u->off;
            near = 1;
            diag_note(d, u->sp,
                      "register %d is based here, but the displacement would be %lld (allowed %lld "
                      "to %lld)",
                      u->reg, (long long)dd, (long long)lo, (long long)hi);
        }
    }
    if (!near) {
        if (b.kind == B_SYM)
            diag_help(d,
                      "\"%s\" is external; load its address with a V- or A-type constant and use "
                      "it through a base register",
                      desc);
        else if (a->usings.n == 0)
            diag_help(d, "there is no active USING; add `USING *,R12` (or the register of your "
                         "choice) after loading the base register");
        else
            diag_help(d, "no active USING refers to the section/class of this address");
    }
}

/* ---- parsing of operand text ----------------------------------------------- */

/* an operand split into displacement text and the parenthesized part */
typedef struct {
    Operand disp; /* text before the parenthesis (the whole operand if none) */
    OperandVec inner;
    int parens;
} AddrOp;

static AddrOp split_addr(Operand o) {
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
        case '(':
            if (depth == 0) cand = (long)i;
            depth++;
            break;
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
static const IsaInst *choose_variant(const IsaInst *vars, size_t nv, int n) {
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
        ad = split_addr(o);
        if (!ad.disp.text[0]) {
            diag_help(rep_errorf(a->rep, "ZA2033", asm_op_span(st, o),
                                 "the displacement before the parenthesis is missing"),
                      "write for example `0(R2)`");
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
    AddrOp ad = split_addr(o);
    const IsaOperand *dispf = isa_op(in, (size_t)sl->field);
    int64_t dmax = 4095, dmin = 0, mid = 0, base_reg = 0, disp;
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
    if (value_is_abs(&dv)) {
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
        int reg;
        int64_t d;
        if (!resolve_using(a, dv, dmin, dmax, &reg, &d)) {
            no_addressability(a, e, dv, dmin, dmax);
            return 0;
        }
        disp = d;
        base_reg = reg;
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
    in = choose_variant(vars, nv, (int)ops.n);
    if (!in) {
        bad_arity(a, st, vars, nv, (int)ops.n);
        in = &vars[0];
    }
    if (!asm_need_section(a, st)) return;
    asm_align(a, 2);
    a->star_len = in->length;
    asm_define_label(a, st, in->length, 'I');
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
