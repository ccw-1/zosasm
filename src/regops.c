/* regops.c - walk the register positions of a statement's operands. */
#include <string.h>

#include "asm.h"
#include "isa.h"
#include "regops.h"

char regops_letter(unsigned flags) {
    if (flags & F_VR) return 'v';
    if (flags & F_FPR) return 'f';
    if (flags & F_AR) return 'a';
    if (flags & F_CR) return 'c';
    return 'r';
}

static void put_operands(Buf *out, const OperandVec *v) {
    size_t i;
    for (i = 0; i < v->n; i++) {
        if (i) buf_putc(out, ',');
        buf_puts(out, v->v[i].text);
    }
}

static void visit(OperandVec *v, size_t i, unsigned flags, RegVisit fn, void *ctx) {
    char *r = fn(ctx, v->v[i].text, flags);
    if (r) v->v[i].text = r;
}

char *regops_map(const char *op, RegOpsKind kind, const char *operands, RegVisit fn, void *ctx) {
    OperandVec ops = {0};
    size_t i;
    Buf out = {0};
    if (kind == RO_USING || kind == RO_DROP) {
        split_operands(&ops, operands);
        for (i = (kind == RO_USING ? 1 : 0); i < ops.n; i++) visit(&ops, i, F_GPR, fn, ctx);
    } else {
        size_t nv;
        const IsaInst *vars = isa_lookup(op, &nv), *in;
        IsaSlots sl;
        split_operands(&ops, operands);
        in = vars ? asm_choose_variant(vars, nv, (int)ops.n) : NULL;
        if (!in) return NULL;
        isa_slots(in, &sl);
        for (i = 0; i < ops.n && (int)i < sl.n; i++) {
            const IsaSlot *sk = &sl.s[i];
            const IsaOperand *f = isa_op(in, (size_t)sk->field);
            if (sk->kind == SK_REG) {
                visit(&ops, i, f->flags, fn, ctx);
            } else if (sk->kind == SK_ADDR) {
                AddrOp ad = asm_split_addr(ops.v[i]);
                Buf t = {0};
                size_t k;
                if (!ad.parens || str_trim(ops.v[i].text)[0] == '=') continue; /* not a literal */
                for (k = 0; k < ad.inner.n; k++) {
                    unsigned fl = F_GPR;
                    int is_mid = ad.inner.n == 2 ? k == 0 : sk->mid >= 0;
                    if (is_mid && sk->mid >= 0) {
                        if (sk->mid_kind == 'L') continue;
                        fl = isa_op(in, (size_t)sk->mid)->flags;
                    } else if (is_mid) {
                        continue;
                    }
                    visit(&ad.inner, k, fl, fn, ctx);
                }
                buf_puts(&t, ad.disp.text);
                buf_putc(&t, '(');
                put_operands(&t, &ad.inner);
                buf_putc(&t, ')');
                ops.v[i].text = xstrdup(buf_cstr(&t));
            }
        }
    }
    put_operands(&out, &ops);
    return xstrdup(buf_cstr(&out));
}
