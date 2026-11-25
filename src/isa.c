#include "isa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

int isa_rule(const IsaInst *in, char c, int k, long *arg) {
    const char *p = in->rules;
    if (arg) *arg = 0;
    while (*p) {
        if (*p == c && atoi(p + 1) == k && p[1] >= '0' && p[1] <= '9') {
            const char *q = p + 1;
            while (*q >= '0' && *q <= '9') q++;
            if (*q == '=' && arg) *arg = strtol(q + 1, NULL, c == 'u' ? 16 : 10);
            return 1;
        }
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }
    return 0;
}

int isa_rules_ok(const IsaInst *in, const int64_t v[6]) {
    IsaSlots sl;
    int i;
    long arg;
    if (!in->rules[0]) return 1;
    isa_slots(in, &sl);
    for (i = 0; i < sl.n; i++) {
        const IsaSlot *s = &sl.s[i];
        int64_t x = v[s->field];
        if (s->kind == SK_REG) {
            if (isa_rule(in, 'e', i, NULL) && (x & 1)) return 0;
            if (isa_rule(in, 'f', i, NULL) && (x & 2)) return 0;
        } else if (s->kind == SK_IMM) {
            if (isa_rule(in, 'u', i, &arg) && x >= 0 && x <= 15 && ((arg >> x) & 1)) return 0;
            if (isa_rule(in, 'r', i, &arg) && (x < 0 || x > arg)) return 0;
        } else if (s->kind == SK_ADDR && s->mid >= 0 && s->mid_kind == 'L') {
            int64_t len = v[s->mid] + 1;
            if (isa_rule(in, 'L', i, &arg) && len > arg) return 0;
            if (isa_rule(in, 'E', i, NULL) && (len & 1)) return 0;
        }
    }
    if (isa_rule(in, 'D', 0, NULL) && sl.n >= 2 && sl.s[0].mid >= 0 && sl.s[1].mid >= 0 &&
        v[sl.s[1].mid] + 1 >= v[sl.s[0].mid] + 1)
        return 0;
    return 1;
}

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

static ZA_TLS const char **mnem_list;
static ZA_TLS size_t mnem_count;

const char *const *isa_mnemonics(size_t *n) {
    if (!mnem_list) {
        size_t i;
        mnem_list = xmalloc(isa_inst_count * sizeof *mnem_list);
        for (i = 0; i < isa_inst_count; i++)
            if (i == 0 || strcmp(isa_inst_tab[i].mnemonic, isa_inst_tab[i - 1].mnemonic))
                mnem_list[mnem_count++] = isa_inst_tab[i].mnemonic;
    }
    *n = mnem_count;
    return mnem_list;
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

void isa_slots(const IsaInst *in, IsaSlots *out) {
    size_t nops = isa_nops(in), i;
    out->n = 0;
    for (i = 0; i < nops; i++) {
        unsigned f = isa_op(in, i)->flags;
        IsaSlot sl;
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

/* ---- decoding ----------------------------------------------------------- */

static uint64_t get_bits(const uint8_t *b, int shift, int n) {
    uint64_t v = 0;
    int i;
    for (i = 0; i < n; i++) {
        int pos = shift + i;
        v = v << 1 | (uint64_t)((b[pos / 8] >> (7 - pos % 8)) & 1);
    }
    return v;
}

static void set_mask(uint8_t *m, int shift, int n) {
    int i;
    for (i = 0; i < n; i++) {
        int pos = shift + i;
        m[pos / 8] |= (uint8_t)(0x80 >> (pos % 8));
    }
}

/* the bits of an instruction that carry operands */
static void operand_mask(const IsaInst *in, uint8_t m[6]) {
    size_t i, n = isa_nops(in);
    memset(m, 0, 6);
    for (i = 0; i < n; i++) {
        const IsaOperand *op = isa_op(in, i);
        set_mask(m, op->shift, op->bits);
        if (op->flags & F_VR) set_mask(m, 36, 4);
    }
}

/* entries by first byte: built once */
static ZA_TLS const IsaInst **by_first[256];
static ZA_TLS size_t by_first_n[256];
static ZA_TLS uint8_t (*masks)[6];

void isa_free_thread(void) {
    int k;
    free(mnem_list);
    mnem_list = NULL;
    mnem_count = 0;
    if (masks) {
        free(masks);
        masks = NULL;
    }
    for (k = 0; k < 256; k++) {
        free(by_first[k]);
        by_first[k] = NULL;
        by_first_n[k] = 0;
    }
}

static void build_index(void) {
    size_t i, c[256];
    int k;
    memset(c, 0, sizeof c);
    masks = xmalloc(isa_inst_count * sizeof *masks);
    for (i = 0; i < isa_inst_count; i++) {
        const IsaInst *in = &isa_inst_tab[i];
        by_first_n[in->opcode[0] | in->orv[0]]++;
        operand_mask(in, masks[i]);
    }
    for (k = 0; k < 256; k++) by_first[k] = xmalloc((by_first_n[k] + 1) * sizeof **by_first);
    for (i = 0; i < isa_inst_count; i++) {
        const IsaInst *in = &isa_inst_tab[i];
        int f = in->opcode[0] | in->orv[0];
        by_first[f][c[f]++] = in;
    }
}

static int popcount6(const uint8_t m[6]) {
    int n = 0, i, j;
    for (i = 0; i < 6; i++)
        for (j = 0; j < 8; j++) n += (m[i] >> j) & 1;
    return n;
}

const IsaInst *isa_decode(const uint8_t *b, size_t n, int64_t vals[6]) {
    const IsaInst *best = NULL;
    int best_bits = 0;
    int64_t bv[6] = {0};
    size_t i, len, k;
    if (n < 2) return NULL;
    len = b[0] < 0x40 ? 2 : b[0] < 0xC0 ? 4 : 6;
    if (n < len) return NULL;
    if (!masks) build_index();
    for (k = 0; k < by_first_n[b[0]]; k++) {
        const IsaInst *in = by_first[b[0]][k];
        uint8_t m[6], fixed;
        int ok = 1, bits;
        int64_t v[6] = {0};
        uint8_t re[6];
        char err[80];
        size_t nf = isa_nops(in), f;
        if (in->length != len) continue;
        operand_mask(in, m);
        for (i = 0; i < len && ok; i++) {
            fixed = (uint8_t)(in->opcode[i] | in->orv[i]);
            if ((uint8_t)(b[i] & ~m[i]) != (uint8_t)(fixed & ~m[i])) ok = 0;
        }
        if (!ok) continue;
        bits = popcount6(m);
        if (best && bits >= best_bits) continue;
        for (f = 0; f < nf; f++) {
            const IsaOperand *op = isa_op(in, f);
            uint64_t u = get_bits(b, op->shift, op->bits);
            if ((op->flags & F_DISP) && (op->flags & F_SIGNED) && op->bits == 20)
                u = get_bits(b, op->shift + 12, 8) << 12 | get_bits(b, op->shift, 12);
            if ((op->flags & F_VR) && get_bits(b, 36, 4)) {
                switch (op->shift) {
                case 8: u |= (uint64_t)((get_bits(b, 36, 4) >> 3) & 1) << 4; break;
                case 12: u |= (uint64_t)((get_bits(b, 36, 4) >> 2) & 1) << 4; break;
                case 16: u |= (uint64_t)((get_bits(b, 36, 4) >> 1) & 1) << 4; break;
                case 32: u |= (uint64_t)(get_bits(b, 36, 4) & 1) << 4; break;
                }
            }
            if ((op->flags & (F_SIGNED | F_PCREL)) && op->bits < 64 && (u >> (op->bits - 1)) & 1)
                v[f] = (int64_t)u - ((int64_t)1 << op->bits);
            else
                v[f] = (int64_t)u;
        }
        if (isa_encode(in, v, nf, re, err, sizeof err) != 0 || memcmp(re, b, len) != 0) continue;
        best = in;
        best_bits = bits;
        memcpy(bv, v, sizeof bv);
    }
    if (best) memcpy(vals, bv, sizeof bv);
    return best;
}
