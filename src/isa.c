#include "isa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

size_t isa_nops(const IsaInst *in) {
    size_t n = 0;
    while (n < 6 && in->ops[n]) n++;
    return n;
}

const IsaOperand *isa_op(const IsaInst *in, size_t i) { return &isa_operand_tab[in->ops[i]]; }

const IsaInst *isa_lookup(const char *mnemonic, size_t *n) {
    size_t lo = 0, hi = isa_inst_count, first, cnt = 0;
    char *m = za_strlower(mnemonic);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (strcmp(isa_inst_tab[mid].mnemonic, m) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    first = lo;
    while (first + cnt < isa_inst_count && !strcmp(isa_inst_tab[first + cnt].mnemonic, m)) cnt++;
    free(m);
    *n = cnt;
    return cnt ? &isa_inst_tab[first] : NULL;
}

const char *const *isa_mnemonics(size_t *n) {
    static const char **list;
    static size_t count;
    if (!list) {
        size_t i;
        list = xmalloc(isa_inst_count * sizeof *list);
        for (i = 0; i < isa_inst_count; i++)
            if (i == 0 || strcmp(isa_inst_tab[i].mnemonic, isa_inst_tab[i - 1].mnemonic))
                list[count++] = isa_inst_tab[i].mnemonic;
    }
    *n = count;
    return list;
}

int isa_check(const IsaOperand *op, int64_t v, char *err, size_t errsz) {
    unsigned bits = op->bits;
    if (op->flags & F_VR) {
        if (v < 0 || v > 31) {
            snprintf(err, errsz, "vector register %lld out of range 0-31", (long long)v);
            return -1;
        }
        return 0;
    }
    if ((op->flags & F_SIGNED) && !(op->flags & F_DISP)) {
        /* immediates take any value that fits, signed or unsigned */
        int64_t lo = -((int64_t)1 << (bits - 1)), hi = bits >= 63 ? INT64_MAX : ((int64_t)1 << bits) - 1;
        if (v < lo || v > hi) {
            snprintf(err, errsz, "value %lld does not fit a %u-bit field", (long long)v, bits);
            return -1;
        }
        return 0;
    }
    if (op->flags & (F_SIGNED | F_PCREL)) {
        int64_t lo = -((int64_t)1 << (bits - 1)), hi = ((int64_t)1 << (bits - 1)) - 1;
        if (v < lo || v > hi) {
            snprintf(err, errsz, "value %lld does not fit a signed %u-bit field", (long long)v, bits);
            return -1;
        }
        return 0;
    }
    if (v < 0 || v >= ((int64_t)1 << bits)) {
        snprintf(err, errsz, "value %lld does not fit an unsigned %u-bit field", (long long)v, bits);
        return -1;
    }
    return 0;
}

/* store the low n bits of v at bit position shift (from the msb of b[0]) */
static void put_bits(uint8_t *b, int shift, int n, uint64_t v) {
    int i;
    for (i = 0; i < n; i++) {
        int bit = (int)((v >> (n - 1 - i)) & 1), pos = shift + i;
        if (bit)
            b[pos / 8] |= (uint8_t)(0x80 >> (pos % 8));
        else
            b[pos / 8] &= (uint8_t)~(0x80 >> (pos % 8));
    }
}

int isa_encode(const IsaInst *in, const int64_t *vals, size_t nvals, uint8_t out[6], char *err,
               size_t errsz) {
    size_t n = isa_nops(in), i;
    uint8_t rxb = 0;
    if (nvals != n) {
        snprintf(err, errsz, "%s needs %u operand fields, got %u", in->mnemonic, (unsigned)n,
                 (unsigned)nvals);
        return -1;
    }
    memcpy(out, in->opcode, 6);
    for (i = 0; i < n; i++) {
        const IsaOperand *op = isa_op(in, i);
        int64_t v = vals[i];
        char e2[160];
        if (isa_check(op, v, e2, sizeof e2)) {
            snprintf(err, errsz, "%s: %s", in->mnemonic, e2);
            return -1;
        }
        if ((op->flags & F_VR) && (v & 0x10)) {
            /* the fifth (high) vector register bit lives in RXB */
            switch (op->shift) {
            case 8: rxb |= 0x8; break;
            case 12: rxb |= 0x4; break;
            case 16: rxb |= 0x2; break;
            case 32: rxb |= 0x1; break;
            }
        }
        if ((op->flags & F_DISP) && (op->flags & F_SIGNED) && op->bits == 20) {
            /* long displacement: DL (12 bits) at shift, DH (8 bits) behind it */
            uint64_t u = (uint64_t)v & 0xFFFFF;
            put_bits(out, op->shift, 12, u & 0xFFF);
            put_bits(out, op->shift + 12, 8, u >> 12);
            continue;
        }
        put_bits(out, op->shift, op->bits, (uint64_t)v & (((uint64_t)1 << op->bits) - 1));
    }
    if (rxb) put_bits(out, 36, 4, rxb);
    for (i = 0; i < 6; i++) out[i] |= in->orv[i];
    return 0;
}
