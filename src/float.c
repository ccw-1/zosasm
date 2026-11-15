/* float.c - E, D and L constants: hexadecimal floating point (default or
 * with the H extension) and IEEE binary floating point (B extension).
 *
 * The decimal text is converted exactly, with a small arbitrary precision
 * integer type, so the result does not depend on the host's floating point
 * format. The value is the rational N / D with D a power of ten. */
#include <stdlib.h>
#include <string.h>

#include "asm.h"

/* ---- minimal unsigned bignum ------------------------------------------ */

typedef struct {
    uint32_t *d;
    size_t n, cap; /* n limbs in use, no leading zero limb */
} Big;

static void big_trim(Big *b) {
    while (b->n > 0 && b->d[b->n - 1] == 0) b->n--;
}

static void big_reserve(Big *b, size_t n) {
    if (n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4;
        if (c < n) c = n;
        b->d = xrealloc(b->d, c * sizeof(uint32_t));
        b->cap = c;
    }
}

static Big big_new(void) {
    Big b;
    b.d = NULL;
    b.n = b.cap = 0;
    return b;
}

static Big big_u64(uint64_t v) {
    Big b = big_new();
    big_reserve(&b, 2);
    b.d[0] = (uint32_t)v;
    b.d[1] = (uint32_t)(v >> 32);
    b.n = 2;
    big_trim(&b);
    return b;
}

static Big big_copy(const Big *a) {
    Big b = big_new();
    big_reserve(&b, a->n);
    if (a->n) memcpy(b.d, a->d, a->n * sizeof(uint32_t));
    b.n = a->n;
    return b;
}

static int big_is_zero(const Big *b) { return b->n == 0; }

static void big_mul_small(Big *b, uint32_t m) {
    uint64_t carry = 0;
    size_t i;
    for (i = 0; i < b->n; i++) {
        uint64_t t = (uint64_t)b->d[i] * m + carry;
        b->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) {
        big_reserve(b, b->n + 1);
        b->d[b->n++] = (uint32_t)carry;
    }
    big_trim(b);
}

static void big_add_small(Big *b, uint32_t v) {
    uint64_t carry = v;
    size_t i;
    for (i = 0; i < b->n && carry; i++) {
        uint64_t t = (uint64_t)b->d[i] + carry;
        b->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) {
        big_reserve(b, b->n + 1);
        b->d[b->n++] = (uint32_t)carry;
    }
}

static size_t big_bitlen(const Big *b) {
    uint32_t top;
    size_t bits = 0;
    if (!b->n) return 0;
    top = b->d[b->n - 1];
    while (top) {
        bits++;
        top >>= 1;
    }
    return (b->n - 1) * 32 + bits;
}

static int big_bit(const Big *b, size_t i) {
    if (i / 32 >= b->n) return 0;
    return (int)((b->d[i / 32] >> (i % 32)) & 1);
}

static void big_shl(Big *b, size_t bits) {
    size_t limbs = bits / 32, sh = bits % 32, i;
    if (!b->n) return;
    big_reserve(b, b->n + limbs + 1);
    b->d[b->n + limbs] = 0;
    for (i = b->n; i-- > 0;) {
        uint32_t v = b->d[i];
        if (sh) {
            b->d[i + limbs + 1] |= v >> (32 - sh);
            b->d[i + limbs] = v << sh;
        } else {
            b->d[i + limbs] = v;
        }
    }
    for (i = 0; i < limbs; i++) b->d[i] = 0;
    b->n += limbs + 1;
    big_trim(b);
}

static void big_shr(Big *b, size_t bits) {
    size_t limbs = bits / 32, sh = bits % 32, i;
    if (limbs >= b->n) {
        b->n = 0;
        return;
    }
    for (i = 0; i + limbs < b->n; i++) {
        uint32_t lo = b->d[i + limbs] >> sh;
        uint32_t hi = (sh && i + limbs + 1 < b->n) ? b->d[i + limbs + 1] << (32 - sh) : 0;
        b->d[i] = lo | hi;
    }
    b->n -= limbs;
    big_trim(b);
}

static int big_cmp(const Big *a, const Big *b) {
    size_t i;
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (i = a->n; i-- > 0;)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

/* a -= b, a >= b */
static void big_sub(Big *a, const Big *b) {
    int64_t borrow = 0;
    size_t i;
    for (i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->d[i] - borrow - (i < b->n ? (int64_t)b->d[i] : 0);
        if (t < 0) {
            t += (int64_t)1 << 32;
            borrow = 1;
        } else {
            borrow = 0;
        }
        a->d[i] = (uint32_t)t;
    }
    big_trim(a);
}

static void big_add(Big *a, const Big *b) {
    uint64_t carry = 0;
    size_t i, n = a->n > b->n ? a->n : b->n;
    big_reserve(a, n + 1);
    for (i = a->n; i < n + 1; i++) a->d[i] = 0;
    for (i = 0; i < n; i++) {
        uint64_t t = (uint64_t)a->d[i] + (i < b->n ? b->d[i] : 0) + carry;
        a->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    a->d[n] = (uint32_t)carry;
    a->n = n + 1;
    big_trim(a);
}

/* q = num / den, r = num % den (shift and subtract) */
static void big_divmod(const Big *num, const Big *den, Big *q, Big *r) {
    size_t i, bits = big_bitlen(num);
    *q = big_new();
    big_reserve(q, num->n + 1);
    memset(q->d, 0, (num->n + 1) * sizeof(uint32_t));
    q->n = num->n;
    *r = big_new();
    for (i = bits; i-- > 0;) {
        big_shl(r, 1);
        if (big_bit(num, i)) {
            if (r->n == 0) {
                big_reserve(r, 1);
                r->d[0] = 0;
                r->n = 1;
            }
            r->d[0] |= 1;
        }
        if (big_cmp(r, den) >= 0) {
            big_sub(r, den);
            q->d[i / 32] |= (uint32_t)1 << (i % 32);
        }
    }
    big_trim(q);
}

/* the low n bytes, big-endian */
static void big_to_be(const Big *b, uint8_t *out, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        size_t byte = i, limb = byte / 4;
        out[n - 1 - i] = limb < b->n ? (uint8_t)(b->d[limb] >> (8 * (byte % 4))) : 0;
    }
}

static Big big_pow10(unsigned e) {
    Big b = big_u64(1);
    while (e >= 9) {
        big_mul_small(&b, 1000000000u);
        e -= 9;
    }
    while (e--) big_mul_small(&b, 10);
    return b;
}

/* ---- decimal text -> N / D --------------------------------------------- */

/* returns 1 ok, 0 syntax error, -1 out of range */
static int parse_decimal(const char *t, int *neg, Big *num, Big *den) {
    size_t ndig = 0;
    long fracdig = 0, e10 = 0, ex = 0;
    int seen_dot = 0, exp_neg = 0;
    *neg = 0;
    *num = big_new();
    big_reserve(num, 1);
    if (*t == '+' || *t == '-') *neg = *t++ == '-';
    for (; *t; t++) {
        if (za_isdigit((unsigned char)*t)) {
            big_mul_small(num, 10);
            big_add_small(num, (uint32_t)(*t - '0'));
            ndig++;
            if (seen_dot) fracdig++;
            if (ndig > 4000) return -1;
        } else if (*t == '.' && !seen_dot) {
            seen_dot = 1;
        } else {
            break;
        }
    }
    if (ndig == 0) return 0;
    if (*t == 'e' || *t == 'E') {
        int exd = 0;
        t++;
        if (*t == '+' || *t == '-') exp_neg = *t++ == '-';
        for (; za_isdigit((unsigned char)*t); t++) {
            if (ex < 100000) ex = ex * 10 + (*t - '0');
            exd++;
        }
        if (!exd) return 0;
        if (exp_neg) ex = -ex;
    }
    if (*t) return 0;
    e10 = ex - fracdig;
    if (e10 > 6000 || e10 < -6000) return -1;
    *den = big_u64(1);
    if (!big_is_zero(num)) {
        if (e10 >= 0) {
            size_t k;
            for (k = 0; k < (size_t)e10 / 9; k++) big_mul_small(num, 1000000000u);
            for (k = 0; k < (size_t)e10 % 9; k++) big_mul_small(num, 10);
        } else {
            *den = big_pow10((unsigned)-e10);
        }
    }
    return 1;
}

/* ---- hexadecimal floating point ---------------------------------------- */

static void scale16(const Big *n, const Big *d, long e, Big *a, Big *b) {
    *a = big_copy(n);
    *b = big_copy(d);
    if (e >= 0)
        big_shl(b, (size_t)(4 * e));
    else
        big_shl(a, (size_t)(4 * -e));
}

static int hfp_bytes(const Big *N, const Big *D, int neg, int n, uint8_t *out, const char **err) {
    int digits;
    long e;
    Big a, b, a16, num, den, q, r, limit, two_num;
    int ch;
    uint8_t sign = neg ? 0x80 : 0;
    if (n < 1 || (n > 8 && n != 16)) {
        *err = "hexadecimal floating-point constants need a length of 1 to 8 or 16 bytes";
        return -1;
    }
    digits = n == 16 ? 28 : 2 * n - 2;
    /* find e with 1/16 <= v / 16^e < 1 */
    e = ((long)big_bitlen(N) - (long)big_bitlen(D)) / 4;
    for (;;) {
        scale16(N, D, e, &a, &b);
        if (big_cmp(&a, &b) >= 0) {
            e++;
            continue;
        }
        a16 = big_copy(&a);
        big_shl(&a16, 4);
        if (big_cmp(&a16, &b) < 0) {
            e--;
            continue;
        }
        break;
    }
    /* mantissa = round(v / 16^e * 16^digits) */
    {
        long s = digits - e;
        num = big_copy(N);
        den = big_copy(D);
        if (s >= 0)
            big_shl(&num, (size_t)(4 * s));
        else
            big_shl(&den, (size_t)(4 * -s));
        two_num = big_copy(&num);
        big_shl(&two_num, 1);
        big_add(&two_num, &den);
        big_shl(&den, 1);
        big_divmod(&two_num, &den, &q, &r);
        limit = big_u64(1);
        big_shl(&limit, (size_t)(4 * digits));
        if (big_cmp(&q, &limit) >= 0) { /* rounding carried out of the fraction */
            big_shr(&q, 4);
            e++;
        }
    }
    ch = (int)e + 64;
    if (ch < 0 || ch > 127) {
        *err = "the value is outside the range of hexadecimal floating point";
        return -1;
    }
    memset(out, 0, (size_t)n);
    if (n == 16) {
        Big hi = big_copy(&q);
        big_shr(&hi, 56);
        out[0] = (uint8_t)(sign | ch);
        big_to_be(&hi, out + 1, 7);
        out[8] = (uint8_t)(sign | ((ch - 14) & 0x7F));
        big_to_be(&q, out + 9, 7);
        return 1;
    }
    out[0] = (uint8_t)(sign | ch);
    if (n > 1) big_to_be(&q, out + 1, (size_t)n - 1);
    return 1;
}

/* ---- IEEE binary floating point ---------------------------------------- */

static int cmp_pow2(const Big *N, const Big *D, long e) {
    Big a = big_copy(N), b = big_copy(D);
    if (e >= 0)
        big_shl(&b, (size_t)e);
    else
        big_shl(&a, (size_t)-e);
    return big_cmp(&a, &b);
}

static int ieee_bytes(const Big *N, const Big *D, int neg, int n, uint8_t *out, const char **err) {
    int p, bias;
    long emin, emax, e2, eff, s;
    Big num, den, q, r, r2, one, res, frac;
    static char msg[96];
    switch (n) {
    case 4:
        p = 24;
        bias = 127;
        break;
    case 8:
        p = 53;
        bias = 1023;
        break;
    case 16:
        p = 113;
        bias = 16383;
        break;
    default:
        *err = "binary floating-point constants need a length of 4, 8 or 16 bytes";
        return -1;
    }
    emax = bias;
    emin = 1 - bias;
    e2 = (long)big_bitlen(N) - (long)big_bitlen(D) - 1;
    while (cmp_pow2(N, D, e2 + 1) >= 0) e2++;
    while (cmp_pow2(N, D, e2) < 0) e2--;
    eff = e2 < emin ? emin : e2;
    s = p - 1 - eff;
    num = big_copy(N);
    den = big_copy(D);
    if (s >= 0)
        big_shl(&num, (size_t)s);
    else
        big_shl(&den, (size_t)-s);
    big_divmod(&num, &den, &q, &r);
    r2 = big_copy(&r);
    big_shl(&r2, 1);
    {
        int c = big_cmp(&r2, &den);
        if (c > 0 || (c == 0 && big_bit(&q, 0))) {
            one = big_u64(1);
            big_add(&q, &one);
        }
    }
    if (big_bitlen(&q) > (size_t)p) { /* carried into the next binade */
        big_shr(&q, 1);
        eff++;
    }
    if (eff > emax) {
        sprintf(msg, "the value is outside the range of a %d-byte binary floating-point number", n);
        *err = msg;
        return -1;
    }
    frac = big_copy(&q);
    if (big_bit(&q, (size_t)p - 1)) {
        Big hidden = big_u64(1);
        big_shl(&hidden, (size_t)p - 1);
        big_sub(&frac, &hidden);
        res = big_u64((uint64_t)(eff + bias));
    } else {
        res = big_u64(0);
    }
    big_shl(&res, (size_t)p - 1);
    big_add(&res, &frac);
    if (neg) {
        Big sbit = big_u64(1);
        big_shl(&sbit, (size_t)(8 * n - 1));
        big_add(&res, &sbit);
    }
    big_to_be(&res, out, (size_t)n);
    return 1;
}

int float_bytes(const char *text, int ieee, int n, uint8_t *out, const char **err) {
    Big N, D;
    int neg, r;
    *err = NULL;
    r = parse_decimal(text, &neg, &N, &D);
    if (r == -1) {
        *err = "the number is too large or too small";
        return -1;
    }
    if (r == 0) return 0;
    if (n < 1) n = 1;
    if (big_is_zero(&N)) {
        if (ieee ? (n != 4 && n != 8 && n != 16) : (n > 8 && n != 16)) {
            *err = ieee ? "binary floating-point constants need a length of 4, 8 or 16 bytes"
                        : "hexadecimal floating-point constants need a length of 1 to 8 or 16 "
                          "bytes";
            return -1;
        }
        memset(out, 0, (size_t)n);
        return 1;
    }
    return ieee ? ieee_bytes(&N, &D, neg, n, out, err) : hfp_bytes(&N, &D, neg, n, out, err);
}

int dc_float(Asm *a, Stmt *st, const DcSpec *sp, DcVec *out) {
    int n = (int)dc_deflen(sp);
    OperandVec items = {0};
    size_t k;
    if (sp->has_len) n = (int)sp->length;
    split_nominal(sp, &items);
    for (k = 0; k < items.n; k++) {
        char *t = str_trim(items.v[k].text);
        uint8_t *b = xcalloc((size_t)(n < 1 ? 1 : n) + 1);
        const char *err;
        int r = float_bytes(t, sp->ext == 'B', n, b, &err);
        DcVal v;
        if (r == 0) {
            diag_help(dc_err(a, st, "ZA2050", items.v[k].off, (int)strlen(items.v[k].text) + 1,
                             "\"%s\" is not a decimal floating-point number", items.v[k].text),
                      "write for example `%c'1.5'`, `%c'-2E3'` or `%c'0'`", sp->typ, sp->typ,
                      sp->typ);
            return 0;
        }
        if (r < 0) {
            dc_err(a, st, "ZA2053", items.v[k].off, (int)strlen(items.v[k].text) + 1, "%s", err);
            return 0;
        }
        memset(&v, 0, sizeof v);
        v.b = b;
        v.n = (size_t)n;
        vec_push(out, v);
    }
    return 1;
}
