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

/* Big buffers for an assembly are allocated from the Module arena, so
 * asm_free() releases them; outside an assembly (the float unit tests) they
 * use malloc.  The arena cannot grow a block in place, so copy on growth. */
static Arena *big_arena;

void big_set_arena(Arena *a) { big_arena = a; }

static void big_reserve(Big *b, size_t n) {
    if (n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4;
        uint32_t *d;
        if (c < n) c = n;
        d = big_arena ? arena_alloc(big_arena, c * sizeof(uint32_t))
                      : xmalloc(c * sizeof(uint32_t));
        if (b->n) memcpy(d, b->d, b->n * sizeof(uint32_t));
        if (!big_arena) free(b->d);
        b->d = d;
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
    *den = big_new();
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

/* ---- rounding ----------------------------------------------------------- */

/* Whether the magnitude q (the integer part of a quotient with remainder r
 * over den) must be incremented. mode is the HLASM rounding mode: 1 and 12
 * half away from zero, 4 and 8 half to even, 13 half down, 5 and 9 toward
 * zero, 6 and 10 toward +infinity, 7 and 11 toward -infinity, 14 away from
 * zero. */
static int round_up_p(int mode, int neg, const Big *q, const Big *r, const Big *den) {
    Big r2 = big_copy(r);
    int c, inexact = !big_is_zero(r);
    big_shl(&r2, 1);
    c = big_cmp(&r2, den);
    switch (mode) {
    case 1:
    case 12:
        return c >= 0;
    case 4:
    case 8:
        return c > 0 || (c == 0 && big_bit(q, 0));
    case 13:
        return c > 0;
    case 5:
    case 9:
        return 0;
    case 6:
    case 10:
        return !neg && inexact;
    case 7:
    case 11:
        return neg && inexact;
    case 14:
        return inexact;
    }
    return c >= 0;
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

static int hfp_core(const Big *N, const Big *D, int neg, int n, long scale, int mode, uint8_t *out,
                    const char **err);

/* Lengths 9 to 15 (HLASM): the first n bytes of the extended format. With 9
 * bytes only the high-order double is there (rounded) plus the exponent byte
 * of the low-order one; with more, the digits of the low-order double are
 * rounded to the bytes that fit. A length of 1 holds no digit at all. */
static int hfp_bytes(const Big *N, const Big *D, int neg, int n, long scale, int mode, uint8_t *out,
                     const char **err) {
    uint8_t t[16];
    int r, i;
    if (n == 1) {
        *err = "precision lost: a hexadecimal floating-point constant of 1 byte holds no digit "
               "(HLASM ASMA073E)";
        return -1;
    }
    if (n < 9 || n > 15) return hfp_core(N, D, neg, n, scale, mode, out, err);
    if (n == 9) {
        r = hfp_core(N, D, neg, 8, scale, mode, t, err);
        if (r < 0) return r;
        memcpy(out, t, 8);
        out[8] = (uint8_t)((t[0] & 0x80) | (((t[0] & 0x7F) - 14) & 0x7F));
        return r;
    }
    r = hfp_core(N, D, neg, 16, scale, mode, t, err);
    if (r < 0) return r;
    if (t[n] >> 4 >= 8) { /* round the kept digits up */
        for (i = n - 1; i > 8; i--)
            if (++t[i]) break;
    }
    memcpy(out, t, (size_t)n);
    return r;
}

static int hfp_core(const Big *N, const Big *D, int neg, int n, long scale, int mode, uint8_t *out,
                    const char **err) {
    int digits;
    long e;
    Big a, b, a16, num, den, q, r, limit;
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
    /* mantissa = round(v / 16^(e+scale) * 16^digits); a scale modifier shifts the fraction
     * right by that many hexadecimal digits and raises the characteristic */
    e += scale;
    {
        long s = digits - e;
        num = big_copy(N);
        den = big_copy(D);
        if (s >= 0)
            big_shl(&num, (size_t)(4 * s));
        else
            big_shl(&den, (size_t)(4 * -s));
        big_divmod(&num, &den, &q, &r);
        if (round_up_p(mode, neg, &q, &r, &den)) big_add_small(&q, 1);
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

static Big big_ones(size_t bits) {
    Big b = big_u64(1);
    big_shl(&b, bits);
    {
        Big one = big_u64(1);
        big_sub(&b, &one);
    }
    return b;
}

/* The exponent width of the binary floating-point type being converted (8, 11
 * or 15 bits for E, D and L) when the length is not the natural one: HLASM
 * builds the constant in a format with that exponent and 8*n-1-bits fraction
 * bits. 0: use the natural format of the length. */
static int ieee_ebits;

void float_set_type(char typ) { ieee_ebits = typ == 'E' ? 8 : typ == 'D' ? 11 : typ == 'L' ? 15 : 0; }

static void ieee_params(int n, int *p, int *bias) {
    if (ieee_ebits && n != (ieee_ebits == 8 ? 4 : ieee_ebits == 11 ? 8 : 16)) {
        *p = 8 * n - ieee_ebits;
        *bias = (1 << (ieee_ebits - 1)) - 1;
        return;
    }
    switch (n) {
    case 4:
        *p = 24;
        *bias = 127;
        break;
    case 8:
        *p = 53;
        *bias = 1023;
        break;
    default:
        *p = 113;
        *bias = 16383;
        break;
    }
}

/* the special values (INF) (QNAN) (SNAN) (NAN) (MAX) (MIN) (DMIN) */
static void ieee_special(const char *kind, int neg, int n, uint8_t *out) {
    int p, bias;
    size_t fb, ebits = (size_t)(8 * n);
    uint64_t expv = 0;
    Big frac = big_u64(0), res;
    ieee_params(n, &p, &bias);
    fb = (size_t)p - 1;
    ebits -= (size_t)p;
    if (!strcmp(kind, "MAX")) {
        expv = ((uint64_t)1 << ebits) - 2;
        frac = big_ones(fb);
    } else if (!strcmp(kind, "MIN")) {
        expv = 1;
    } else if (!strcmp(kind, "DMIN")) {
        frac = big_u64(1);
    } else {
        expv = ((uint64_t)1 << ebits) - 1;
        if (strcmp(kind, "INF")) {
            frac = big_u64(!strcmp(kind, "QNAN") ? 3 : !strcmp(kind, "SNAN") ? 1 : 2);
            big_shl(&frac, fb - 2);
        }
    }
    res = big_u64(expv);
    big_shl(&res, fb);
    big_add(&res, &frac);
    if (neg) {
        Big sbit = big_u64(1);
        big_shl(&sbit, (size_t)(8 * n - 1));
        big_add(&res, &sbit);
    }
    big_to_be(&res, out, (size_t)n);
}

static int ieee_bytes(const Big *N, const Big *D, int neg, int n, int mode, uint8_t *out,
                      const char **err, const char **warn) {
    int p, bias;
    long emin, emax, e2, eff, s;
    Big num, den, q, r, res, frac;
    static char msg[160];
    (void)err;
    ieee_params(n, &p, &bias);
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
    if (round_up_p(mode, neg, &q, &r, &den)) big_add_small(&q, 1);
    if (big_bitlen(&q) > (size_t)p) { /* carried into the next binade */
        big_shr(&q, 1);
        eff++;
    }
    if (eff > emax) {
        /* overflow: infinity or the largest number, depending on the rounding mode */
        int inf = mode == 1 || mode == 4 || (mode == 6 && !neg) || (mode == 7 && neg);
        sprintf(msg, "the value is outside the range of a %d-byte binary floating-point number; "
                     "%s is used",
                n, inf ? "infinity" : "the largest number");
        *warn = msg;
        ieee_special(inf ? "INF" : "MAX", neg, n, out);
        return 2;
    }
    frac = big_copy(&q);
    if (big_bit(&q, (size_t)p - 1)) {
        Big hidden = big_u64(1);
        big_shl(&hidden, (size_t)p - 1);
        big_sub(&frac, &hidden);
        res = big_u64((uint64_t)(eff + bias));
    } else {
        res = big_u64(0);
        if (!big_is_zero(&r))
            *warn = big_is_zero(&q) ? "the value is too small and becomes zero"
                                    : "the value is below the smallest normalized number and "
                                      "loses precision";
    }
    big_shl(&res, (size_t)p - 1);
    big_add(&res, &frac);
    if (neg) {
        Big sbit = big_u64(1);
        big_shl(&sbit, (size_t)(8 * n - 1));
        big_add(&res, &sbit);
    }
    big_to_be(&res, out, (size_t)n);
    return *warn ? 2 : 1;
}

/* ---- decimal floating point (IEEE 754-2008, densely packed decimal) ------ */

typedef struct {
    int bytes, p, w; /* length, digits, bits of the exponent continuation */
    long qmin, qmax, bias;
} DfpFmt;

static const DfpFmt dfp_fmts[] = {
    {4, 7, 6, -101, 90, 101},
    {8, 16, 8, -398, 369, 398},
    {16, 34, 12, -6176, 6111, 6176},
};

/* three decimal digits to a 10 bit declet */
static unsigned dpd_encode(int d1, int d2, int d3) {
    int a = (d1 >> 3) & 1, b = (d1 >> 2) & 1, c = (d1 >> 1) & 1, d = d1 & 1;
    int e = (d2 >> 3) & 1, f = (d2 >> 2) & 1, g = (d2 >> 1) & 1, h = d2 & 1;
    int i = (d3 >> 3) & 1, j = (d3 >> 2) & 1, k = (d3 >> 1) & 1, m = d3 & 1;
    int pp, qq, rr, ss, tt, uu, vv, ww, xx, yy = m;
    switch (a * 4 + e * 2 + i) {
    case 0:
        pp = b, qq = c, rr = d, ss = f, tt = g, uu = h, vv = 0, ww = j, xx = k;
        break;
    case 1:
        pp = b, qq = c, rr = d, ss = f, tt = g, uu = h, vv = 1, ww = 0, xx = 0;
        break;
    case 2:
        pp = b, qq = c, rr = d, ss = j, tt = k, uu = h, vv = 1, ww = 0, xx = 1;
        break;
    case 4:
        pp = j, qq = k, rr = d, ss = f, tt = g, uu = h, vv = 1, ww = 1, xx = 0;
        break;
    case 6:
        pp = j, qq = k, rr = d, ss = 0, tt = 0, uu = h, vv = 1, ww = 1, xx = 1;
        break;
    case 5:
        pp = f, qq = g, rr = d, ss = 0, tt = 1, uu = h, vv = 1, ww = 1, xx = 1;
        break;
    case 3:
        pp = b, qq = c, rr = d, ss = 1, tt = 0, uu = h, vv = 1, ww = 1, xx = 1;
        break;
    default:
        pp = 0, qq = 0, rr = d, ss = 1, tt = 1, uu = h, vv = 1, ww = 1, xx = 1;
        break;
    }
    return (unsigned)((pp << 9) | (qq << 8) | (rr << 7) | (ss << 6) | (tt << 5) | (uu << 4) |
                      (vv << 3) | (ww << 2) | (xx << 1) | yy);
}

/* sign, 5 bit combination start G, w more exponent bits, then declets */
static void dfp_pack(const DfpFmt *f, int neg, int top2, long low, int d0, const char *digs,
                     uint8_t *out) {
    Big res = big_u64(neg ? 1 : 0), t;
    int g, k;
    if (d0 <= 7)
        g = (top2 << 3) | d0;
    else
        g = (3 << 3) | (top2 << 1) | (d0 & 1);
    big_shl(&res, 5 + (size_t)f->w);
    t = big_u64((uint64_t)((g << f->w) | (int)low));
    big_add(&res, &t);
    for (k = 0; k + 2 < f->p; k += 3) {
        t = big_u64(dpd_encode(digs[k] - '0', digs[k + 1] - '0', digs[k + 2] - '0'));
        big_shl(&res, 10);
        big_add(&res, &t);
    }
    big_to_be(&res, out, (size_t)f->bytes);
}

/* digs: exactly p digits, exponent q */
static void dfp_finite(const DfpFmt *f, int neg, const char *digs, long q, uint8_t *out) {
    long e = q + f->bias;
    dfp_pack(f, neg, (int)(e >> f->w), e & (((long)1 << f->w) - 1), digs[0] - '0', digs + 1, out);
}

static void dfp_special(const DfpFmt *f, const char *kind, int neg, uint8_t *out) {
    char digs[40];
    int i;
    if (!strcmp(kind, "INF") || !strcmp(kind, "QNAN") || !strcmp(kind, "NAN") ||
        !strcmp(kind, "SNAN")) {
        Big res = big_u64(neg ? 1 : 0), t;
        size_t rest = (size_t)f->bytes * 8 - 1;
        int inf = !strcmp(kind, "INF");
        big_shl(&res, 5);
        t = big_u64(inf ? 0x1E : 0x1F);
        big_add(&res, &t);
        big_shl(&res, rest - 5);
        if (!strcmp(kind, "SNAN")) {
            t = big_u64(1);
            big_shl(&t, rest - 6);
            big_add(&res, &t);
        }
        big_to_be(&res, out, (size_t)f->bytes);
        return;
    }
    memset(digs, '0', (size_t)f->p);
    digs[f->p] = 0;
    if (!strcmp(kind, "MAX")) {
        for (i = 0; i < f->p; i++) digs[i] = '9';
        dfp_finite(f, neg, digs, f->qmax, out);
    } else if (!strcmp(kind, "MIN")) {
        digs[0] = '1';
        dfp_finite(f, neg, digs, f->qmin, out);
    } else { /* DMIN */
        digs[f->p - 1] = '1';
        dfp_finite(f, neg, digs, f->qmin, out);
    }
}

/* Round the digit string d (length *len) by dropping its last `drop` digits.
 * A string shorter than that is first padded with zeros on the left. Returns
 * whether anything but zeros was dropped. */
static int dfp_round_drop(char *d, size_t *len, size_t drop, int neg, int mode) {
    size_t l = *len, i, keep;
    if (drop > l + 1) drop = l + 1;
    int first = 0, rest = 0, inexact, inc, odd, c;
    if (drop >= l) {
        memmove(d + (drop - l + 1), d, l + 1);
        memset(d, '0', drop - l + 1);
        l = drop + 1;
    }
    keep = l - drop;
    first = d[keep] - '0';
    for (i = keep + 1; i < l; i++)
        if (d[i] != '0') rest = 1;
    inexact = first != 0 || rest;
    c = first > 5 || (first == 5 && rest) ? 1 : first == 5 ? 0 : -1;
    odd = (d[keep - 1] - '0') & 1;
    switch (mode) {
    case 12:
        inc = c >= 0;
        break;
    case 13:
        inc = c > 0;
        break;
    case 9:
        inc = 0;
        break;
    case 10:
        inc = !neg && inexact;
        break;
    case 11:
        inc = neg && inexact;
        break;
    case 14:
        inc = inexact;
        break;
    case 15: { /* round for reround: truncate, but never leave a 0 or 5 behind */
        int last = d[keep - 1] - '0';
        inc = inexact && (last == 0 || last == 5);
        break;
    }
    default: /* 8 */
        inc = c > 0 || (c == 0 && odd);
        break;
    }
    d[keep] = 0;
    l = keep;
    if (inc) {
        for (i = l; i-- > 0;) {
            if (d[i] == '9') {
                d[i] = '0';
            } else {
                d[i]++;
                break;
            }
        }
        if (i == (size_t)-1) { /* 99..9 + 1 */
            memmove(d + 1, d, l + 1);
            d[0] = '1';
            l++;
        }
    }
    /* strip leading zeros, keep one digit */
    for (i = 0; i + 1 < l && d[i] == '0'; i++) {
    }
    if (i) {
        memmove(d, d + i, l - i + 1);
        l -= i;
    }
    *len = l;
    return inexact;
}

static int dfp_bytes(const char *text, int neg, int n, long exp10, int mode, uint8_t *out,
                     const char **err, const char **warn) {
    const DfpFmt *f = NULL;
    char *d;
    size_t len = 0, i, nd = 0;
    long frac = 0, ex = 0, q;
    int seen_dot = 0, zero, exp_neg = 0;
    static char msg[160];
    for (i = 0; i < sizeof dfp_fmts / sizeof dfp_fmts[0]; i++)
        if (dfp_fmts[i].bytes == n) f = &dfp_fmts[i];
    if (!f) {
        *err = "decimal floating-point constants need a length of 4, 8 or 16 bytes";
        return -1;
    }
    d = xmalloc(strlen(text) + 80);
    for (; *text; text++) {
        if (za_isdigit((unsigned char)*text)) {
            d[nd++] = *text;
            if (seen_dot) frac++;
        } else if (*text == '.' && !seen_dot) {
            seen_dot = 1;
        } else {
            break;
        }
    }
    if (!nd) {
        free(d);
        return 0;
    }
    if (*text == 'e' || *text == 'E') {
        int exd = 0;
        text++;
        if (*text == '+' || *text == '-') exp_neg = *text++ == '-';
        for (; za_isdigit((unsigned char)*text); text++) {
            if (ex < 100000000) ex = ex * 10 + (*text - '0');
            exd++;
        }
        if (!exd) {
            free(d);
            return 0;
        }
        if (exp_neg) ex = -ex;
    }
    if (*text) {
        free(d);
        return 0;
    }
    d[nd] = 0;
    for (i = 0; i + 1 < nd && d[i] == '0'; i++) {
    }
    memmove(d, d + i, nd - i + 1);
    len = nd - i;
    zero = len == 1 && d[0] == '0';
    q = ex + exp10 - frac;
    if (mode < 8) mode = 8;
    *warn = NULL;
    if (zero) {
        if (q > f->qmax) q = f->qmax;
        if (q < f->qmin) q = f->qmin;
    } else {
        if (len > (size_t)f->p) {
            size_t drop = len - (size_t)f->p;
            if (dfp_round_drop(d, &len, drop, neg, mode)) *warn = "the value is rounded to fit the number of digits";
            q += (long)drop;
            if (len > (size_t)f->p) { /* 99..9 rounded up to 100..0 */
                d[--len] = 0;
                q++;
            }
        }
        if (q < f->qmin) { /* subnormal: fewer digits */
            size_t drop = (size_t)(f->qmin - q);
            if (dfp_round_drop(d, &len, drop, neg, mode)) *warn = "the value is below the smallest normalized number and loses precision";
            q = f->qmin;
            if (len == 1 && d[0] == '0') *warn = "the value is too small and becomes zero";
        }
        while (q > f->qmax && len < (size_t)f->p) { /* fold down */
            d[len++] = '0';
            d[len] = 0;
            q--;
        }
        if (q > f->qmax) {
            int inf = mode == 8 || mode == 12 || mode == 13 || mode == 14 ||
                      (mode == 10 && !neg) || (mode == 11 && neg);
            sprintf(msg, "the value is outside the range of a %d-byte decimal floating-point "
                         "number; %s is used",
                    n, inf ? "infinity" : "the largest number");
            *warn = msg;
            dfp_special(f, inf ? "INF" : "MAX", neg, out);
            free(d);
            return 2;
        }
    }
    /* p digits, zeros on the left */
    if (len < (size_t)f->p) {
        memmove(d + (size_t)f->p - len, d, len + 1);
        memset(d, '0', (size_t)f->p - len);
    }
    dfp_finite(f, neg, d, q, out);
    free(d);
    return *warn ? 2 : 1;
}

/* a *= b */
static void big_mul_big(Big *a, const Big *b) {
    Big r = big_new();
    size_t i, j;
    if (!a->n || !b->n) {
        a->n = 0;
        return;
    }
    big_reserve(&r, a->n + b->n);
    memset(r.d, 0, (a->n + b->n) * sizeof(uint32_t));
    r.n = a->n + b->n;
    for (i = 0; i < a->n; i++) {
        uint64_t carry = 0;
        for (j = 0; j < b->n; j++) {
            uint64_t t = (uint64_t)a->d[i] * b->d[j] + r.d[i + j] + carry;
            r.d[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        r.d[i + b->n] = (uint32_t)carry;
    }
    big_trim(&r);
    *a = r;
}

/* multiply N / D by 10^e */
static void mul_pow10(Big *N, Big *D, long e) {
    if (e >= 0) {
        Big p = big_pow10((unsigned)e);
        big_mul_big(N, &p);
        if (!big_arena) free(p.d);
    } else {
        Big p = big_pow10((unsigned)-e);
        big_mul_big(D, &p);
        if (!big_arena) free(p.d);
    }
}

static int is_word(const char *t, const char *w) {
    size_t i;
    for (i = 0; w[i]; i++)
        if (za_toupper((unsigned char)t[i]) != w[i]) return 0;
    return t[i] == 0;
}

/* E, D and L constants. ext is 0 (hexadecimal), 'H' (hexadecimal, all
 * rounding modes), 'Q' (hexadecimal L with quadword alignment), 'B' (binary)
 * or 'D' (decimal). The nominal value is a decimal number with an optional
 * exponent and an optional rounding mode (R4, R8, ...), or a special value in
 * parentheses. 1 ok, 2 ok with a warning in *warn, 0 not a number, -1 error
 * (err says why). */
int float_const(const char *text, char ext, int n, long exp10, long scale, uint8_t *out,
                const char **err, const char **warn) {
    char *t0 = xstrdup(text), *t = t0, *num = NULL;
    Big N = {0}, D = {0};
    size_t l;
    int mode = 0, neg = 0, r, parsed = 0, ret = 0, num_alloc = 0;
    int ieee = ext == 'B', dec = ext == 'D';
    *err = NULL;
    *warn = NULL;
    l = strlen(t);
    while (l && za_isspace((unsigned char)t[l - 1])) t[--l] = 0;
    {
        size_t i = l;
        while (i > 0 && za_isdigit((unsigned char)t[i - 1])) i--;
        if (i < l && i > 0 && (t[i - 1] == 'R' || t[i - 1] == 'r')) {
            mode = atoi(t + i);
            if (l - i > 3) mode = 99;
            t[i - 1] = 0;
            l = i - 1;
            while (l && za_isspace((unsigned char)t[l - 1])) t[--l] = 0;
            if (!(ext == 'D' ? mode >= 8 && mode <= 15
                             : (ext == 'B' || ext == 'H') && mode != 0 && mode != 2 && mode != 3 &&
                                   mode <= 7)) {
                *err = ext == 'D'   ? "a decimal floating-point rounding mode is R8 to R15"
                       : ext == 'B' || ext == 'H'
                           ? "a rounding mode for binary and hexadecimal floating point is R1 "
                             "or R4 to R7"
                           : "a rounding mode needs the type extension B, D or H, for example "
                             "EB'1.5R5'";
                ret = -1;
                goto done;
            }
        }
    }
    if (!mode) mode = dec ? 8 : ieee ? 4 : 1;
    while (*t && za_isspace((unsigned char)*t)) t++;
    if (*t == '+' || *t == '-') {
        neg = *t++ == '-';
        if (*t == '+' || *t == '-') {
            ret = 0;
            goto done;
        }
    }
    if (*t == '(') { /* special values */
        char *k = t + 1;
        size_t kl = strlen(k);
        const char *const all[] = {"SNAN", "QNAN", "NAN", "INF", "MAX", "MIN", "DMIN"};
        const char *kind = NULL;
        size_t i;
        if (!kl || k[kl - 1] != ')') {
            ret = 0;
            goto done;
        }
        k[kl - 1] = 0;
        for (i = 0; i < 7; i++)
            if (is_word(k, all[i])) kind = all[i];
        if (!kind) {
            ret = 0;
            goto done;
        }
        if (!ieee && !dec && strcmp(kind, "MAX") && strcmp(kind, "MIN") && strcmp(kind, "DMIN")) {
            ret = 0;
            goto done;
        }
        if (dec) {
            if (n != 4 && n != 8 && n != 16) {
                *err = "decimal floating-point constants need a length of 4, 8 or 16 bytes";
                ret = -1;
                goto done;
            }
            dfp_special(&dfp_fmts[n == 4 ? 0 : n == 8 ? 1 : 2], kind, neg, out);
        } else if (ieee) {
            ieee_special(kind, neg, n, out);
        } else {
            if (n < 1 || (n > 8 && n != 16)) {
                *err = "hexadecimal floating-point constants need a length of 1 to 8 or 16 "
                       "bytes";
                ret = -1;
                goto done;
            }
            memset(out, 0, (size_t)n);
            if (n == 16) {
                if (!strcmp(kind, "MAX")) {
                    memset(out + 1, 0xFF, 7);
                    memset(out + 9, 0xFF, 7);
                    out[0] = 0x7F;
                    out[8] = 0x71;
                } else if (!strcmp(kind, "MIN")) {
                    out[1] = 0x10;
                    out[8] = 0x72;
                } else {
                    out[15] = 1;
                    out[8] = 0x72;
                }
            } else if (!strcmp(kind, "MAX")) {
                out[0] = 0x7F;
                memset(out + 1, 0xFF, (size_t)n - 1);
            } else if (!strcmp(kind, "MIN")) {
                if (n > 1) out[1] = 0x10;
            } else if (n > 1) {
                out[n - 1] = 1;
            }
            if (neg) {
                out[0] |= 0x80;
                if (n == 16) out[8] |= 0x80;
            }
        }
        ret = 1;
        goto done;
    }
    if (dec) {
        if (exp10 > 100000 || exp10 < -100000) {
            *err = "the exponent is too large or too small";
            ret = -1;
            goto done;
        }
        ret = dfp_bytes(t, neg, n, exp10, mode, out, err, warn);
        goto done;
    }
    {
        num = neg ? xasprintf("-%s", t) : t;
        num_alloc = neg;
        r = parse_decimal(num, &neg, &N, &D);
        parsed = 1;
        if (r == -1) {
            *err = "the number is too large or too small";
            ret = -1;
            goto done;
        }
        if (r == 0) {
            ret = 0;
            goto done;
        }
        if (n < 1) n = 1;
        if (!big_is_zero(&N) && exp10) {
            if (exp10 > 6000 || exp10 < -6000) {
                *err = "the number is too large or too small";
                ret = -1;
                goto done;
            }
            mul_pow10(&N, &D, exp10);
        }
        if (big_is_zero(&N)) {
            if (!ieee && n > 16) {
                *err = "hexadecimal floating-point constants need a length of 1 to 16 bytes";
                ret = -1;
                goto done;
            }
            memset(out, 0, (size_t)n);
            if (neg) out[0] = 0x80; /* a negative zero keeps its sign */
            if (neg && n == 16 && !ieee) out[8] = 0x80;
            ret = 1;
            goto done;
        }
        ret = ieee ? ieee_bytes(&N, &D, neg, n, mode, out, err, warn)
                   : hfp_bytes(&N, &D, neg, n, scale, mode, out, err);
    }
done:
    if (num_alloc) free(num);
    if (parsed && !big_arena) {
        free(N.d);
        free(D.d);
    }
    free(t0);
    return ret;
}

int float_bytes(const char *text, int ieee, int n, uint8_t *out, const char **err) {
    const char *warn;
    int r = float_const(text, ieee ? 'B' : 0, n, 0, 0, out, err, &warn);
    return r == 2 ? 1 : r;
}

/* F and H: the nominal value (optionally a decimal number with fraction and
 * exponent, a leading U for unsigned) times 10^exp10 times 2^scale, rounded
 * to a whole number (half away from zero), in n bytes of two's complement.
 * 1 ok, 0 not a number, -1 out of range (err says why). */
int fixed_bytes(const char *text, int n, long exp10, long scale, uint8_t *out, const char **err) {
    Big N, D, q, r, lim, two;
    int neg, u = 0, rc;
    *err = NULL;
    if (*text == 'U' || *text == 'u') {
        u = 1;
        text++;
        if (*text == '+' || *text == '-') return 0;
    }
    rc = parse_decimal(text, &neg, &N, &D);
    if (rc == -1) {
        *err = "the number is too large or too small";
        return -1;
    }
    if (rc == 0) return 0;
    if (u && neg) return 0;
    if (exp10 > 6000 || exp10 < -6000 || scale > 1000 || scale < -1000) {
        *err = "the number is too large or too small";
        return -1;
    }
    if (!big_is_zero(&N)) {
        if (exp10) mul_pow10(&N, &D, exp10);
        if (scale > 0)
            big_shl(&N, (size_t)scale);
        else if (scale < 0)
            big_shl(&D, (size_t)-scale);
    }
    /* q = floor((2N + D) / 2D) */
    two = big_copy(&N);
    big_shl(&two, 1);
    big_add(&two, &D);
    big_shl(&D, 1);
    big_divmod(&two, &D, &q, &r);
    lim = big_u64(1);
    big_shl(&lim, (size_t)(8 * n));
    if (neg && !big_is_zero(&q)) {
        Big half = big_copy(&lim);
        big_shr(&half, 1);
        if (big_cmp(&q, &half) > 0) {
            *err = "the value does not fit in the length of the constant";
            return -1;
        }
        big_sub(&lim, &q); /* 2^(8n) - q */
        q = lim;
    } else {
        Big top = big_copy(&lim);
        if (!u) { /* signed values stop at 2^(8n-1) - 1 (HLASM ASMA072E); U'n' is unsigned */
            big_shr(&top, 1);
        }
        if (big_cmp(&q, &top) >= 0) {
            *err = "the value does not fit in the length of the constant";
            return -1;
        }
    }
    big_to_be(&q, out, (size_t)n);
    return 1;
}

int dc_float(Asm *a, Stmt *st, const DcSpec *sp, DcVec *out) {
    int n = (int)dc_deflen(sp);
    OperandVec items = {0};
    size_t k;
    if (sp->has_len) n = (int)sp->length;
    split_nominal(sp, &items);
    vec_push(&a->opvecs, items);
    for (k = 0; k < items.n; k++) {
        char *t = str_trim(items.v[k].text);
        uint8_t *b = xcalloc((size_t)(n < 1 ? 1 : n) + 1);
        const char *err, *warn;
        int r;
        DcVal v;
        float_set_type(sp->ext == 'B' ? sp->typ : 0);
        r = float_const(t, sp->ext, n, (long)sp->exp10, (long)sp->scale, b, &err, &warn);
        float_set_type(0);
        if (r == 0) {
            diag_help(dc_err(a, st, "ZA2050", items.v[k].off, (int)strlen(items.v[k].text) + 1,
                             "\"%s\" is not a decimal floating-point number", items.v[k].text),
                      "write for example `%c'1.5'`, `%c'-2E3'` or `%c'0'`", sp->typ, sp->typ,
                      sp->typ);
            free(t);
            free(b);
            return 0;
        }
        if (r < 0) {
            dc_err(a, st, "ZA2053", items.v[k].off, (int)strlen(items.v[k].text) + 1, "%s", err);
            free(t);
            free(b);
            return 0;
        }
        if (r == 2 && a->pass == 1)
            rep_warnf(a->rep, "ZA2066", stmt_operand_span(st, items.v[k].off, (int)strlen(items.v[k].text)),
                      "%s", warn);
        memset(&v, 0, sizeof v);
        v.b = b;
        v.n = (size_t)n;
        vec_push(out, v);
        free(t);
    }
    return 1;
}
