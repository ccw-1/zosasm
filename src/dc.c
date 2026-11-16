/* dc.c - DC and DS: parsing of constant specifications and generation of
 * the values (character, hex, binary, fixed point, address, decimal). */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "ebcdic.h"

#define DC_TYPES "C X B F H A Y V Q J R P Z E D L"

int64_t dc_alignment(const DcSpec *sp) {
    switch (sp->typ) {
    case 'F':
    case 'A':
    case 'Q':
    case 'J':
    case 'R':
        return sp->ext == 'D' ? 8 : 4;
    case 'H':
    case 'Y':
        return 2;
    case 'V':
    case 'E':
        return 4;
    case 'D':
        return 8;
    case 'L':
        return sp->ext == 'Q' ? 16 : 8;
    }
    return 1;
}

int64_t dc_deflen(const DcSpec *sp) {
    switch (sp->typ) {
    case 'F':
    case 'A':
    case 'Q':
    case 'J':
    case 'R':
        return sp->ext == 'D' ? 8 : 4;
    case 'H':
    case 'Y':
        return 2;
    case 'V':
    case 'E':
        return 4;
    case 'D':
        return 8;
    case 'L':
        return 16;
    case 'C':
        return sp->ext == 'U' ? 2 : 1;
    }
    return 1;
}

Diag *dc_err(Asm *a, Stmt *st, const char *code, int off, int n, const char *fmt, ...) {
    va_list ap;
    char *msg;
    Diag *d;
    va_start(ap, fmt);
    msg = xvasprintf(fmt, ap);
    va_end(ap);
    st->bad = 1;
    d = rep_errorf(a->rep, code, stmt_operand_span(st, off, n), "%s", msg);
    free(msg);
    return d;
}

void be_bytes(uint64_t v, int n, uint8_t *out) {
    int i;
    for (i = n - 1; i >= 0; i--) {
        out[i] = (uint8_t)v;
        v >>= 8;
    }
}

static uint8_t *zeros(size_t n) { return xcalloc(n ? n : 1); }

/* a number or (expression) at s[*i]; *i advances past it */
static int dc_number(Asm *a, Stmt *st, const char *s, size_t *i, int base, const char *what,
                     int64_t *out) {
    size_t len = strlen(s);
    if (*i < len && za_isdigit((unsigned char)s[*i])) {
        size_t j = *i;
        int64_t n = 0;
        int over = 0;
        while (j < len && za_isdigit((unsigned char)s[j])) {
            n = n * 10 + (s[j] - '0');
            if (n > ((int64_t)1 << 31)) over = 1, n = (int64_t)1 << 32;
            j++;
        }
        if (over) {
            dc_err(a, st, "ZA1004", base + (int)*i, (int)(j - *i), "%s %.*s is too large", what,
                   (int)(j - *i), s + *i);
            return 0;
        }
        *i = j;
        *out = n;
        return 1;
    }
    if (*i < len && s[*i] == '(') {
        long j = match_paren(s, *i);
        Operand o;
        char *w;
        int ok;
        if (j < 0) {
            dc_err(a, st, "ZA1003", base + (int)*i, 1, "unbalanced parenthesis");
            return 0;
        }
        o.text = xstrndup(s + *i + 1, (size_t)j - *i - 1);
        o.off = base + (int)*i + 1;
        w = xasprintf("the %s", what);
        ok = asm_abs_expr(a, st, o, w, out);
        free(w);
        *i = (size_t)j + 1;
        return ok;
    }
    dc_err(a, st, "ZA2011", base + (int)*i, 1, "a number or (expression) is expected for the %s",
           what);
    return 0;
}

/* the same with an optional sign */
static int dc_snumber(Asm *a, Stmt *st, const char *s, size_t *i, int base, const char *what,
                      int64_t *out) {
    int neg = 0;
    if (s[*i] == '+' || s[*i] == '-') {
        neg = s[*i] == '-';
        (*i)++;
    }
    if (!dc_number(a, st, s, i, base, what, out)) return 0;
    if (neg) *out = -*out;
    return 1;
}

/* range checks of the scale and exponent modifiers (HLASM Language Reference,
 * "Scale modifier" and "Exponent modifier") */
static int dc_check_modifiers(Asm *a, Stmt *st, DcSpec *sp) {
    int fixed = sp->typ == 'F' || sp->typ == 'H';
    int fp = sp->typ == 'E' || sp->typ == 'D' || sp->typ == 'L';
    if (!sp->has_scale && !sp->has_exp) return 1;
    if (!fixed && !fp) {
        int at = sp->has_scale ? sp->scale_off : sp->exp_off;
        diag_help(dc_err(a, st, "ZA2056", sp->off + at, 1,
                         "scale and exponent modifiers are only allowed for F, H, E, D and L "
                         "constants, not for %c",
                         sp->typ),
                  "remove the modifier");
        return 0;
    }
    if (sp->has_scale) {
        int64_t lo = fixed ? -187 : 0, hi = fixed ? 346 : sp->typ == 'L' ? 28 : 14;
        if (fp && sp->ext == 'D') {
            dc_err(a, st, "ZA2057", sp->off + sp->scale_off, 1,
                   "a decimal floating-point constant has no scale modifier");
            return 0;
        } else if (fp && sp->ext == 'B') {
            if (a->pass == 1)
                rep_warnf(a->rep, "ZA2059", stmt_operand_span(st, sp->off + sp->scale_off, 1),
                          "binary floating point has no scale modifier; it is ignored");
            sp->has_scale = 0;
        } else if (sp->scale < lo || sp->scale > hi) {
            dc_err(a, st, "ZA2057", sp->off + sp->scale_off, 1,
                   "scale modifier %lld is outside %lld to %lld for type %c%s", (long long)sp->scale,
                   (long long)lo, (long long)hi, sp->typ, fixed ? "" : " (hexadecimal)");
            return 0;
        }
    }
    if (sp->has_exp) {
        int wide = fp && (sp->ext == 'B' || sp->ext == 'H');
        int64_t lo = wide ? -2147483647 - 1 : -85, hi = wide ? 2147483647 : 75;
        if (sp->exp10 < lo || sp->exp10 > hi) {
            dc_err(a, st, "ZA2058", sp->off + sp->exp_off, 1,
                   "exponent modifier %lld is outside %lld to %lld", (long long)sp->exp10,
                   (long long)lo, (long long)hi);
            return 0;
        }
    }
    return 1;
}

/* Parse one operand of DC or DS: [dup]T[ext][Lnn]['nominal' | (nominal)].
 * Reports errors and returns 0 when the operand cannot be used. */
int dc_parse(Asm *a, Stmt *st, Operand o, DcSpec *sp) {
    const char *s = o.text;
    size_t len = strlen(s), i = 0;
    memset(sp, 0, sizeof *sp);
    sp->dup = 1;
    sp->off = o.off;
    sp->n = (int)len;
    if (!len) {
        diag_help(dc_err(a, st, "ZA2010", o.off, 1, "empty DC/DS operand"),
                  "write [duplication factor] type [length] ['value'], for example `4F'0'`, "
                  "`CL8'ABC'` or `A(LABEL)`");
        return 0;
    }
    if (za_isdigit((unsigned char)s[0]) || s[0] == '(') {
        if (!dc_number(a, st, s, &i, o.off, "duplication factor", &sp->dup)) return 0;
    }
    if (i >= len) {
        diag_help(dc_err(a, st, "ZA2012", o.off + (int)i - 1, 1, "the constant type is missing"),
                  "valid types: %s", DC_TYPES);
        return 0;
    }
    sp->typ = (char)za_toupper((unsigned char)s[i]);
    {
        const char *why = NULL;
        switch (sp->typ) {
        case 'S':
            why = "S-type address";
            break;
        case 'G':
            why = "graphic";
            break;
        }
        if (why) {
            diag_help(dc_err(a, st, "ZA9002", o.off + (int)i, 1, "%s constants are not supported yet",
                             why),
                      "supported types: %s", DC_TYPES);
            return 0;
        }
    }
    if (!strchr("CXBFHAYVQJRPZEDL", sp->typ)) {
        diag_help(dc_err(a, st, "ZA2013", o.off + (int)i, 1, "unknown constant type \"%c\"", s[i]),
                  "valid types: %s", DC_TYPES);
        return 0;
    }
    i++;
    if (i < len) {
        char c = (char)za_toupper((unsigned char)s[i]);
        int fp = sp->typ == 'E' || sp->typ == 'D' || sp->typ == 'L';
        if (sp->typ == 'C' && (c == 'A' || c == 'E')) {
            sp->ext = c;
            i++;
        } else if (sp->typ == 'C' && c == 'U') {
            sp->ext = 'U';
            i++;
        } else if ((sp->typ == 'F' || sp->typ == 'A' || sp->typ == 'Q' || sp->typ == 'J' ||
                    sp->typ == 'R') &&
                   c == 'D') {
            sp->ext = 'D';
            i++;
        } else if (sp->typ == 'Q' && c == 'Y') {
            dc_err(a, st, "ZA9002", o.off + (int)i - 1, 2,
                   "QY (long displacement) offset constants are not supported yet");
            return 0;
        } else if (fp && (c == 'B' || c == 'H')) {
            sp->ext = c;
            i++;
        } else if (fp && c == 'D') {
            sp->ext = 'D';
            i++;
        } else if (sp->typ == 'L' && c == 'Q') {
            sp->ext = 'Q';
            i++;
        }
    }
    /* modifiers */
    while (i < len) {
        char c = (char)za_toupper((unsigned char)s[i]);
        if (c == 'L') {
            int64_t n;
            i++;
            if (i < len && s[i] == '.') {
                dc_err(a, st, "ZA9002", o.off + (int)i - 1, 2,
                       "bit-length modifiers are not supported yet");
                return 0;
            }
            if (!dc_number(a, st, s, &i, o.off, "length modifier", &n)) return 0;
            sp->length = n;
            sp->has_len = 1;
            continue;
        }
        if ((c == 'S' || c == 'E') && i + 1 < len &&
            (za_isdigit((unsigned char)s[i + 1]) || s[i + 1] == '-' || s[i + 1] == '+' ||
             s[i + 1] == '(')) {
            int64_t n;
            int at = (int)i;
            i++;
            if (!dc_snumber(a, st, s, &i, o.off, c == 'S' ? "scale modifier" : "exponent modifier",
                            &n))
                return 0;
            if (c == 'S') {
                sp->has_scale = 1;
                sp->scale = n;
                sp->scale_off = at;
            } else {
                sp->has_exp = 1;
                sp->exp10 = n;
                sp->exp_off = at;
            }
            continue;
        }
        break;
    }
    /* nominal value */
    if (i < len) {
        if (s[i] == '\'') {
            long j = close_quote(s, i);
            if (j < 0) {
                dc_err(a, st, "ZA0002", o.off + (int)i, (int)(len - i), "unterminated quoted string");
                return 0;
            }
            sp->has_nom = sp->quoted = 1;
            sp->nom = xstrndup(s + i + 1, (size_t)j - i - 1);
            sp->nom_off = o.off + (int)i + 1;
            i = (size_t)j + 1;
        } else if (s[i] == '(') {
            long j = match_paren(s, i);
            if (j < 0) {
                dc_err(a, st, "ZA1003", o.off + (int)i, 1, "unbalanced parenthesis");
                return 0;
            }
            sp->has_nom = 1;
            sp->nom = xstrndup(s + i + 1, (size_t)j - i - 1);
            sp->nom_off = o.off + (int)i + 1;
            i = (size_t)j + 1;
        } else {
            diag_help(dc_err(a, st, "ZA2014", o.off + (int)i, (int)(len - i),
                             "unexpected \"%s\" in the constant specification", s + i),
                      "the nominal value goes in quotes ('...') or, for A/Y/V, in parentheses");
            return 0;
        }
        if (i < len) {
            dc_err(a, st, "ZA2014", o.off + (int)i, (int)(len - i),
                   "unexpected \"%s\" after the nominal value", s + i);
            return 0;
        }
    }
    /* A/Y/V take parentheses, everything else quotes */
    if (sp->has_nom) {
        int addr = strchr("AYVQJR", sp->typ) != NULL;
        if (addr && sp->quoted) {
            diag_help(dc_err(a, st, "ZA2015", sp->nom_off - 1, (int)strlen(sp->nom) + 2,
                             "%c-type values are written in parentheses", sp->typ),
                      "for example `%c(LABEL)`", sp->typ);
            return 0;
        }
        if (!addr && !sp->quoted) {
            diag_help(dc_err(a, st, "ZA2015", sp->nom_off - 1, (int)strlen(sp->nom) + 2,
                             "%c-type values are written in quotes", sp->typ),
                      "for example `%c'1'`", sp->typ);
            return 0;
        }
    }
    if (sp->has_len) {
        int64_t max = 8;
        if (strchr("CXBPZ", sp->typ)) max = 65535;
        if (sp->typ == 'L') max = 16;
        if (sp->length > max || (sp->length < 1 && !(sp->length == 0 && sp->dup >= 0))) {
            dc_err(a, st, "ZA2016", o.off, (int)len,
                   "length modifier %lld is outside 1 to %lld for type %c", (long long)sp->length,
                   (long long)max, sp->typ);
            return 0;
        }
    }
    if (sp->typ == 'C' && sp->ext == 'U' && sp->has_len && (sp->length & 1)) {
        diag_help(dc_err(a, st, "ZA2042", o.off, (int)len,
                         "the length of a CU constant must be a multiple of 2, not %lld",
                         (long long)sp->length),
                  "UTF-16 uses 2 bytes per code unit; CU'ab' is 4 bytes long");
        return 0;
    }
    if (!dc_check_modifiers(a, st, sp)) return 0;
    if (sp->dup < 0) {
        dc_err(a, st, "ZA2017", o.off, (int)len, "negative duplication factor");
        return 0;
    }
    return 1;
}

void split_nominal(const DcSpec *sp, OperandVec *out) {
    size_t i, start = 0, n = strlen(sp->nom);
    for (i = 0; i <= n; i++) {
        if (i == n || sp->nom[i] == ',') {
            Operand o;
            o.text = xstrndup(sp->nom + start, i - start);
            o.off = sp->nom_off + (int)start;
            vec_push(out, o);
            start = i + 1;
        }
    }
}

static void push_val(DcVec *v, uint8_t *b, size_t n) {
    DcVal x;
    memset(&x, 0, sizeof x);
    x.b = b;
    x.n = n;
    vec_push(v, x);
}

static char *unescape_quotes(const char *s) {
    Buf b = {0};
    size_t i;
    for (i = 0; s[i]; i++) {
        buf_putc(&b, s[i]);
        if ((s[i] == '\'' && s[i + 1] == '\'') || (s[i] == '&' && s[i + 1] == '&')) i++;
    }
    return xstrdup(buf_cstr(&b));
}

static int dc_char(Asm *a, Stmt *st, const DcSpec *sp, DcVec *out) {
    char *str = unescape_quotes(sp->nom);
    size_t sl = strlen(str), n, i;
    uint8_t *b;
    uint8_t pad = (sp->ext != 'E' && za_ascii) ? 0x20 : 0x40;
    if (sp->ext == 'U') { /* UTF-16, big endian, padded with U+0020 */
        Buf bb = {0};
        for (i = 0; i < sl;) {
            uint32_t cp;
            i += utf8_next(str + i, sl - i, &cp);
            if (cp >= 0x10000) {
                cp -= 0x10000;
                buf_putc(&bb, (int)(0xD8 | (cp >> 18)));
                buf_putc(&bb, (int)((cp >> 10) & 0xFF));
                buf_putc(&bb, (int)(0xDC | ((cp >> 8) & 3)));
                buf_putc(&bb, (int)(cp & 0xFF));
            } else {
                buf_putc(&bb, (int)(cp >> 8));
                buf_putc(&bb, (int)(cp & 0xFF));
            }
        }
        n = bb.len;
        b = xmalloc(n + 2);
        memcpy(b, bb.p ? bb.p : "", n);
        if (sp->has_len) {
            size_t want = (size_t)sp->length;
            if (want < n && want >= 2 && (b[want - 2] & 0xFC) == 0xD8) {
                diag_help(dc_err(a, st, "ZA2049", sp->nom_off - 1, (int)strlen(sp->nom) + 2,
                                 "the length %zu cuts a UTF-16 surrogate pair in two", want),
                          "use a length of %zu or more", want + 2);
                return 0;
            }
            {
                uint8_t *nb = xmalloc(want + 2);
                for (i = 0; i < want; i++) nb[i] = i < n ? b[i] : (i & 1 ? 0x20 : 0x00);
                b = nb;
                n = want;
            }
        } else if (n == 0) {
            diag_help(dc_err(a, st, "ZA2019", sp->nom_off - 1, 2,
                             "an empty character constant needs a length modifier"),
                      "write for example `CUL2''` for one blank");
            return 0;
        }
        push_val(out, b, n);
        return 1;
    }
    if (sp->ext == 'A') {
        Buf bb = {0};
        pad = 0x20;
        for (i = 0; i < sl;) {
            uint32_t cp;
            i += utf8_next(str + i, sl - i, &cp);
            buf_putc(&bb, cp > 255 ? '?' : (int)cp);
        }
        n = bb.len;
        b = xmalloc(n + 1);
        memcpy(b, bb.p ? bb.p : "", n);
    } else {
        b = sp->ext == 'E' ? ebcdic_from_source(str, sl, &n) : charset_from_source(str, sl, &n);
    }
    if (sp->has_len) {
        size_t want = (size_t)sp->length;
        uint8_t *nb = xmalloc(want + 1);
        for (i = 0; i < want; i++) nb[i] = i < n ? b[i] : pad;
        b = nb;
        n = want;
    } else if (n == 0) {
        diag_help(dc_err(a, st, "ZA2019", sp->nom_off - 1, 2,
                         "an empty character constant needs a length modifier"),
                  "write for example `CL1''` for one blank");
        return 0;
    }
    push_val(out, b, n);
    return 1;
}

static int dc_bits(Asm *a, Stmt *st, const DcSpec *sp, DcVec *out) {
    OperandVec items = {0};
    size_t k, m;
    split_nominal(sp, &items);
    for (k = 0; k < items.n; k++) {
        const char *t = items.v[k].text;
        size_t tl = strlen(t), nn;
        uint8_t *nib, *b;
        size_t nb;
        if (!tl) {
            dc_err(a, st, "ZA2020", items.v[k].off, 1, "empty value in %c constant", sp->typ);
            return 0;
        }
        nib = xmalloc(tl + 8);
        nn = 0;
        for (m = 0; m < tl; m++) {
            int c = (unsigned char)t[m], d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (sp->typ == 'X' && za_isxdigit(c))
                d = za_toupper(c) - 'A' + 10;
            else
                d = 255;
            if (d == 255 || (sp->typ == 'B' && d > 1)) {
                dc_err(a, st, "ZA2021", items.v[k].off + (int)m, 1, "\"%c\" is not a %s", c,
                       sp->typ == 'B' ? "binary digit (0 or 1)" : "hexadecimal digit");
                return 0;
            }
            nib[nn++] = (uint8_t)d;
        }
        if (sp->typ == 'X') {
            size_t off = nn % 2;
            nb = (nn + off) / 2;
            b = xcalloc(nb + 1);
            for (m = 0; m < nn; m++) {
                size_t pos = m + off;
                b[pos / 2] |= (uint8_t)(pos % 2 ? nib[m] : nib[m] << 4);
            }
        } else {
            size_t off = (8 - nn % 8) % 8;
            nb = (nn + off) / 8;
            b = xcalloc(nb + 1);
            for (m = 0; m < nn; m++) {
                size_t pos = m + off;
                b[pos / 8] |= (uint8_t)(nib[m] << (7 - pos % 8));
            }
        }
        if (sp->has_len) {
            size_t want = (size_t)sp->length;
            uint8_t *nbv = xcalloc(want + 1);
            if (nb >= want)
                memcpy(nbv, b + (nb - want), want);
            else
                memcpy(nbv + (want - nb), b, nb);
            b = nbv;
            nb = want;
        }
        push_val(out, b, nb);
    }
    return 1;
}

static int dc_int(Asm *a, Stmt *st, const DcSpec *sp, DcVec *out) {
    int n = (int)dc_deflen(sp);
    OperandVec items = {0};
    size_t k;
    if (sp->has_len) n = (int)sp->length;
    split_nominal(sp, &items);
    for (k = 0; k < items.n; k++) {
        char *t = str_trim(items.v[k].text);
        uint8_t *b = zeros((size_t)(n < 1 ? 1 : n));
        const char *err;
        int r = n < 1 ? 1 : fixed_bytes(t, n, (long)sp->exp10, (long)sp->scale, b, &err);
        if (r == 0) {
            diag_help(dc_err(a, st, "ZA2022", items.v[k].off, (int)strlen(items.v[k].text) + 1,
                             "\"%s\" is not a decimal number", items.v[k].text),
                      "write for example `%c'25'`, `%c'-1.5'`, `%c'2E3'` or `%c'U200'`", sp->typ,
                      sp->typ, sp->typ, sp->typ);
            return 0;
        }
        if (r < 0) {
            diag_help(dc_err(a, st, "ZA2023", items.v[k].off, (int)strlen(items.v[k].text), "%s",
                             err),
                      "with length %d the value, after scaling, must fit in %d byte(s); use a "
                      "longer length such as %cL%d",
                      n, n, sp->typ, n + 1);
            return 0;
        }
        push_val(out, b, (size_t)(n < 1 ? 0 : n));
    }
    return 1;
}

/* Turn the value of an A/Y operand into bytes and a relocation (pass 2). */
static int addr_con(Asm *a, Stmt *st, const Expr *e, Value v, int n, DcVal *res) {
    Term *terms = xmalloc((size_t)(v.nterms + 1) * sizeof *terms);
    int nt = 0, i;
    memset(res, 0, sizeof *res);
    res->b = zeros((size_t)n);
    res->n = (size_t)n;
    /* terms based on dummy-section pieces carry their offset in abs */
    for (i = 0; i < v.nterms; i++) {
        Base b = v.terms[i].base;
        if (b.kind == B_PIECE && ((Piece *)b.p)->cls->sect->kind == K_DSECT) continue;
        terms[nt++] = v.terms[i];
    }
    if (nt == 0) {
        if (n >= 1 && n < 8) {
            int64_t lo = -((int64_t)1 << (8 * n - 1)), hi = ((int64_t)1 << (8 * n)) - 1;
            if (v.abs < lo || v.abs > hi) {
                diag_help(rep_errorf(a->rep, "ZA2023", e->sp, "value %lld does not fit in %d byte(s)",
                                     (long long)v.abs, n),
                          "allowed range: %lld to %lld", (long long)lo, (long long)hi);
                st->bad = 1;
                return 0;
            }
        }
        be_bytes((uint64_t)v.abs, n, res->b);
        return 1;
    }
    {
        /* each term is +1 or -1; additions first. One relocation item per term. */
        int ok = nt >= 1 && nt <= 5, k, np = 0;
        DcRelTerm items[5];
        for (i = 0; ok && i < nt; i++)
            if (terms[i].coef != 1 && terms[i].coef != -1) ok = 0;
        if (!ok) {
            diag_help(diag_label(rep_errorf(a->rep, "ZA2025", e->sp,
                                            "this expression cannot be relocated"),
                                 "it combines relocatable terms in a way that has no relocation"),
                      "an address constant can hold up to 5 relocatable terms, each added or "
                      "subtracted once, plus an absolute value; for example A(X-Y) or "
                      "A(EXT1-EXT2+4)");
            st->bad = 1;
            return 0;
        }
        if (n != 3 && n != 4 && n != 8) {
            diag_help(rep_errorf(a->rep, "ZA2024", e->sp,
                                 "a relocatable address cannot be stored in %d byte(s)", n),
                      "relocatable A-type constants are 3, 4 or 8 bytes long (A, AL3, AD)");
            st->bad = 1;
            return 0;
        }
        for (k = 0; k < 2; k++) /* k == 0: additions, k == 1: subtractions */
            for (i = 0; i < nt; i++) {
                DcRelTerm *it = &items[np];
                if ((terms[i].coef == -1) != k) continue;
                memset(it, 0, sizeof *it);
                it->sub = k;
                if (terms[i].base.kind == B_CLASS) {
                    it->cls = terms[i].base.p;
                } else if (terms[i].base.kind == B_SYM) {
                    it->target = terms[i].base.p;
                } else {
                    rep_errorf(a->rep, "ZA2025", e->sp, "this expression cannot be relocated");
                    st->bad = 1;
                    return 0;
                }
                np++;
            }
        be_bytes((uint64_t)v.abs, n, res->b);
        res->has_rel = 1;
        res->kind = REL_ADDR;
        res->target = items[0].target;
        res->cls = items[0].cls;
        res->sub = items[0].sub;
        res->nmore = np - 1;
        for (k = 1; k < np; k++) res->more[k - 1] = items[k];
        return 1;
    }
}

static int dc_addr(Asm *a, Stmt *st, const DcSpec *sp, DcVec *out) {
    int n = (int)dc_deflen(sp), good = 1;
    OperandVec items = {0};
    size_t k;
    if (sp->has_len) n = (int)sp->length;
    split_operands(&items, sp->nom);
    for (k = 0; k < items.n; k++) {
        Operand it = items.v[k];
        Expr *e;
        Value v;
        DcVal val;
        it.off += sp->nom_off;
        e = asm_expr(a, st, it);
        if (!e) {
            good = 0;
            continue;
        }
        if (!asm_eval(a, st, e, 0, &v)) {
            good = 0;
            push_val(out, zeros((size_t)n), (size_t)n);
            continue;
        }
        if (!a->laid || !v.defined) {
            push_val(out, zeros((size_t)n), (size_t)n);
            continue;
        }
        if (!addr_con(a, st, e, v, n, &val)) good = 0;
        vec_push(out, val);
    }
    return good;
}

static int dc_v(Asm *a, Stmt *st, const DcSpec *sp, DcVec *out) {
    int n = (int)dc_deflen(sp), good = 1;
    OperandVec items = {0};
    size_t k;
    if (sp->has_len) n = (int)sp->length;
    if (n != 3 && n != 4 && n != 8) {
        dc_err(a, st, "ZA2024", sp->off, sp->n, "a V-type constant is 3, 4 or 8 bytes long, not %d",
               n);
        return 0;
    }
    split_operands(&items, sp->nom);
    for (k = 0; k < items.n; k++) {
        Operand it = items.v[k];
        char *raw = str_trim(it.text), *name;
        const char *why = NULL;
        int tl = (int)strlen(it.text);
        Symbol *s;
        DcVal v;
        name = sym_text(raw, &why);
        if (!name) {
            diag_help(dc_err(a, st, "ZA1021", sp->nom_off + it.off, tl,
                             "\"%s\" is not a valid symbol name", raw),
                      "%s", why);
            good = 0;
            continue;
        }
        s = asm_lookup(a, name);
        if (!s) {
            if (a->pass == 1) {
                int created;
                s = asm_define(a, st, name, stmt_operand_span(st, sp->nom_off + it.off, tl), &created);
                if (created) s->kind = SYM_EXT;
            }
        } else if (s->kind != SYM_EXT) {
            diag_help(diag_note(dc_err(a, st, "ZA2026", sp->nom_off + it.off, tl,
                                       "V-type constant names \"%s\", which is defined in this "
                                       "module",
                                       name),
                                s->def, "\"%s\" is defined here", name),
                      "use an A-type constant for symbols of this module");
            good = 0;
            continue;
        }
        memset(&v, 0, sizeof v);
        v.b = zeros((size_t)n);
        v.n = (size_t)n;
        v.has_rel = 1;
        v.target = s;
        v.kind = REL_V;
        vec_push(out, v);
    }
    return good;
}

/* the class named name (a class name or a part name) */
static Class *find_class(Asm *a, const char *name) {
    size_t i, k;
    Section *cur = a->sect;
    for (i = 0; i <= a->mod->sections.n; i++) {
        Section *sec = i == 0 ? cur : a->mod->sections.v[i - 1];
        if (!sec || sec->kind == K_DSECT) continue;
        for (k = 0; k < sec->classes.n; k++) {
            Class *c = sec->classes.v[k];
            if (c->part ? !strcmp(c->part, name) : !strcmp(c->name, name)) return c;
        }
    }
    return NULL;
}

/* Q (offset), J (length) and R (PSECT address) constants */
static int dc_xd(Asm *a, Stmt *st, const DcSpec *sp, int is_ds, DcVec *out) {
    int n = (int)dc_deflen(sp), good = 1;
    int big = sp->ext == 'D';
    OperandVec items = {0};
    size_t k;
    if (sp->has_len) n = (int)sp->length;
    if (sp->typ == 'Q' ? (n < 1 || n > (big ? 8 : 4))
                       : sp->typ == 'J' ? !(n == 2 || n == 3 || n == 4 || n == 8)
                                        : !(n == 3 || n == 4 || (big && n == 8))) {
        diag_help(dc_err(a, st, "ZA2029", sp->off, sp->n,
                         "a %c%s-type constant cannot be %d byte(s) long", sp->typ,
                         big ? "D" : "", n),
                  sp->typ == 'Q'   ? "Q is 1 to 4 bytes, QD 1 to 8"
                  : sp->typ == 'J' ? "J and JD are 2 to 4 bytes or 8"
                                   : "R is 3 or 4 bytes, RD 3, 4 or 8");
        return 0;
    }
    split_operands(&items, sp->nom);
    for (k = 0; k < items.n; k++) {
        Operand it = items.v[k];
        char *raw = str_trim(it.text), *name;
        const char *why = NULL;
        int tl = (int)strlen(it.text), off = sp->nom_off + it.off;
        Symbol *s;
        DcVal v;
        name = sym_text(raw, &why);
        if (!name) {
            diag_help(dc_err(a, st, "ZA1021", off, tl, "\"%s\" is not a valid symbol name", raw),
                      "%s", why);
            good = 0;
            continue;
        }
        memset(&v, 0, sizeof v);
        v.b = zeros((size_t)n);
        v.n = (size_t)n;
        if (is_ds) {
            vec_push(out, v);
            continue;
        }
        if (a->pass == 1) {
            if (sp->typ != 'R') {
                XdRef r;
                r.name = name;
                r.sp = stmt_operand_span(st, off, tl);
                r.sect = a->sect;
                vec_push(&a->xdrefs, r);
            }
            vec_push(out, v);
            continue;
        }
        s = asm_lookup(a, name);
        v.has_rel = 1;
        if (sp->typ == 'R') {
            v.kind = REL_R;
            if (!s || (s->kind != SYM_EXT && s->kind != SYM_SECT && s->kind != SYM_LABEL) ||
                (s->kind == SYM_SECT && s->sect->kind == K_DSECT) ||
                (s->kind == SYM_LABEL && !s->entry)) {
                diag_help(dc_err(a, st, "ZA2060", off, tl,
                                 "R-type constant names \"%s\", which is not an external symbol, "
                                 "a section or an ENTRY label",
                                 name),
                          "R(SYMBOL) holds the PSECT address of SYMBOL (see XATTR PSECT)");
                good = 0;
            }
            v.target = s;
        } else {
            v.kind = sp->typ == 'Q' ? REL_Q : REL_LEN;
            if (s && (s->kind == SYM_DXD || s->xd)) {
                v.target = s;
            } else if (s && s->kind == SYM_SECT && s->piece && s->piece->cls->part &&
                       !strcmp(s->piece->cls->part, name)) {
                v.cls = s->piece->cls;
            } else if (sp->typ == 'J' && (v.cls = find_class(a, name)) != NULL) {
                /* the length of a class */
            } else {
                diag_help(dc_err(a, st, "ZA2060", off, tl,
                                 "%c-type constant names \"%s\", which is not %s", sp->typ, name,
                                 sp->typ == 'Q' ? "an external dummy section or part"
                                                : "an external dummy section, part or class"),
                          "define it with DXD, or name a DSECT or a part (CATTR ...,PART(name))");
                good = 0;
            }
        }
        vec_push(out, v);
    }
    return good;
}

static int dc_decimal(Asm *a, Stmt *st, const DcSpec *sp, DcVec *out) {
    OperandVec items = {0};
    size_t k, m;
    split_nominal(sp, &items);
    for (k = 0; k < items.n; k++) {
        char *t = str_trim(items.v[k].text);
        const char *p = t;
        int neg = 0, dots = 0;
        uint8_t *dig = xmalloc(strlen(t) + 2);
        size_t nd = 0, nb;
        uint8_t *b;
        if (*p == '+' || *p == '-') neg = *p++ == '-';
        for (; *p; p++) {
            if (za_isdigit((unsigned char)*p)) {
                dig[nd++] = (uint8_t)(*p - '0');
            } else if (*p == '.' && dots == 0) {
                dots++;
            } else {
                const char *at = strchr(items.v[k].text, *p);
                diag_help(dc_err(a, st, "ZA2027", items.v[k].off + (int)(at - items.v[k].text), 1,
                                 "\"%c\" is not valid in a %c constant", *p, sp->typ),
                          "write an optional sign and decimal digits, for example `%c'-123'`",
                          sp->typ);
                return 0;
            }
        }
        if (nd == 0) {
            dc_err(a, st, "ZA2020", items.v[k].off, 1, "a %c constant needs at least one digit",
                   sp->typ);
            return 0;
        }
        if (sp->typ == 'P') {
            size_t total = nd + 1, off = total % 2;
            nb = (total + off) / 2;
            b = xcalloc(nb + 1);
            for (m = 0; m < total; m++) {
                size_t pos = m + off;
                uint8_t nibv = m < nd ? dig[m] : (uint8_t)(neg ? 0xD : 0xC);
                b[pos / 2] |= (uint8_t)(pos % 2 ? nibv : nibv << 4);
            }
            if (sp->has_len) {
                size_t want = (size_t)sp->length;
                uint8_t *nbv;
                if (nb > want) {
                    dc_err(a, st, "ZA2028", items.v[k].off, (int)strlen(items.v[k].text),
                           "packed value needs %d byte(s) but the length is %d", (int)nb,
                           (int)want);
                    return 0;
                }
                nbv = xcalloc(want + 1);
                memcpy(nbv + (want - nb), b, nb);
                b = nbv;
                nb = want;
            }
        } else {
            nb = nd;
            b = xmalloc(nb + 1);
            for (m = 0; m < nd; m++) b[m] = (uint8_t)(0xF0 | dig[m]);
            b[nb - 1] = (uint8_t)((neg ? 0xD0 : 0xC0) | (b[nb - 1] & 0x0F));
            if (sp->has_len) {
                size_t want = (size_t)sp->length;
                uint8_t *nbv;
                if (nb > want) {
                    dc_err(a, st, "ZA2028", items.v[k].off, (int)strlen(items.v[k].text),
                           "zoned value needs %d byte(s) but the length is %d", (int)nb, (int)want);
                    return 0;
                }
                nbv = xmalloc(want + 1);
                memset(nbv, 0xF0, want);
                memcpy(nbv + (want - nb), b, nb);
                b = nbv;
                nb = want;
            }
        }
        push_val(out, b, nb);
    }
    return 1;
}

/* the values of one parsed operand; for DS the data is only sized */
static int dc_values(Asm *a, Stmt *st, const DcSpec *sp, int is_ds, DcVec *vals) {
    int ok = 0;
    size_t i;
    if (!sp->has_nom) {
        int64_t n;
        if (!is_ds) {
            diag_help(dc_err(a, st, "ZA2018", sp->off, sp->n, "DC needs a nominal value"),
                      "use DS to reserve storage without a value");
            return 0;
        }
        n = dc_deflen(sp);
        if (sp->has_len) n = sp->length;
        push_val(vals, zeros((size_t)n), (size_t)n);
        return 1;
    }
    switch (sp->typ) {
    case 'C':
        ok = dc_char(a, st, sp, vals);
        break;
    case 'X':
    case 'B':
        ok = dc_bits(a, st, sp, vals);
        break;
    case 'F':
    case 'H':
        ok = dc_int(a, st, sp, vals);
        break;
    case 'A':
    case 'Y':
        ok = dc_addr(a, st, sp, vals);
        break;
    case 'V':
        ok = dc_v(a, st, sp, vals);
        break;
    case 'Q':
    case 'J':
    case 'R':
        ok = dc_xd(a, st, sp, is_ds, vals);
        break;
    case 'P':
    case 'Z':
        ok = dc_decimal(a, st, sp, vals);
        break;
    case 'E':
    case 'D':
    case 'L':
        ok = dc_float(a, st, sp, vals);
        break;
    }
    if (ok && is_ds) {
        for (i = 0; i < vals->n; i++) {
            DcVal *v = &vals->v[i];
            size_t n = v->n;
            memset(v, 0, sizeof *v);
            v->b = zeros(n);
            v->n = n;
        }
    }
    return ok;
}

/* DC and DS */
void dir_dc(Asm *a, Stmt *st) {
    int is_ds = za_streqi(st->op, "DS");
    OperandVec ops = {0};
    int label_done = 0;
    size_t k;
    if (!asm_need_section(a, st)) return;
    if (!asm_ops(a, st, 1, -1, &ops)) {
        asm_define_label(a, st, 1, 0);
        return;
    }
    for (k = 0; k < ops.n; k++) {
        DcSpec sp;
        DcVec vals = {0};
        int ok = dc_parse(a, st, ops.v[k], &sp);
        int64_t total = 0, r;
        size_t j;
        if (ok && k == 0) a->star_len = sp.has_len ? sp.length : strchr("CXBPZ", sp.typ) ? 0 : dc_deflen(&sp);
        if (ok) ok = dc_values(a, st, &sp, is_ds, &vals);
        if (!ok) {
            if (!label_done) {
                asm_define_label(a, st, 1, 0);
                label_done = 1;
            }
            continue;
        }
        if (!sp.has_len) asm_align(a, dc_alignment(&sp));
        if (!label_done) {
            int64_t l = vals.n > 0 ? (int64_t)vals.v[0].n : 0;
            Symbol *ls = asm_define_label(a, st, l, sp.typ);
            if (ls) {
                ls->count = sp.dup * (int64_t)vals.n;
                ls->align = sp.has_len ? 1 : (int)dc_alignment(&sp);
                if (sp.has_scale) ls->scale = (int)sp.scale;
            }
            label_done = 1;
        }
        for (j = 0; j < vals.n; j++) total += (int64_t)vals.v[j].n;
        if (is_ds) {
            asm_advance(a, total * sp.dup);
            continue;
        }
        for (r = 0; r < sp.dup; r++) {
            for (j = 0; j < vals.n; j++) {
                DcVal *v = &vals.v[j];
                asm_put_val(a, v);
            }
        }
    }
}

/* DXD: an external dummy section, a pseudo register of the class B_PRV; its
 * length and alignment are those of the DS operands (nothing is stored) */
void dir_dxd(Asm *a, Stmt *st) {
    OperandVec ops = {0};
    int64_t loc = 0;
    int64_t align = 1;
    size_t k;
    Symbol *s;
    int created;
    if (!asm_need_section(a, st)) return;
    if (!st->name[0]) {
        if (a->pass == 1) {
            diag_help(rep_errorf(a->rep, "ZA2061", st->op_span, "DXD needs a name"),
                      "write the name in column 1, for example `WORK     DXD   CL100`");
            st->bad = 1;
        }
        return;
    }
    if (a->sect->kind == K_DSECT) {
        if (a->pass == 1) {
            rep_errorf(a->rep, "ZA2063", st->op_span, "DXD is not allowed in a DSECT");
            st->bad = 1;
        }
        return;
    }
    if (!asm_ops(a, st, 1, -1, &ops)) return;
    for (k = 0; k < ops.n; k++) {
        DcSpec sp;
        DcVec vals = {0};
        int64_t total = 0, al;
        size_t j;
        if (!dc_parse(a, st, ops.v[k], &sp) || !dc_values(a, st, &sp, 1, &vals)) continue;
        al = sp.has_len ? 1 : dc_alignment(&sp);
        if (al > 1) loc += (al - loc % al) % al;
        if (al > align) align = al;
        for (j = 0; j < vals.n; j++) total += (int64_t)vals.v[j].n;
        loc += total * sp.dup;
    }
    if (a->pass != 1 || st->bad) return;
    if (!asm_valid_stmt_name(st)) {
        rep_errorf(a->rep, "ZA1021", st->name_span, "\"%s\" is not a valid symbol name", st->name);
        st->bad = 1;
        return;
    }
    s = asm_define(a, st, st->name, st->name_span, &created);
    if (!created) return;
    s->kind = SYM_DXD;
    s->len = 1;
    s->xd = 1;
    s->xd_len = loc;
    s->xd_align = (int)align;
    s->xd_sect = a->sect;
    s->sect = a->sect;
    a->sect->prv = 1;
}

/* CXD: a fullword for the total length of the external dummy sections */
void dir_cxd(Asm *a, Stmt *st) {
    OperandVec ops = {0};
    static const uint8_t zero[4];
    if (!asm_need_section(a, st)) return;
    if (!asm_ops(a, st, 0, 0, &ops)) return;
    asm_align(a, 4);
    {
        Symbol *ls = asm_define_label(a, st, 4, 'F');
        if (ls) ls->align = 4;
    }
    asm_reloc(a, 4, NULL, NULL, REL_CXD, 0);
    asm_put(a, zero, 4);
    if (a->pass == 1) a->sect->prv = 1;
}

/* CCW, CCW0 and CCW1: an 8 byte channel command word, doubleword aligned.
 * Format 0: command, 24 bit address, flags, 0, count. Format 1: command,
 * flags, count, 31 bit address. */
void dir_ccw(Asm *a, Stmt *st) {
    int fmt1 = za_streqi(st->op, "CCW1");
    OperandVec ops = {0};
    Symbol *ls;
    uint8_t head[8] = {0}, tail[8] = {0};
    int64_t cmd = 0, flags = 0, count = 0;
    DcVal addr;
    int ok = 1, k;
    size_t nh, nt;
    static const char *const what[] = {"the command code", "the data address", "the flags",
                                       "the data count"};
    static const int64_t maxv[] = {255, 0, 255, 65535};
    if (!asm_need_section(a, st)) return;
    if (!asm_ops(a, st, 4, 4, &ops)) {
        asm_define_label(a, st, 8, 'W');
        return;
    }
    asm_align(a, 8);
    ls = asm_define_label(a, st, 8, 'W');
    if (ls) {
        ls->count = 1;
        ls->align = 8;
    }
    memset(&addr, 0, sizeof addr);
    addr.n = fmt1 ? 4 : 3;
    addr.b = zeros(addr.n);
    if (a->laid) {
        int64_t v[4] = {0, 0, 0, 0};
        for (k = 0; k < 4; k++) {
            if (k == 1) {
                Expr *e = asm_expr(a, st, ops.v[1]);
                Value val;
                if (!e || !asm_eval(a, st, e, 0, &val)) {
                    ok = 0;
                } else if (val.defined && !addr_con(a, st, e, val, (int)addr.n, &addr)) {
                    ok = 0;
                } else if (!val.defined) {
                    ok = 0;
                }
                continue;
            }
            if (!asm_abs_expr(a, st, ops.v[k], what[k], &v[k])) {
                ok = 0;
                continue;
            }
            if (v[k] < 0 || v[k] > maxv[k]) {
                diag_help(rep_errorf(a->rep, "ZA2064", asm_op_span(st, ops.v[k]),
                                     "%s is %lld; it must be 0 to %lld", what[k], (long long)v[k],
                                     (long long)maxv[k]),
                          "a channel command word field is %d bit(s) wide", k == 3 ? 16 : 8);
                st->bad = 1;
                ok = 0;
            }
        }
        cmd = v[0];
        flags = v[2];
        count = v[3];
        if (ok && !fmt1 && (flags & 3))
            rep_warnf(a->rep, "ZA2065", asm_op_span(st, ops.v[2]),
                      "bits 38 and 39 of a format-0 CCW must be zero (flags %lld)",
                      (long long)flags);
    }
    if (fmt1) {
        head[0] = (uint8_t)cmd;
        head[1] = (uint8_t)flags;
        head[2] = (uint8_t)(count >> 8);
        head[3] = (uint8_t)count;
        nh = 4;
        nt = 0;
    } else {
        head[0] = (uint8_t)cmd;
        nh = 1;
        tail[0] = (uint8_t)flags;
        tail[2] = (uint8_t)(count >> 8);
        tail[3] = (uint8_t)count;
        nt = 4;
    }
    (void)ok;
    asm_put(a, head, nh);
    asm_put_val(a, &addr);
    if (nt) asm_put(a, tail, nt);
}

