/* macro_val.c - values and the built-in functions of the macro language (MACRO.md 4, 5). */
#include <stdlib.h>
#include <string.h>

#include "macro_int.h"
#include "ebcdic.h"

#define MAX_STR ((size_t)4 << 20)

/* ---- construction -------------------------------------------------------- */

MVal v_int(int64_t i) {
    MVal v;
    memset(&v, 0, sizeof v);
    v.t = V_INT;
    v.i = i;
    return v;
}

MVal v_str(const char *s) {
    MVal v;
    memset(&v, 0, sizeof v);
    v.t = V_STR;
    v.s = s;
    v.n = strlen(s);
    return v;
}

MVal v_strn(const char *s, size_t n) {
    MVal v;
    memset(&v, 0, sizeof v);
    v.t = V_STR;
    v.s = xstrndup(s, n);
    v.n = n;
    return v;
}

MVal v_list(const MVal *items, size_t n) {
    MVal v;
    memset(&v, 0, sizeof v);
    v.t = V_LIST;
    v.items = items;
    v.n = n;
    return v;
}

const char *v_text(MVal v) {
    switch (v.t) {
    case V_INT:
        return xasprintf("%lld", (long long)v.i);
    case V_STR:
        return v.s;
    case V_LIST: {
        Buf b = {0};
        size_t i;
        buf_putc(&b, '(');
        for (i = 0; i < v.n; i++) {
            if (i) buf_putc(&b, ',');
            buf_puts(&b, v_text(v.items[i]));
        }
        buf_putc(&b, ')');
        return xstrdup(buf_cstr(&b));
    }
    }
    return "";
}

int v_truth(MVal v) {
    switch (v.t) {
    case V_INT:
        return v.i != 0;
    case V_STR:
    case V_LIST:
        return v.n > 0;
    }
    return 0;
}

/* ---- numbers ------------------------------------------------------------- */

static int digit_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}

int v_parse_int(const char *s, int64_t *out) {
    size_t n, i;
    uint64_t v = 0;
    int neg = 0;
    while (*s && is_blank((unsigned char)*s)) s++;
    n = strlen(s);
    while (n > 0 && is_blank((unsigned char)s[n - 1])) n--;
    if (n == 0) return 0;
    if (n >= 3 && s[1] == '\'' && s[n - 1] == '\'') {
        int t = za_toupper((unsigned char)s[0]);
        if (t == 'X' || t == 'B') {
            int base = t == 'X' ? 16 : 2;
            if (n == 3) return 0;
            for (i = 2; i < n - 1; i++) {
                int d = digit_val((unsigned char)s[i]);
                if (d >= base) return 0;
                v = v * (uint64_t)base + (uint64_t)d;
            }
            *out = (int64_t)v;
            return 1;
        }
        if (t == 'C') {
            size_t k = 0;
            for (i = 2; i < n - 1; i++) {
                if (s[i] == '\'') {
                    if (i + 1 >= n - 1 || s[i + 1] != '\'') return 0;
                    i++;
                }
                if (++k > 8) return 0;
                v = (v << 8) | ZA_E(s[i]);
            }
            if (k == 0) return 0;
            *out = (int64_t)v;
            return 1;
        }
        return 0;
    }
    i = 0;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i = 1;
    }
    if (i >= n) return 0;
    if (i + 2 < n + 0 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        for (i += 2; i < n; i++) {
            int d = digit_val((unsigned char)s[i]);
            if (d >= 16) return 0;
            v = v * 16 + (uint64_t)d;
        }
    } else if (i + 2 < n + 0 && s[i] == '0' && (s[i + 1] == 'b' || s[i + 1] == 'B')) {
        for (i += 2; i < n; i++) {
            if (s[i] != '0' && s[i] != '1') return 0;
            v = v * 2 + (uint64_t)(s[i] - '0');
        }
    } else {
        for (; i < n; i++) {
            if (!za_isdigit((unsigned char)s[i])) return 0;
            v = v * 10 + (uint64_t)(s[i] - '0');
        }
    }
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return 1;
}

/* ---- lists ---------------------------------------------------------------- */

int v_is_list(MVal v) {
    if (v.t == V_LIST) return 1;
    if (v.t == V_STR && v.n >= 2 && v.s[0] == '(') return match_paren(v.s, 0) == (long)v.n - 1;
    return 0;
}

size_t v_items(MVal v, const MVal **items) {
    if (v.t == V_LIST) {
        *items = v.items;
        return v.n;
    }
    if (v_is_list(v)) {
        char *inner = xstrndup(v.s + 1, v.n - 2);
        OperandVec ov = {0};
        MVal *a;
        size_t i, k = 0;
        for (i = 0; inner[i] && is_blank((unsigned char)inner[i]); i++) {
        }
        if (!inner[i]) {
            *items = NULL;
            return 0;
        }
        split_operands(&ov, inner);
        a = xcalloc(ov.n * sizeof *a);
        for (i = 0; i < ov.n; i++) a[k++] = v_str(ov.v[i].text);
        *items = a;
        return k;
    }
    if (v.t == V_STR && v.n == 0) {
        *items = NULL;
        return 0;
    }
    {
        MVal *a = xcalloc(sizeof *a);
        a[0] = v;
        *items = a;
        return 1;
    }
}

/* ---- argument helpers ------------------------------------------------------ */

static int64_t arg_int(Ctx *c, MVal a) {
    int64_t i;
    if (a.t == V_INT) return a.i;
    if (a.t == V_STR && v_parse_int(a.s, &i)) return i;
    ma_err(c, "ZA4112", "\"%s\" is not a number", v_text(a));
    return 0;
}

static const char *arg_text(MVal a) { return v_text(a); }

static MVal empty_str(void) { return v_str(""); }

/* ---- functions -------------------------------------------------------------- */

static MVal f_len(Ctx *c, MVal *a, int n) {
    (void)c;
    (void)n;
    return v_int((int64_t)strlen(arg_text(a[0])));
}

static MVal f_count(Ctx *c, MVal *a, int n) {
    const MVal *it;
    (void)c;
    (void)n;
    return v_int((int64_t)v_items(a[0], &it));
}

static MVal f_substr(Ctx *c, MVal *a, int n) {
    const char *s = arg_text(a[0]);
    int64_t len = (int64_t)strlen(s), st = arg_int(c, a[1]), cnt;
    if (st < 0 || st > len) {
        ma_err(c, "ZA4112", "substr: start %lld is outside the string (length %lld)", (long long)st,
               (long long)len);
        return empty_str();
    }
    cnt = n > 2 ? arg_int(c, a[2]) : len - st;
    if (cnt < 0 || st + cnt > len) {
        ma_err(c, "ZA4112", "substr: %lld characters from %lld run past the end (length %lld)",
               (long long)cnt, (long long)st, (long long)len);
        return empty_str();
    }
    return v_strn(s + st, (size_t)cnt);
}

static MVal f_index(Ctx *c, MVal *a, int n) {
    const char *s = arg_text(a[0]), *t = arg_text(a[1]), *p;
    (void)c;
    (void)n;
    if (!*t) return v_int(-1);
    p = strstr(s, t);
    return v_int(p ? (int64_t)(p - s) : -1);
}

static MVal f_upper(Ctx *c, MVal *a, int n) {
    (void)c;
    (void)n;
    return v_str(za_strupper(arg_text(a[0])));
}

static MVal f_lower(Ctx *c, MVal *a, int n) {
    (void)c;
    (void)n;
    return v_str(za_strlower(arg_text(a[0])));
}

static MVal f_trim(Ctx *c, MVal *a, int n) {
    const char *s = arg_text(a[0]);
    size_t l;
    (void)c;
    (void)n;
    while (*s && is_blank((unsigned char)*s)) s++;
    l = strlen(s);
    while (l > 0 && is_blank((unsigned char)s[l - 1])) l--;
    return v_strn(s, l);
}

static MVal f_repeat(Ctx *c, MVal *a, int n) {
    const char *s = arg_text(a[0]);
    int64_t k = arg_int(c, a[1]);
    Buf b = {0};
    (void)n;
    if (k < 0 || (size_t)k * strlen(s) > MAX_STR) {
        ma_err(c, "ZA4112", "repeat: count %lld is out of range", (long long)k);
        return empty_str();
    }
    while (k-- > 0) buf_puts(&b, s);
    return v_str(xstrdup(buf_cstr(&b)));
}

static MVal f_double(Ctx *c, MVal *a, int n) {
    const char *s = arg_text(a[0]);
    Buf b = {0};
    (void)c;
    (void)n;
    for (; *s; s++) {
        buf_putc(&b, *s);
        if (*s == '\'' || *s == '&') buf_putc(&b, *s);
    }
    return v_str(xstrdup(buf_cstr(&b)));
}

static MVal f_quote(Ctx *c, MVal *a, int n) {
    const char *s = arg_text(a[0]);
    Buf b = {0};
    (void)c;
    (void)n;
    buf_putc(&b, '\'');
    for (; *s; s++) {
        buf_putc(&b, *s);
        if (*s == '\'') buf_putc(&b, *s);
    }
    buf_putc(&b, '\'');
    return v_str(xstrdup(buf_cstr(&b)));
}

static MVal f_byte(Ctx *c, MVal *a, int n) {
    int64_t k = arg_int(c, a[0]);
    char t[2];
    (void)n;
    if (k < 1 || k > 255) {
        ma_err(c, "ZA4112", "byte: code %lld is not in 1..255", (long long)k);
        return empty_str();
    }
    t[0] = (char)k;
    t[1] = 0;
    return v_strn(t, 1);
}

static MVal f_chars(Ctx *c, MVal *a, int n) {
    const char *s = arg_text(a[0]);
    size_t l = strlen(s), i;
    MVal *it = xcalloc((l + 1) * sizeof *it);
    (void)c;
    (void)n;
    for (i = 0; i < l; i++) it[i] = v_strn(s + i, 1);
    return v_list(it, l);
}

static MVal f_split(Ctx *c, MVal *a, int n) {
    const char *s = arg_text(a[0]), *sep = arg_text(a[1]), *p;
    size_t sl = strlen(sep), k = 0;
    Vec(MVal) out = {0};
    (void)n;
    if (sl == 0) {
        ma_err(c, "ZA4112", "split: the separator is empty");
        return v_list(NULL, 0);
    }
    if (!*s) return v_list(NULL, 0);
    while ((p = strstr(s + k, sep)) != NULL) {
        vec_push(&out, v_strn(s + k, (size_t)(p - s) - k));
        k = (size_t)(p - s) + sl;
    }
    vec_push(&out, v_str(xstrdup(s + k)));
    return v_list(out.v, out.n);
}

static MVal f_join(Ctx *c, MVal *a, int n) {
    const MVal *it;
    size_t cnt = v_items(a[0], &it), i;
    const char *sep = arg_text(a[1]);
    Buf b = {0};
    (void)c;
    (void)n;
    for (i = 0; i < cnt; i++) {
        if (i) buf_puts(&b, sep);
        buf_puts(&b, v_text(it[i]));
    }
    return v_str(xstrdup(buf_cstr(&b)));
}

static MVal f_list(Ctx *c, MVal *a, int n) {
    MVal *it = xcalloc((size_t)(n + 1) * sizeof *it);
    int i;
    (void)c;
    for (i = 0; i < n; i++) it[i] = a[i];
    return v_list(it, (size_t)n);
}

/* fill(n, v): a list of n copies of v (the declared array of HLASM LCLA &A(n)) */
static MVal f_fill(Ctx *c, MVal *a, int n) {
    int64_t k = arg_int(c, a[0]), i;
    MVal *it;
    (void)n;
    if (k < 0 || k > 1000000) {
        ma_err(c, "ZA4112", "fill: the size %lld is out of range", (long long)k);
        k = 0;
    }
    it = xcalloc((size_t)(k + 1) * sizeof *it);
    for (i = 0; i < k; i++) it[i] = a[1];
    return v_list(it, (size_t)k);
}

static MVal f_int(Ctx *c, MVal *a, int n) {
    (void)n;
    return v_int(arg_int(c, a[0]));
}

static MVal f_str(Ctx *c, MVal *a, int n) {
    (void)c;
    (void)n;
    return v_str(arg_text(a[0]));
}

static MVal f_radix(Ctx *c, MVal *a, int n, int bits) {
    uint64_t v = (uint64_t)arg_int(c, a[0]);
    int64_t digits = n > 1 ? arg_int(c, a[1]) : 0;
    char tmp[72];
    int k = 0, i;
    Buf b = {0};
    if (digits < 0 || digits > 64) {
        ma_err(c, "ZA4112", "the number of digits %lld is out of range", (long long)digits);
        return empty_str();
    }
    do {
        int d = (int)(v & (uint64_t)((1 << bits) - 1));
        tmp[k++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        v >>= bits;
    } while (v);
    while (k < digits) tmp[k++] = '0';
    for (i = k - 1; i >= 0; i--) buf_putc(&b, tmp[i]);
    return v_str(xstrdup(buf_cstr(&b)));
}

static MVal f_hex(Ctx *c, MVal *a, int n) { return f_radix(c, a, n, 4); }
static MVal f_bin(Ctx *c, MVal *a, int n) { return f_radix(c, a, n, 1); }

static MVal f_min(Ctx *c, MVal *a, int n) {
    int64_t m = arg_int(c, a[0]);
    int i;
    for (i = 1; i < n; i++) {
        int64_t k = arg_int(c, a[i]);
        if (k < m) m = k;
    }
    return v_int(m);
}

static MVal f_max(Ctx *c, MVal *a, int n) {
    int64_t m = arg_int(c, a[0]);
    int i;
    for (i = 1; i < n; i++) {
        int64_t k = arg_int(c, a[i]);
        if (k > m) m = k;
    }
    return v_int(m);
}

static MVal f_abs(Ctx *c, MVal *a, int n) {
    int64_t k = arg_int(c, a[0]);
    (void)n;
    return v_int(k < 0 ? (int64_t)(0 - (uint64_t)k) : k);
}

static MVal f_empty(Ctx *c, MVal *a, int n) {
    (void)c;
    (void)n;
    return v_int(a[0].t == V_INT ? 0 : a[0].n == 0);
}

int ma_defined(Ctx *c, const char *lname);

static MVal f_defined(Ctx *c, MVal *a, int n) {
    char *nm = za_strlower(arg_text(a[0]));
    (void)n;
    return v_int(ma_defined(c, nm));
}

/* the length attribute of a constant written as a term, 0 when it is not one */
static int64_t const_length(const char *s) {
    size_t n = strlen(s), i, k = 0;
    int t;
    if (n < 3 || s[1] != '\'' || s[n - 1] != '\'') return 0;
    t = za_toupper((unsigned char)s[0]);
    switch (t) {
    case 'C':
        for (i = 2; i < n - 1; i++) {
            if ((s[i] == '\'' || s[i] == '&') && i + 1 < n - 1 && s[i + 1] == s[i]) i++;
            k++;
        }
        return (int64_t)k;
    case 'X':
        return (int64_t)((n - 3 + 1) / 2);
    case 'B':
        return (int64_t)((n - 3 + 7) / 8);
    case 'F':
    case 'E':
        return 4;
    case 'H':
        return 2;
    case 'D':
        return 8;
    }
    return 0;
}

static MVal f_attr(Ctx *c, MVal *a, int n) {
    const char *s = arg_text(a[0]), *k = arg_text(a[1]);
    int64_t x;
    (void)n;
    if (strlen(k) != 1) {
        ma_err(c, "ZA4112", "attr: the attribute is one letter, T or L");
        return empty_str();
    }
    switch (za_toupper((unsigned char)k[0])) {
    case 'T':
        if (a[0].t == V_INT) return v_str("N");
        if (!*s) return v_str("O");
        if (v_parse_int(s, &x)) return v_str("N");
        return v_str("U");
    case 'L': {
        int64_t l = const_length(s);
        if (l > 0) return v_int(l);
        if (a[0].t == V_INT) return v_int(4);
        ma_warn(c, "ZA4130", "the length attribute of \"%s\" is not available at macro time", s);
        return v_int(1);
    }
    case 'D':
    case 'I':
    case 'S':
    case 'O':
        ma_warn(c, "ZA4130", "the attribute %c' of \"%s\" is not available at macro time",
                za_toupper((unsigned char)k[0]), s);
        return v_int(0);
    }
    ma_err(c, "ZA4112", "attr: unknown attribute \"%s\"", k);
    return empty_str();
}

static MVal f_aread(Ctx *c, MVal *a, int n) {
    char *r = ma_aread(c);
    (void)a;
    (void)n;
    return v_str(r ? r : "");
}

static const Fn fn_table[] = {
    {"len", 1, 1, f_len},       {"count", 1, 1, f_count},   {"substr", 2, 3, f_substr},
    {"index", 2, 2, f_index},   {"upper", 1, 1, f_upper},   {"lower", 1, 1, f_lower},
    {"trim", 1, 1, f_trim},     {"repeat", 2, 2, f_repeat}, {"double", 1, 1, f_double},
    {"quote", 1, 1, f_quote},   {"byte", 1, 1, f_byte},     {"chars", 1, 1, f_chars},
    {"split", 2, 2, f_split},   {"join", 2, 2, f_join},     {"list", 0, -1, f_list},
    {"int", 1, 1, f_int},       {"str", 1, 1, f_str},       {"hex", 1, 2, f_hex},
    {"bin", 1, 2, f_bin},       {"min", 1, -1, f_min},      {"max", 1, -1, f_max},
    {"abs", 1, 1, f_abs},       {"empty", 1, 1, f_empty},   {"defined", 1, 1, f_defined},
    {"attr", 2, 2, f_attr},      {"fill", 2, 2, f_fill},      {"aread", 0, 0, f_aread},
};

const Fn *fn_find(const char *name) {
    size_t i;
    for (i = 0; i < sizeof fn_table / sizeof fn_table[0]; i++)
        if (!strcmp(fn_table[i].name, name)) return &fn_table[i];
    return NULL;
}
