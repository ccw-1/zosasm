/* cblock.c - inline scalar C-like blocks. See docs/CBLOCK.md.
 *
 * The implementation covers expressions, bound registers, scalar locals,
 * assignments, integer operators, control flow including switch and goto,
 * byte pointers, and the memory and scan intrinsics. The parser is
 * deliberately small and reports ZA5xxx diagnostics. */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "cblock.h"
#include "util.h"

typedef struct {
    int bits;  /* 32 or 64 */
    int unsig; /* unsigned */
    int ptr;   /* a byte/void pointer value */
    int target; /* named opaque pointer target: 0 byte/void, else CBlock->ptypenames index + 1 */
    int elem_bits;  /* typed pointer element width: 0 byte/void/named, else 32 or 64 */
    int elem_unsig;
    int flt;        /* scalar is float (bits 32) or double (bits 64) */
    int dfp;        /* scalar is decimal floating point */
    int vec;        /* value is a 128-bit vector */
    int vbits;      /* vector element width: 8, 16, 32 or 64 */
    int vunsig;     /* vector element is unsigned */
    int vfloat;     /* vector element is float or double */
    int vbool;      /* vector element is a comparison mask */
    int elem_flt;   /* pointer element is float/double */
    int elem_dfp;   /* pointer element is decimal floating point */
} CType;

typedef enum { V_CIN, V_COUT, V_CINOUT, V_LOCAL } VKind;

typedef struct CBVar {
    char *name;
    CType type;
    VKind kind;
    int reg;       /* bound register, -1 otherwise */
    int slot;      /* temp-area slot index */
    int initialized;
    char *lit;     /* string literal the variable was initialized with, or NULL */
    size_t lit_len;
} CBVar;

typedef Vec(CBVar *) CBVarVec;

typedef enum { E_NUM, E_STR, E_VAR, E_UN, E_BIN, E_CAST, E_COND, E_INC, E_CALL, E_DEREF, E_ADDR } CBEKind;

typedef struct CBExpr CBExpr;
static void cblock_expr_free(CBExpr *e);
static int const_eval(CBExpr *e, int64_t *out);
struct CBExpr {
    CBEKind kind;
    CType type;
    int op; /* operator character or OP_* */
    int64_t num;
    double fnum;
    CBVar *var;
    CBExpr *a, *b, *c, *d, *arg5, *arg6;
    char *name; /* E_CALL intrinsic */
    char *str;  /* E_STR bytes, not necessarily NUL terminated */
    size_t slen; /* E_STR length */
    int align;  /* E_STR table alignment, 0/1 = none */
    char *sym, *field; /* __sizeof/__offsetof symbol arguments */
    int read_before;
    int line, col;
};

typedef enum { S_EXPR, S_ASSIGN, S_DECL, S_IF, S_WHILE, S_DO, S_FOR, S_BREAK, S_CONTINUE,
               S_LABEL, S_GOTO, S_SWITCH, S_CASE, S_DEFAULT,
               S_MEMCPY, S_MEMMOVE, S_MEMSET } CBSKind;
typedef struct CBStmt CBStmt;
typedef Vec(CBStmt) CBStmtVec;
struct CBStmt {
    CBSKind kind;
    CBVar *var; /* S_ASSIGN to a variable, NULL for an lvalue expression */
    CBExpr *lval; /* S_ASSIGN to *p or p[i] */
    int op; /* assignment operator */
    CBExpr *expr;
    CBExpr *cond;
    CBExpr *step;
    char *label; /* S_LABEL internal name, S_GOTO target, S_CASE/S_DEFAULT label */
    int64_t num; /* S_CASE value */
    CBStmtVec body;
    CBStmtVec other;
    CBStmtVec init;
    int line;
};

typedef struct CBString {
    char *data;
    size_t len;
    int align;
    char *label;
    char *dc;       /* assembler DC text for a decimal floating literal */
} CBString;

typedef struct {
    const char *file;
    int line;
    char *name;
    CBVarVec vars;
    StrMap byname;
    int scratch[16];
    int nscratch;
    int fscratch[16];
    int nfscratch;
    int vscratch[32];
    int nvscratch;
    Vec(char *) ptypenames; /* opaque pointer target names, index + 1 in CType.target */
    struct CBString *lits; /* string literal tables, emitted after the block */
    size_t nlits, lits_cap;
    char *lit_end; /* branch target after the literal tables */
    int temp_reg;
    int elf;           /* -elf: use STFLE, else read the z/OS facility list at 200 */
    long temp_size;
    int temp_down;
    int save_r0, save_r1;
    int base_slots;
    int next_var_slot;
    int label_no;
    int label_seq;
    StrMap labels; /* C label name -> internal assembler label */
    Reporter *rep;
    SrcVec *out;
    const CBlockLine *body;
    size_t body_n;
    CBlockDebugVec *debug;
    char *start_label, *end_label;
} CBlock;

/* multi-character operators */
enum { OP_SHL = 256, OP_SHR, OP_LE, OP_GE, OP_EQ, OP_NE,
       OP_ADDEQ, OP_SUBEQ, OP_MULEQ, OP_DIVEQ, OP_MODEQ,
       OP_ANDEQ, OP_OREQ, OP_XOREQ, OP_SHLEQ, OP_SHREQ,
       OP_LAND, OP_LOR, OP_INC, OP_DEC };

/* ---- small helpers ------------------------------------------------------ */

static CType type_of(int bits, int unsig) {
    CType t;
    t.bits = bits;
    t.unsig = unsig;
    t.ptr = 0;
    t.target = 0;
    t.elem_bits = 0;
    t.elem_unsig = 0;
    t.flt = 0;
    t.dfp = 0;
    t.vec = 0;
    t.vbits = 0;
    t.vunsig = 0;
    t.vfloat = 0;
    t.vbool = 0;
    t.elem_flt = 0;
    t.elem_dfp = 0;
    return t;
}
static CType type_ptr(void) {
    CType t;
    t.bits = 64;
    t.unsig = 1;
    t.ptr = 1;
    t.target = 0;
    t.elem_bits = 8; /* byte pointer: pointer arithmetic in bytes, *p is one byte */
    t.elem_unsig = 1;
    t.flt = 0;
    t.dfp = 0;
    t.vec = 0;
    t.vbits = 0;
    t.vunsig = 0;
    t.vfloat = 0;
    t.vbool = 0;
    t.elem_flt = 0;
    t.elem_dfp = 0;
    return t;
}
static CType type_ptr_scalar(int bits, int unsig) {
    CType t = type_ptr();
    t.elem_bits = bits;
    t.elem_unsig = unsig;
    t.elem_flt = 0;
    t.elem_dfp = 0;
    return t;
}
static CType type_fp(int bits) {
    CType t = type_of(bits, 1);
    t.flt = 1;
    t.dfp = 0;
    return t;
}
static CType type_dfp(int bits) {
    CType t = type_fp(bits);
    t.dfp = 1;
    return t;
}
static CType type_vec(int vbits, int vunsig, int vfloat, int vbool) {
    CType t = type_of(128, 1);
    t.vec = 1;
    t.vbits = vbits;
    t.vunsig = vunsig;
    t.vfloat = vfloat;
    t.vbool = vbool;
    return t;
}

static int vec_suffix_type(const char *suffix, CType *t) {
    int bits, unsig = 0, flt = 0;
    if (!strcmp(suffix, "s1")) bits = 8;
    else if (!strcmp(suffix, "u1")) { bits = 8; unsig = 1; }
    else if (!strcmp(suffix, "s2")) bits = 16;
    else if (!strcmp(suffix, "u2")) { bits = 16; unsig = 1; }
    else if (!strcmp(suffix, "s4")) bits = 32;
    else if (!strcmp(suffix, "u4")) { bits = 32; unsig = 1; }
    else if (!strcmp(suffix, "s8")) bits = 64;
    else if (!strcmp(suffix, "u8")) { bits = 64; unsig = 1; }
    else if (!strcmp(suffix, "f4")) { bits = 32; flt = 1; }
    else if (!strcmp(suffix, "f8")) { bits = 64; flt = 1; }
    else return 0;
    *t = type_vec(bits, unsig, flt, 0);
    return 1;
}

static CType vec_scalar_type(const char *suffix) {
    if (!strcmp(suffix, "s1") || !strcmp(suffix, "s2") || !strcmp(suffix, "s4"))
        return type_of(32, 0);
    if (!strcmp(suffix, "u1") || !strcmp(suffix, "u2") || !strcmp(suffix, "u4"))
        return type_of(32, 1);
    if (!strcmp(suffix, "s8")) return type_of(64, 0);
    if (!strcmp(suffix, "u8")) return type_of(64, 1);
    if (!strcmp(suffix, "f4")) return type_fp(32);
    if (!strcmp(suffix, "f8")) return type_fp(64);
    return type_of(32, 0);
}

static int vec_element_type(const char *suffix, CType *t) {
    if (!strcmp(suffix, "b4")) { *t = type_vec(32, 0, 0, 1); return 1; }
    if (!strcmp(suffix, "b8")) { *t = type_vec(64, 0, 0, 1); return 1; }
    return vec_suffix_type(suffix, t);
}

static int vec_suffix_code(const char *suffix) {
    if (!strcmp(suffix, "s1") || !strcmp(suffix, "u1")) return 0;
    if (!strcmp(suffix, "s2") || !strcmp(suffix, "u2")) return 1;
    if (!strcmp(suffix, "s4") || !strcmp(suffix, "u4") || !strcmp(suffix, "f4")) return 2;
    return 3;
}

static CType type_ptr_fp(int bits) {
    CType t = type_ptr();
    t.elem_bits = bits;
    t.elem_unsig = 1;
    t.elem_flt = 1;
    t.elem_dfp = 0;
    return t;
}

static CType type_ptr_named(CBlock *b, const char *name) {
    CType t = type_ptr();
    size_t i;
    t.elem_bits = 0;
    t.elem_unsig = 0;
    t.flt = 0;
    t.dfp = 0;
    t.vec = 0;
    t.vbits = 0;
    t.vunsig = 0;
    t.vfloat = 0;
    t.vbool = 0;
    t.elem_flt = 0;
    t.elem_dfp = 0;
    for (i = 0; i < b->ptypenames.n; i++)
        if (!strcmp(b->ptypenames.v[i], name)) {
            t.target = (int)i + 1;
            return t;
        }
    vec_push(&b->ptypenames, xstrdup(name));
    t.target = (int)b->ptypenames.n;
    return t;
}

static int type_same(CType a, CType b) {
    return a.bits == b.bits && a.unsig == b.unsig && a.ptr == b.ptr && a.target == b.target &&
           a.elem_bits == b.elem_bits && a.elem_unsig == b.elem_unsig &&
           a.flt == b.flt && a.elem_flt == b.elem_flt &&
           a.dfp == b.dfp && a.elem_dfp == b.elem_dfp &&
           a.vec == b.vec && a.vbits == b.vbits && a.vunsig == b.vunsig &&
           a.vfloat == b.vfloat && a.vbool == b.vbool;
}

static int vec_type_text(const char *text, CType *t);

static int type_parse(const char *s, CType *t) {
    char *p = xstrdup(s);
    char *q = str_trim(p);
    int r = 0;
    size_t n = strlen(q);
    while (n > 0 && (q[n - 1] == ' ' || q[n - 1] == '\t')) q[--n] = 0;
    if (n > 5 && !za_strncmpi(q, "const ", 6)) { q += 6; n -= 6; }
    if (vec_type_text(q, t)) { free(p); return 1; }
    while (n > 0 && (q[n - 1] == ' ' || q[n - 1] == '\t')) q[--n] = 0;
    if (n > 0 && q[n - 1] == '*') {
        q[--n] = 0;
        while (n > 0 && (q[n - 1] == ' ' || q[n - 1] == '\t')) q[--n] = 0;
        if (za_streqi(q, "uint8_t") || za_streqi(q, "int8_t") ||
            za_streqi(q, "uint16_t") || za_streqi(q, "int16_t") ||
            za_streqi(q, "float") || za_streqi(q, "double") ||
            za_streqi(q, "_Decimal32") || za_streqi(q, "_Decimal64") ||
            za_streqi(q, "_Decimal128") ||
            za_streqi(q, "long double") || za_streqi(q, "void") || za_streqi(q, "char")) {
            if (za_streqi(q, "_Decimal32") || za_streqi(q, "_Decimal64") ||
                za_streqi(q, "_Decimal128") || za_streqi(q, "float") ||
                za_streqi(q, "double") || za_streqi(q, "long double"))
                *t = type_ptr_fp(za_streqi(q, "_Decimal32") || za_streqi(q, "float") ? 32 :
                                 za_streqi(q, "_Decimal64") || za_streqi(q, "double") ? 64 : 128);
            else
                *t = (za_streqi(q, "uint16_t") || za_streqi(q, "int16_t"))
                         ? type_ptr_scalar(16, za_streqi(q, "uint16_t"))
                         : type_ptr();
            free(p);
            return 1;
        }
    }
    if (za_streqi(q, "_Decimal32")) { *t = type_dfp(32); r = 1; }
    else if (za_streqi(q, "_Decimal64")) { *t = type_dfp(64); r = 1; }
    else if (za_streqi(q, "_Decimal128")) { *t = type_dfp(128); r = 1; }
    else if (za_streqi(q, "float")) { *t = type_fp(32); r = 1; }
    else if (za_streqi(q, "double")) { *t = type_fp(64); r = 1; }
    else if (za_streqi(q, "long double")) { *t = type_fp(128); r = 1; }
    else if (za_streqi(q, "int16_t")) { *t = type_of(16, 0); r = 1; }
    else if (za_streqi(q, "uint16_t")) { *t = type_of(16, 1); r = 1; }
    else if (za_streqi(q, "int32_t")) { *t = type_of(32, 0); r = 1; }
    else if (za_streqi(q, "uint32_t")) { *t = type_of(32, 1); r = 1; }
    else if (za_streqi(q, "int64_t")) { *t = type_of(64, 0); r = 1; }
    else if (za_streqi(q, "uint64_t")) { *t = type_of(64, 1); r = 1; }
    else if (za_streqi(q, "int")) { *t = type_of(32, 0); r = 1; }
    else if (za_streqi(q, "unsigned")) { *t = type_of(32, 1); r = 1; }
    free(p);
    return r;
}
static int vec_type_text(const char *text, CType *t) {
    char *buf = xstrdup(text), *w[8], *p;
    int n = 0, i, vbits = 0, vunsig = 0, vfloat = 0, vbool = 0;
    for (p = buf; *p && n < 8; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        w[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    i = 0;
    if (i < n && za_streqi(w[i], "__vector")) i++;
    else { free(buf); return 0; }
    if (i < n && za_streqi(w[i], "bool")) { vbool = 1; i++; }
    if (i < n && za_streqi(w[i], "signed")) i++;
    else if (i < n && za_streqi(w[i], "unsigned")) { vunsig = 1; i++; }
    if (i >= n) { free(buf); return 0; }
    if (za_streqi(w[i], "char")) vbits = 8;
    else if (za_streqi(w[i], "short")) vbits = 16;
    else if (za_streqi(w[i], "int")) vbits = 32;
    else if (za_streqi(w[i], "long")) {
        vbits = 64;
        if (++i < n && za_streqi(w[i], "long")) { }
        else { free(buf); return 0; }
    } else if (za_streqi(w[i], "float")) { vbits = 32; vfloat = 1; }
    else if (za_streqi(w[i], "double")) { vbits = 64; vfloat = 1; }
    else { free(buf); return 0; }
    if (vbool && vfloat) { free(buf); return 0; }
    *t = type_vec(vbits, vunsig, vfloat, vbool);
    free(buf);
    return 1;
}

static int type_text_parse(CBlock *b, const char *text, CType *t) {
    char *p = xstrdup(text), *q = str_trim(p);
    size_t n = strlen(q);
    int r = 0;
    while (n > 0 && (q[n - 1] == ' ' || q[n - 1] == '\t')) q[--n] = 0;
    if (n > 5 && !za_strncmpi(q, "const ", 6)) { q += 6; n -= 6; }
    while (n > 0 && (q[n - 1] == ' ' || q[n - 1] == '\t')) q[--n] = 0;
    if (n > 0 && q[n - 1] == '*') {
        q[--n] = 0;
        while (n > 0 && (q[n - 1] == ' ' || q[n - 1] == '\t')) q[--n] = 0;
        if (za_streqi(q, "uint8_t") || za_streqi(q, "int8_t") ||
            za_streqi(q, "uint16_t") || za_streqi(q, "int16_t") ||
            za_streqi(q, "float") || za_streqi(q, "double") ||
            za_streqi(q, "_Decimal32") || za_streqi(q, "_Decimal64") ||
            za_streqi(q, "_Decimal128") ||
            za_streqi(q, "long double") || za_streqi(q, "void") || za_streqi(q, "char")) {
            if (za_streqi(q, "_Decimal32") || za_streqi(q, "_Decimal64") ||
                za_streqi(q, "_Decimal128") || za_streqi(q, "float") ||
                za_streqi(q, "double") || za_streqi(q, "long double"))
                *t = type_ptr_fp(za_streqi(q, "_Decimal32") || za_streqi(q, "float") ? 32 :
                                 za_streqi(q, "_Decimal64") || za_streqi(q, "double") ? 64 : 128);
            else
                *t = (za_streqi(q, "uint16_t") || za_streqi(q, "int16_t"))
                         ? type_ptr_scalar(16, za_streqi(q, "uint16_t"))
                         : type_ptr();
            r = 1;
        } else {
            CType base;
            if (type_parse(q, &base)) {
                *t = type_ptr_scalar(base.bits, base.unsig);
                r = 1;
            } else {
                *t = type_ptr_named(b, q);
                r = 1;
            }
        }
    } else {
        r = type_parse(q, t);
    }
    free(p);
    return r;
}

#if 0
static int type_parse_old(const char *s, CType *t) {
    if (za_streqi(s, "int32_t")) { *t = type_of(32, 0); return 1; }
    if (za_streqi(s, "uint32_t")) { *t = type_of(32, 1); return 1; }
    if (za_streqi(s, "int64_t")) { *t = type_of(64, 0); return 1; }
    if (za_streqi(s, "uint64_t")) { *t = type_of(64, 1); return 1; }
    if (za_streqi(s, "int")) { *t = type_of(32, 0); return 1; }
    if (za_streqi(s, "unsigned")) { *t = type_of(32, 1); return 1; }
    return 0;
}
#endif

static CType type_usual(CType a, CType b) {
    if (a.dfp || b.dfp) return type_dfp(a.dfp && b.dfp ? (a.bits > b.bits ? a.bits : b.bits) :
                                         (a.dfp ? a.bits : b.bits));
    if (a.flt || b.flt) return type_fp(a.flt && b.flt ? (a.bits > b.bits ? a.bits : b.bits) :
                                       (a.flt ? a.bits : b.bits));
    if (a.bits < 32 && b.bits < 32) return type_of(32, 0); /* integer promotion */
    if (a.bits == 64 || b.bits == 64)
        return type_of(64, (a.bits == 64 ? a.unsig : b.unsig));
    if (a.bits == 32 && b.bits == 32) return type_of(32, a.unsig || b.unsig);
    return a.bits == 32 ? a : b;
}

static char *trim(const char *s) { return str_trim(s); }

static int freg_parse(const char *s, int *out) {
    const char *p = s;
    char kind;
    int n;
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != '%') return 0;
    kind = (char)za_tolower((unsigned char)*p++);
    if (kind != 'f' || !za_isdigit((unsigned char)*p)) return 0;
    n = 0;
    while (za_isdigit((unsigned char)*p)) {
        n = n * 10 + (*p - '0');
        p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p || n > 15) return 0;
    *out = n;
    return 1;
}

static int vreg_parse(const char *s, int *out) {
    const char *p = s;
    char kind;
    int n;
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != '%') return 0;
    kind = (char)za_tolower((unsigned char)*p++);
    if (kind != 'v' || !za_isdigit((unsigned char)*p)) return 0;
    n = 0;
    while (za_isdigit((unsigned char)*p)) {
        n = n * 10 + (*p - '0');
        p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p || n > 31) return 0;
    *out = n;
    return 1;
}

static int reg_parse(const char *s, int *out) {
    const char *p = s;
    char kind;
    int n;
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != '%') return 0;
    kind = (char)za_tolower((unsigned char)*p++);
    if (kind != 'r' || !za_isdigit((unsigned char)*p)) return 0;
    n = 0;
    while (za_isdigit((unsigned char)*p)) {
        n = n * 10 + (*p - '0');
        p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p || n > 15) return 0;
    *out = n;
    return 1;
}

/* ---- diagnostics -------------------------------------------------------- */

static void cb_error(CBlock *b, int line, int col, const char *code, const char *fmt, ...) {
    va_list ap;
    char *msg;
    va_start(ap, fmt);
    msg = xvasprintf(fmt, ap);
    va_end(ap);
    rep_errorf(b->rep, code, span_make(b->file, line, col, 1), "%s", msg);
    free(msg);
}

/* ---- captured lines and the C lexer ------------------------------------ */

typedef enum { TK_EOF, TK_ID, TK_NUM, TK_FP, TK_STR, TK_OP } TokKind;
typedef struct {
    TokKind kind;
    char *s; /* TK_ID, TK_STR */
    size_t slen; /* TK_STR */
    int64_t num;
    double fnum;
    int op;
    int line, col;
} Tok;

typedef struct {
    CBlock *b;
    const char *src;
    int *line_at;
    size_t n, pos;
    Tok tok;
    Vec(char *) ids;
} Lex;

static int is_id_start(int c) { return za_isalpha(c) || c == '_'; }
static int is_id_char(int c) { return is_id_start(c) || za_isdigit(c); }

static void lex_skip(Lex *l) {
    for (;;) {
        while (l->pos < l->n && (l->src[l->pos] == ' ' || l->src[l->pos] == '\t' ||
                                 l->src[l->pos] == '\n' || l->src[l->pos] == '\r'))
            l->pos++;
        if (l->pos + 1 < l->n && l->src[l->pos] == '/' && l->src[l->pos + 1] == '/') {
            while (l->pos < l->n && l->src[l->pos] != '\n') l->pos++;
            continue;
        }
        if (l->pos + 1 < l->n && l->src[l->pos] == '/' && l->src[l->pos + 1] == '*') {
            int line = l->line_at[l->pos];
            l->pos += 2;
            while (l->pos + 1 < l->n && !(l->src[l->pos] == '*' && l->src[l->pos + 1] == '/')) l->pos++;
            if (l->pos + 1 < l->n) l->pos += 2;
            else cb_error(l->b, line, 1, "ZA5001", "unterminated C comment");
            continue;
        }
        break;
    }
}

static void lex_next(Lex *l) {
    size_t start;
    int line, col;
    if (l->tok.s) { /* strings stay owned by ids until the block is freed */ }
    l->tok.s = NULL;
    lex_skip(l);
    start = l->pos;
    line = l->pos < l->n ? l->line_at[l->pos] : l->b->line;
    col = 1;
    if (l->pos < l->n) {
        size_t p = l->pos;
        while (p > 0 && l->src[p - 1] != '\n') { p--; col++; }
    }
    if (l->pos >= l->n) {
        l->tok.kind = TK_EOF;
        l->tok.line = line;
        l->tok.col = col;
        return;
    }
    if (l->src[l->pos] == '"') {
        Buf b = {0};
        char *str;
        size_t n = 0;
        for (;;) {
            size_t j = l->pos + 1;
            while (j < l->n && l->src[j] != '"') {
                int c = (unsigned char)l->src[j++];
                if (c == '\\' && j < l->n) {
                    c = (unsigned char)l->src[j++];
                    switch (c) {
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'v': c = '\v'; break;
                    case 'a': c = '\a'; break;
                    case '\\': break;
                    case '\'': break;
                    case '"': break;
                    case '?': break;
                    case 'x': {
                        int v = 0, k = 0;
                        while (j < l->n && k < 2) {
                            int h = (unsigned char)l->src[j], d;
                            if (h >= '0' && h <= '9') d = h - '0';
                            else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                            else break;
                            v = v * 16 + d;
                            j++;
                            k++;
                        }
                        c = v;
                        break;
                    }
                    default:
                        if (c >= '0' && c <= '7') {
                            int v = c - '0', k = 1;
                            while (j < l->n && k < 3 && l->src[j] >= '0' && l->src[j] <= '7') {
                                v = v * 8 + (l->src[j] - '0');
                                j++;
                                k++;
                            }
                            c = v & 0xFF;
                        }
                        break;
                    }
                }
                buf_putc(&b, (char)c);
                n++;
            }
            if (j >= l->n) {
                cb_error(l->b, line, col, "ZA5001", "unterminated string literal");
                break;
            }
            l->pos = j + 1;
            lex_skip(l);
            if (!(l->pos < l->n && l->src[l->pos] == '"')) break;
            /* adjacent string literals are concatenated, as in C */
        }
        str = xmalloc(n + 1);
        memcpy(str, b.p ? b.p : "", n);
        str[n] = 0;
        free(b.p);
        l->tok.kind = TK_STR;
        l->tok.s = str;
        l->tok.slen = n;
        vec_push(&l->ids, str);
        return;
    } else if (is_id_start((unsigned char)l->src[l->pos])) {
        size_t j = l->pos + 1;
        while (j < l->n && is_id_char((unsigned char)l->src[j])) j++;
        l->tok.kind = TK_ID;
        l->tok.s = xstrndup(l->src + l->pos, j - l->pos);
        vec_push(&l->ids, l->tok.s);
        l->pos = j;
    } else if (za_isdigit((unsigned char)l->src[l->pos])) {
        uint64_t v = 0;
        int base = 10, suffix = 0, digits = 0;
        size_t j = l->pos;
        if (l->src[j] == '0' && j + 1 < l->n && (l->src[j + 1] == 'x' || l->src[j + 1] == 'X')) {
            base = 16; j += 2;
        } else if (l->src[j] == '0' && j + 1 < l->n && (l->src[j + 1] == 'b' || l->src[j + 1] == 'B')) {
            base = 2; j += 2;
        }
        for (; j < l->n; j++) {
            int c = (unsigned char)l->src[j], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            if (d >= base || v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) {
                cb_error(l->b, line, col, "ZA5001", "integer literal is too large");
                break;
            }
            v = v * (uint64_t)base + (uint64_t)d;
            digits++;
        }
        if (!digits) cb_error(l->b, line, col, "ZA5001", "invalid integer literal");
        if (base == 10 && j < l->n && (l->src[j] == '.' || l->src[j] == 'e' || l->src[j] == 'E')) {
            char *end = NULL;
            double d = strtod(l->src + l->pos, &end);
            size_t fend = (size_t)(end - l->src);
            if (fend > j) {
                j = fend;
                if (j < l->n && (l->src[j] == 'f' || l->src[j] == 'F')) { suffix = 1; j++; }
                else if (j < l->n && (l->src[j] == 'l' || l->src[j] == 'L')) { suffix = 2; j++; }
                else if (j + 1 < l->n && (l->src[j] == 'd' || l->src[j] == 'D')) {
                    if (l->src[j + 1] == 'f' || l->src[j + 1] == 'F') { suffix = 3; j += 2; }
                    else if (l->src[j + 1] == 'd' || l->src[j + 1] == 'D') { suffix = 4; j += 2; }
                    else if (l->src[j + 1] == 'l' || l->src[j + 1] == 'L') { suffix = 5; j += 2; }
                }
                l->tok.kind = TK_FP;
                l->tok.fnum = d;
                l->tok.op = suffix;
                l->tok.s = xstrndup(l->src + l->pos, j - l->pos);
                vec_push(&l->ids, l->tok.s);
                l->tok.line = line;
                l->tok.col = col;
                l->pos = j;
                return;
            }
        }
        while (j < l->n && (l->src[j] == 'u' || l->src[j] == 'U' || l->src[j] == 'l' || l->src[j] == 'L')) {
            suffix |= (l->src[j] == 'u' || l->src[j] == 'U') ? 1 : 2;
            j++;
        }
        l->tok.kind = TK_NUM;
        l->tok.num = (int64_t)v;
        /* the literal type is inferred in the parser from the value and suffix */
        l->tok.line = line;
        l->tok.col = col;
        l->pos = j;
        if (suffix == 2 && v <= INT64_MAX) { /* long/long long: make it 64 bit */ l->tok.op = 1; }
        else l->tok.op = suffix;
    } else {
        int c = (unsigned char)l->src[l->pos++];
        int op = c;
        if (c == '&' && l->pos < l->n && l->src[l->pos] == '&') { op = OP_LAND; l->pos++; }
        else if (c == '|' && l->pos < l->n && l->src[l->pos] == '|') { op = OP_LOR; l->pos++; }
        else if (c == '+' && l->pos < l->n && l->src[l->pos] == '+') { op = OP_INC; l->pos++; }
        else if (c == '-' && l->pos < l->n && l->src[l->pos] == '-') { op = OP_DEC; l->pos++; }
        else if (c == '+' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_ADDEQ; l->pos++; }
        else if (c == '-' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_SUBEQ; l->pos++; }
        else if (c == '*' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_MULEQ; l->pos++; }
        else if (c == '/' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_DIVEQ; l->pos++; }
        else if (c == '%' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_MODEQ; l->pos++; }
        else if (c == '&' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_ANDEQ; l->pos++; }
        else if (c == '|' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_OREQ; l->pos++; }
        else if (c == '^' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_XOREQ; l->pos++; }
        else if (c == '<' && l->pos + 1 < l->n && l->src[l->pos] == '<' && l->src[l->pos + 1] == '=') { op = OP_SHLEQ; l->pos += 2; }
        else if (c == '>' && l->pos + 1 < l->n && l->src[l->pos] == '>' && l->src[l->pos + 1] == '=') { op = OP_SHREQ; l->pos += 2; }
        else if (c == '<' && l->pos < l->n && l->src[l->pos] == '<') { op = OP_SHL; l->pos++; }
        else if (c == '>' && l->pos < l->n && l->src[l->pos] == '>') { op = OP_SHR; l->pos++; }
        else if (c == '<' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_LE; l->pos++; }
        else if (c == '>' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_GE; l->pos++; }
        else if (c == '=' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_EQ; l->pos++; }
        else if (c == '!' && l->pos < l->n && l->src[l->pos] == '=') { op = OP_NE; l->pos++; }
        l->tok.kind = TK_OP;
        l->tok.op = op;
    }
    l->tok.line = line;
    l->tok.col = col;
    if (l->tok.kind == TK_OP && l->tok.op == 0) l->tok.op = start;
}

static int tok_is(const Lex *l, int op) {
    return l->tok.kind == TK_OP && l->tok.op == op;
}
static int tok_id(const Lex *l, const char *s) {
    return l->tok.kind == TK_ID && za_streqi(l->tok.s, s);
}

static int peek_is_star(Lex *l) {
    Lex t = *l;
    int r;
    t.ids.v = NULL;
    t.ids.n = t.ids.cap = 0;
    t.tok.s = NULL;
    lex_next(&t);
    r = tok_is(&t, '*');
    {
        size_t i;
        for (i = 0; i < t.ids.n; i++) free(t.ids.v[i]);
        free(t.ids.v);
    }
    return r;
}

static int after_tok_colon(const Lex *l) {
    const char *s = l->src + l->pos;
    while (*s == ' ' || *s == '\t') s++;
    return *s == ':' && s[1] != ':';
}

/* ---- parser ------------------------------------------------------------- */

typedef struct {
    CBlock *b;
    Lex l;
    int err;
    int loop_depth;
    int switch_depth;
    int switch_default[16];
} Parser;

static CBExpr *parse_cond(Parser *p);
static CBExpr *parse_assignment(Parser *p);
static CBExpr *parse_unary(Parser *p);
static void parse_stmt(Parser *p, CBStmtVec *st);

static CBExpr *mk_expr(CBEKind k, CType t, int line, int col) {
    CBExpr *e = xcalloc(sizeof *e);
    e->kind = k;
    e->type = t;
    e->line = line;
    e->col = col;
    return e;
}

static CBExpr *mk_cast(CBExpr *e, CType t) {
    CBExpr *c;
    if (!e || type_same(e->type, t)) return e;
    c = mk_expr(E_CAST, t, e->line, e->col);
    c->a = e;
    return c;
}

static CBExpr *mk_bin(Parser *p, int op, CBExpr *a, CBExpr *b) {
    CType t;
    (void)p;
    if ((op == '+' || op == '-') && b->type.ptr && !a->type.ptr) {
        if (op == '+') {
            CBExpr *sw = a;
            a = b;
            b = sw;
        } else {
            cb_error(p->b, a->line, a->col, "ZA5002", "pointer subtraction needs a pointer on the left");
            p->err = 1;
            return a;
        }
    }
    if (op == OP_EQ || op == OP_NE || op == OP_LE || op == OP_GE || op == '<' || op == '>' ||
        op == OP_LAND || op == OP_LOR) {
        t = type_of(32, 0);
        if (op != OP_LAND && op != OP_LOR && a->type.ptr && b->type.ptr) {
            /* pointer comparison: the operands stay pointers */
        }
    } else if (op == '+' && (a->type.ptr || b->type.ptr)) {
        if (a->type.ptr && b->type.ptr) {
            cb_error(p->b, a->line, a->col, "ZA5002", "cannot add two pointers");
            p->err = 1;
            return a;
        }
        t = a->type.ptr ? a->type : b->type;
    } else if (op == '-' && a->type.ptr && b->type.ptr) {
        t = type_of(64, 0); /* byte pointer difference */
    } else if (op == '-' && a->type.ptr) {
        t = type_ptr();
    } else if (a->type.ptr || b->type.ptr) {
        cb_error(p->b, a->line, a->col, "ZA5002", "unsupported pointer operation");
        p->err = 1;
        return a;
    } else {
        t = type_usual(a->type, b->type);
        a = mk_cast(a, t);
        b = mk_cast(b, t);
    }
    {
        CBExpr *e = mk_expr(E_BIN, t, a->line, a->col);
        e->op = op;
        e->a = a;
        e->b = b;
        return e;
    }
}

static int parse_call_args(Parser *p, CBExpr **args, int nargs) {
    int i;
    Lex *l = &p->l;
    for (i = 0; i < nargs; i++) {
        if (i) {
            if (!tok_is(l, ',')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ',' in builtin call");
                return 0;
            }
            lex_next(l);
        }
        args[i] = parse_assignment(p);
        if (!args[i]) return 0;
    }
    if (!tok_is(l, ')')) {
        cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ')' in builtin call");
        return 0;
    }
    lex_next(l);
    return 1;
}

static int is_zero_expr(CBExpr *e) {
    int64_t v;
    return e && e->kind == E_NUM && !e->type.flt && const_eval(e, &v) && v == 0;
}

static CBExpr *parse_primary(Parser *p) {
    Lex *l = &p->l;
    CBExpr *e;
    int line = l->tok.line, col = l->tok.col;
    if (l->tok.kind == TK_STR) {
        e = mk_expr(E_STR, type_ptr(), line, col);
        e->str = xmalloc(l->tok.slen + 1);
        memcpy(e->str, l->tok.s, l->tok.slen);
        e->str[l->tok.slen] = 0;
        e->slen = l->tok.slen;
        lex_next(l);
        return e;
    }
    if (l->tok.kind == TK_FP) {
        int kind = l->tok.op;
        if (kind >= 3) e = mk_expr(E_NUM, type_dfp(kind == 3 ? 32 : kind == 4 ? 64 : 128), line, col);
        else e = mk_expr(E_NUM, kind == 2 ? type_fp(64) : type_fp(kind ? 32 : 64), line, col);
        e->fnum = l->tok.fnum;
        if (kind >= 3 && l->tok.s) {
            size_t n = strlen(l->tok.s);
            if (n >= 2) n -= 2;
            e->str = xstrndup(l->tok.s, n);
        }
        lex_next(l);
        if (kind == 2) return mk_cast(e, type_fp(128));
        return e;
    }
    if (l->tok.kind == TK_NUM) {
        uint64_t v = (uint64_t)l->tok.num;
        CType t;
        int suffix = l->tok.op;
        if (suffix & 2) {
            t = (suffix & 1) || v > INT64_MAX ? type_of(64, 1) : type_of(64, 0);
        } else if (suffix & 1) {
            t = v <= UINT32_MAX ? type_of(32, 1) : type_of(64, 1);
        } else if (v <= INT32_MAX) t = type_of(32, 0);
        else if (v <= UINT32_MAX) t = type_of(32, 1);
        else if (v <= INT64_MAX) t = type_of(64, 0);
        else t = type_of(64, 1);
        e = mk_expr(E_NUM, t, line, col);
        e->num = (int64_t)v;
        lex_next(l);
        return e;
    }
    if (l->tok.kind == TK_ID) {
        char *name = xstrdup(l->tok.s);
        if (!strcmp(name, "__sizeof") || !strcmp(name, "__offsetof")) {
            int is_off = !strcmp(name, "__offsetof");
            char *sym = NULL, *field = NULL, *iname = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(') || (lex_next(l), l->tok.kind != TK_ID)) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001",
                         "expected a section name in %s", iname);
                free(iname); free(sym); free(field);
                p->err = 1;
                return NULL;
            }
            sym = xstrdup(l->tok.s);
            lex_next(l);
            if (is_off) {
                if (!tok_is(l, ',')) {
                    cb_error(p->b, l->tok.line, l->tok.col, "ZA5001",
                             "expected ',' in __offsetof");
                    free(iname); free(sym);
                    p->err = 1;
                    return NULL;
                }
                lex_next(l);
                if (l->tok.kind != TK_ID) {
                    cb_error(p->b, l->tok.line, l->tok.col, "ZA5001",
                             "expected a field name in __offsetof");
                    free(iname); free(sym);
                    p->err = 1;
                    return NULL;
                }
                field = xstrdup(l->tok.s);
                lex_next(l);
            }
            if (!tok_is(l, ')')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ')' after %s", iname);
                free(iname); free(sym); free(field);
                p->err = 1;
                return NULL;
            }
            lex_next(l);
            {
                CBExpr *e = mk_expr(E_CALL, type_of(64, 1), line, col);
                e->name = iname;
                e->sym = sym;
                e->field = field;
                return e;
            }
        }
        if (!strcmp(name, "__stfle") || !strcmp(name, "__facility") ||
            !strcmp(name, "__facility_test")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            int argc = !strcmp(name, "__facility_test") ? 2 : 1;
            e = mk_expr(E_CALL, type_of(32, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
        if (!strcmp(e->name, "__stfle")) {
                if (!e->a->type.ptr) {
                    cb_error(p->b, line, col, "ZA5003", "__stfle needs a storage pointer");
                    p->err = 1;
                }
            } else if (!strcmp(e->name, "__facility_test")) {
                if (!e->a->type.ptr || e->b->type.ptr || e->b->type.flt) {
                    cb_error(p->b, line, col, "ZA5003",
                             "__facility_test needs a list pointer and an integer bit number");
                    p->err = 1;
                }
            } else {
                if (e->a->type.ptr || e->a->type.flt) {
                    cb_error(p->b, line, col, "ZA5003", "%s needs an integer bit number", e->name);
                    p->err = 1;
                }
            }
            return e;
        }
        if (!strcmp(name, "__cu12") || !strcmp(name, "__cu14") ||
            !strcmp(name, "__cu21") || !strcmp(name, "__cu24") ||
            !strcmp(name, "__cu41") || !strcmp(name, "__cu42")) {
            CBExpr *e;
            CBExpr *args[6] = {0, 0, 0, 0, 0, 0};
            int has_mask = strcmp(name, "__cu41") && strcmp(name, "__cu42");
            int has_bad = strcmp(name, "__cu21") && strcmp(name, "__cu24");
            int argc = has_mask && has_bad ? 6 : 5;
            e = mk_expr(E_CALL, type_of(32, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) { cblock_expr_free(e); p->err = 1; return NULL; }
            e->a = args[0]; e->b = args[1]; e->c = args[2]; e->d = args[3];
            if (has_bad) e->arg5 = args[4];
            if (has_mask) {
                int mi = has_bad ? 5 : 4;
                e->num = args[mi]->num;
                cblock_expr_free(args[mi]);
            }
            {
                int bad = !e->a->type.ptr || e->b->type.ptr || !e->c->type.ptr || e->d->type.ptr;
                if (has_bad && (!e->arg5->type.ptr || e->arg5->type.flt)) bad = 1;
                if (has_mask && (e->num < 0 || e->num > 15)) bad = 1;
                if (bad) {
                    cb_error(p->b, line, col, "ZA5003",
                             "%s needs two pointers, two lengths%s%s", e->name,
                             has_bad ? " and an invalid-character pointer" : "",
                             has_mask ? " and a constant mask 0..15" : "");
                    p->err = 1;
                }
            }
            return e;
        }
        if (!strcmp(name, "__clcle") || !strcmp(name, "__clclu") ||
            !strcmp(name, "__mvcle") || !strcmp(name, "__mvclu")) {
            CBExpr *e;
            CBExpr *args[5] = {0, 0, 0, 0, 0};
            e = mk_expr(E_CALL, type_of(32, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 5)) { cblock_expr_free(e); p->err = 1; return NULL; }
            e->a = args[0]; e->b = args[1]; e->c = args[2]; e->d = args[3]; e->arg5 = args[4];
            if (!e->a->type.ptr || e->b->type.ptr || e->c->type.ptr ||
                !e->d->type.ptr || e->arg5->type.ptr) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs a pointer, length, pad, pointer and length", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__prno_query") || !strcmp(name, "__prno_trng") ||
            !strcmp(name, "__rng_bytes") || !strcmp(name, "__trng_available") ||
            !strcmp(name, "__rng_available") || !strcmp(name, "__prno")) {
            CBExpr *e;
            CBExpr *args[6] = {0, 0, 0, 0, 0, 0};
            int is_generic = !strcmp(name, "__prno");
            int is_query = !strcmp(name, "__prno_query");
            int is_trng = !strcmp(name, "__prno_trng") || !strcmp(name, "__rng_bytes");
            int is_avail = !strcmp(name, "__trng_available") || !strcmp(name, "__rng_available");
            int argc = is_generic ? 6 : is_avail ? 0 : is_query ? 1 : 2;
            e = mk_expr(E_CALL, type_of(32, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2]; e->d = args[3];
            e->arg5 = args[4]; e->arg6 = args[5];
            {
                int bad = 0;
                if (is_query && (!e->a->type.ptr)) bad = 1;
                if (is_trng && (!e->a->type.ptr || e->b->type.ptr || e->b->type.flt)) bad = 1;
                if (is_generic) {
                    if (e->a->type.ptr || e->a->type.flt ||
                        (!e->b->type.ptr && !is_zero_expr(e->b)) ||
                        (!e->c->type.ptr && !is_zero_expr(e->c)) ||
                        e->d->type.ptr || e->d->type.flt ||
                        (!e->arg5->type.ptr && !is_zero_expr(e->arg5)) ||
                        e->arg6->type.ptr || e->arg6->type.flt) bad = 1;
                }
                if (bad) {
                    cb_error(p->b, line, col, "ZA5003",
                             is_generic ? "__prno needs a function code, parameter pointer, "
                                          "two operand pointers and two lengths"
                                        : "%s has an argument of the wrong type", e->name);
                    p->err = 1;
                }
            }
            return e;
        }
        if (!strcmp(name, "__clcl")) {
            CBExpr *e;
            CBExpr *args[5] = {0, 0, 0, 0, 0};
            e = mk_expr(E_CALL, type_of(32, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 5)) { cblock_expr_free(e); p->err = 1; return NULL; }
            e->a = args[0]; e->b = args[1]; e->c = args[2]; e->d = args[3]; e->arg5 = args[4];
            if (!e->a->type.ptr || !e->b->type.ptr || !e->c->type.ptr || !e->d->type.ptr ||
                e->arg5->type.ptr) {
                cb_error(p->b, line, col, "ZA5003",
                         "__clcl needs two pointers, two length pointers and a pad byte");
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__tre") || !strcmp(name, "__srstu") ||
            !strcmp(name, "__troo") || !strcmp(name, "__trot") ||
            !strcmp(name, "__trto") || !strcmp(name, "__trtt")) {
            CBExpr *e;
            CBExpr *args[6] = {0, 0, 0, 0, 0, 0};
            int is_tr = strcmp(name, "__tre") && strcmp(name, "__srstu");
            int is_tre = !strcmp(name, "__tre");
            int argc = is_tr ? 6 : 4;
            e = mk_expr(E_CALL, type_of(32, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2]; e->d = args[3];
            if (is_tr) { e->arg5 = args[4]; e->num = args[5]->num; cblock_expr_free(args[5]); }
            {
                int bad = 0;
                if (is_tre) {
                    if (!e->a->type.ptr || e->b->type.ptr || !e->c->type.ptr || e->d->type.ptr) bad = 1;
                } else if (!strcmp(e->name, "__srstu")) {
                    if (!e->a->type.ptr || !e->b->type.ptr || e->c->type.ptr || !e->d->type.ptr) bad = 1;
                } else {
                    if (!e->a->type.ptr || !e->b->type.ptr || e->c->type.ptr ||
                        !e->d->type.ptr || e->arg5->type.ptr || e->arg5->type.flt ||
                        e->num < 0 || e->num > 15) bad = 1;
                }
                if (bad) {
                    cb_error(p->b, line, col, "ZA5003",
                             "%s needs string pointers, an integer length%s", e->name,
                             is_tr ? ", a test character and a constant mask 0..15" : "");
                    p->err = 1;
                }
            }
            return e;
        }
        if (!strcmp(name, "__edmk") || !strcmp(name, "__trt") || !strcmp(name, "__trtr")) {
            CBExpr *e;
            CBExpr *args[5] = {0, 0, 0, 0, 0};
            int argc = !strcmp(name, "__edmk") ? 4 : 5;
            e = mk_expr(E_CALL, type_of(32, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            e->d = args[3]; e->arg5 = args[4];
            if (!e->a->type.ptr || !e->b->type.ptr || e->c->type.ptr ||
                !e->d->type.ptr || (argc == 5 && !e->arg5->type.ptr)) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs pointers and an integer length", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__strlen") || !strcmp(name, "__memchr") ||
            !strcmp(name, "__memcmp") || !strcmp(name, "__strcmp") ||
            !strcmp(name, "__strcpy")) {
            CBExpr *e = mk_expr(E_CALL, type_of(64, 1), line, col);
            int want = !strcmp(name, "__strlen") ? 1 :
                       (!strcmp(name, "__strcmp") || !strcmp(name, "__strcpy")) ? 2 : 3;
            int is_memchr = !strcmp(name, "__memchr");
            int is_strcpy = !strcmp(name, "__strcpy");
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after intrinsic");
                free(e->name); free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            e->a = parse_assignment(p);
            if (want >= 2) {
                if (!tok_is(l, ',')) {
                    cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ',' in intrinsic");
                    free(e->name); free(e); p->err = 1; return NULL;
                }
                lex_next(l);
                e->b = parse_assignment(p);
            }
            if (want == 3) {
                if (!tok_is(l, ',')) {
                    cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ',' in intrinsic");
                    free(e->name); free(e); p->err = 1; return NULL;
                }
                lex_next(l);
                e->c = parse_assignment(p);
            }
            if (!tok_is(l, ')')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ')' in intrinsic");
                free(e->name); free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!e->a || (want >= 2 && !e->b) || (want == 3 && !e->c)) {
                cblock_expr_free(e);
                p->err = 1;
                return NULL;
            }
            if (!e->a->type.ptr) {
                cb_error(p->b, line, col, "ZA5003", "%s needs a pointer", e->name);
                p->err = 1;
            }
            if (!is_memchr && want >= 2 && e->b && !e->b->type.ptr) {
                cb_error(p->b, line, col, "ZA5003", "%s needs pointer arguments", e->name);
                p->err = 1;
            }
            if (is_strcpy && e->b && !e->b->type.ptr) {
                cb_error(p->b, line, col, "ZA5003", "__strcpy needs two pointers");
                p->err = 1;
            }
            if (want == 3 && e->c && e->c->type.ptr) {
                cb_error(p->b, line, col, "ZA5003", "%s needs an integer length", e->name);
                p->err = 1;
            }
            if (!strcmp(e->name, "__strlen")) e->type = type_of(64, 1);
            else if (is_memchr || is_strcpy) e->type = type_ptr();
            else e->type = type_of(32, 0);
            return e;
        }
        if (!strcmp(name, "__vec_round") || !strcmp(name, "__vec_roundc") ||
            !strcmp(name, "__vec_roundm") || !strcmp(name, "__vec_roundp") ||
            !strcmp(name, "__vec_roundz") || !strcmp(name, "__vec_trunc") ||
            !strcmp(name, "__vec_ceil") || !strcmp(name, "__vec_floor")) {
            CBExpr *e;
            CBExpr *arg;
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            arg = parse_assignment(p);
            if (!arg || !tok_is(l, ')')) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            e->a = arg;
            if (!arg->type.vec || !arg->type.vfloat ||
                (arg->type.vbits != 32 && arg->type.vbits != 64)) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs a float or double vector", e->name);
                p->err = 1;
            } else e->type = arg->type;
            return e;
        }
        if (!strncmp(name, "__vec_gather_element_", 21) ||
            !strncmp(name, "__vec_scatter_element_", 22)) {
            CBExpr *e;
            CBExpr *args[4] = {0, 0, 0, 0};
            const char *suffix;
            CType vt;
            int gather = !strncmp(name, "__vec_gather_element_", 21);
            suffix = gather ? name + 21 : name + 22;
            if (!vec_element_type(suffix, &vt)) {
                cb_error(p->b, line, col, "ZA5002", "unsupported gather/scatter suffix");
                p->err = 1; free(name); return NULL;
            }
            e = mk_expr(E_CALL, gather ? vt : type_of(32, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 4)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2]; e->d = args[3];
            if (!e->a->type.vec || !e->b->type.vec || !e->c->type.ptr ||
                e->d->type.ptr || e->d->type.flt || e->d->type.vec) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs a vector, an index vector, a pointer and a constant mask", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_msum_u128")) {
            CBExpr *e;
            CBExpr *args[4] = {0, 0, 0, 0};
            e = mk_expr(E_CALL, type_vec(8, 1, 0, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 4)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2]; e->d = args[3];
            if (!e->a->type.vec || !e->b->type.vec || !e->c->type.vec ||
                e->a->type.vbits != 64 || e->b->type.vbits != 64 ||
                e->c->type.vbits != 8 || e->d->type.ptr ||
                e->d->type.flt || e->d->type.vec) {
                cb_error(p->b, line, col, "ZA5003",
                         "__vec_msum_u128 needs two 64-bit vectors, a byte accumulator and a mask");
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_sum_u128")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            e = mk_expr(E_CALL, type_vec(8, 1, 0, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || !e->b->type.vec ||
                e->a->type.vbits != e->b->type.vbits ||
                (e->a->type.vbits != 32 && e->a->type.vbits != 64) ||
                e->a->type.vfloat) {
                cb_error(p->b, line, col, "ZA5003",
                         "__vec_sum_u128 needs two word or doubleword vectors");
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_sum2") || !strcmp(name, "__vec_sum4")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            e = mk_expr(E_CALL, type_vec(32, 1, 0, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || !e->b->type.vec ||
                e->a->type.vbits != e->b->type.vbits || e->a->type.vfloat ||
                (!strcmp(e->name, "__vec_sum2")
                     ? (e->a->type.vbits != 16 && e->a->type.vbits != 32)
                     : (e->a->type.vbits != 8 && e->a->type.vbits != 16))) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s has unsupported vector arguments", e->name);
                p->err = 1;
            } else e->type = type_vec(!strcmp(e->name, "__vec_sum2") ? 64 : 32, 1, 0, 0);
            return e;
        }
        if (!strcmp(name, "__vec_gfmsum_accum")) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            e = mk_expr(E_CALL, type_vec(16, 1, 0, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 3)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (!e->a->type.vec || !e->b->type.vec || !e->c->type.vec ||
                e->a->type.vbits != e->b->type.vbits ||
                (e->a->type.vbits != 8 && e->a->type.vbits != 16 &&
                 e->a->type.vbits != 32 && e->a->type.vbits != 64)) {
                cb_error(p->b, line, col, "ZA5003",
                         "__vec_gfmsum_accum needs two equal-width vectors and an accumulator");
                p->err = 1;
            } else e->type = type_vec(e->a->type.vbits == 64 ? 8 : e->a->type.vbits * 2,
                                      1, 0, 0);
            return e;
        }
        if (!strncmp(name, "__vec_splat__", 13)) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            CType vt;
            const char *suffix = name + 13;
            if (!vec_element_type(suffix, &vt)) {
                cb_error(p->b, line, col, "ZA5002", "unsupported splat suffix");
                p->err = 1; free(name); return NULL;
            }
            e = mk_expr(E_CALL, vt, line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || e->b->type.ptr || e->b->type.flt ||
                e->b->type.vec) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs a vector and an integer index", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_double") || !strcmp(name, "__vec_float") ||
            !strcmp(name, "__vec_signed") || !strcmp(name, "__vec_unsigned")) {
            CBExpr *e;
            CBExpr *arg;
            CType rt;
            int outbits, vfloat, in_flt;
            if (!strcmp(name, "__vec_double")) { outbits = 64; vfloat = 1; }
            else if (!strcmp(name, "__vec_float")) { outbits = 32; vfloat = 1; }
            else if (!strcmp(name, "__vec_signed")) { outbits = 64; vfloat = 0; }
            else { outbits = 64; vfloat = 0; }
            in_flt = !strcmp(name, "__vec_signed") || !strcmp(name, "__vec_unsigned");
            rt = type_vec(outbits, !strcmp(name, "__vec_unsigned"), vfloat, 0);
            e = mk_expr(E_CALL, rt, line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            arg = parse_assignment(p);
            if (!arg || !tok_is(l, ')')) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            e->a = arg;
            if (!arg->type.vec ||
                (in_flt ? !arg->type.vfloat : arg->type.vfloat) ||
                (outbits == 32 ? arg->type.vbits != 32 : arg->type.vbits != 64)) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s has the wrong vector type", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_doublee") || !strcmp(name, "__vec_floate")) {
            CBExpr *e;
            CBExpr *arg;
            CType rt = !strcmp(name, "__vec_doublee") ? type_vec(64, 0, 1, 0) :
                                                      type_vec(32, 0, 1, 0);
            e = mk_expr(E_CALL, rt, line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            arg = parse_assignment(p);
            if (!arg || !tok_is(l, ')')) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            e->a = arg;
            if (!arg->type.vec || !arg->type.vfloat ||
                (!strcmp(e->name, "__vec_doublee") ? arg->type.vbits != 32 : arg->type.vbits != 64)) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s has the wrong vector type", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_fp_test_data_class")) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 3)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (!e->a->type.vec || !e->a->type.vfloat ||
                (e->a->type.vbits != 32 && e->a->type.vbits != 64) ||
                e->b->type.ptr || e->b->type.flt || e->b->type.vec ||
                !e->c->type.ptr) {
                cb_error(p->b, line, col, "ZA5003",
                         "__vec_fp_test_data_class needs a float/double vector, a mask and an int pointer");
                p->err = 1;
            } else e->type = type_vec(e->a->type.vbits, 0, 0, 1);
            return e;
        }
        if (!strcmp(name, "__vec_genmask") || !strcmp(name, "__vec_genmasks_8") ||
            !strcmp(name, "__vec_genmasks_16") || !strcmp(name, "__vec_genmasks_32") ||
            !strcmp(name, "__vec_genmasks_64") || !strcmp(name, "__vec_test_mask")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            int argc = !strcmp(name, "__vec_genmask") ? 1 :
                       !strcmp(name, "__vec_test_mask") ? 2 : 2;
            CType rt = type_of(32, 0);
            if (!strcmp(name, "__vec_genmask") || !strcmp(name, "__vec_genmasks_8"))
                rt = type_vec(8, 1, 0, 0);
            else if (!strcmp(name, "__vec_genmasks_16"))
                rt = type_vec(16, 1, 0, 0);
            else if (!strcmp(name, "__vec_genmasks_32"))
                rt = type_vec(32, 1, 0, 0);
            else if (!strcmp(name, "__vec_genmasks_64"))
                rt = type_vec(64, 1, 0, 0);
            e = mk_expr(E_CALL, rt, line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!strcmp(e->name, "__vec_test_mask")) {
                if (!e->a->type.vec || !e->b->type.vec) {
                    cb_error(p->b, line, col, "ZA5003",
                             "__vec_test_mask needs two vectors");
                    p->err = 1;
                }
            } else if (e->a->type.ptr || e->a->type.flt || e->a->type.vec ||
                       (e->b && (e->b->type.ptr || e->b->type.flt || e->b->type.vec))) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs integer immediates", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_xld2") || !strcmp(name, "__vec_xlw4") ||
            !strcmp(name, "__vec_xstd2") || !strcmp(name, "__vec_xstw4")) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            int load = !strcmp(name, "__vec_xld2") || !strcmp(name, "__vec_xlw4");
            CType rt = type_ptr();
            int argc = load ? 2 : 3;
            if (!load) rt = type_of(32, 0);
            e = mk_expr(E_CALL, rt, line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (load) {
                if (e->a->type.ptr || e->a->type.flt || !e->b->type.ptr) {
                    cb_error(p->b, line, col, "ZA5003",
                             "%s needs an integer offset and a pointer", e->name);
                    p->err = 1;
                } else {
                    e->type = type_vec(e->b->type.elem_bits ? e->b->type.elem_bits : 8,
                                       e->b->type.elem_unsig, e->b->type.elem_flt, 0);
                }
            } else {
                if (!e->a->type.vec || e->b->type.ptr || e->b->type.flt ||
                    !e->c->type.ptr) {
                    cb_error(p->b, line, col, "ZA5003",
                             "%s needs a vector, an integer offset and a pointer", e->name);
                    p->err = 1;
                }
            }
            return e;
        }
        if (!strcmp(name, "__vec_extend_s64")) {
            CBExpr *e;
            CBExpr *arg;
            e = mk_expr(E_CALL, type_vec(64, 0, 0, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            arg = parse_assignment(p);
            if (!arg || !tok_is(l, ')')) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            e->a = arg;
            if (!arg->type.vec || arg->type.vfloat || arg->type.vbool ||
                (arg->type.vbits != 8 && arg->type.vbits != 16 &&
                 arg->type.vbits != 32)) {
                cb_error(p->b, line, col, "ZA5003",
                         "__vec_extend_s64 needs a signed byte, halfword or word vector");
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_all_eq") || !strcmp(name, "__vec_all_eq_8") ||
            !strcmp(name, "__vec_all_ne") || !strcmp(name, "__vec_all_ne_8") ||
            !strcmp(name, "__vec_any_eq") || !strcmp(name, "__vec_any_eq_8") ||
            !strcmp(name, "__vec_any_ne") || !strcmp(name, "__vec_any_ne_8")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            e = mk_expr(E_CALL, type_of(32, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || !e->b->type.vec ||
                e->a->type.vfloat || e->a->type.vbool) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs two integer vector arguments", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_addc_u128") || !strcmp(name, "__vec_subc_u128") ||
            !strcmp(name, "__vec_subec_u128")) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            int argc = !strcmp(name, "__vec_subec_u128") ? 3 : 2;
            e = mk_expr(E_CALL, type_vec(8, 1, 0, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (!e->a->type.vec || !e->b->type.vec ||
                e->a->type.vbits != 8 || e->b->type.vbits != 8 ||
                (argc == 3 && (!e->c->type.vec || e->c->type.vbits != 8))) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs byte vectors", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_add_u128") || !strcmp(name, "__vec_adde_u128") ||
            !strcmp(name, "__vec_sub_u128") || !strcmp(name, "__vec_sube_u128")) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            int argc = (!strcmp(name, "__vec_adde_u128") ||
                        !strcmp(name, "__vec_sube_u128")) ? 3 : 2;
            e = mk_expr(E_CALL, type_vec(8, 1, 0, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (!e->a->type.vec || !e->b->type.vec ||
                e->a->type.vbits != 8 || e->b->type.vbits != 8 ||
                (argc == 3 && (!e->c->type.vec || e->c->type.vbits != 8))) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs byte vectors", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_abs") || !strcmp(name, "__vec_abs_8") ||
            !strcmp(name, "__vec_nabs")) {
            CBExpr *e;
            CBExpr *arg;
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            arg = parse_assignment(p);
            if (!arg || !tok_is(l, ')')) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            e->a = arg;
            if (!arg->type.vec ||
                (!arg->type.vfloat && arg->type.vbool)) {
                cb_error(p->b, line, col, "ZA5003", "%s needs a numeric vector", e->name);
                p->err = 1;
            } else if (!strcmp(e->name, "__vec_nabs") && !arg->type.vfloat) {
                cb_error(p->b, line, col, "ZA5003", "__vec_nabs needs a float or double vector");
                p->err = 1;
            } else if (!strcmp(e->name, "__vec_abs_8") &&
                       !((arg->type.vfloat && arg->type.vbits == 64) ||
                         (!arg->type.vfloat && arg->type.vbits == 64))) {
                cb_error(p->b, line, col, "ZA5003", "__vec_abs_8 needs a 64-bit vector");
                p->err = 1;
            } else e->type = arg->type;
            return e;
        }
        if (!strcmp(name, "__vec_ctd") || !strcmp(name, "__vec_ctsl") ||
            !strcmp(name, "__vec_ctul")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            CType rt = !strcmp(name, "__vec_ctd") ? type_vec(64, 0, 1, 0) :
                       type_vec(64, !strcmp(name, "__vec_ctul"), 0, 0);
            e = mk_expr(E_CALL, rt, line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || e->b->type.ptr || e->b->type.flt ||
                e->b->type.vec) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs a vector and an integer rounding mode", e->name);
                p->err = 1;
            } else if (!strcmp(e->name, "__vec_ctd")) {
                if (e->a->type.vfloat || e->a->type.vbits != 64) {
                    cb_error(p->b, line, col, "ZA5003",
                             "__vec_ctd needs a 64-bit integer vector");
                    p->err = 1;
                }
            } else if (!e->a->type.vfloat || e->a->type.vbits != 64) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs a double vector", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_splat_s8_") || !strcmp(name, "__vec_splat_s16_") ||
            !strcmp(name, "__vec_splat_s32_") || !strcmp(name, "__vec_splat_s64_") ||
            !strcmp(name, "__vec_splat_u8_") || !strcmp(name, "__vec_splat_u16_") ||
            !strcmp(name, "__vec_splat_u32_") || !strcmp(name, "__vec_splat_u64_")) {
            CBExpr *e;
            CBExpr *arg;
            int bits = strstr(name, "s8_") || strstr(name, "u8_") ? 8 :
                       strstr(name, "s16_") || strstr(name, "u16_") ? 16 :
                       strstr(name, "s32_") || strstr(name, "u32_") ? 32 : 64;
            int unsig = strstr(name, "_u") != NULL;
            e = mk_expr(E_CALL, type_vec(bits, unsig, 0, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            arg = parse_assignment(p);
            if (!arg || !tok_is(l, ')')) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            e->a = arg;
            if (arg->type.ptr || arg->type.flt || arg->type.vec) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs an integer immediate", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_popcnt")) {
            CBExpr *e;
            CBExpr *arg;
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            arg = parse_assignment(p);
            if (!arg || !tok_is(l, ')')) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            e->a = arg;
            if (!arg->type.vec || arg->type.vfloat) {
                cb_error(p->b, line, col, "ZA5003", "__vec_popcnt needs an integer vector");
                p->err = 1;
            } else e->type = type_vec(arg->type.vbits, 1, 0, 0);
            return e;
        }
        if (!strncmp(name, "__vec_search_string_cc", 22) ||
            !strncmp(name, "__vec_search_string_until_zero_cc", 33)) {
            CBExpr *e;
            CBExpr *args[4] = {0, 0, 0, 0};
            e = mk_expr(E_CALL, type_vec(8, 1, 0, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 4)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2]; e->d = args[3];
            if (!e->a->type.vec || !e->b->type.vec || !e->c->type.vec ||
                !e->d->type.ptr) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs three vectors and a condition-code pointer", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_all_ge") || !strcmp(name, "__vec_all_ge_8") ||
            !strcmp(name, "__vec_all_gt") || !strcmp(name, "__vec_all_gt_8") ||
            !strcmp(name, "__vec_all_le") || !strcmp(name, "__vec_all_le_8") ||
            !strcmp(name, "__vec_all_lt") || !strcmp(name, "__vec_all_lt_8") ||
            !strcmp(name, "__vec_all_nge") || !strcmp(name, "__vec_all_ngt") ||
            !strcmp(name, "__vec_all_nle") || !strcmp(name, "__vec_all_nlt") ||
            !strcmp(name, "__vec_any_ge") || !strcmp(name, "__vec_any_ge_8") ||
            !strcmp(name, "__vec_any_gt") || !strcmp(name, "__vec_any_gt_8") ||
            !strcmp(name, "__vec_any_le") || !strcmp(name, "__vec_any_le_8") ||
            !strcmp(name, "__vec_any_lt") || !strcmp(name, "__vec_any_lt_8") ||
            !strcmp(name, "__vec_any_nge") || !strcmp(name, "__vec_any_ngt") ||
            !strcmp(name, "__vec_any_nle") || !strcmp(name, "__vec_any_nlt")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            e = mk_expr(E_CALL, type_of(32, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || !e->b->type.vec ||
                e->a->type.vbits != e->b->type.vbits) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs two compatible vector arguments", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_meadd") || !strcmp(name, "__vec_mhadd") ||
            !strcmp(name, "__vec_mladd") || !strcmp(name, "__vec_moadd")) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 3)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (!e->a->type.vec || !e->b->type.vec || !e->c->type.vec ||
                e->a->type.vfloat || e->a->type.vbool ||
                e->a->type.vbits != e->b->type.vbits) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs integer vector arguments", e->name);
                p->err = 1;
            } else e->type = e->c->type;
            return e;
        }
        if (!strcmp(name, "__vec_madd") || !strcmp(name, "__vec_msub") ||
            !strcmp(name, "__vec_nmadd") || !strcmp(name, "__vec_nmsub")) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 3)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (!e->a->type.vec || !e->b->type.vec || !e->c->type.vec ||
                !e->a->type.vfloat || e->a->type.vbits != e->b->type.vbits ||
                e->a->type.vbits != e->c->type.vbits) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs three equal-width float/double vectors", e->name);
                p->err = 1;
            } else e->type = e->a->type;
            return e;
        }
        if (!strcmp(name, "__vec_mule") || !strcmp(name, "__vec_mulh") ||
            !strcmp(name, "__vec_mulo")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || !e->b->type.vec ||
                e->a->type.vbits != e->b->type.vbits ||
                e->a->type.vfloat || e->a->type.vbool ||
                (e->a->type.vbits != 8 && e->a->type.vbits != 16 &&
                 e->a->type.vbits != 32)) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs two equal-width integer vectors", e->name);
                p->err = 1;
            } else {
                int out = !strcmp(e->name, "__vec_mulh") ? e->a->type.vbits :
                          e->a->type.vbits * 2;
                e->type = type_vec(out, e->a->type.vunsig, 0, 0);
            }
            return e;
        }
        if (!strcmp(name, "__vec_addc") || !strcmp(name, "__vec_subc")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || !e->b->type.vec ||
                e->a->type.vbits != e->b->type.vbits ||
                e->a->type.vfloat || e->a->type.vbool) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs two equal-width unsigned vector arguments", e->name);
                p->err = 1;
            } else e->type = e->a->type;
            return e;
        }
        if (!strcmp(name, "__vec_gfmsum")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            e = mk_expr(E_CALL, type_vec(16, 1, 0, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || !e->b->type.vec ||
                e->a->type.vbits != e->b->type.vbits ||
                (e->a->type.vbits != 8 && e->a->type.vbits != 16 &&
                 e->a->type.vbits != 32 && e->a->type.vbits != 64)) {
                cb_error(p->b, line, col, "ZA5003",
                         "__vec_gfmsum needs two equal-width byte, halfword, word or doubleword vectors");
                p->err = 1;
            } else e->type = type_vec(e->a->type.vbits == 64 ? 8 : e->a->type.vbits * 2,
                                      1, 0, 0);
            return e;
        }
        if (!strcmp(name, "__vec_load_pair_s8") || !strcmp(name, "__vec_load_pair_u8")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            e = mk_expr(E_CALL, type_vec(64, name[strlen(name) - 2] == 'u', 0, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (e->a->type.ptr || e->a->type.vec || e->a->type.flt ||
                e->b->type.ptr || e->b->type.vec || e->b->type.flt) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs two integer scalars", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_bperm")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            e = mk_expr(E_CALL, type_vec(64, 1, 0, 0), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || !e->b->type.vec ||
                e->a->type.vbits != 8 || e->b->type.vbits != 8) {
                cb_error(p->b, line, col, "ZA5003",
                         "__vec_bperm needs two byte vectors");
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_sqrt")) {
            CBExpr *e;
            CBExpr *arg = NULL;
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            arg = parse_assignment(p);
            if (!arg || !tok_is(l, ')')) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            e->a = arg;
            if (!arg->type.vec || !arg->type.vfloat) {
                cb_error(p->b, line, col, "ZA5003",
                         "__vec_sqrt needs a float or double vector");
                p->err = 1;
            } else e->type = arg->type;
            return e;
        }
        if (!strcmp(name, "__vec_andc") || !strcmp(name, "__vec_andc_8") ||
            !strcmp(name, "__vec_nand") || !strcmp(name, "__vec_nor") ||
            !strcmp(name, "__vec_nor_8") || !strcmp(name, "__vec_orc") ||
            !strcmp(name, "__vec_eqv")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || !e->b->type.vec) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs two vector arguments", e->name);
                p->err = 1;
            } else e->type = e->a->type;
            return e;
        }
        if (!strcmp(name, "__vec_srdb")) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 3)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (!e->a->type.vec || !e->b->type.vec ||
                e->c->type.ptr || e->c->type.flt || e->c->type.vec) {
                cb_error(p->b, line, col, "ZA5003",
                         "__vec_srdb needs two vectors and a byte count");
                p->err = 1;
            } else e->type = e->a->type;
            return e;
        }
        if (!strcmp(name, "__vec_sldw")) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 3)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (!e->a->type.vec || !e->b->type.vec ||
                e->c->type.ptr || e->c->type.flt || e->c->type.vec) {
                cb_error(p->b, line, col, "ZA5003",
                         "__vec_sldw needs two vectors and a word count");
                p->err = 1;
            } else e->type = e->a->type;
            return e;
        }
        if (!strcmp(name, "__vec_sld") || !strcmp(name, "__vec_sldb")) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 3)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (!e->a->type.vec || !e->b->type.vec ||
                e->c->type.ptr || e->c->type.flt || e->c->type.vec) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs two vectors and an integer count", e->name);
                p->err = 1;
            } else e->type = e->a->type;
            return e;
        }
        if (!strcmp(name, "__vec_rl") || !strcmp(name, "__vec_rl_8") ||
            !strcmp(name, "__vec_rli") || !strcmp(name, "__vec_rl_mask")) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            int argc = !strcmp(name, "__vec_rl_mask") ? 3 :
                       !strcmp(name, "__vec_rli") ? 2 : 2;
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (!e->a->type.vec) {
                cb_error(p->b, line, col, "ZA5003", "%s needs a vector", e->name);
                p->err = 1;
            } else if (!strcmp(e->name, "__vec_rli")) {
                if (e->b->type.ptr || e->b->type.flt || e->b->type.vec) {
                    cb_error(p->b, line, col, "ZA5003", "__vec_rli needs an integer count");
                    p->err = 1;
                }
            } else if (!e->b->type.vec) {
                cb_error(p->b, line, col, "ZA5003", "%s needs a vector count", e->name);
                p->err = 1;
            }
            if (!strcmp(e->name, "__vec_rl_mask") &&
                (e->c->type.ptr || e->c->type.flt || e->c->type.vec)) {
                cb_error(p->b, line, col, "ZA5003", "__vec_rl_mask needs an integer mask");
                p->err = 1;
            }
            e->type = e->a->type;
            return e;
        }
        if (!strcmp(name, "__vec_sll") || !strcmp(name, "__vec_srl") ||
            !strcmp(name, "__vec_sral") || !strcmp(name, "__vec_slb") ||
            !strcmp(name, "__vec_srab")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || !e->b->type.vec) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs two vector arguments", e->name);
                p->err = 1;
            } else e->type = e->a->type;
            return e;
        }
        if (!strcmp(name, "__vec_mergeh") || !strcmp(name, "__vec_mergeh_8") ||
            !strcmp(name, "__vec_mergel") || !strcmp(name, "__vec_mergel_8") ||
            !strcmp(name, "__vec_checksum") ||
            !strcmp(name, "__vec_cntlz") || !strcmp(name, "__vec_cnttz")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            CType rt;
            int argc = (!strcmp(name, "__vec_checksum")) ? 2 :
                       (!strcmp(name, "__vec_cntlz") || !strcmp(name, "__vec_cnttz")) ? 1 : 2;
            if (!strcmp(name, "__vec_checksum")) rt = type_vec(32, 1, 0, 0);
            else rt = type_of(128, 1);
            e = mk_expr(E_CALL, rt, line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || (argc == 2 && !e->b->type.vec)) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs vector arguments", e->name);
                p->err = 1;
            } else if (!strcmp(e->name, "__vec_checksum")) {
                e->type = type_vec(32, 1, 0, 0);
                if (e->a->type.vbits != 32 || e->b->type.vbits != 32) {
                    cb_error(p->b, line, col, "ZA5003",
                             "__vec_checksum needs two 32-bit vectors");
                    p->err = 1;
                }
            } else if (!strcmp(e->name, "__vec_cntlz") || !strcmp(e->name, "__vec_cnttz")) {
                e->type = type_vec(e->a->type.vbits, 1, 0, 0);
            } else e->type = e->a->type;
            return e;
        }
        if (!strcmp(name, "__vec_pack") || !strcmp(name, "__vec_pack_8") ||
            !strcmp(name, "__vec_packs") || !strcmp(name, "__vec_packs_8") ||
            !strcmp(name, "__vec_packs_cc") || !strcmp(name, "__vec_packsu") ||
            !strcmp(name, "__vec_packsu_8") || !strcmp(name, "__vec_packsu_cc") ||
            !strncmp(name, "__vec_unpackh_", 14) ||
            !strncmp(name, "__vec_unpackl_", 14)) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            CType rt;
            int unpack = !strncmp(name, "__vec_unpackh_", 14) ||
                         !strncmp(name, "__vec_unpackl_", 14);
            int argc = unpack ? 1 :
                       (!strcmp(name, "__vec_packs_cc") ||
                        !strcmp(name, "__vec_packsu_cc")) ? 3 : 2;
            if (unpack) {
                const char *suffix = name + 14;
                int outbits = !strncmp(suffix + 1, "2", 1) ? 16 :
                              !strncmp(suffix + 1, "4", 1) ? 32 : 64;
                rt = type_vec(outbits, suffix[0] == 'u', 0, 0);
            } else {
                argc = (!strcmp(name, "__vec_packs_cc") ||
                        !strcmp(name, "__vec_packsu_cc")) ? 3 : 2;
                rt = type_of(128, 1);
            }
            e = mk_expr(E_CALL, rt, line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (!e->a->type.vec || (!unpack && !e->b->type.vec) ||
                (unpack ? e->a->type.vfloat : e->a->type.vfloat || e->b->type.vfloat)) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs integer vector arguments", e->name);
                p->err = 1;
            }
            if (!unpack) {
                e->type = type_vec(e->a->type.vbits / 2, e->a->type.vunsig, 0,
                                   e->a->type.vbool);
                if (e->c && !e->c->type.ptr) {
                    cb_error(p->b, line, col, "ZA5003",
                             "%s needs a condition-code pointer", e->name);
                    p->err = 1;
                }
            }
            return e;
        }
        if (!strncmp(name, "__vec_extract_", 14) ||
            !strncmp(name, "__vec_insert_and_zero_", 22) ||
            !strncmp(name, "__vec_insert_", 13) ||
            !strncmp(name, "__vec_promote_", 14)) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            const char *suffix;
            CType vt;
            int kind; /* 0 extract, 1 insert, 2 promote, 3 insert_and_zero */
            int argc;
            if (!strncmp(name, "__vec_extract_", 14)) {
                kind = 0; suffix = name + 14; argc = 2;
            } else if (!strncmp(name, "__vec_insert_and_zero_", 22)) {
                kind = 3; suffix = name + 22; argc = 1;
            } else if (!strncmp(name, "__vec_insert_", 13)) {
                kind = 1; suffix = name + 13; argc = 3;
            } else {
                kind = 2; suffix = name + 14; argc = 2;
            }
            if (!vec_suffix_type(suffix, &vt)) {
                cb_error(p->b, line, col, "ZA5002", "unsupported vector element suffix");
                p->err = 1; free(name); return NULL;
            }
            e = mk_expr(E_CALL, kind == 0 ? vec_scalar_type(suffix) : vt, line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (kind == 0) {
                if (!e->a->type.vec || e->b->type.ptr || e->b->type.vec || e->b->type.flt) {
                    cb_error(p->b, line, col, "ZA5003", "__vec_extract needs a vector and an index");
                    p->err = 1;
                }
            } else if (kind == 1) {
                if (e->a->type.vec || !e->b->type.vec || e->c->type.vec ||
                    e->c->type.ptr || e->c->type.flt) {
                    cb_error(p->b, line, col, "ZA5003",
                             "__vec_insert needs a scalar, a vector and an index");
                    p->err = 1;
                }
            } else if (kind == 2) {
                if (e->a->type.vec || e->b->type.vec || e->b->type.ptr || e->b->type.flt) {
                    cb_error(p->b, line, col, "ZA5003",
                             "__vec_promote needs a scalar and an index");
                    p->err = 1;
                }
            } else if (!e->a->type.ptr) {
                cb_error(p->b, line, col, "ZA5003", "__vec_insert_and_zero needs a pointer");
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__vec_max") || !strcmp(name, "__vec_max_8") ||
            !strcmp(name, "__vec_min") || !strcmp(name, "__vec_min_8") ||
            !strcmp(name, "__vec_avg")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || !e->b->type.vec || e->a->type.vbool ||
                e->a->type.vbits != e->b->type.vbits ||
                e->a->type.vfloat != e->b->type.vfloat) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs two compatible vector arguments", e->name);
                p->err = 1;
            } else e->type = e->a->type;
            return e;
        }
        if (!strcmp(name, "__vec_cmpge") || !strcmp(name, "__vec_cmpge_8") ||
            !strcmp(name, "__vec_cmpgt") || !strcmp(name, "__vec_cmpgt_8") ||
            !strcmp(name, "__vec_cmple") || !strcmp(name, "__vec_cmple_8") ||
            !strcmp(name, "__vec_cmplt") || !strcmp(name, "__vec_cmplt_8")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || !e->b->type.vec || e->a->type.vbool ||
                (e->a->type.vfloat && e->a->type.vbits != 32 && e->a->type.vbits != 64)) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs two compatible vector arguments", e->name);
                p->err = 1;
            } else {
                e->type = type_vec(e->a->type.vbits, 0, 0, 1);
            }
            return e;
        }
        if (!strcmp(name, "__vec_cmpeq") || !strcmp(name, "__vec_cmpeq_8")) {
            CBExpr *e;
            CBExpr *args[2] = {0, 0};
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 2)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1];
            if (!e->a->type.vec || !e->b->type.vec || e->a->type.vbool ||
                (e->a->type.vfloat && e->a->type.vbits != 32 && e->a->type.vbits != 64)) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs two compatible vector arguments", e->name);
                p->err = 1;
            } else {
                e->type = type_vec(e->a->type.vbits, 0, 0, 1);
            }
            return e;
        }
        if (!strcmp(name, "__vec_sel") || !strcmp(name, "__vec_sel_8") ||
            !strcmp(name, "__vec_perm") || !strcmp(name, "__vec_permi")) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            int argc = !strcmp(name, "__vec_permi") ? 3 : 3;
            e = mk_expr(E_CALL, type_of(128, 1), line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (!strcmp(e->name, "__vec_permi")) {
                if (!e->a->type.vec || !e->b->type.vec ||
                    e->c->type.vec || e->c->type.ptr || e->c->type.flt) {
                    cb_error(p->b, line, col, "ZA5003",
                             "__vec_permi needs two vectors and an integer selector");
                    p->err = 1;
                }
            } else if (!e->a->type.vec || !e->b->type.vec ||
                       !e->c->type.vec) {
                cb_error(p->b, line, col, "ZA5003",
                         "%s needs vector arguments", e->name);
                p->err = 1;
            }
            e->type = e->a->type;
            return e;
        }
        if (!strncmp(name, "__vec_load_len_", 15) ||
            !strncmp(name, "__vec_load_bndry_", 17) ||
            !strncmp(name, "__vec_splats_", 13) ||
            !strcmp(name, "__vec_store_len") || !strcmp(name, "__vec_store_len_r")) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            CType vt;
            const char *suffix = NULL;
            int argc = !strncmp(name, "__vec_splats_", 13) ? 1 :
                       (!strncmp(name, "__vec_load_len_", 15) ||
                        !strncmp(name, "__vec_load_bndry_", 17)) ? 2 : 3;
            if (!strncmp(name, "__vec_load_len_", 15)) suffix = name + 15;
            else if (!strncmp(name, "__vec_load_bndry_", 17)) suffix = name + 17;
            else if (!strncmp(name, "__vec_splats_", 13)) suffix = name + 13;
            else {
                vt = type_vec(8, 1, 0, 0);
                if (!strcmp(name, "__vec_store_len_r")) vt = type_vec(8, 1, 0, 0);
            }
            if (suffix && !vec_suffix_type(suffix, &vt)) {
                cb_error(p->b, line, col, "ZA5002", "unsupported vector type suffix");
                p->err = 1;
                free(name);
                return NULL;
            }
            if (!suffix && (!strcmp(name, "__vec_store_len") || !strcmp(name, "__vec_store_len_r")))
                vt = type_vec(8, 1, 0, 0);
            e = mk_expr(E_CALL, (!strncmp(name, "__vec_load_len_", 15) ||
                                 !strncmp(name, "__vec_load_bndry_", 17) ||
                                 !strncmp(name, "__vec_splats_", 13)) ? vt : type_of(32, 0),
                        line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (!strncmp(e->name, "__vec_splats_", 13)) {
                if (e->a->type.ptr || e->a->type.vec) {
                    cb_error(p->b, line, col, "ZA5003", "a vector splat needs a scalar");
                    p->err = 1;
                }
            } else if (!strncmp(e->name, "__vec_load_len_", 15) ||
                       !strncmp(e->name, "__vec_load_bndry_", 17)) {
                if (!e->a->type.ptr || e->b->type.ptr) {
                    cb_error(p->b, line, col, "ZA5003", "vector load needs a pointer and a length");
                    p->err = 1;
                }
            } else {
                if (!e->a->type.vec || !e->b->type.ptr || e->c->type.ptr) {
                    cb_error(p->b, line, col, "ZA5003",
                             "vector store needs a vector, a pointer and a length");
                    p->err = 1;
                }
            }
            return e;
        }
        if (!strcmp(name, "__lper") || !strcmp(name, "__lpdr") ||
            !strcmp(name, "__lner") || !strcmp(name, "__lndr") ||
            !strcmp(name, "__sqer") || !strcmp(name, "__sqdr") ||
            !strcmp(name, "__fier") || !strcmp(name, "__fidr") ||
            !strcmp(name, "__fixr") || !strcmp(name, "__cfer") ||
            !strcmp(name, "__cfdr") || !strcmp(name, "__cfxr") ||
            !strcmp(name, "__lpxr") || !strcmp(name, "__lnxr") ||
            !strcmp(name, "__sqxr") || !strcmp(name, "__cger") ||
            !strcmp(name, "__cgdr") || !strcmp(name, "__cgxr")) {
            CBExpr *e;
            CBExpr *args[3] = {0, 0, 0};
            CBExpr *bad = NULL;
            int argc = (!strcmp(name, "__lper") || !strcmp(name, "__lpdr") ||
                        !strcmp(name, "__lner") || !strcmp(name, "__lndr") ||
                        !strcmp(name, "__lpxr") || !strcmp(name, "__lnxr")) ? 2 :
                       (!strcmp(name, "__cfer") || !strcmp(name, "__cfdr") ||
                        !strcmp(name, "__cfxr") || !strcmp(name, "__cger") ||
                        !strcmp(name, "__cgdr") || !strcmp(name, "__cgxr")) ? 3 : 1;
            CType rt = type_of(32, 0);
            if (!strcmp(name, "__sqer") || !strcmp(name, "__fier")) rt = type_fp(32);
            else if (!strcmp(name, "__sqdr") || !strcmp(name, "__fidr")) rt = type_fp(64);
            else if (!strcmp(name, "__sqxr") || !strcmp(name, "__fixr")) rt = type_fp(128);
            e = mk_expr(E_CALL, rt, line, col);
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (!strcmp(e->name, "__lper") || !strcmp(e->name, "__lner")) {
                if (!e->a->type.ptr || !e->b->type.flt || e->b->type.bits != 32 ||
                    e->b->type.dfp) {
                    cb_error(p->b, line, col, "ZA5003", "%s needs (float *, float)", e->name);
                    p->err = 1;
                }
            } else if (!strcmp(e->name, "__lpdr") || !strcmp(e->name, "__lndr")) {
                if (!e->a->type.ptr || !e->b->type.flt || e->b->type.bits != 64 ||
                    e->b->type.dfp) {
                    cb_error(p->b, line, col, "ZA5003", "%s needs (double *, double)", e->name);
                    p->err = 1;
                }
            } else if (!strcmp(e->name, "__sqer") || !strcmp(e->name, "__fier")) {
                if (!e->a->type.flt || e->a->type.bits != 32 || e->a->type.dfp)
                    bad = e->a;
            } else if (!strcmp(e->name, "__sqdr") || !strcmp(e->name, "__fidr")) {
                if (!e->a->type.flt || e->a->type.bits != 64 || e->a->type.dfp)
                    bad = e->a;
            } else if (!strcmp(e->name, "__sqxr") || !strcmp(e->name, "__fixr")) {
                if (!e->a->type.flt || e->a->type.bits != 128 || e->a->type.dfp)
                    bad = e->a;
            } else if (!strcmp(e->name, "__lpxr") || !strcmp(e->name, "__lnxr")) {
                if (!e->a->type.ptr || !e->b->type.flt || e->b->type.bits != 128 ||
                    e->b->type.dfp) bad = e->a;
            } else {
                if (!e->a->type.ptr || e->b->type.ptr || e->b->type.flt ||
                    e->c->type.ptr || !e->c->type.flt) {
                    bad = e->c;
                }
            }
            if (bad) {
                cb_error(p->b, line, col, "ZA5003", "%s has an argument of the wrong type", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__efpc") || !strcmp(name, "__sfpc") ||
            !strcmp(name, "__fiebr") || !strcmp(name, "__fidbr") ||
            !strcmp(name, "__lpebr") || !strcmp(name, "__lpdbr") ||
            !strcmp(name, "__lnebr") || !strcmp(name, "__lndbr") ||
            !strcmp(name, "__sqebr") || !strcmp(name, "__sqdbr") ||
            !strcmp(name, "__cfebr") || !strcmp(name, "__cfdbr") ||
            !strcmp(name, "__cgebr") || !strcmp(name, "__cgdbr") ||
            !strcmp(name, "__maebr") || !strcmp(name, "__madbr") ||
            !strcmp(name, "__msebr") || !strcmp(name, "__msdbr") ||
            !strcmp(name, "__fmadd") || !strcmp(name, "__fmadds") ||
            !strcmp(name, "__fmsub") || !strcmp(name, "__fmsubs") ||
            !strcmp(name, "__sqxbr") || !strcmp(name, "__fixbr") ||
            !strcmp(name, "__lpxbr") || !strcmp(name, "__lnxbr") ||
            !strcmp(name, "__cfxbr") || !strcmp(name, "__cgxbr") ||
            !strcmp(name, "__tceb") || !strcmp(name, "__tcdb") ||
            !strcmp(name, "__tcxb") || !strcmp(name, "__srnm") ||
            !strcmp(name, "__thder") || !strcmp(name, "__thdr") ||
            !strcmp(name, "__tbedr") || !strcmp(name, "__tbdr") ||
            !strcmp(name, "__diebr") || !strcmp(name, "__didbr") ||
            !strcmp(name, "__cdzt") || !strcmp(name, "__cxzt") ||
            !strcmp(name, "__czdt") || !strcmp(name, "__czxt")) {
            CBExpr *e = mk_expr(E_CALL, type_of(32, 0), line, col);
            CBExpr *args[5] = {0, 0, 0, 0, 0};
            int argc = (!strcmp(name, "__efpc")) ? 0 :
                       (!strcmp(name, "__sfpc") || !strcmp(name, "__sqebr") ||
                        !strcmp(name, "__sqdbr")) ? 1 :
                       (!strcmp(name, "__fiebr") || !strcmp(name, "__fidbr") ||
                        !strcmp(name, "__lpebr") || !strcmp(name, "__lpdbr") ||
                        !strcmp(name, "__lnebr") || !strcmp(name, "__lndbr")) ? 2 :
                       (!strcmp(name, "__cfebr") || !strcmp(name, "__cfdbr") ||
                        !strcmp(name, "__cgebr") || !strcmp(name, "__cgdbr") ||
                        !strcmp(name, "__cfxbr") || !strcmp(name, "__cgxbr")) ? 3 :
                       (!strcmp(name, "__maebr") || !strcmp(name, "__madbr") ||
                        !strcmp(name, "__msebr") || !strcmp(name, "__msdbr") ||
                        !strcmp(name, "__fmadd") || !strcmp(name, "__fmadds") ||
                        !strcmp(name, "__fmsub") || !strcmp(name, "__fmsubs")) ? 3 :
                       (!strcmp(name, "__fixbr") || !strcmp(name, "__lpxbr") ||
                        !strcmp(name, "__lnxbr") || !strcmp(name, "__tceb") ||
                        !strcmp(name, "__tcdb") || !strcmp(name, "__tcxb") ||
                        !strcmp(name, "__thder") || !strcmp(name, "__thdr")) ? 2 :
                       (!strcmp(name, "__tbedr") || !strcmp(name, "__tbdr") ||
                        !strcmp(name, "__cdzt") || !strcmp(name, "__cxzt")) ? 3 :
                       (!strcmp(name, "__czdt") || !strcmp(name, "__czxt")) ? 4 :
                       (!strcmp(name, "__diebr") || !strcmp(name, "__didbr")) ? 5 : 1;
            CBExpr *bad = NULL;
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            e->d = args[3]; e->arg5 = args[4];
            if (!strcmp(e->name, "__efpc")) e->type = type_of(32, 0);
            else if (!strcmp(e->name, "__sqebr") || !strcmp(e->name, "__fiebr"))
                e->type = type_fp(32);
            else if (!strcmp(e->name, "__sqdbr") || !strcmp(e->name, "__fidbr"))
                e->type = type_fp(64);
            else if (!strcmp(e->name, "__maebr") || !strcmp(e->name, "__msebr") ||
                     !strcmp(e->name, "__fmadds") || !strcmp(e->name, "__fmsubs"))
                e->type = type_fp(32);
            else if (!strcmp(e->name, "__madbr") || !strcmp(e->name, "__msdbr") ||
                     !strcmp(e->name, "__fmadd") || !strcmp(e->name, "__fmsub"))
                e->type = type_fp(64);
            else if (!strcmp(e->name, "__sqxbr") || !strcmp(e->name, "__fixbr"))
                e->type = type_fp(128);
            else if (!strcmp(e->name, "__cdzt")) e->type = type_dfp(64);
            else if (!strcmp(e->name, "__cxzt")) e->type = type_dfp(128);
            else e->type = type_of(32, 0);
            if (!strcmp(e->name, "__fiebr")) {
                if (e->a->type.flt || !e->b->type.flt || e->b->type.bits != 32) bad = e->b;
            } else if (!strcmp(e->name, "__fidbr")) {
                if (e->a->type.flt || !e->b->type.flt || e->b->type.bits != 64) bad = e->b;
            } else if (!strcmp(e->name, "__sqebr")) {
                if (!e->a->type.flt || e->a->type.bits != 32) bad = e->a;
            } else if (!strcmp(e->name, "__sqdbr")) {
                if (!e->a->type.flt || e->a->type.bits != 64) bad = e->a;
            } else if (!strcmp(e->name, "__lpebr") || !strcmp(e->name, "__lnebr")) {
                if (!e->a->type.ptr || !e->b->type.flt || e->b->type.bits != 32) bad = e->a;
            } else if (!strcmp(e->name, "__lpdbr") || !strcmp(e->name, "__lndbr")) {
                if (!e->a->type.ptr || !e->b->type.flt || e->b->type.bits != 64) bad = e->a;
            } else if (!strcmp(e->name, "__cfebr") || !strcmp(e->name, "__cgebr")) {
                if (!e->a->type.ptr || e->b->type.flt || !e->c->type.flt ||
                    e->c->type.bits != 32) bad = e->c;
            } else if (!strcmp(e->name, "__cfdbr") || !strcmp(e->name, "__cgdbr")) {
                if (!e->a->type.ptr || e->b->type.flt || !e->c->type.flt ||
                    e->c->type.bits != 64) bad = e->c;
            } else if (!strcmp(e->name, "__cdzt") || !strcmp(e->name, "__cxzt")) {
                if (!e->a->type.ptr || e->b->type.ptr || e->b->type.flt ||
                    e->c->type.ptr || e->c->type.flt) bad = e->a;
            } else if (!strcmp(e->name, "__czdt")) {
                if (!e->a->type.dfp || e->a->type.bits != 64 || !e->b->type.ptr ||
                    e->c->type.ptr || e->c->type.flt || e->d->type.ptr ||
                    e->d->type.flt) bad = e->a;
            } else if (!strcmp(e->name, "__czxt")) {
                if (!e->a->type.dfp || e->a->type.bits != 128 || !e->b->type.ptr ||
                    e->c->type.ptr || e->c->type.flt || e->d->type.ptr ||
                    e->d->type.flt) bad = e->a;
            } else if (!strcmp(e->name, "__thder")) {
                if (!e->a->type.ptr || !e->b->type.flt || e->b->type.bits != 32) bad = e->b;
            } else if (!strcmp(e->name, "__thdr")) {
                if (!e->a->type.ptr || !e->b->type.flt || e->b->type.bits != 64) bad = e->b;
            } else if (!strcmp(e->name, "__tbedr") || !strcmp(e->name, "__tbdr")) {
                if (!e->a->type.ptr || e->b->type.ptr || e->b->type.flt ||
                    !e->c->type.flt || e->c->type.bits != 64) bad = e->c;
            } else if (!strcmp(e->name, "__diebr") || !strcmp(e->name, "__didbr")) {
                if (!e->a->type.ptr || !e->b->type.ptr || !e->c->type.flt ||
                    !e->d->type.flt || !e->arg5 || e->arg5->type.ptr ||
                    e->arg5->type.flt || e->c->type.bits != e->d->type.bits ||
                    (e->c->type.bits != 32 && e->c->type.bits != 64))
                    bad = e->c;
            } else if (!strcmp(e->name, "__tceb")) {
                if (!e->a->type.flt || e->a->type.bits != 32 || e->b->type.ptr ||
                    e->b->type.flt) bad = e->a;
            } else if (!strcmp(e->name, "__tcdb")) {
                if (!e->a->type.flt || e->a->type.bits != 64 || e->b->type.ptr ||
                    e->b->type.flt) bad = e->a;
            } else if (!strcmp(e->name, "__tcxb")) {
                if (!e->a->type.flt || e->a->type.bits != 128 || e->b->type.ptr ||
                    e->b->type.flt) bad = e->a;
            } else if (!strcmp(e->name, "__srnm")) {
                if (e->a->type.ptr || e->a->type.flt) bad = e->a;
            } else if (!strcmp(e->name, "__sqxbr")) {
                if (!e->a->type.flt || e->a->type.bits != 128) bad = e->a;
            } else if (!strcmp(e->name, "__fixbr")) {
                if (e->a->type.ptr || !e->b->type.flt || e->b->type.bits != 128) bad = e->b;
            } else if (!strcmp(e->name, "__lpxbr") || !strcmp(e->name, "__lnxbr")) {
                if (!e->a->type.ptr || !e->b->type.flt || e->b->type.bits != 128) bad = e->a;
            } else if (!strcmp(e->name, "__cfxbr") || !strcmp(e->name, "__cgxbr")) {
                if (!e->a->type.ptr || e->b->type.flt || !e->c->type.flt ||
                    e->c->type.bits != 128) bad = e->c;
            } else if (!strcmp(e->name, "__maebr") || !strcmp(e->name, "__msebr") ||
                       !strcmp(e->name, "__fmadds") || !strcmp(e->name, "__fmsubs")) {
                if (!e->a->type.flt || !e->b->type.flt || !e->c->type.flt ||
                    e->a->type.bits != 32 || e->b->type.bits != 32 || e->c->type.bits != 32)
                    bad = e->a;
            } else if (!strcmp(e->name, "__madbr") || !strcmp(e->name, "__msdbr") ||
                       !strcmp(e->name, "__fmadd") || !strcmp(e->name, "__fmsub")) {
                if (!e->a->type.flt || !e->b->type.flt || !e->c->type.flt ||
                    e->a->type.bits != 64 || e->b->type.bits != 64 || e->c->type.bits != 64)
                    bad = e->a;
            } else if (!strcmp(e->name, "__sfpc")) {
                if (e->a->type.ptr) bad = e->a;
            }
            if (bad) {
                cb_error(p->b, bad->line, bad->col, "ZA5003", "%s has an argument of the wrong type",
                         e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__popcnt") || !strcmp(name, "__popcnt4") ||
            !strcmp(name, "__popcnt8") || !strcmp(name, "__lrv") ||
            !strcmp(name, "__lrvh") || !strcmp(name, "__lrvg") ||
            !strcmp(name, "__strv") || !strcmp(name, "__strvh") ||
            !strcmp(name, "__strvg") || !strcmp(name, "__stck") ||
            !strcmp(name, "__stcke") || !strcmp(name, "__stckf")) {
            CBExpr *e = mk_expr(E_CALL, type_of(64, 1), line, col);
            CBExpr *args[2] = {0, 0};
            int argc = (!strcmp(name, "__strv") || !strcmp(name, "__strvg") ||
                        !strcmp(name, "__strvh")) ? 2 : 1;
            CBExpr *bad = NULL;
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0];
            e->b = args[1];
            if (!strcmp(e->name, "__popcnt") || !strcmp(e->name, "__popcnt8"))
                e->type = type_of(64, 1);
            else if (!strcmp(e->name, "__popcnt4") || !strcmp(e->name, "__lrv"))
                e->type = type_of(32, 0);
            else if (!strcmp(e->name, "__lrvh"))
                e->type = type_of(16, 1);
            else if (!strcmp(e->name, "__strv") || !strcmp(e->name, "__strvg") ||
                     !strcmp(e->name, "__stck") || !strcmp(e->name, "__stcke") ||
                     !strcmp(e->name, "__stckf"))
                e->type = type_of(32, 0);
            else
                e->type = type_of(64, 1);
            if (!strcmp(e->name, "__lrv") || !strcmp(e->name, "__lrvh") ||
                !strcmp(e->name, "__lrvg") || !strcmp(e->name, "__stck") ||
                !strcmp(e->name, "__stcke") || !strcmp(e->name, "__stckf")) {
                if (!e->a->type.ptr) bad = e->a;
            }
            if (!strcmp(e->name, "__strv") || !strcmp(e->name, "__strvg") ||
                !strcmp(e->name, "__strvh")) {
                if (e->a->type.ptr) bad = e->a;
                if (!e->b || !e->b->type.ptr) bad = e->b;
            }
            if (bad) {
                cb_error(p->b, bad->line, bad->col, "ZA5003", "%s has an argument of the wrong type",
                         e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__lad") || !strcmp(name, "__ladl") ||
            !strcmp(name, "__lan") || !strcmp(name, "__lax") ||
            !strcmp(name, "__lao") || !strcmp(name, "__ladg") ||
            !strcmp(name, "__ladlg") || !strcmp(name, "__lang") ||
            !strcmp(name, "__laxg") || !strcmp(name, "__laog")) {
            CBExpr *e = mk_expr(E_CALL, type_of(32, 0), line, col);
            CBExpr *args[3] = {0, 0, 0};
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 3)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            if (!e->a->type.ptr || e->b->type.ptr || !e->c->type.ptr) {
                cb_error(p->b, line, col, "ZA5003", "%s needs (pointer, value, pointer)", e->name);
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__lpd") || !strcmp(name, "__lpdg")) {
            CBExpr *e = mk_expr(E_CALL, type_of(32, 0), line, col);
            CBExpr *args[4] = {0, 0, 0, 0};
            int i, bad = 0;
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 4)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2]; e->d = args[3];
            for (i = 0; i < 4; i++) if (!args[i] || !args[i]->type.ptr) bad = 1;
            if (bad) {
                cb_error(p->b, line, col, "ZA5003", "__lpd needs four pointers");
                p->err = 1;
            }
            return e;
        }
        if (!strcmp(name, "__csg")) {
            CBExpr *e = mk_expr(E_CALL, type_of(32, 0), line, col);
            CBExpr *args[3] = {0, 0, 0};
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 3) || !args[0]->type.ptr ||
                !args[1]->type.ptr || !args[2]->type.ptr) {
                if (!p->err)
                    cb_error(p->b, line, col, "ZA5003", "__csg needs three pointers");
                p->err = 1;
                return e;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            return e;
        }
        {
            int argn = 0, dec4 = 0;
            if (!strcmp(name, "__ap") || !strcmp(name, "__cp") ||
                !strcmp(name, "__sp") || !strcmp(name, "__mp") ||
                !strcmp(name, "__dp") || !strcmp(name, "__zap") ||
                !strcmp(name, "__pack") || !strcmp(name, "__unpk"))
                argn = 4, dec4 = 1;
            else if (!strcmp(name, "__nc") || !strcmp(name, "__oc") ||
                     !strcmp(name, "__xc") || !strcmp(name, "__tr") ||
                     !strcmp(name, "__ed"))
                argn = 3;
            else if (!strcmp(name, "__tp"))
                argn = 2;
            else if (!strcmp(name, "__srp"))
                argn = 4;
            else if (!strcmp(name, "__cvd"))
                argn = 2;
            else if (!strcmp(name, "__cvb"))
                argn = 1;
            if (argn) {
                CBExpr *e = mk_expr(E_CALL, type_of(32, 0), line, col);
                CBExpr *args[4] = {0, 0, 0, 0};
                int i, bad = 0;
                e->name = xstrdup(name);
                free(name);
                lex_next(l);
                if (!tok_is(l, '(')) {
                    cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                    cblock_expr_free(e); p->err = 1; return NULL;
                }
                lex_next(l);
                if (!parse_call_args(p, args, argn)) {
                    cblock_expr_free(e); p->err = 1; return NULL;
                }
                e->a = args[0]; e->b = args[1]; e->c = args[2]; e->d = args[3];
                if (!strcmp(e->name, "__srp")) {
                    if (!e->a->type.ptr || e->b->type.ptr || e->c->type.ptr || e->d->type.ptr)
                        bad = 1;
                } else if (!strcmp(e->name, "__tp")) {
                    if (!e->a->type.ptr || e->b->type.ptr) bad = 1;
                } else if (dec4) {
                    if (!e->a->type.ptr || e->b->type.ptr || !e->c->type.ptr || e->d->type.ptr)
                        bad = 1;
                } else if (argn == 3) {
                    if (!e->a->type.ptr || !e->b->type.ptr || e->c->type.ptr) bad = 1;
                } else if (argn == 2) {
                    if (e->a->type.ptr || !e->b->type.ptr) bad = 1;
                } else if (!e->a->type.ptr) {
                    bad = 1;
                }
                for (i = 0; i < argn; i++) if (!args[i]) bad = 1;
                if (bad) {
                    cb_error(p->b, line, col, "ZA5003", "%s has an argument of the wrong type", e->name);
                    p->err = 1;
                }
                return e;
            }
        }
        if (!strcmp(name, "__cdsg")) {
            CBExpr *e = mk_expr(E_CALL, type_of(32, 0), line, col);
            CBExpr *args[3] = {0, 0, 0};
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, 3) || !args[0]->type.ptr ||
                !args[1]->type.ptr || !args[2]->type.ptr) {
                if (!p->err)
                    cb_error(p->b, line, col, "ZA5003", "__cdsg needs three pointers");
                p->err = 1;
                return e;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            return e;
        }
        if (!strcmp(name, "__cvbg") || !strcmp(name, "__cvdg") ||
            !strcmp(name, "__pka") || !strcmp(name, "__pku") ||
            !strcmp(name, "__unpka") || !strcmp(name, "__unpku") ||
            !strcmp(name, "__mvcrl")) {
            CBExpr *e = mk_expr(E_CALL, type_of(32, 0), line, col);
            CBExpr *args[3] = {0, 0, 0};
            int argc = !strcmp(name, "__cvbg") ? 1 : !strcmp(name, "__cvdg") ? 2 : 3;
            int i, bad = 0;
            e->name = xstrdup(name);
            free(name);
            lex_next(l);
            if (!tok_is(l, '(')) {
                cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected '(' after builtin");
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            lex_next(l);
            if (!parse_call_args(p, args, argc)) {
                cblock_expr_free(e); p->err = 1; return NULL;
            }
            e->a = args[0]; e->b = args[1]; e->c = args[2];
            for (i = 0; i < argc; i++) if (!args[i]) bad = 1;
            if (!strcmp(e->name, "__cvbg")) {
                if (!e->a->type.ptr) bad = 1;
                e->type = type_of(64, 0);
            } else if (!strcmp(e->name, "__cvdg")) {
                if (e->a->type.ptr || !e->b->type.ptr) bad = 1;
            } else if (!strcmp(e->name, "__pka") || !strcmp(e->name, "__pku")) {
                if (!e->a->type.ptr || !e->b->type.ptr || e->c->type.ptr) bad = 1;
            } else if (!strcmp(e->name, "__unpka") || !strcmp(e->name, "__unpku")) {
                if (!e->a->type.ptr || e->b->type.ptr || !e->c->type.ptr) bad = 1;
            } else if (!strcmp(e->name, "__mvcrl")) {
                if (!e->a->type.ptr || !e->b->type.ptr || e->c->type.ptr) bad = 1;
            }
            if (bad) {
                cb_error(p->b, line, col, "ZA5003", "%s has an argument of the wrong type", e->name);
                p->err = 1;
            }
            return e;
        }
        CBVar *v = strmap_get(&p->b->byname, name);
        CType t;
        if (type_parse(name, &t)) {
            cb_error(p->b, line, col, "ZA5002", "type name \"%s\" is not an expression", name);
            free(name);
            lex_next(l);
            return NULL;
        }
        if (!v) {
            cb_error(p->b, line, col, "ZA5004", "undeclared variable \"%s\"", name);
            free(name);
            lex_next(l);
            return NULL;
        }
        free(name);
        e = mk_expr(E_VAR, v->type, line, col);
        e->var = v;
        e->read_before = !v->initialized;
        lex_next(l);
        return e;
    }
    if (tok_is(l, '(')) {
        lex_next(l);
        if (l->tok.kind == TK_ID) {
            CType ct;
            char *tn = xstrdup(l->tok.s);
            if (tok_id(l, "long")) {
                lex_next(l);
                if (!tok_id(l, "double")) {
                    cb_error(p->b, l->tok.line, l->tok.col, "ZA5001",
                             "expected double after long in a cast");
                    free(tn);
                    p->err = 1;
                    return NULL;
                }
                lex_next(l);
                ct = type_fp(128);
                if (tok_is(l, '*')) {
                    ct = type_ptr_fp(128);
                    lex_next(l);
                }
                free(tn);
                if (!tok_is(l, ')')) {
                    cb_error(p->b, l->tok.line, l->tok.col, "ZA5001",
                             "expected ')' after a cast type");
                    p->err = 1;
                    return NULL;
                }
                lex_next(l);
                e = parse_unary(p);
                if (!e) return NULL;
                return mk_cast(e, ct);
            }
            if (type_parse(tn, &ct)) {
                lex_next(l);
                free(tn);
                if (tok_is(l, '*')) {
                    ct = type_ptr_scalar(ct.bits, ct.unsig);
                    lex_next(l);
                    if (!tok_is(l, ')')) {
                        cb_error(p->b, l->tok.line, l->tok.col, "ZA5001",
                                 "expected ')' after a cast type");
                        p->err = 1;
                        return NULL;
                    }
                    lex_next(l);
                    e = parse_unary(p);
                    if (!e) return NULL;
                    return mk_cast(e, ct);
                }
                if (!tok_is(l, ')')) {
                    cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ')' after a cast type");
                    p->err = 1;
                    return NULL;
                }
                lex_next(l);
                e = parse_unary(p);
                if (!e) return NULL;
                return mk_cast(e, ct);
            }
            if (peek_is_star(l) && !strmap_get(&p->b->byname, tn)) {
                lex_next(l); /* move to * */
                lex_next(l); /* past * */
                ct = type_ptr_named(p->b, tn);
                free(tn);
                if (!tok_is(l, ')')) {
                    cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ')' after a cast type");
                    p->err = 1;
                    return NULL;
                }
                lex_next(l);
                e = parse_unary(p);
                if (!e) return NULL;
                return mk_cast(e, ct);
            }
            free(tn);
        }
        e = parse_cond(p);
        if (!tok_is(l, ')')) {
            cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "expected ')'");
            p->err = 1;
            return e;
        }
        lex_next(l);
        return e;
    }
    cb_error(p->b, line, col, "ZA5001", "expected an expression");
    p->err = 1;
    return NULL;
}

static CBExpr *parse_postfix(Parser *p) {
    CBExpr *e = parse_primary(p);
    while (e && tok_is(&p->l, '[')) {
        CBExpr *idx, *base, *d;
        int line = e->line, col = e->col;
        lex_next(&p->l);
        idx = parse_assignment(p);
        if (!tok_is(&p->l, ']')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ']'");
            p->err = 1;
            return e;
        }
        lex_next(&p->l);
        if (!idx) { p->err = 1; return e; }
        if (!e->type.ptr) {
            cb_error(p->b, line, col, "ZA5003", "p[i] needs a pointer on the left");
            p->err = 1;
            return e;
        }
        if (e->type.target) {
            cb_error(p->b, line, col, "ZA5002",
                     "cannot index a named DSECT pointer; use __offsetof and a byte pointer");
            p->err = 1;
            return e;
        }
        base = mk_bin(p, '+', e, idx);
        if (!base) { p->err = 1; return e; }
        d = mk_expr(E_DEREF, e->type.elem_bits ? type_of(e->type.elem_bits, e->type.elem_unsig)
                                               : type_of(8, 1), line, col);
        d->a = base;
        e = d;
    }
    while (e && (tok_is(&p->l, OP_INC) || tok_is(&p->l, OP_DEC))) {
        CBExpr *n;
        if (e->kind != E_VAR) {
            cb_error(p->b, e->line, e->col, "ZA5003", "++/-- needs a variable");
            p->err = 1;
            return e;
        }
        if (e->type.flt || e->type.vec) {
            cb_error(p->b, e->line, e->col, "ZA5003",
                     e->type.vec ? "++/-- on a vector is not implemented"
                                 : "++/-- on a floating-point value is not implemented");
            p->err = 1;
            return e;
        }
        n = mk_expr(E_INC, e->type, e->line, e->col);
        n->op = p->l.tok.op;
        n->var = e->var;
        n->num = 1; /* postfix */
        n->read_before = e->read_before;
        lex_next(&p->l);
        e = n;
    }
    return e;
}

static CBExpr *parse_unary(Parser *p) {
    Lex *l = &p->l;
    int line = l->tok.line, col = l->tok.col;
    if (tok_id(l, "__align")) {
        CBExpr *e;
        int64_t n;
        int align = 1;
        lex_next(l);
        if (!tok_is(l, '(')) {
            cb_error(p->b, line, col, "ZA5001", "__align needs a power-of-two alignment");
            p->err = 1;
            return NULL;
        }
        lex_next(l);
        if (l->tok.kind != TK_NUM || l->tok.num < 1 || l->tok.num > 4096 ||
            (l->tok.num & (l->tok.num - 1)) != 0) {
            cb_error(p->b, l->tok.line, l->tok.col, "ZA5001",
                     "__align needs a power-of-two number from 1 to 4096");
            p->err = 1;
            return NULL;
        }
        n = l->tok.num;
        align = (int)n;
        lex_next(l);
        if (!tok_is(l, ')')) {
            cb_error(p->b, l->tok.line, l->tok.col, "ZA5001", "__align needs one number");
            p->err = 1;
            return NULL;
        }
        lex_next(l);
        e = parse_unary(p);
        if (!e) return NULL;
        if (e->kind != E_STR) {
            cb_error(p->b, e->line, e->col, "ZA5001", "__align applies only to a string literal");
            p->err = 1;
            return e;
        }
        e->align = align;
        return e;
    }
    if (tok_is(l, '*')) {
        CBExpr *a, *e;
        lex_next(l);
        a = parse_unary(p);
        if (!a) return NULL;
        if (!a->type.ptr) {
            cb_error(p->b, a->line, a->col, "ZA5003", "cannot dereference a non-pointer");
            p->err = 1;
            return a;
        }
        if (a->type.target) {
            cb_error(p->b, a->line, a->col, "ZA5002",
                     "cannot dereference a named DSECT pointer; use __offsetof and a byte pointer");
            p->err = 1;
            return a;
        }
        e = mk_expr(E_DEREF, a->type.elem_flt ? type_fp(a->type.elem_bits) :
                                a->type.elem_bits ? type_of(a->type.elem_bits, a->type.elem_unsig)
                                                  : type_of(8, 1), line, col);
        e->a = a;
        return e;
    }
    if (tok_is(l, '&')) {
        CBExpr *a;
        lex_next(l);
        a = parse_unary(p);
        if (!a) return NULL;
        if (a->kind == E_DEREF) return a->a;
        if (a->kind != E_VAR) {
            cb_error(p->b, a->line, a->col, "ZA5003", "& needs a variable with storage");
            p->err = 1;
            return a;
        }
        if (a->var->kind != V_LOCAL) {
            cb_error(p->b, a->line, a->col, "ZA5003", "& needs a local variable, not a bound register");
            p->err = 1;
            return a;
        }
        {
            CBExpr *e = mk_expr(E_ADDR, type_ptr(), line, col);
            e->var = a->var;
            cblock_expr_free(a);
            return e;
        }
    }
    if (tok_is(l, OP_INC) || tok_is(l, OP_DEC)) {
        int op = l->tok.op;
        CBExpr *a, *e;
        lex_next(l);
        a = parse_unary(p);
        if (!a) return NULL;
        if (a->kind != E_VAR) {
            cb_error(p->b, a->line, a->col, "ZA5003", "++/-- needs a variable");
            p->err = 1;
            return a;
        }
        if (a->type.flt || a->type.vec) {
            cb_error(p->b, a->line, a->col, "ZA5003",
                     a->type.vec ? "++/-- on a vector is not implemented"
                                 : "++/-- on a floating-point value is not implemented");
            p->err = 1;
            return a;
        }
        e = mk_expr(E_INC, a->type, line, col);
        e->op = op;
        e->var = a->var;
        e->num = 0; /* prefix */
        e->read_before = a->read_before;
        return e;
    }
    if (tok_is(l, '+') || tok_is(l, '-') || tok_is(l, '!') || tok_is(l, '~')) {
        int op = l->tok.op;
        CBExpr *a;
        CBExpr *e;
        lex_next(l);
        a = parse_unary(p);
        if (!a) return NULL;
        e = mk_expr(E_UN, op == '!' ? type_of(32, 0) : a->type, line, col);
        e->op = op;
        e->a = a;
        return e;
    }
    return parse_postfix(p);
}

static int prec_of(int op) {
    switch (op) {
    case OP_LOR: return 1;
    case OP_LAND: return 2;
    case '*': case '/': case '%': return 10;
    case '+': case '-': return 9;
    case OP_SHL: case OP_SHR: return 8;
    case '<': case '>': case OP_LE: case OP_GE: return 7;
    case OP_EQ: case OP_NE: return 6;
    case '&': return 5;
    case '^': return 4;
    case '|': return 3;
    default: return 0;
    }
}

static CBExpr *parse_binary(Parser *p, int minprec) {
    CBExpr *a = parse_unary(p);
    if (!a) return NULL;
    for (;;) {
        int op = p->l.tok.kind == TK_OP ? p->l.tok.op : 0;
        int prec = prec_of(op);
        CBExpr *b;
        if (!prec || prec < minprec) break;
        lex_next(&p->l);
        b = parse_binary(p, prec + 1);
        if (!b) return a;
        a = mk_bin(p, op, a, b);
    }
    return a;
}

static CBExpr *parse_cond(Parser *p) {
    CBExpr *c = parse_binary(p, 1);
    if (c && tok_is(&p->l, '?')) {
        CBExpr *a, *b, *e;
        CType t;
        int line = c->line, col = c->col;
        lex_next(&p->l);
        a = parse_cond(p);
        if (!tok_is(&p->l, ':')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ':' in ?:");
            p->err = 1;
            return c;
        }
        lex_next(&p->l);
        b = parse_cond(p);
        if (!a || !b) return c;
        t = type_usual(a->type, b->type);
        e = mk_expr(E_COND, t, line, col);
        e->a = c;
        e->b = mk_cast(a, t);
        e->c = mk_cast(b, t);
        return e;
    }
    return c;
}

static CBExpr *parse_assignment(Parser *p) { return parse_cond(p); }

static CBVar *var_new(CBlock *b, const char *name, CType t, VKind k, int reg) {
    CBVar *v = xcalloc(sizeof *v);
    v->name = xstrdup(name);
    v->type = t;
    v->kind = k;
    v->reg = reg;
    v->slot = b->next_var_slot;
    b->next_var_slot += (t.vec || t.bits == 128) ? 2 : 1;
    v->initialized = k != V_COUT;
    vec_push(&b->vars, v);
    strmap_put(&b->byname, v->name, v);
    return v;
}

static int parse_type(Parser *p, CType *t) {
    if (p->l.tok.kind == TK_ID && tok_id(&p->l, "const")) {
        lex_next(&p->l);
        if (p->l.tok.kind != TK_ID) return 0;
    }
    if (p->l.tok.kind != TK_ID) return 0;
    if (za_streqi(p->l.tok.s, "uint8_t") || za_streqi(p->l.tok.s, "void") ||
        za_streqi(p->l.tok.s, "char")) {
        lex_next(&p->l);
        if (!tok_is(&p->l, '*')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5002",
                     "only pointers to uint8_t/void/char are implemented");
            p->err = 1;
            return 1; /* the caller reports no declaration */
        }
        lex_next(&p->l);
        *t = type_ptr();
        return 1;
    }
    if (tok_id(&p->l, "__vector")) {
        int vbits = 0, vunsig = 0, vfloat = 0, vbool = 0;
        lex_next(&p->l);
        if (tok_id(&p->l, "bool")) { vbool = 1; lex_next(&p->l); }
        if (tok_id(&p->l, "signed")) lex_next(&p->l);
        else if (tok_id(&p->l, "unsigned")) { vunsig = 1; lex_next(&p->l); }
        if (tok_id(&p->l, "char")) vbits = 8;
        else if (tok_id(&p->l, "short")) vbits = 16;
        else if (tok_id(&p->l, "int")) vbits = 32;
        else if (tok_id(&p->l, "long")) {
            vbits = 64;
            lex_next(&p->l);
            if (!tok_id(&p->l, "long")) {
                cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5002",
                         "vector long must be long long");
                p->err = 1;
                return 1;
            }
        } else if (tok_id(&p->l, "float")) { vbits = 32; vfloat = 1; }
        else if (tok_id(&p->l, "double")) { vbits = 64; vfloat = 1; }
        else {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5002",
                     "unsupported vector element type");
            p->err = 1;
            return 1;
        }
        lex_next(&p->l);
        *t = type_vec(vbits, vunsig, vfloat, vbool);
        return 1;
    }
    if (tok_id(&p->l, "long")) {
        lex_next(&p->l);
        if (!tok_id(&p->l, "double")) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5002", "only long double is implemented");
            p->err = 1;
            return 1;
        }
        lex_next(&p->l);
        if (tok_is(&p->l, '*')) {
            lex_next(&p->l);
            *t = type_ptr_fp(128);
        } else *t = type_fp(128);
        return 1;
    }
    if (peek_is_star(&p->l)) {
        char *name = xstrdup(p->l.tok.s);
        CType base;
        if (type_parse(name, &base) && !base.ptr) {
            *t = base.flt ? type_ptr_fp(base.bits) : type_ptr_scalar(base.bits, base.unsig);
        } else {
            *t = type_ptr_named(p->b, name);
        }
        lex_next(&p->l);
        lex_next(&p->l);
        free(name);
        return 1;
    }
    if (!type_parse(p->l.tok.s, t)) return 0;
    lex_next(&p->l);
    return 1;
}

static void parse_decl(Parser *p, CBStmtVec *st, CType t) {
    char *name;
    CBVar *v;
    CBExpr *init = NULL;
    int line = p->l.tok.line;
    if (p->l.tok.kind != TK_ID) {
        cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected a variable name");
        p->err = 1;
        return;
    }
    name = xstrdup(p->l.tok.s);
    lex_next(&p->l);
    if (strmap_get(&p->b->byname, name)) {
        cb_error(p->b, line, 1, "ZA5004", "\"%s\" is already defined", name);
        free(name);
        p->err = 1;
        return;
    }
    v = var_new(p->b, name, t, V_LOCAL, -1);
    free(name);
    if (tok_is(&p->l, '=')) {
        lex_next(&p->l);
        init = parse_assignment(p);
        if (init && init->kind == E_STR) {
            v->lit = xmalloc(init->slen + 1);
            memcpy(v->lit, init->str, init->slen);
            v->lit[init->slen] = 0;
            v->lit_len = init->slen;
        }
    }
    if (!tok_is(&p->l, ';')) {
        cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ';' after declaration");
        p->err = 1;
    } else lex_next(&p->l);
    {
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = S_DECL;
        s.var = v;
        s.expr = init ? mk_cast(init, v->type) : NULL;
        s.line = line;
        vec_push(st, s);
    }
}

static int is_assign_op(int op) {
    return op == '=' || op == OP_ADDEQ || op == OP_SUBEQ || op == OP_MULEQ ||
           op == OP_DIVEQ || op == OP_MODEQ || op == OP_ANDEQ || op == OP_OREQ ||
           op == OP_XOREQ || op == OP_SHLEQ || op == OP_SHREQ;
}

static int assign_base(int op) {
    switch (op) {
    case OP_ADDEQ: return '+';
    case OP_SUBEQ: return '-';
    case OP_MULEQ: return '*';
    case OP_DIVEQ: return '/';
    case OP_MODEQ: return '%';
    case OP_ANDEQ: return '&';
    case OP_OREQ: return '|';
    case OP_XOREQ: return '^';
    case OP_SHLEQ: return OP_SHL;
    case OP_SHREQ: return OP_SHR;
    default: return op;
    }
}

static int const_eval(CBExpr *e, int64_t *out) {
    int64_t a, b;
    if (!e) return 0;
    switch (e->kind) {
    case E_NUM: if (e->type.flt) return 0; *out = e->num; return 1;
    case E_UN:
        if (!const_eval(e->a, &a)) return 0;
        if (e->op == '+') *out = a;
        else if (e->op == '-') *out = (int64_t)(0 - (uint64_t)a);
        else if (e->op == '~') *out = ~a;
        else if (e->op == '!') *out = !a;
        else return 0;
        return 1;
    case E_BIN:
        if (!const_eval(e->a, &a) || !const_eval(e->b, &b)) return 0;
        switch (e->op) {
        case '+': *out = (int64_t)((uint64_t)a + (uint64_t)b); return 1;
        case '-': *out = (int64_t)((uint64_t)a - (uint64_t)b); return 1;
        case '*': *out = (int64_t)((uint64_t)a * (uint64_t)b); return 1;
        case '/': if (!b) return 0; *out = a / b; return 1;
        case '%': if (!b) return 0; *out = a % b; return 1;
        case '&': *out = a & b; return 1;
        case '|': *out = a | b; return 1;
        case '^': *out = a ^ b; return 1;
        case OP_SHL: *out = (int64_t)((uint64_t)a << (b & 63)); return 1;
        case OP_SHR: *out = a >> (b & 63); return 1;
        default: return 0;
        }
    default:
        return 0;
    }
}

static void parse_stmt(Parser *p, CBStmtVec *st) {
    CType t;
    CBExpr *e;
    int line = p->l.tok.line;
    if (p->l.tok.kind == TK_ID &&
        (tok_id(&p->l, "__memcpy") || tok_id(&p->l, "__memmove") || tok_id(&p->l, "__memset"))) {
        int kind = tok_id(&p->l, "__memcpy") ? S_MEMCPY
                 : tok_id(&p->l, "__memmove") ? S_MEMMOVE : S_MEMSET;
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = (CBSKind)kind;
        s.line = line;
        lex_next(&p->l);
        if (!tok_is(&p->l, '(')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '(' after intrinsic");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        s.expr = parse_assignment(p);
        if (!tok_is(&p->l, ',')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ',' in intrinsic");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        s.step = parse_assignment(p);
        if (!tok_is(&p->l, ',')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ',' in intrinsic");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        s.cond = parse_assignment(p);
        if (!tok_is(&p->l, ')')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ')' in intrinsic");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (!tok_is(&p->l, ';')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ';' after intrinsic");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (!s.expr || !s.step || !s.cond) { p->err = 1; return; }
        if (!s.expr->type.ptr) {
            cb_error(p->b, line, 1, "ZA5003", "the first argument must be a pointer");
            p->err = 1;
            return;
        }
        if (kind != S_MEMSET && !s.step->type.ptr) {
            cb_error(p->b, line, 1, "ZA5003", "the source argument must be a pointer");
            p->err = 1;
            return;
        }
        if (kind == S_MEMSET && s.step->type.ptr) {
            cb_error(p->b, line, 1, "ZA5003", "the fill value must be an integer");
            p->err = 1;
            return;
        }
        if (s.cond->type.ptr) {
            cb_error(p->b, line, 1, "ZA5003", "the length must be an integer");
            p->err = 1;
            return;
        }
        vec_push(st, s);
        return;
    }
    if (p->switch_depth > 0 && p->l.tok.kind == TK_ID && tok_id(&p->l, "case")) {
        CBStmt s;
        CBExpr *v;
        int64_t cv;
        lex_next(&p->l);
        v = parse_assignment(p);
        if (!v || !const_eval(v, &cv)) {
            cb_error(p->b, line, 1, "ZA5003", "case value must be an integer constant");
            p->err = 1;
            return;
        }
        if (!tok_is(&p->l, ':')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ':' after case");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        memset(&s, 0, sizeof s);
        s.kind = S_CASE;
        s.num = cv;
        s.label = xasprintf("$CB_C%d", ++p->b->label_seq);
        s.line = line;
        vec_push(st, s);
        return;
    }
    if (p->switch_depth > 0 && p->l.tok.kind == TK_ID && tok_id(&p->l, "default")) {
        CBStmt s;
        if (p->switch_default[p->switch_depth - 1]) {
            cb_error(p->b, line, p->l.tok.col, "ZA5004", "duplicate default in switch");
            p->err = 1;
            return;
        }
        p->switch_default[p->switch_depth - 1] = 1;
        lex_next(&p->l);
        if (!tok_is(&p->l, ':')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ':' after default");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        memset(&s, 0, sizeof s);
        s.kind = S_DEFAULT;
        s.label = xasprintf("$CB_D%d", ++p->b->label_seq);
        s.line = line;
        vec_push(st, s);
        return;
    }
    if (p->l.tok.kind == TK_ID && after_tok_colon(&p->l)) {
        char *name = xstrdup(p->l.tok.s);
        char *lab;
        CBStmt s;
        if (strmap_get(&p->b->labels, name)) {
            cb_error(p->b, line, p->l.tok.col, "ZA5004", "label \"%s\" is already defined", name);
            free(name);
            p->err = 1;
            return;
        }
        lab = xasprintf("$CB_L%d", ++p->b->label_seq);
        strmap_put(&p->b->labels, name, lab);
        free(name);
        lex_next(&p->l); /* name */
        lex_next(&p->l); /* : */
        memset(&s, 0, sizeof s);
        s.kind = S_LABEL;
        s.label = xstrdup(lab);
        s.line = line;
        vec_push(st, s);
        if (!tok_is(&p->l, '}') && p->l.tok.kind != TK_EOF) parse_stmt(p, st);
        return;
    }
    if (tok_is(&p->l, ';')) { lex_next(&p->l); return; }
    if (tok_is(&p->l, '{')) {
        lex_next(&p->l);
        while (!tok_is(&p->l, '}') && p->l.tok.kind != TK_EOF && !p->err) parse_stmt(p, st);
        if (tok_is(&p->l, '}')) lex_next(&p->l);
        else cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '}'");
        return;
    }
    if (p->l.tok.kind == TK_ID && tok_id(&p->l, "switch")) {
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = S_SWITCH;
        s.line = line;
        lex_next(&p->l);
        if (!tok_is(&p->l, '(')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '(' after switch");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        s.cond = parse_assignment(p);
        if (!tok_is(&p->l, ')')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ')' after switch expression");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (!tok_is(&p->l, '{')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '{' after switch");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (p->switch_depth >= 16) {
            cb_error(p->b, line, 1, "ZA5008", "switch nested too deeply");
            p->err = 1;
            return;
        }
        p->switch_depth++;
        p->switch_default[p->switch_depth - 1] = 0;
        while (!tok_is(&p->l, '}') && p->l.tok.kind != TK_EOF && !p->err)
            parse_stmt(p, &s.body);
        p->switch_depth--;
        if (!tok_is(&p->l, '}')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '}' after switch");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        vec_push(st, s);
        return;
    }
    if (p->l.tok.kind == TK_ID && tok_id(&p->l, "if")) {
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = S_IF;
        s.line = line;
        lex_next(&p->l);
        if (!tok_is(&p->l, '(')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '(' after if");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        s.cond = parse_assignment(p);
        if (!tok_is(&p->l, ')')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ')' after if condition");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        parse_stmt(p, &s.body);
        if (p->l.tok.kind == TK_ID && tok_id(&p->l, "else")) {
            lex_next(&p->l);
            parse_stmt(p, &s.other);
        }
        vec_push(st, s);
        return;
    }
    if (p->l.tok.kind == TK_ID && tok_id(&p->l, "while")) {
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = S_WHILE;
        s.line = line;
        lex_next(&p->l);
        if (!tok_is(&p->l, '(')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '(' after while");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        s.cond = parse_assignment(p);
        if (!tok_is(&p->l, ')')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ')' after while condition");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        p->loop_depth++;
        parse_stmt(p, &s.body);
        p->loop_depth--;
        vec_push(st, s);
        return;
    }
    if (p->l.tok.kind == TK_ID && tok_id(&p->l, "do")) {
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = S_DO;
        s.line = line;
        lex_next(&p->l);
        p->loop_depth++;
        parse_stmt(p, &s.body);
        p->loop_depth--;
        if (!(p->l.tok.kind == TK_ID && tok_id(&p->l, "while"))) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected while after do body");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (!tok_is(&p->l, '(')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '(' after while");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        s.cond = parse_assignment(p);
        if (!tok_is(&p->l, ')')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ')' after do-while condition");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (!tok_is(&p->l, ';')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ';' after do-while");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        vec_push(st, s);
        return;
    }
    if (p->l.tok.kind == TK_ID && tok_id(&p->l, "for")) {
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = S_FOR;
        s.line = line;
        lex_next(&p->l);
        if (!tok_is(&p->l, '(')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected '(' after for");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (tok_is(&p->l, ';')) lex_next(&p->l);
        else parse_stmt(p, &s.init);
        if (!tok_is(&p->l, ';')) {
            s.cond = parse_assignment(p);
            if (!tok_is(&p->l, ';')) {
                cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ';' in for");
                p->err = 1;
                return;
            }
        }
        lex_next(&p->l);
        if (!tok_is(&p->l, ')')) s.step = parse_assignment(p);
        if (!tok_is(&p->l, ')')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ')' in for");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        p->loop_depth++;
        parse_stmt(p, &s.body);
        p->loop_depth--;
        vec_push(st, s);
        return;
    }
    if (p->l.tok.kind == TK_ID && tok_id(&p->l, "goto")) {
        CBStmt s;
        lex_next(&p->l);
        if (p->l.tok.kind != TK_ID) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "goto needs a label");
            p->err = 1;
            return;
        }
        memset(&s, 0, sizeof s);
        s.kind = S_GOTO;
        s.label = xstrdup(p->l.tok.s);
        s.line = line;
        lex_next(&p->l);
        if (!tok_is(&p->l, ';')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ';' after goto");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        vec_push(st, s);
        return;
    }
    if (p->l.tok.kind == TK_ID && (tok_id(&p->l, "break") || tok_id(&p->l, "continue"))) {
        int brk = tok_id(&p->l, "break");
        if (p->loop_depth == 0 && (!brk || p->switch_depth == 0)) {
            cb_error(p->b, line, p->l.tok.col, "ZA5008", "%s is only valid in a loop%s",
                     brk ? "break" : "continue", brk ? " or switch" : "");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        if (!tok_is(&p->l, ';')) {
            cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ';'");
            p->err = 1;
            return;
        }
        lex_next(&p->l);
        {
            CBStmt s;
            memset(&s, 0, sizeof s);
            s.kind = brk ? S_BREAK : S_CONTINUE;
            s.line = line;
            vec_push(st, s);
        }
        return;
    }
    if (p->l.tok.kind == TK_ID &&
        (tok_id(&p->l, "return") || tok_id(&p->l, "switch") || tok_id(&p->l, "goto"))) {
        cb_error(p->b, line, p->l.tok.col, "ZA5008", "\"%s\" is not implemented", p->l.tok.s);
        p->err = 1;
        return;
    }
    if (parse_type(p, &t)) {
        parse_decl(p, st, t);
        return;
    }
    e = parse_assignment(p);
    if (!e) { p->err = 1; return; }
    if (p->l.tok.kind == TK_OP && is_assign_op(p->l.tok.op)) {
        int op = p->l.tok.op;
        CBVar *v;
        CBExpr *rhs;
        CBStmt s;
        if (e->kind != E_VAR && e->kind != E_DEREF) {
            cb_error(p->b, e->line, e->col, "ZA5003",
                     "the left side of an assignment must be a variable or *p");
            p->err = 1;
            return;
        }
        v = e->kind == E_VAR ? e->var : NULL;
        if (v) {
            if (op != '=' && e->read_before) {
                cb_error(p->b, e->line, e->col, "ZA5003", "\"%s\" is read before it is assigned",
                         v->name);
                p->err = 1;
                return;
            }
            if (v->kind == V_CIN) {
                cb_error(p->b, e->line, e->col, "ZA5003",
                         "\"%s\" is a cin variable and cannot be assigned", v->name);
                p->err = 1;
                return;
            }
        }
        lex_next(&p->l);
        rhs = parse_assignment(p);
        if (!rhs) { p->err = 1; return; }
        if (v) {
            free(v->lit);
            v->lit = NULL;
            v->lit_len = 0;
            if (op == '=' && rhs->kind == E_STR) {
                v->lit = xmalloc(rhs->slen + 1);
                memcpy(v->lit, rhs->str, rhs->slen);
                v->lit[rhs->slen] = 0;
                v->lit_len = rhs->slen;
            }
        }
        memset(&s, 0, sizeof s);
        s.kind = S_ASSIGN;
        s.var = v;
        s.lval = v ? NULL : e;
        s.op = assign_base(op);
        if (v && v->type.ptr) {
            if (op != '=' && s.op != '+' && s.op != '-') {
                cb_error(p->b, line, 1, "ZA5003", "a pointer supports only =, += and -=");
                p->err = 1;
                return;
            }
            s.expr = op == '=' ? mk_cast(rhs, v->type) : rhs;
        } else {
            s.expr = mk_cast(rhs, e->type);
        }
        s.line = line;
        vec_push(st, s);
        if (v) v->initialized = 1;
    } else {
        CBStmt s;
        memset(&s, 0, sizeof s);
        s.kind = S_EXPR;
        s.expr = e;
        s.line = line;
        vec_push(st, s);
    }
    if (!tok_is(&p->l, ';')) {
        cb_error(p->b, p->l.tok.line, p->l.tok.col, "ZA5001", "expected ';' after statement");
        p->err = 1;
    } else lex_next(&p->l);
}

static int check_gotos(CBlock *b, CBStmtVec *v) {
    size_t i;
    for (i = 0; i < v->n; i++) {
        CBStmt *s = &v->v[i];
        if (s->kind == S_GOTO && !strmap_get(&b->labels, s->label)) {
            cb_error(b, s->line, 1, "ZA5004", "goto target \"%s\" is not defined", s->label);
            return 0;
        }
        if (!check_gotos(b, &s->body) || !check_gotos(b, &s->other) || !check_gotos(b, &s->init))
            return 0;
    }
    return 1;
}

/* ---- metadata parsing --------------------------------------------------- */

static int pragma_split(const char *line, char **word, char **rest) {
    const char *p = line;
    size_t n = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '#') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (za_strncmpi(p, "pragma", 6) != 0) return 0;
    p += 6;
    while (*p == ' ' || *p == '\t') p++;
    while (p[n] && p[n] != ' ' && p[n] != '\t') n++;
    if (!n) return 0;
    *word = za_strlower_own(xstrndup(p, n));
    *rest = trim(p + n);
    return 1;
}

static void add_scratch(CBlock *b, int reg, int line) {
    int i;
    for (i = 0; i < b->nscratch; i++)
        if (b->scratch[i] == reg) {
            cb_error(b, line, 1, "ZA5005", "register %%r%d is listed twice in cscratch", reg);
            return;
        }
    if (b->nscratch >= 16) {
        cb_error(b, line, 1, "ZA5005", "too many cscratch registers");
        return;
    }
    b->scratch[b->nscratch++] = reg;
}

static void add_fscratch(CBlock *b, int reg, int line) {
    int i;
    for (i = 0; i < b->nfscratch; i++)
        if (b->fscratch[i] == reg) {
            cb_error(b, line, 1, "ZA5005", "register %%f%d is listed twice in cscratch", reg);
            return;
        }
    if (b->nfscratch >= 16) {
        cb_error(b, line, 1, "ZA5005", "too many cscratch floating-point registers");
        return;
    }
    b->fscratch[b->nfscratch++] = reg;
}

static void add_vscratch(CBlock *b, int reg, int line) {
    int i;
    for (i = 0; i < b->nvscratch; i++)
        if (b->vscratch[i] == reg) {
            cb_error(b, line, 1, "ZA5005", "register %%v%d is listed twice in cscratch", reg);
            return;
        }
    if (b->nvscratch >= 32) {
        cb_error(b, line, 1, "ZA5005", "too many cscratch vector registers");
        return;
    }
    b->vscratch[b->nvscratch++] = reg;
}

static int binding_parse(CBlock *b, const char *rest, int line, VKind kind) {
    char *s = xstrdup(rest);
    char *p = s;
    for (;;) {
        char *comma = strchr(p, ',');
        char *decl = NULL, *eq, *vn = NULL, *rn = NULL;
        CType t;
        int reg, i, bad = 0, isf = 0;
        if (comma) *comma = 0;
        decl = trim(p);
        if (*decl) {
            size_t n;
            char *name_start;
            eq = strchr(decl, '=');
            if (!eq) {
                cb_error(b, line, 1, "ZA5005", "expected \"type name=%%rN\" in cblock metadata");
                bad = 1;
            } else {
                *eq = 0;
                decl = trim(decl);
                n = strlen(decl);
                while (n > 0 && (decl[n - 1] == ' ' || decl[n - 1] == '\t')) decl[--n] = 0;
                while (n > 0 && is_id_char((unsigned char)decl[n - 1])) n--;
                name_start = decl + n;
                if (!*name_start) {
                    cb_error(b, line, 1, "ZA5005", "expected a variable name in \"%s\"", decl);
                    bad = 1;
                } else {
                    char *type_text = xstrndup(decl, n);
                    vn = xstrdup(name_start);
                    rn = trim(eq + 1);
                    if (!type_text_parse(b, type_text, &t)) {
                        cb_error(b, line, 1, "ZA5002", "unsupported cblock type \"%s\"", type_text);
                        bad = 1;
                    } else if (t.vec ? !vreg_parse(rn, &reg) :
                               t.flt ? !freg_parse(rn, &reg) : !reg_parse(rn, &reg)) {
                        cb_error(b, line, 1, "ZA5005",
                                 t.vec ? "\"%s\" is not a %%v0-%%v31 register binding for a vector"
                                 : t.flt ? "\"%s\" is not a %%f0-%%f15 register binding for a float or double"
                                       : "\"%s\" is not a %%r0-%%r15 register binding", rn);
                        bad = 1;
                    } else {
                        isf = t.flt;
                        for (i = 0; i < (int)b->vars.n; i++)
                            if (b->vars.v[i]->reg == reg && b->vars.v[i]->type.vec == t.vec &&
                                b->vars.v[i]->type.flt == isf) {
                                cb_error(b, line, 1, "ZA5005", "register %s%d is bound more than once",
                                         t.vec ? "%%v" : isf ? "%%f" : "%%r", reg);
                                bad = 1;
                                break;
                            }
                        if (!bad && strmap_get(&b->byname, vn)) {
                            cb_error(b, line, 1, "ZA5004", "\"%s\" is already defined", vn);
                            bad = 1;
                        }
                        if (!bad) var_new(b, vn, t, kind, reg);
                    }
                    free(type_text);
                }
            }
        }
        free(vn);
        free(rn);
        free(decl);
        if (bad) { free(s); return 0; }
        if (!comma) break;
        p = comma + 1;
    }
    free(s);
    return 1;
}

static int ctemp_parse(CBlock *b, const char *rest, int line) {
    char *s = xstrdup(rest), *a, *c, *d;
    char *comma;
    int reg;
    long size;
    char *end = NULL;
    a = s;
    comma = strchr(a, ',');
    if (!comma) goto bad;
    *comma = 0;
    c = comma + 1;
    comma = strchr(c, ',');
    if (!comma) goto bad;
    *comma = 0;
    d = comma + 1;
    {
        char *ta = trim(a), *tc = trim(c), *td = trim(d);
        int ok;
        if (!reg_parse(ta, &reg)) { free(ta); free(tc); free(td); goto bad; }
        if (reg == 0 || reg == 1) {
            cb_error(b, line, 1, "ZA5005",
                     "ctemp cannot use %%r0 or %%r1 as its base register (internal scratch)");
            free(ta); free(tc); free(td); free(s); return 0;
        }
        errno = 0;
        size = strtol(tc, &end, 10);
        if (errno || !end || *end || size <= 0 || size % 8 != 0) {
            free(ta); free(tc); free(td); goto bad;
        }
        if (b->temp_reg >= 0) {
            cb_error(b, line, 1, "ZA5005", "ctemp is given more than once");
            free(ta); free(tc); free(td); free(s); return 0;
        }
        b->temp_reg = reg;
        b->temp_size = size;
        b->temp_down = za_streqi(td, "down");
        ok = b->temp_down || za_streqi(td, "up");
        free(ta); free(tc); free(td);
        if (!ok) goto bad;
    }
    free(s);
    return 1;
bad:
    cb_error(b, line, 1, "ZA5005", "ctemp takes \"%%rN,SIZE,up|down\" with SIZE a multiple of 8");
    free(s);
    return 0;
}

static int has_scratch(const CBlock *b, int reg) {
    int i;
    for (i = 0; i < b->nscratch; i++) if (b->scratch[i] == reg) return 1;
    return 0;
}

static void cblock_expr_free(CBExpr *e) {
    if (!e) return;
    cblock_expr_free(e->a);
    cblock_expr_free(e->b);
    cblock_expr_free(e->c);
    cblock_expr_free(e->d);
    cblock_expr_free(e->arg5);
    cblock_expr_free(e->arg6);
    free(e->name);
    free(e->str);
    free(e->sym);
    free(e->field);
    free(e);
}

static void cblock_stmt_vec_free(CBStmtVec *v);

static void cblock_stmt_free(CBStmt *s) {
    cblock_expr_free(s->expr);
    cblock_expr_free(s->cond);
    cblock_expr_free(s->step);
    free(s->label);
    cblock_stmt_vec_free(&s->body);
    cblock_stmt_vec_free(&s->other);
    cblock_stmt_vec_free(&s->init);
}

static void cblock_stmt_vec_free(CBStmtVec *v) {
    size_t i;
    for (i = 0; i < v->n; i++) cblock_stmt_free(&v->v[i]);
    free(v->v);
    memset(v, 0, sizeof *v);
}

/* ---- code generation ---------------------------------------------------- */

typedef struct {
    CBlock *b;
    int base_slots;
    int slot_top;
    int max_slot;
    int ctrl_depth;
    int ctrl_loop[16];
    const char *ctrl_break[16];
    const char *ctrl_continue[16];
} Cg;

static int alloc_slots(Cg *cg, int n) {
    int s = cg->slot_top;
    cg->slot_top += n;
    if (cg->slot_top > cg->max_slot) cg->max_slot = cg->slot_top;
    return s;
}

static int alloc_slot(Cg *cg) { return alloc_slots(cg, 1); }

static long slot_off(CBlock *b, int slot) {
    if (b->temp_down) return b->temp_size - (long)(slot + 1) * 8;
    return (long)slot * 8;
}

static const char *csrc_for(const CBlock *b, int line) {
    size_t i;
    for (i = 0; i < b->body_n; i++)
        if (b->body[i].line == line) return b->body[i].text;
    return NULL;
}

static void asm_raw(CBlock *b, int line, char *s) {
    SrcLine sl;
    memset(&sl, 0, sizeof sl);
    sl.file = b->file;
    sl.line = line;
    sl.text = s;
    {
        const char *cs = csrc_for(b, line);
        if (cs) sl.orig = xstrdup(cs);
    }
    vec_push(b->out, sl);
}

static void asm_line(CBlock *b, int line, const char *fmt, ...) {
    va_list ap;
    char *s, *p;
    va_start(ap, fmt);
    s = xvasprintf(fmt, ap);
    va_end(ap);
    if (s[0] == '*') {
        asm_raw(b, line, s);
        return;
    }
    p = xasprintf("         %s", s);
    free(s);
    asm_raw(b, line, p);
}

static void emit_label(CBlock *b, int line, const char *label) {
    asm_raw(b, line, xasprintf("%s DS 0H", label));
}

static void load_slot(CBlock *b, int line, int slot, CType t, int reg) {
    long off = slot_off(b, slot);
    if (t.bits == 64) asm_line(b, line, "LG %%r%d,%ld(%%r%d)", reg, off, b->temp_reg);
    else if (t.bits == 16)
        asm_line(b, line, t.unsig ? "LLGH %%r%d,%ld(%%r%d)" : "LGH %%r%d,%ld(%%r%d)",
                 reg, off, b->temp_reg);
    else if (t.unsig) asm_line(b, line, "LLGF %%r%d,%ld(%%r%d)", reg, off, b->temp_reg);
    else asm_line(b, line, "LGF %%r%d,%ld(%%r%d)", reg, off, b->temp_reg);
}

static void store_slot(CBlock *b, int line, int slot, CType t, int reg) {
    long off = slot_off(b, slot);
    if (t.bits == 64) asm_line(b, line, "STG %%r%d,%ld(%%r%d)", reg, off, b->temp_reg);
    else if (t.bits == 16) asm_line(b, line, "STH %%r%d,%ld(%%r%d)", reg, off, b->temp_reg);
    else asm_line(b, line, "ST %%r%d,%ld(%%r%d)", reg, off, b->temp_reg);
}

static void load_fp_slot(CBlock *b, int line, int slot, CType t, int fpr) {
    long off = slot_off(b, slot);
    if (t.bits == 128) {
        asm_line(b, line, "LD %%f%d,%ld(%%r%d)", fpr, off, b->temp_reg);
        asm_line(b, line, "LD %%f%d,%ld(%%r%d)", fpr + 2, slot_off(b, slot + 1), b->temp_reg);
        return;
    }
    asm_line(b, line, t.bits == 32 ? "LE %%f%d,%ld(%%r%d)" : "LD %%f%d,%ld(%%r%d)",
             fpr, off, b->temp_reg);
}

static void store_fp_slot(CBlock *b, int line, int slot, CType t, int fpr) {
    long off = slot_off(b, slot);
    if (t.bits == 128) {
        asm_line(b, line, "STD %%f%d,%ld(%%r%d)", fpr, off, b->temp_reg);
        asm_line(b, line, "STD %%f%d,%ld(%%r%d)", fpr + 2, slot_off(b, slot + 1), b->temp_reg);
        return;
    }
    asm_line(b, line, t.bits == 32 ? "STE %%f%d,%ld(%%r%d)" : "STD %%f%d,%ld(%%r%d)",
             fpr, off, b->temp_reg);
}

static void load_vec_slot(CBlock *b, int line, int slot, int vr) {
    asm_line(b, line, "VL %%v%d,%ld(%%r%d)", vr, slot_off(b, slot), b->temp_reg);
}

static void store_vec_slot(CBlock *b, int line, int slot, int vr) {
    asm_line(b, line, "VST %%v%d,%ld(%%r%d)", vr, slot_off(b, slot), b->temp_reg);
}

static void store_fp_mem(CBlock *b, int line, CType t, int fpr, int addr) {
    if (t.bits == 128) {
        asm_line(b, line, "STD %%f%d,0(%%r%d)", fpr, addr);
        asm_line(b, line, "STD %%f%d,8(%%r%d)", fpr + 2, addr);
        return;
    }
    asm_line(b, line, t.bits == 32 ? "STE %%f%d,0(%%r%d)" : "STD %%f%d,0(%%r%d)", fpr, addr);
}

static void copy_slot(CBlock *b, int line, int dst, int src, CType t) {
    if (t.vec || t.bits == 128) {
        asm_line(b, line, "LG %%r0,%ld(%%r%d)", slot_off(b, src), b->temp_reg);
        asm_line(b, line, "STG %%r0,%ld(%%r%d)", slot_off(b, dst), b->temp_reg);
        asm_line(b, line, "LG %%r0,%ld(%%r%d)", slot_off(b, src + 1), b->temp_reg);
        asm_line(b, line, "STG %%r0,%ld(%%r%d)", slot_off(b, dst + 1), b->temp_reg);
        return;
    }
    load_slot(b, line, src, t, 0);
    store_slot(b, line, dst, t, 0);
}

static void load_imm(CBlock *b, int line, int64_t v, CType t, int reg) {
    uint64_t u = (uint64_t)v;
    if (t.bits == 32) {
        if (t.unsig && u > INT32_MAX) asm_line(b, line, "LLILF %%r%d,%llu", reg, (unsigned long long)u);
        else asm_line(b, line, "LGFI %%r%d,%lld", reg, (long long)v);
    } else if (v >= INT32_MIN && v <= INT32_MAX) {
        asm_line(b, line, "LGFI %%r%d,%lld", reg, (long long)v);
    } else if (!t.unsig && v >= 0 && u <= UINT32_MAX) {
        asm_line(b, line, "LLILF %%r%d,%llu", reg, (unsigned long long)u);
    } else {
        asm_line(b, line, "IIHF %%r%d,%lu", reg, (unsigned long)(u >> 32));
        asm_line(b, line, "IILF %%r%d,%lu", reg, (unsigned long)(u & 0xFFFFFFFFu));
    }
}

static void emit_compare(CBlock *b, int line, CType t, int op, int r1, int r2, int slot) {
    int mask;
    const char *lab = xasprintf("$CB%d", ++b->label_no);
    switch (op) {
    case '<': mask = 4; break;
    case '>': mask = 2; break;
    case OP_LE: mask = 12; break;
    case OP_GE: mask = 10; break;
    case OP_EQ: mask = 8; break;
    default: mask = 14; break; /* != */
    }
    if (t.bits == 64) asm_line(b, line, t.unsig ? "CLGR %%r%d,%%r%d" : "CGR %%r%d,%%r%d", r1, r2);
    else asm_line(b, line, t.unsig ? "CLR %%r%d,%%r%d" : "CR %%r%d,%%r%d", r1, r2);
    asm_line(b, line, "LHI %%r0,1");
    asm_line(b, line, "BRCL %d,%s", mask, lab);
    asm_line(b, line, "LHI %%r0,0");
    emit_label(b, line, lab);
    store_slot(b, line, slot, type_of(32, 0), 0);
    free((char *)lab);
}

static int find_div_pair(CBlock *b, int *even, int *divisor) {
    int i, j, k;
    for (i = 0; i < b->nscratch; i++) {
        int e = b->scratch[i];
        if (e % 2) continue;
        for (j = 0; j < b->nscratch; j++)
            if (b->scratch[j] == e + 1) {
                for (k = 0; k < b->nscratch; k++)
                    if (b->scratch[k] != e && b->scratch[k] != e + 1) {
                        *even = e;
                        *divisor = b->scratch[k];
                        return 1;
                    }
            }
    }
    return 0;
}

static char *new_label(CBlock *b) { return xasprintf("$CB%d", ++b->label_no); }

static char *literal_add(CBlock *b, const char *data, size_t len, int align) {
    CBString *l;
    if (b->nlits == b->lits_cap) {
        b->lits_cap = b->lits_cap ? b->lits_cap * 2 : 4;
        b->lits = xrealloc(b->lits, b->lits_cap * sizeof *b->lits);
    }
    l = &b->lits[b->nlits++];
    l->data = xmalloc(len + 1);
    memcpy(l->data, data, len);
    l->data[len] = 0;
    l->len = len;
    l->align = align;
    l->label = new_label(b);
    l->dc = NULL;
    return l->label;
}

static char *literal_add_dc(CBlock *b, const char *dc, int align) {
    CBString *l;
    if (b->nlits == b->lits_cap) {
        b->lits_cap = b->lits_cap ? b->lits_cap * 2 : 4;
        b->lits = xrealloc(b->lits, b->lits_cap * sizeof *b->lits);
    }
    l = &b->lits[b->nlits++];
    l->data = NULL;
    l->len = 0;
    l->align = align;
    l->label = new_label(b);
    l->dc = xstrdup(dc);
    return l->label;
}

static void emit_literal_data(CBlock *b, int line) {
    size_t i;
    for (i = 0; i < b->nlits; i++) {
        CBString *l = &b->lits[i];
        size_t at = 0, total = l->len + 1;
        if (l->align == 2) asm_line(b, line, "DS 0H");
        else if (l->align == 4) asm_line(b, line, "DS 0F");
        else if (l->align == 8) asm_line(b, line, "DS 0D");
        else if (l->align > 8) asm_line(b, line, "CNOP 0,%d", l->align);
        emit_label(b, line, l->label);
        if (l->dc) {
            asm_line(b, line, "%s", l->dc);
            continue;
        }
        while (at < total) {
            Buf h = {0};
            size_t k = total - at > 16 ? 16 : total - at;
            size_t j;
            for (j = 0; j < k; j++) {
                unsigned char c = at + j == l->len ? 0 : (unsigned char)l->data[at + j];
                buf_printf(&h, "%02X", c);
            }
            asm_line(b, line, "DC X'%s'", buf_cstr(&h));
            free(h.p);
            at += k;
        }
    }
    emit_label(b, line, b->lit_end);
}

static void emit_test(CBlock *b, int line, CType t) {
    asm_line(b, line, t.bits == 64 ? "LTGR %%r0,%%r0" : "LTR %%r0,%%r0");
}

static int pick_fpr(CBlock *b, int used1, int used2);
static int pick_fpr_pair(CBlock *b, int used1, int used2);
static int pick_vr(CBlock *b, int used1, int used2);
static int pick_vr4(CBlock *b, int u1, int u2, int u3, int u4);
static int pick_addr(CBlock *b, int used1, int used2);
static int pick_addr3(CBlock *b, int used1, int used2, int used3);
static int pick_addr4(CBlock *b, int used1, int used2, int used3, int used4);
static int pick_addr5(CBlock *b, int u1, int u2, int u3, int u4, int u5);
static int pick_pair(CBlock *b, int used1, int used2, int *second);
static int eval(Cg *cg, CBExpr *e, int *slotp);

static int load_fp_expr(Cg *cg, CBExpr *e, int fpr) {
    int slot;
    if (!eval(cg, e, &slot)) return 0;
    load_fp_slot(cg->b, e->line, slot, e->type, fpr);
    return 1;
}

/* Load an already evaluated value from a temporary slot into an FPR,
 * converting integer values to the requested floating-point width. */
static void load_fp_from_slot(CBlock *b, int line, int slot, CType src, int fpr, int bits) {
    if (src.flt) {
        load_fp_slot(b, line, slot, src, fpr);
        if (src.bits == bits) return;
        if (bits == 128) {
            asm_line(b, line, src.bits == 32 ? "LXEBR %%f%d,%%f%d" : "LXDBR %%f%d,%%f%d", fpr, fpr);
        } else if (src.bits == 128) {
            asm_line(b, line, bits == 64 ? "LDXBR %%f%d,%%f%d" : "LEXBR %%f%d,%%f%d", fpr, fpr);
        } else {
            asm_line(b, line, bits == 64 ? "LDEBR %%f%d,%%f%d" : "LEDBR %%f%d,%%f%d", fpr, fpr);
        }
        return;
    }
    load_slot(b, line, slot, src, 0);
    if (bits == 128) {
        if (src.bits == 64) {
            if (src.unsig) asm_line(b, line, "CXLGBR %%f%d,0,%%r0,0", fpr);
            else asm_line(b, line, "CXGBR %%f%d,%%r0", fpr);
        } else {
            if (src.unsig) asm_line(b, line, "CXLFBR %%f%d,0,%%r0,0", fpr);
            else asm_line(b, line, "CXFBR %%f%d,%%r0", fpr);
        }
    } else if (bits == 64) {
        if (src.unsig) asm_line(b, line, "CELGBR %%f%d,0,%%r0,0", fpr);
        else asm_line(b, line, "CDGBR %%f%d,%%r0", fpr);
    } else {
        if (src.unsig) asm_line(b, line, "CELFBR %%f%d,0,%%r0,0", fpr);
        else asm_line(b, line, "CEFBR %%f%d,%%r0", fpr);
    }
}

static void dfp_convert(CBlock *b, int line, int fpr, int from, int to) {
    if (from == to) return;
    if (from == 32 && to == 64) {
        asm_line(b, line, "LDETR %%f%d,%%f%d,8", fpr, fpr);
    } else if (from == 64 && to == 128) {
        asm_line(b, line, "LXDTR %%f%d,%%f%d,0", fpr, fpr);
    } else if (from == 32 && to == 128) {
        asm_line(b, line, "LDETR %%f%d,%%f%d,8", fpr, fpr);
        asm_line(b, line, "LXDTR %%f%d,%%f%d,0", fpr, fpr);
    } else if (from == 64 && to == 32) {
        asm_line(b, line, "LEDTR %%f%d,0,%%f%d,0", fpr, fpr);
    } else if (from == 128 && to == 64) {
        asm_line(b, line, "LDXTR %%f%d,0,%%f%d,0", fpr, fpr);
    } else if (from == 128 && to == 32) {
        asm_line(b, line, "LDXTR %%f%d,0,%%f%d,0", fpr, fpr);
        asm_line(b, line, "LEDTR %%f%d,0,%%f%d,0", fpr, fpr);
    }
}

static void load_dfp_from_slot(CBlock *b, int line, int slot, CType src, int fpr, int bits) {
    if (src.dfp) {
        load_fp_slot(b, line, slot, src, fpr);
        dfp_convert(b, line, fpr, src.bits, bits);
        return;
    }
    load_slot(b, line, slot, src, 0);
    if (bits == 128) {
        if (src.bits == 64) {
            if (src.unsig) asm_line(b, line, "CXLGTR %%f%d,0,%%r0,0", fpr);
            else asm_line(b, line, "CXGTR %%f%d,%%r0", fpr);
        } else {
            if (src.unsig) asm_line(b, line, "CXLFTR %%f%d,0,%%r0,0", fpr);
            else asm_line(b, line, "CXFTR %%f%d,0,%%r0,0", fpr);
        }
    } else {
        if (src.bits == 64) {
            if (src.unsig) asm_line(b, line, "CDLGTR %%f%d,0,%%r0,0", fpr);
            else asm_line(b, line, "CDGTR %%f%d,%%r0", fpr);
        } else {
            if (src.unsig) asm_line(b, line, "CDLFTR %%f%d,0,%%r0,0", fpr);
            else asm_line(b, line, "CDFTR %%f%d,0,%%r0,0", fpr);
        }
    }
}

static int emit_value_test(Cg *cg, CBExpr *e) {
    CBlock *b = cg->b;
    int slot, fp, zero;
    if (!e->type.flt) {
        if (!eval(cg, e, &slot)) return 0;
        load_slot(b, e->line, slot, e->type, 0);
        emit_test(b, e->line, e->type);
        return 1;
    }
    if (e->type.dfp) {
        int abits = e->type.bits == 32 ? 64 : e->type.bits;
        fp = abits == 128 ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
        if (fp < 0) {
            cb_error(b, e->line, e->col, "ZA5005",
                     "a decimal condition needs a cscratch FPR");
            return 0;
        }
        if (!eval(cg, e, &slot)) return 0;
        load_dfp_from_slot(b, e->line, slot, e->type, fp, abits);
        asm_line(b, e->line, abits == 128 ? "LTXTR %%f%d,%%f%d" : "LTDTR %%f%d,%%f%d", fp, fp);
        return 1;
    }
    if (e->type.bits == 128) {
        fp = pick_fpr_pair(b, -1, -1);
        zero = pick_fpr_pair(b, fp, -1);
        if (fp < 0 || zero < 0) {
            cb_error(b, e->line, e->col, "ZA5005",
                     "a long double condition needs two cscratch FPR pairs");
            return 0;
        }
        if (!eval(cg, e, &slot)) return 0;
        load_fp_slot(b, e->line, slot, e->type, fp);
        asm_line(b, e->line, "LZXR %%f%d", zero);
        asm_line(b, e->line, "CXBR %%f%d,%%f%d", fp, zero);
        return 1;
    }
    fp = pick_fpr(b, -1, -1);
    zero = pick_fpr(b, fp, -1);
    if (fp < 0 || zero < 0) {
        cb_error(b, e->line, e->col, "ZA5005", "a floating condition needs two cscratch FPRs");
        return 0;
    }
    if (!eval(cg, e, &slot)) return 0;
    load_fp_slot(b, e->line, slot, e->type, fp);
    asm_line(b, e->line, e->type.bits == 64 ? "LZDR %%f%d" : "LZER %%f%d", zero);
    asm_line(b, e->line, e->type.bits == 64 ? "CDBR %%f%d,%%f%d" : "CEBR %%f%d,%%f%d", fp, zero);
    return 1;
}

/* Check message-security-assist extension 5, facility bit 57. On MVS the
 * z/OS facility list at real location 200 has it; on ELF use STFLE. */
static int emit_msa5_check(Cg *cg, int line, const char *absent, int *tmpout) {
    CBlock *b = cg->b;
    int tmp = alloc_slots(cg, 4);
    if (b->elf) {
        asm_line(b, line, "LA %%r1,%ld(%%r%d)", slot_off(b, tmp), b->temp_reg);
        asm_line(b, line, "LGHI %%r0,3");
        asm_line(b, line, "STFLE 0(%%r1)");
        asm_line(b, line, "LG %%r0,0(%%r1)");
    } else {
        asm_line(b, line, "LG %%r0,200(%%r0)");
    }
    asm_line(b, line, "NILF %%r0,64");
    asm_line(b, line, "BRC 8,%s", absent);
    if (tmpout) *tmpout = tmp;
    return 1;
}

static int eval_float_bin(Cg *cg, CBExpr *e, int *slotp) {
    CBlock *b = cg->b;
    CType ot = type_usual(e->a->type, e->b->type);
    int fa, fb, sa, sb, res;
    int isx = ot.bits == 128;
    if (ot.dfp) {
        int abits = ot.bits == 32 ? 64 : ot.bits;
        int ax = abits == 128;
        if ((e->a->type.flt && !e->a->type.dfp) ||
            (e->b->type.flt && !e->b->type.dfp)) {
            cb_error(b, e->line, e->col, "ZA5002",
                     "BFP and DFP operands cannot be mixed");
            return 0;
        }
        if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sb)) return 0;
        fa = ax ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
        fb = ax ? pick_fpr_pair(b, fa, -1) : pick_fpr(b, fa, -1);
        if (fa < 0 || fb < 0) {
            cb_error(b, e->line, e->col, "ZA5005",
                     ax ? "a DFP128 expression needs two cscratch FPR pairs"
                        : "a decimal expression needs two cscratch FPRs");
            return 0;
        }
        load_dfp_from_slot(b, e->line, sa, e->a->type, fa, abits);
        load_dfp_from_slot(b, e->line, sb, e->b->type, fb, abits);
        if (e->op == OP_EQ || e->op == OP_NE || e->op == '<' || e->op == '>' ||
            e->op == OP_LE || e->op == OP_GE) {
            char *aval = new_label(b);
            int mask;
            asm_line(b, e->line, ax ? "CXTR %%f%d,%%f%d" : "CDTR %%f%d,%%f%d", fa, fb);
            switch (e->op) {
            case '<': mask = 4; break;
            case '>': mask = 2; break;
            case OP_LE: mask = 12; break;
            case OP_GE: mask = 10; break;
            case OP_EQ: mask = 8; break;
            default: mask = 14; break;
            }
            asm_line(b, e->line, "LHI %%r0,1");
            asm_line(b, e->line, "BRCL %d,%s", mask, aval);
            asm_line(b, e->line, "LHI %%r0,0");
            emit_label(b, e->line, aval);
            free(aval);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        switch (e->op) {
        case '+': asm_line(b, e->line, ax ? "AXTR %%f%d,%%f%d,%%f%d" : "ADTR %%f%d,%%f%d,%%f%d", fa, fa, fb); break;
        case '-': asm_line(b, e->line, ax ? "SXTR %%f%d,%%f%d,%%f%d" : "SDTR %%f%d,%%f%d,%%f%d", fa, fa, fb); break;
        case '*': asm_line(b, e->line, ax ? "MXTR %%f%d,%%f%d,%%f%d" : "MDTR %%f%d,%%f%d,%%f%d", fa, fa, fb); break;
        case '/': asm_line(b, e->line, ax ? "DXTR %%f%d,%%f%d,%%f%d" : "DDTR %%f%d,%%f%d,%%f%d", fa, fa, fb); break;
        default:
            cb_error(b, e->line, e->col, "ZA5002",
                     "decimal operator is not implemented (only + - * / and comparisons)");
            return 0;
        }
        if (ot.bits == 32) dfp_convert(b, e->line, fa, 64, 32);
        res = alloc_slots(cg, ot.bits == 128 ? 2 : 1);
        store_fp_slot(b, e->line, res, ot, fa);
        *slotp = res;
        return 1;
    }
    if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sb)) return 0;
    fa = isx ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
    fb = isx ? pick_fpr_pair(b, fa, -1) : pick_fpr(b, fa, -1);
    if (fa < 0 || fb < 0) {
        cb_error(b, e->line, e->col, "ZA5005",
                 isx ? "a long double expression needs two cscratch FPR pairs"
                     : "a floating expression needs two cscratch FPRs");
        return 0;
    }
    load_fp_from_slot(b, e->line, sa, e->a->type, fa, ot.bits);
    load_fp_from_slot(b, e->line, sb, e->b->type, fb, ot.bits);
    if (e->op == OP_EQ || e->op == OP_NE || e->op == '<' || e->op == '>' ||
        e->op == OP_LE || e->op == OP_GE) {
        const char *aval = xasprintf("$CB%d", ++b->label_no);
        int mask;
        if (isx) asm_line(b, e->line, "CXBR %%f%d,%%f%d", fa, fb);
        else asm_line(b, e->line, ot.bits == 64 ? "CDBR %%f%d,%%f%d" : "CEBR %%f%d,%%f%d", fa, fb);
        switch (e->op) {
        case '<': mask = 4; break;
        case '>': mask = 2; break;
        case OP_LE: mask = 12; break;
        case OP_GE: mask = 10; break;
        case OP_EQ: mask = 8; break;
        default: mask = 14; break;
        }
        asm_line(b, e->line, "LHI %%r0,1");
        asm_line(b, e->line, "BRCL %d,%s", mask, aval);
        asm_line(b, e->line, "LHI %%r0,0");
        emit_label(b, e->line, aval);
        free((char *)aval);
        res = alloc_slot(cg);
        store_slot(b, e->line, res, type_of(32, 0), 0);
        *slotp = res;
        return 1;
    }
    switch (e->op) {
    case '+': asm_line(b, e->line, isx ? "AXBR %%f%d,%%f%d" : ot.bits == 64 ? "ADBR %%f%d,%%f%d" : "AEBR %%f%d,%%f%d", fa, fb); break;
    case '-': asm_line(b, e->line, isx ? "SXBR %%f%d,%%f%d" : ot.bits == 64 ? "SDBR %%f%d,%%f%d" : "SEBR %%f%d,%%f%d", fa, fb); break;
    case '*': asm_line(b, e->line, isx ? "MXBR %%f%d,%%f%d" : ot.bits == 64 ? "MDBR %%f%d,%%f%d" : "MEBR %%f%d,%%f%d", fa, fb); break;
    case '/': asm_line(b, e->line, isx ? "DXBR %%f%d,%%f%d" : ot.bits == 64 ? "DDBR %%f%d,%%f%d" : "DEBR %%f%d,%%f%d", fa, fb); break;
    default:
        cb_error(b, e->line, e->col, "ZA5002",
                 "floating-point operator is not implemented (only + - * / and comparisons)");
        return 0;
    }
    res = alloc_slots(cg, isx ? 2 : 1);
    store_fp_slot(b, e->line, res, ot, fa);
    *slotp = res;
    return 1;
}

static int eval_bin(Cg *cg, CBExpr *e, int *slotp) {
    CBlock *b = cg->b;
    int s0, s1, res;
    CType t = e->type;
    CType ot = type_usual(e->a->type, e->b->type);
    if (e->type.vec || e->a->type.vec || e->b->type.vec) {
        cb_error(b, e->line, e->col, "ZA5002", "vector operators are not implemented");
        return 0;
    }
    if (e->op == OP_LAND || e->op == OP_LOR) {
        char *lab = new_label(b), *end = new_label(b);
        res = alloc_slot(cg);
        if (!emit_value_test(cg, e->a)) { free(lab); free(end); return 0; }
        asm_line(b, e->line, "BRCL %d,%s", e->op == OP_LAND ? 8 : 7, lab);
        if (!emit_value_test(cg, e->b)) { free(lab); free(end); return 0; }
        asm_line(b, e->line, "BRCL 8,%s", lab);
        asm_line(b, e->line, "LHI %%r0,1");
        asm_line(b, e->line, "BRCL 15,%s", end);
        emit_label(b, e->line, lab);
        asm_line(b, e->line, "LHI %%r0,0");
        emit_label(b, e->line, end);
        store_slot(b, e->line, res, type_of(32, 0), 0);
        free(lab); free(end);
        *slotp = res;
        return 1;
    }
    if (ot.flt && !e->a->type.ptr && !e->b->type.ptr) return eval_float_bin(cg, e, slotp);
    if (!eval(cg, e->a, &s0) || !eval(cg, e->b, &s1)) return 0;
    if (e->op == OP_EQ || e->op == OP_NE || e->op == '<' || e->op == '>' || e->op == OP_LE ||
        e->op == OP_GE) {
        load_slot(b, e->line, s0, e->a->type, 0);
        load_slot(b, e->line, s1, e->b->type, 1);
        res = alloc_slot(cg);
        emit_compare(b, e->line, (e->a->type.ptr || e->b->type.ptr) ? type_ptr() : ot, e->op, 0, 1, res);
        *slotp = res;
        return 1;
    }
    if (e->a->type.ptr || e->b->type.ptr) {
        CType pt = e->a->type.ptr ? e->a->type : e->b->type;
        int shift = pt.elem_bits == 128 ? 4 : pt.elem_bits == 64 ? 3 :
                    pt.elem_bits == 32 ? 2 : pt.elem_bits == 16 ? 1 : 0;
        load_slot(b, e->line, s0, e->a->type, 0);
        load_slot(b, e->line, s1, e->b->type, 1);
        if (e->a->type.ptr && e->b->type.ptr) {
            asm_line(b, e->line, "SGR %%r0,%%r1");
            if (shift) asm_line(b, e->line, "SRLG %%r0,%%r0,%d", shift);
        } else {
            if (shift) asm_line(b, e->line, "SLLG %%r1,%%r1,%d", shift);
            asm_line(b, e->line, e->op == '-' ? "SGR %%r0,%%r1" : "AGR %%r0,%%r1");
        }
        res = alloc_slot(cg);
        store_slot(b, e->line, res, e->type, 0);
        *slotp = res;
        return 1;
    }
    load_slot(b, e->line, s0, e->a->type, 0);
    load_slot(b, e->line, s1, e->b->type, 1);
    if (e->op == '/' || e->op == '%') {
        int even, divisor;
        if (!find_div_pair(b, &even, &divisor)) {
            cb_error(b, e->line, e->col, "ZA5005",
                     "division needs an even/odd scratch pair plus a third scratch register");
            return 0;
        }
        if (ot.unsig) asm_line(b, e->line, "LGHI %%r%d,0", even);
        load_slot(b, e->line, s0, e->a->type, even + 1);
        load_slot(b, e->line, s1, e->b->type, divisor);
        asm_line(b, e->line, ot.unsig ? "DLGR %%r%d,%%r%d" : "DSGR %%r%d,%%r%d", even, divisor);
        res = alloc_slot(cg);
        /* DSGR: remainder=R1, quotient=R1+1.  DLGR keeps the 128-bit
         * quotient in R1/R1+1; for values that fit 64 bits the low half is
         * R1+1 and the remainder is not used. */
        store_slot(b, e->line, res, t, e->op == '%' ? even : even + 1);
        *slotp = res;
        return 1;
    }
    switch (e->op) {
    case '+': asm_line(b, e->line, t.bits == 64 ? "AGR %%r0,%%r1" : "AR %%r0,%%r1"); break;
    case '-': asm_line(b, e->line, t.bits == 64 ? "SGR %%r0,%%r1" : "SR %%r0,%%r1"); break;
    case '*': asm_line(b, e->line, t.bits == 64 ? "MSGR %%r0,%%r1" : "MSR %%r0,%%r1"); break;
    case '&': asm_line(b, e->line, t.bits == 64 ? "NGR %%r0,%%r1" : "NR %%r0,%%r1"); break;
    case '|': asm_line(b, e->line, t.bits == 64 ? "OGR %%r0,%%r1" : "OR %%r0,%%r1"); break;
    case '^': asm_line(b, e->line, t.bits == 64 ? "XGR %%r0,%%r1" : "XR %%r0,%%r1"); break;
    case OP_SHL: asm_line(b, e->line, t.bits == 64 ? "SLLG %%r0,%%r0,0(%%r1)" : "SLL %%r0,0(%%r1)"); break;
    case OP_SHR:
        asm_line(b, e->line, t.unsig ? (t.bits == 64 ? "SRLG %%r0,%%r0,0(%%r1)" : "SRL %%r0,0(%%r1)")
                                     : (t.bits == 64 ? "SRAG %%r0,%%r0,0(%%r1)" : "SRA %%r0,0(%%r1)"));
        break;
    default:
        cb_error(b, e->line, e->col, "ZA5002", "unsupported cblock operator");
        return 0;
    }
    res = alloc_slot(cg);
    store_slot(b, e->line, res, t, 0);
    *slotp = res;
    return 1;
}

static int eval(Cg *cg, CBExpr *e, int *slotp) {
    CBlock *b = cg->b;
    int s, s0, res;
    switch (e->kind) {
    case E_NUM:
        if (e->type.dfp) {
            char dc[96];
            char *label;
            int fpr = e->type.bits == 128 ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
            int addr;
            if (fpr < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         e->type.bits == 128 ? "a decimal literal needs a cscratch FPR pair"
                                             : "a decimal literal needs a cscratch FPR");
                return 0;
            }
            addr = pick_addr(b, 0, 0);
            if (addr < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "a decimal literal needs an address register");
                return 0;
            }
            snprintf(dc, sizeof dc, "DC %s'%s'",
                     e->type.bits == 32 ? "ED" : e->type.bits == 64 ? "DD" : "LD",
                     e->str ? e->str : "0");
            label = literal_add_dc(b, dc, e->type.bits == 128 ? 16 : e->type.bits == 64 ? 8 : 4);
            asm_line(b, e->line, "LARL %%r%d,%s", addr, label);
            if (e->type.bits == 32) asm_line(b, e->line, "LE %%f%d,0(%%r%d)", fpr, addr);
            else {
                asm_line(b, e->line, "LD %%f%d,0(%%r%d)", fpr, addr);
                if (e->type.bits == 128) asm_line(b, e->line, "LD %%f%d,8(%%r%d)", fpr + 2, addr);
            }
            res = alloc_slots(cg, e->type.bits == 128 ? 2 : 1);
            store_fp_slot(b, e->line, res, e->type, fpr);
            *slotp = res;
            return 1;
        }
        if (e->type.flt) {
            uint64_t u;
            if (e->type.bits == 32) {
                float f = (float)e->fnum;
                uint32_t w;
                memcpy(&w, &f, sizeof w);
                u = w;
            } else {
                memcpy(&u, &e->fnum, sizeof u);
            }
            load_imm(b, e->line, (int64_t)u, type_of(e->type.bits, 1), 0);
        } else load_imm(b, e->line, e->num, e->type, 0);
        s = alloc_slot(cg);
        store_slot(b, e->line, s, e->type, 0);
        *slotp = s;
        return 1;
    case E_STR: {
        char *label = literal_add(b, e->str, e->slen, e->align);
        asm_line(b, e->line, "LARL %%r1,%s", label);
        res = alloc_slot(cg);
        store_slot(b, e->line, res, type_ptr(), 1);
        *slotp = res;
        return 1;
    }
    case E_VAR:
        if (e->read_before) {
            cb_error(b, e->line, e->col, "ZA5003", "\"%s\" is read before it is assigned",
                     e->var->name);
            return 0;
        }
        s = alloc_slots(cg, (e->var->type.vec || e->var->type.bits == 128) ? 2 : 1);
        copy_slot(b, e->line, s, e->var->slot, e->var->type);
        *slotp = s;
        return 1;
    case E_DEREF: {
        int sa, addr;
        if (!eval(cg, e->a, &sa)) return 0;
        addr = pick_addr(b, 0, 0);
        if (addr < 0) {
            cb_error(b, e->line, e->col, "ZA5005", "a dereference needs a nonzero scratch register");
            return 0;
        }
        load_slot(b, e->line, sa, type_ptr(), addr);
        if (e->type.bits == 128) {
            int fp = pick_fpr_pair(b, -1, -1);
            if (fp < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "a long double dereference needs a cscratch FPR pair");
                return 0;
            }
            asm_line(b, e->line, "LD %%f%d,0(%%r%d)", fp, addr);
            asm_line(b, e->line, "LD %%f%d,8(%%r%d)", fp + 2, addr);
            res = alloc_slots(cg, 2);
            store_fp_slot(b, e->line, res, e->type, fp);
        } else {
            if (e->type.bits == 64) asm_line(b, e->line, "LG %%r0,0(%%r%d)", addr);
            else if (e->type.bits == 32) asm_line(b, e->line, "L %%r0,0(%%r%d)", addr);
            else if (e->type.bits == 16)
                asm_line(b, e->line, e->type.unsig ? "LLGH %%r0,0(%%r%d)" : "LGH %%r0,0(%%r%d)", addr);
            else asm_line(b, e->line, "LLGC %%r0,0(%%r%d)", addr);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, e->type, 0);
        }
        *slotp = res;
        return 1;
    }
    case E_ADDR: {
        int addr;
        if (!e->var || e->var->kind != V_LOCAL) {
            cb_error(b, e->line, e->col, "ZA5003", "& needs a local variable with storage");
            return 0;
        }
        addr = pick_addr(b, 0, 0);
        if (addr < 0) {
            cb_error(b, e->line, e->col, "ZA5005", "& needs a nonzero scratch register");
            return 0;
        }
        asm_line(b, e->line, "LA %%r%d,%ld(%%r%d)", addr, slot_off(b, e->var->slot), b->temp_reg);
        res = alloc_slot(cg);
        store_slot(b, e->line, res, type_ptr(), addr);
        *slotp = res;
        return 1;
    }
    case E_CAST:
        if (e->type.vec || e->a->type.vec) {
            cb_error(b, e->line, e->col, "ZA5002", "vector conversions are not implemented");
            return 0;
        }
        if (e->type.dfp || e->a->type.dfp) {
            if (e->type.dfp) {
                int abits = e->type.bits == 32 ? 64 : e->type.bits;
                int fp = abits == 128 ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
                if (fp < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             abits == 128 ? "a DFP128 conversion needs a cscratch FPR pair"
                                          : "a decimal conversion needs a cscratch FPR");
                    return 0;
                }
                if (e->a->type.flt && !e->a->type.dfp) {
                    cb_error(b, e->line, e->col, "ZA5002",
                             "BFP to DFP conversion is not implemented");
                    return 0;
                }
                if (!eval(cg, e->a, &s0)) return 0;
                load_dfp_from_slot(b, e->line, s0, e->a->type, fp, abits);
                if (e->type.bits == 32) dfp_convert(b, e->line, fp, 64, 32);
                res = alloc_slots(cg, e->type.bits == 128 ? 2 : 1);
                store_fp_slot(b, e->line, res, e->type, fp);
                *slotp = res;
                return 1;
            }
            if (e->a->type.flt && !e->a->type.dfp) {
                cb_error(b, e->line, e->col, "ZA5002",
                         "DFP to BFP conversion is not implemented");
                return 0;
            }
            if (e->a->type.dfp) {
                int abits = e->a->type.bits == 32 ? 64 : e->a->type.bits;
                int fp = abits == 128 ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
                if (fp < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             abits == 128 ? "a DFP128 conversion needs a cscratch FPR pair"
                                          : "a decimal conversion needs a cscratch FPR");
                    return 0;
                }
                if (!eval(cg, e->a, &s0)) return 0;
                load_dfp_from_slot(b, e->line, s0, e->a->type, fp, abits);
                if (e->type.bits == 64) {
                    if (e->type.unsig) asm_line(b, e->line, abits == 128 ? "CLGXTR %%r0,9,%%f%d,0" :
                                                                          "CLGDTR %%r0,9,%%f%d,0", fp);
                    else asm_line(b, e->line, abits == 128 ? "CGXTR %%r0,9,%%f%d" :
                                                              "CGDTR %%r0,9,%%f%d", fp);
                } else {
                    if (e->type.unsig) asm_line(b, e->line, abits == 128 ? "CLFXTR %%r0,9,%%f%d,0" :
                                                                          "CLFDTR %%r0,9,%%f%d,0", fp);
                    else asm_line(b, e->line, abits == 128 ? "CFXTR %%r0,9,%%f%d,0" :
                                                              "CFDTR %%r0,9,%%f%d,0", fp);
                }
                res = alloc_slot(cg);
                store_slot(b, e->line, res, e->type, 0);
                *slotp = res;
                return 1;
            }
        }
        if (e->type.flt) {
            int fp = e->type.bits == 128 ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
            if (fp < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         e->type.bits == 128 ? "a long double conversion needs a cscratch FPR pair"
                                             : "a float conversion needs a cscratch FPR");
                return 0;
            }
            if (!eval(cg, e->a, &s0)) return 0;
            load_fp_from_slot(b, e->line, s0, e->a->type, fp, e->type.bits);
            res = alloc_slots(cg, e->type.bits == 128 ? 2 : 1);
            store_fp_slot(b, e->line, res, e->type, fp);
            *slotp = res;
            return 1;
        }
        if (e->a->type.flt) {
            int srcbits = e->a->type.bits;
            int fp = srcbits == 128 ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
            if (fp < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         srcbits == 128 ? "a long double conversion needs a cscratch FPR pair"
                                        : "a float conversion needs a cscratch FPR");
                return 0;
            }
            if (!eval(cg, e->a, &s0)) return 0;
            load_fp_slot(b, e->line, s0, e->a->type, fp);
            if (srcbits == 128) {
                if (e->type.bits == 64) {
                    if (e->type.unsig) asm_line(b, e->line, "CLGXBR %%r0,5,%%f%d,0", fp);
                    else asm_line(b, e->line, "CGXBR %%r0,5,%%f%d", fp);
                } else {
                    if (e->type.unsig) asm_line(b, e->line, "CLFXBR %%r0,5,%%f%d,0", fp);
                    else asm_line(b, e->line, "CFXBR %%r0,5,%%f%d", fp);
                }
            } else if (e->type.bits == 64) {
                if (e->type.unsig)
                    asm_line(b, e->line, srcbits == 64 ? "CLGDBR %%r0,5,%%f%d,0"
                                                       : "CLGEBR %%r0,5,%%f%d,0", fp);
                else asm_line(b, e->line, srcbits == 64 ? "CGDBR %%r0,5,%%f%d"
                                                        : "CGEBR %%r0,5,%%f%d", fp);
            } else {
                if (e->type.unsig)
                    asm_line(b, e->line, srcbits == 64 ? "CLFDBR %%r0,5,%%f%d,0"
                                                       : "CLFEBR %%r0,5,%%f%d,0", fp);
                else asm_line(b, e->line, srcbits == 64 ? "CFDBR %%r0,5,%%f%d"
                                                        : "CFEBR %%r0,5,%%f%d", fp);
            }
            res = alloc_slot(cg);
            store_slot(b, e->line, res, e->type, 0);
            *slotp = res;
            return 1;
        }
        if (!eval(cg, e->a, &s0)) return 0;
        load_slot(b, e->line, s0, e->a->type, 0);
        if (e->type.bits == 64 && e->a->type.bits == 32)
            asm_line(b, e->line, e->a->type.unsig ? "LLGFR %%r0,%%r0" : "LGFR %%r0,%%r0");
        res = alloc_slot(cg);
        store_slot(b, e->line, res, e->type, 0);
        *slotp = res;
        return 1;
    case E_UN:
        if (e->a->type.vec) {
            cb_error(b, e->line, e->col, "ZA5002", "vector unary operators are not implemented");
            return 0;
        }
        if (e->a->type.flt) {
            int isx = e->a->type.bits == 128;
            if (e->a->type.dfp) {
                int abits = e->a->type.bits == 32 ? 64 : e->a->type.bits;
                int fp = abits == 128 ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
                if (fp < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "a decimal unary operator needs a cscratch FPR");
                    return 0;
                }
                if (!eval(cg, e->a, &s0)) return 0;
                load_dfp_from_slot(b, e->line, s0, e->a->type, fp, abits);
                if (e->op == '!') {
                    char *lab = new_label(b);
                    if (abits == 128) asm_line(b, e->line, "LTXTR %%f%d,%%f%d", fp, fp);
                    else asm_line(b, e->line, "LTDTR %%f%d,%%f%d", fp, fp);
                    asm_line(b, e->line, "LHI %%r0,1");
                    asm_line(b, e->line, "BRCL 8,%s", lab);
                    asm_line(b, e->line, "LHI %%r0,0");
                    emit_label(b, e->line, lab);
                    free(lab);
                } else if (e->op == '-') {
                    asm_line(b, e->line, "LCDFR %%f%d,%%f%d", fp, fp);
                    if (e->a->type.bits == 32) dfp_convert(b, e->line, fp, 64, 32);
                } else if (e->op != '+') {
                    cb_error(b, e->line, e->col, "ZA5002",
                             "decimal unary operator is not implemented");
                    return 0;
                }
                res = alloc_slots(cg, e->a->type.bits == 128 ? 2 : 1);
                store_fp_slot(b, e->line, res, e->a->type, fp);
                *slotp = res;
                return 1;
            }
            int fp = isx ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
            if (fp < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         isx ? "a long double unary operator needs a cscratch FPR pair"
                             : "a floating unary operator needs a cscratch FPR");
                return 0;
            }
            if (e->op == '!') {
                int zero = isx ? pick_fpr_pair(b, fp, -1) : pick_fpr(b, fp, -1);
                const char *lab = xasprintf("$CB%d", ++b->label_no);
                if (zero < 0) {
                    free((char *)lab);
                    cb_error(b, e->line, e->col, "ZA5005",
                             isx ? "a long double condition needs two cscratch FPR pairs"
                                 : "a floating condition needs two cscratch FPRs");
                    return 0;
                }
                if (!eval(cg, e->a, &s0)) { free((char *)lab); return 0; }
                load_fp_slot(b, e->line, s0, e->a->type, fp);
                if (isx) {
                    asm_line(b, e->line, "LZXR %%f%d", zero);
                    asm_line(b, e->line, "CXBR %%f%d,%%f%d", fp, zero);
                } else {
                    asm_line(b, e->line, e->a->type.bits == 64 ? "LZDR %%f%d" : "LZER %%f%d", zero);
                    asm_line(b, e->line, e->a->type.bits == 64 ? "CDBR %%f%d,%%f%d" : "CEBR %%f%d,%%f%d", fp, zero);
                }
                asm_line(b, e->line, "LHI %%r0,1");
                asm_line(b, e->line, "BRCL 8,%s", lab);
                asm_line(b, e->line, "LHI %%r0,0");
                emit_label(b, e->line, lab);
                free((char *)lab);
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (e->op == '-' || e->op == '+') {
                if (!eval(cg, e->a, &s0)) return 0;
                load_fp_slot(b, e->line, s0, e->a->type, fp);
                if (e->op == '-')
                    asm_line(b, e->line, isx ? "LCXBR %%f%d,%%f%d" :
                             e->a->type.bits == 64 ? "LCDBR %%f%d,%%f%d" :
                                                     "LCEBR %%f%d,%%f%d", fp, fp);
                res = alloc_slots(cg, isx ? 2 : 1);
                store_fp_slot(b, e->line, res, e->type, fp);
                *slotp = res;
                return 1;
            }
            cb_error(b, e->line, e->col, "ZA5002", "floating unary operator is not implemented");
            return 0;
        }
        if (!eval(cg, e->a, &s0)) return 0;
        load_slot(b, e->line, s0, e->a->type, 0);
        if (e->op == '!') {
            const char *lab = xasprintf("$CB%d", ++b->label_no);
            asm_line(b, e->line, e->a->type.bits == 64 ? "LTGR %%r0,%%r0" : "LTR %%r0,%%r0");
            asm_line(b, e->line, "LHI %%r0,1");
            asm_line(b, e->line, "BRCL 8,%s", lab);
            asm_line(b, e->line, "LHI %%r0,0");
            emit_label(b, e->line, lab);
            free((char *)lab);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (e->op == '-') asm_line(b, e->line, e->a->type.bits == 64 ? "LNGR %%r0,%%r0" : "LNR %%r0,%%r0");
        else if (e->op == '~') {
            asm_line(b, e->line, e->a->type.bits == 64 ? "LGHI %%r1,-1" : "LHI %%r1,-1");
            asm_line(b, e->line, e->a->type.bits == 64 ? "XGR %%r0,%%r1" : "XR %%r0,%%r1");
        }
        res = alloc_slot(cg);
        store_slot(b, e->line, res, e->type, 0);
        *slotp = res;
        return 1;
    case E_COND: {
        char *els = new_label(b), *end = new_label(b);
        res = alloc_slots(cg, e->type.bits == 128 ? 2 : 1);
        if (!emit_value_test(cg, e->a)) { free(els); free(end); return 0; }
        asm_line(b, e->line, "BRCL 8,%s", els);
        if (!eval(cg, e->b, &s0)) { free(els); free(end); return 0; }
        copy_slot(b, e->line, res, s0, e->type);
        asm_line(b, e->line, "BRCL 15,%s", end);
        emit_label(b, e->line, els);
        if (!eval(cg, e->c, &s0)) { free(els); free(end); return 0; }
        copy_slot(b, e->line, res, s0, e->type);
        emit_label(b, e->line, end);
        free(els); free(end);
        *slotp = res;
        return 1;
    }
    case E_INC:
        if (e->var->kind == V_CIN) {
            cb_error(b, e->line, e->col, "ZA5003", "\"%s\" is a cin variable", e->var->name);
            return 0;
        }
        if (e->read_before) {
            cb_error(b, e->line, e->col, "ZA5003", "\"%s\" is read before it is assigned",
                     e->var->name);
            return 0;
        }
        s = alloc_slot(cg);
        load_slot(b, e->line, e->var->slot, e->var->type, 0);
        if (e->num) {
            /* postfix: keep the old value in r1 */
            asm_line(b, e->line, e->var->type.bits == 64 ? "LGR %%r1,%%r0" : "LR %%r1,%%r0");
        }
        asm_line(b, e->line, e->var->type.bits == 64
                                ? (e->op == OP_INC ? "AGHI %%r0,1" : "AGHI %%r0,-1")
                                : (e->op == OP_INC ? "AHI %%r0,1" : "AHI %%r0,-1"));
        store_slot(b, e->line, e->var->slot, e->var->type, 0);
        if (e->num) store_slot(b, e->line, s, e->var->type, 1);
        else store_slot(b, e->line, s, e->var->type, 0);
        *slotp = s;
        return 1;
    case E_CALL: {
        int sa, sp, sl = -1, sc = -1, sd = -1;
        if (!strcmp(e->name, "__sizeof") || !strcmp(e->name, "__offsetof")) {
            if (e->field)
                asm_line(b, e->line, "LGFI %%r0,offsetof(%s,%s)", e->sym, e->field);
            else
                asm_line(b, e->line, "LGFI %%r0,sizeof(%s)", e->sym);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(64, 1), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__stfle")) {
            int addr;
            if (!eval(cg, e->a, &sa)) return 0;
            addr = pick_addr(b, 0, 0);
            if (addr < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "__stfle needs a nonzero address register");
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), addr);
            asm_line(b, e->line, "STFLE 0(%%r%d)", addr);
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__facility")) {
            int base, count, bitr, shift;
            int tmp;
            char *loop = new_label(b), *done = new_label(b);
            char *zero = new_label(b), *store = new_label(b);
            if (!eval(cg, e->a, &sa)) { free(loop); free(done); free(zero); free(store); return 0; }
            base = pick_addr(b, 0, 0);
            count = pick_addr(b, base, 0);
            bitr = pick_addr3(b, base, count, 0);
            shift = pick_addr4(b, base, count, bitr, 0);
            if (base < 0 || count < 0 || bitr < 0 || shift < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__facility needs three scratch registers");
                free(loop); free(done); free(zero); free(store);
                return 0;
            }
            tmp = alloc_slots(cg, 4);
            load_slot(b, e->line, sa, e->a->type, bitr);
            asm_line(b, e->line, "CGHI %%r%d,255", bitr);
            asm_line(b, e->line, "BRCL 2,%s", zero);
            asm_line(b, e->line, "LGR %%r0,%%r%d", bitr);
            asm_line(b, e->line, "SRL %%r0,6");
            asm_line(b, e->line, "AHI %%r0,1");
            asm_line(b, e->line, "LGR %%r%d,%%r0", count);
            asm_line(b, e->line, "LA %%r%d,%ld(%%r%d)", base, slot_off(b, tmp), b->temp_reg);
            emit_label(b, e->line, loop);
            asm_line(b, e->line, "STFLE 0(%%r%d)", base);
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            asm_line(b, e->line, "AHI %%r%d,-1", count);
            asm_line(b, e->line, "BRCL 8,%s", done);
            asm_line(b, e->line, "LTR %%r0,%%r0");
            asm_line(b, e->line, "BRCL 8,%s", zero);
            asm_line(b, e->line, "AHI %%r%d,8", base);
            asm_line(b, e->line, "BRCL 15,%s", loop);
            emit_label(b, e->line, zero);
            asm_line(b, e->line, "LGHI %%r0,0");
            asm_line(b, e->line, "BRCL 15,%s", store);
            emit_label(b, e->line, done);
            asm_line(b, e->line, "NILF %%r%d,63", bitr);
            asm_line(b, e->line, "LGHI %%r%d,63", shift);
            asm_line(b, e->line, "SGR %%r%d,%%r%d", shift, bitr);
            asm_line(b, e->line, "LGHI %%r%d,1", bitr);
            asm_line(b, e->line, "SLLG %%r%d,%%r%d,0(%%r%d)", bitr, bitr, shift);
            asm_line(b, e->line, "LG %%r0,0(%%r%d)", base);
            asm_line(b, e->line, "NGR %%r0,%%r%d", bitr);
            asm_line(b, e->line, "LGHI %%r0,0");
            asm_line(b, e->line, "BRCL 8,%s", store);
            asm_line(b, e->line, "LGHI %%r0,1");
            emit_label(b, e->line, store);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            free(loop); free(done); free(zero); free(store);
            return 1;
        }
        if (!strcmp(e->name, "__facility_test")) {
            int pa, bitr, shift;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            pa = pick_addr(b, 0, 0);
            bitr = pick_addr(b, pa, 0);
            shift = pick_addr3(b, pa, bitr, 0);
            if (pa < 0 || bitr < 0 || shift < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__facility_test needs three scratch registers");
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), pa);
            load_slot(b, e->line, sp, e->b->type, bitr);
            asm_line(b, e->line, "LGR %%r0,%%r%d", bitr);
            asm_line(b, e->line, "SRL %%r0,6");
            asm_line(b, e->line, "SLLG %%r0,%%r0,3");
            asm_line(b, e->line, "AGR %%r%d,%%r0", pa);
            asm_line(b, e->line, "NILF %%r%d,63", bitr);
            asm_line(b, e->line, "LGHI %%r%d,63", shift);
            asm_line(b, e->line, "SGR %%r%d,%%r%d", shift, bitr);
            asm_line(b, e->line, "LGHI %%r%d,1", bitr);
            asm_line(b, e->line, "SLLG %%r%d,%%r%d,0(%%r%d)", bitr, bitr, shift);
            asm_line(b, e->line, "LG %%r0,0(%%r%d)", pa);
            asm_line(b, e->line, "NGR %%r0,%%r%d", bitr);
            asm_line(b, e->line, "LGHI %%r0,0");
            {
                char *lab = new_label(b);
                asm_line(b, e->line, "BRCL 8,%s", lab);
                asm_line(b, e->line, "LGHI %%r0,1");
                emit_label(b, e->line, lab);
                free(lab);
            }
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
            if (!strcmp(e->name, "__cu12") || !strcmp(e->name, "__cu14") ||
            !strcmp(e->name, "__cu21") || !strcmp(e->name, "__cu24") ||
            !strcmp(e->name, "__cu41") || !strcmp(e->name, "__cu42")) {
            int p1, s1, p2, s2, rbad, has_bad, has_mask;
            const char *ins = NULL;
            char *skip = NULL, *store = NULL;
            has_bad = strcmp(e->name, "__cu21") && strcmp(e->name, "__cu24");
            has_mask = strcmp(e->name, "__cu41") && strcmp(e->name, "__cu42");
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sl) ||
                !eval(cg, e->c, &sp) || !eval(cg, e->d, &sd) ||
                (has_bad && !eval(cg, e->arg5, &sc))) return 0;
            p1 = pick_pair(b, 0, 0, &s1);
            p2 = pick_pair(b, p1, s1, &s2);
            rbad = pick_addr4(b, p1, s1, p2, s2);
            if (p1 < 0 || p2 < 0 || rbad < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs two even-odd cscratch pairs and a scratch register", e->name);
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), p1);
            load_slot(b, e->line, sl, e->b->type, s1);
            load_slot(b, e->line, sp, type_ptr(), p2);
            load_slot(b, e->line, sd, e->d->type, s2);
            if (!strcmp(e->name, "__cu12")) ins = "CU12";
            else if (!strcmp(e->name, "__cu14")) ins = "CU14";
            else if (!strcmp(e->name, "__cu21")) ins = "CU21";
            else if (!strcmp(e->name, "__cu24")) ins = "CU24";
            else if (!strcmp(e->name, "__cu41")) ins = "CU41";
            else ins = "CU42";
            if (has_mask) asm_line(b, e->line, "%s %%r%d,%%r%d,%d", ins, p1, p2, (int)e->num);
            else asm_line(b, e->line, "%s %%r%d,%%r%d", ins, p1, p2);
            asm_line(b, e->line, "BRC 3,*-4");
            if (has_bad) {
                load_slot(b, e->line, sc, type_ptr(), rbad);
                store = new_label(b);
                skip = new_label(b);
                asm_line(b, e->line, "BRC 2,%s", store);
                asm_line(b, e->line, "BRCL 15,%s", skip);
                emit_label(b, e->line, store);
                asm_line(b, e->line, "STG %%r%d,0(%%r%d)", p2, rbad);
                emit_label(b, e->line, skip);
                free(store);
                free(skip);
            }
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__clcle") || !strcmp(e->name, "__clclu") ||
            !strcmp(e->name, "__mvcle") || !strcmp(e->name, "__mvclu")) {
            int p1, s1, p2, s2, rpad, rval, tmp;
            int wide = !strcmp(e->name, "__clclu") || !strcmp(e->name, "__mvclu");
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sl) ||
                !eval(cg, e->c, &sc) || !eval(cg, e->d, &sd) ||
                !eval(cg, e->arg5, &sp)) return 0;
            p1 = pick_pair(b, 0, 0, &s1);
            p2 = pick_pair(b, p1, s1, &s2);
            rpad = pick_addr4(b, p1, s1, p2, s2);
            rval = pick_addr5(b, p1, s1, p2, s2, rpad);
            if (p1 < 0 || p2 < 0 || rpad < 0 || rval < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs two even-odd cscratch pairs and two scratch registers",
                         e->name);
                return 0;
            }
            tmp = alloc_slot(cg);
            load_slot(b, e->line, sa, type_ptr(), p1);
            load_slot(b, e->line, sl, e->b->type, s1);
            asm_line(b, e->line, "NILF %%r%d,16777215", s1);
            load_slot(b, e->line, sd, type_ptr(), p2);
            load_slot(b, e->line, sp, e->arg5->type, s2);
            asm_line(b, e->line, "NILF %%r%d,16777215", s2);
            load_slot(b, e->line, sc, e->c->type, rval);
            asm_line(b, e->line, "LA %%r%d,%ld(%%r%d)", rpad, slot_off(b, tmp), b->temp_reg);
            asm_line(b, e->line, wide ? "STH %%r%d,0(%%r%d)" : "STC %%r%d,0(%%r%d)", rval, rpad);
            if (!strcmp(e->name, "__clcle")) asm_line(b, e->line, "CLCLE %%r%d,%%r%d,0(%%r%d)", p1, p2, rpad);
            else if (!strcmp(e->name, "__clclu")) asm_line(b, e->line, "CLCLU %%r%d,%%r%d,0(%%r%d)", p1, p2, rpad);
            else if (!strcmp(e->name, "__mvcle")) asm_line(b, e->line, "MVCLE %%r%d,%%r%d,0(%%r%d)", p1, p2, rpad);
            else asm_line(b, e->line, "MVCLU %%r%d,%%r%d,0(%%r%d)", p1, p2, rpad);
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__trng_available") || !strcmp(e->name, "__rng_available")) {
            int tmp, res, i, ok = 1;
            char *absent = new_label(b), *done = new_label(b);
            for (i = 0; i < 3; i++) {
                int j, hit = 0;
                for (j = 0; j < b->nscratch; j++) if (b->scratch[j] == i) hit = 1;
                if (!hit) ok = 0;
            }
            if (!ok) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs %%r0, %%r1 and %%r2 in cscratch", e->name);
                free(absent); free(done);
                return 0;
            }
            emit_msa5_check(cg, e->line, absent, &tmp);
            asm_line(b, e->line, "LA %%r1,%ld(%%r%d)", slot_off(b, tmp), b->temp_reg);
            asm_line(b, e->line, "LGHI %%r0,0");
            asm_line(b, e->line, "PRNO %%r2,%%r2");
            asm_line(b, e->line, "LLGC %%r0,14(%%r1)");
            asm_line(b, e->line, "NILF %%r0,32");
            asm_line(b, e->line, "SRL %%r0,5");
            asm_line(b, e->line, "BRCL 15,%s", done);
            emit_label(b, e->line, absent);
            asm_line(b, e->line, "LGHI %%r0,0");
            emit_label(b, e->line, done);
            free(absent); free(done);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__prno_query")) {
            int i, res, ok = 1;
            char *absent = new_label(b), *done = new_label(b);
            for (i = 0; i < 2; i++) {
                int j, hit = 0;
                for (j = 0; j < b->nscratch; j++) if (b->scratch[j] == i) hit = 1;
                if (!hit) ok = 0;
            }
            if (!ok) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs %%r0 and %%r1 in cscratch", e->name);
                free(absent); free(done);
                return 0;
            }
            emit_msa5_check(cg, e->line, absent, NULL);
            if (!eval(cg, e->a, &sa)) { free(absent); free(done); return 0; }
            load_slot(b, e->line, sa, type_ptr(), 1);
            asm_line(b, e->line, "LGHI %%r0,0");
            asm_line(b, e->line, "PRNO %%r2,%%r2");
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            asm_line(b, e->line, "BRCL 15,%s", done);
            emit_label(b, e->line, absent);
            asm_line(b, e->line, "LGHI %%r0,2");
            emit_label(b, e->line, done);
            free(absent); free(done);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__prno_trng") || !strcmp(e->name, "__rng_bytes")) {
            int need[6] = {0, 1, 2, 3, 8, 9}, i, res;
            char *absent = new_label(b), *done = new_label(b);
            for (i = 0; i < 6; i++) {
                int j, hit = 0;
                for (j = 0; j < b->nscratch; j++) if (b->scratch[j] == need[i]) hit = 1;
                if (!hit) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs %%r0, %%r1, %%r2, %%r3, %%r8 and %%r9 in cscratch",
                             e->name);
                    free(absent); free(done);
                    return 0;
                }
            }
            emit_msa5_check(cg, e->line, absent, NULL);
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) {
                free(absent); free(done);
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), 2);
            load_slot(b, e->line, sp, e->b->type, 3);
            asm_line(b, e->line, "LGHI %%r8,0");
            asm_line(b, e->line, "LGHI %%r9,0");
            asm_line(b, e->line, "LGHI %%r0,114");
            asm_line(b, e->line, "PRNO %%r8,%%r2");
            asm_line(b, e->line, "BRC 3,*-4");
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            asm_line(b, e->line, "BRCL 15,%s", done);
            emit_label(b, e->line, absent);
            asm_line(b, e->line, "LGHI %%r0,2");
            emit_label(b, e->line, done);
            free(absent); free(done);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__prno")) {
            int need[6] = {0, 1, 2, 3, 8, 9}, i, res;
            int ss[6];
            char *absent = new_label(b), *done = new_label(b);
            for (i = 0; i < 6; i++) {
                int j, hit = 0;
                for (j = 0; j < b->nscratch; j++) if (b->scratch[j] == need[i]) hit = 1;
                if (!hit) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs %%r0, %%r1, %%r2, %%r3, %%r8 and %%r9 in cscratch", e->name);
                    free(absent); free(done);
                    return 0;
                }
            }
            emit_msa5_check(cg, e->line, absent, NULL);
            if (!eval(cg, e->a, &ss[0]) || !eval(cg, e->b, &ss[1]) ||
                !eval(cg, e->c, &ss[2]) || !eval(cg, e->d, &ss[3]) ||
                !eval(cg, e->arg5, &ss[4]) || !eval(cg, e->arg6, &ss[5])) {
                free(absent); free(done);
                return 0;
            }
            load_slot(b, e->line, ss[0], e->a->type, 0);
            load_slot(b, e->line, ss[1], e->b->type, 1);
            load_slot(b, e->line, ss[2], e->c->type, 2);
            load_slot(b, e->line, ss[3], e->d->type, 3);
            load_slot(b, e->line, ss[4], e->arg5->type, 8);
            load_slot(b, e->line, ss[5], e->arg6->type, 9);
            asm_line(b, e->line, "PRNO %%r2,%%r8");
            asm_line(b, e->line, "BRC 3,*-4");
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            asm_line(b, e->line, "BRCL 15,%s", done);
            emit_label(b, e->line, absent);
            asm_line(b, e->line, "LGHI %%r0,2");
            emit_label(b, e->line, done);
            free(absent); free(done);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__clcl")) {
            int p1, s1, p2, s2, rt;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) ||
                !eval(cg, e->c, &sl) || !eval(cg, e->d, &sd) ||
                !eval(cg, e->arg5, &sc)) return 0;
            p1 = pick_pair(b, 0, 0, &s1);
            p2 = pick_pair(b, p1, s1, &s2);
            rt = pick_addr4(b, p1, s1, p2, s2);
            if (p1 < 0 || p2 < 0 || rt < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__clcl needs two even-odd cscratch pairs and a scratch register");
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), p1);
            load_slot(b, e->line, sp, type_ptr(), p2);
            load_slot(b, e->line, sl, type_ptr(), rt);
            asm_line(b, e->line, "L %%r%d,0(%%r%d)", s1, rt);
            asm_line(b, e->line, "NILF %%r%d,16777215", s1);
            load_slot(b, e->line, sd, type_ptr(), rt);
            asm_line(b, e->line, "L %%r%d,0(%%r%d)", s2, rt);
            asm_line(b, e->line, "NILF %%r%d,16777215", s2);
            load_slot(b, e->line, sc, e->arg5->type, rt);
            asm_line(b, e->line, "SLL %%r%d,24", rt);
            asm_line(b, e->line, "OR %%r%d,%%r%d", s2, rt);
            asm_line(b, e->line, "CLCL %%r%d,%%r%d", p1, p2);
            load_slot(b, e->line, sl, type_ptr(), rt);
            asm_line(b, e->line, "ST %%r%d,0(%%r%d)", s1, rt);
            load_slot(b, e->line, sd, type_ptr(), rt);
            asm_line(b, e->line, "NILF %%r%d,16777215", s2);
            asm_line(b, e->line, "ST %%r%d,0(%%r%d)", s2, rt);
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__tre") || !strcmp(e->name, "__srstu") ||
            !strcmp(e->name, "__troo") || !strcmp(e->name, "__trot") ||
            !strcmp(e->name, "__trto") || !strcmp(e->name, "__trtt")) {
            int need[4] = {0, 0, 0, 0}, nneed = 4, i;
            const char *ins = NULL;
            int is_tre = !strcmp(e->name, "__tre");
            int is_srstu = !strcmp(e->name, "__srstu");
            if (is_tre) { need[0]=0; need[1]=1; need[2]=2; need[3]=3; ins="TRE"; }
            else if (is_srstu) { need[0]=2; need[1]=3; need[2]=0; need[3]=1; ins="SRSTU"; }
            else { need[0]=0; need[1]=1; need[2]=2; need[3]=5; }
            for (i = 0; i < nneed; i++) {
                int j, ok = 0;
                for (j = 0; j < b->nscratch; j++) if (b->scratch[j] == need[i]) ok = 1;
                if (!ok) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs %%r%d in cscratch", e->name, need[i]);
                    return 0;
                }
            }
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) ||
                !eval(cg, e->c, &sl) || !eval(cg, e->d, &sd) ||
                (!is_tre && !is_srstu && !eval(cg, e->arg5, &sc))) return 0;
            if (is_tre) {
                load_slot(b, e->line, sa, type_ptr(), 2);
                load_slot(b, e->line, sp, e->b->type, 3);
                load_slot(b, e->line, sl, type_ptr(), 1);
                load_slot(b, e->line, sd, e->d->type, 0);
                asm_line(b, e->line, "TRE %%r2,%%r1");
            } else if (is_srstu) {
                load_slot(b, e->line, sa, type_ptr(), 2);
                load_slot(b, e->line, sp, type_ptr(), 3);
                load_slot(b, e->line, sl, e->c->type, 0);
                load_slot(b, e->line, sd, type_ptr(), 1);
                asm_line(b, e->line, "SRSTU %%r2,%%r3");
            } else {
                load_slot(b, e->line, sa, type_ptr(), 2);
                load_slot(b, e->line, sp, type_ptr(), 5);
                load_slot(b, e->line, sl, e->c->type, 3);
                load_slot(b, e->line, sd, type_ptr(), 1);
                load_slot(b, e->line, sc, e->arg5->type, 0);
                if (!strcmp(e->name, "__troo")) ins = "TROO";
                else if (!strcmp(e->name, "__trot")) ins = "TROT";
                else if (!strcmp(e->name, "__trto")) ins = "TRTO";
                else ins = "TRTT";
                asm_line(b, e->line, "%s %%r2,%%r5,%d", ins, (int)e->num);
            }
            asm_line(b, e->line, "BRO *-4");
            if (is_srstu) {
                asm_line(b, e->line, "LGR %%r0,%%r2");
                asm_line(b, e->line, "STG %%r0,0(%%r1)");
            }
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__edmk") || !strcmp(e->name, "__trt") || !strcmp(e->name, "__trtr")) {
            int ra, rb, rl, ro, rc = -1;
            char *templ = new_label(b), *end = new_label(b);
            int has1 = 0, has2 = 0, i;
            for (i = 0; i < b->nscratch; i++) {
                if (b->scratch[i] == 1) has1 = 1;
                if (b->scratch[i] == 2) has2 = 1;
            }
            if (!has1 || !has2) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs %%r1 and %%r2 in cscratch", e->name);
                free(templ); free(end);
                return 0;
            }
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) ||
                !eval(cg, e->c, &sl) || !eval(cg, e->d, &sd) ||
                (strcmp(e->name, "__edmk") && !eval(cg, e->arg5, &sc))) {
                free(templ); free(end);
                return 0;
            }
            ra = pick_addr(b, 1, 2);
            rb = pick_addr(b, ra, 2);
            rl = pick_addr3(b, ra, rb, 2);
            ro = pick_addr4(b, ra, rb, rl, 2);
            if (strcmp(e->name, "__edmk")) rc = pick_addr5(b, ra, rb, rl, ro, 2);
            if (ra < 0 || rb < 0 || rl < 0 || ro < 0 ||
                (strcmp(e->name, "__edmk") && rc < 0)) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs enough cscratch registers", e->name);
                free(templ); free(end);
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), ra);
            load_slot(b, e->line, sp, type_ptr(), rb);
            load_slot(b, e->line, sl, e->c->type, rl);
            load_slot(b, e->line, sd, type_ptr(), ro);
            if (strcmp(e->name, "__edmk")) load_slot(b, e->line, sc, type_ptr(), rc);
            asm_line(b, e->line, "BRCL 15,%s", end);
            emit_label(b, e->line, templ);
            if (!strcmp(e->name, "__edmk")) asm_line(b, e->line, "EDMK 0(1,%%r%d),0(%%r%d)", ra, rb);
            else if (!strcmp(e->name, "__trt")) asm_line(b, e->line, "TRT 0(1,%%r%d),0(%%r%d)", ra, rb);
            else asm_line(b, e->line, "TRTR 0(1,%%r%d),0(%%r%d)", ra, rb);
            emit_label(b, e->line, end);
            if (!strcmp(e->name, "__edmk")) asm_line(b, e->line, "LGR %%r1,%%r%d", ra);
            asm_line(b, e->line, "EXRL %%r%d,%s", rl, templ);
            asm_line(b, e->line, "STG %%r1,0(%%r%d)", ro);
            if (!strcmp(e->name, "__trt")) asm_line(b, e->line, "STC %%r2,0(%%r%d)", rc);
            if (!strcmp(e->name, "__trtr")) asm_line(b, e->line, "STC %%r2,0(%%r%d)", rc);
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            free(templ); free(end);
            return 1;
        }
        if (!strcmp(e->name, "__strlen")) {
            if (e->a->kind == E_STR || (e->a->kind == E_VAR && e->a->var->lit)) {
                size_t len = e->a->kind == E_STR ? e->a->slen : e->a->var->lit_len;
                asm_line(b, e->line, "LGFI %%r0,%lu", (unsigned long)len);
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(64, 1), 0);
                *slotp = res;
                return 1;
            }
            int addr, chr;
            if (!eval(cg, e->a, &sa)) return 0;
            addr = pick_addr(b, 0, 0);
            chr = pick_addr(b, addr, 0);
            if (addr < 0 || chr < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "__strlen needs two nonzero scratch registers");
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), addr);
            asm_line(b, e->line, "LGHI %%r0,0");
            {
                char *top = new_label(b), *end = new_label(b);
                emit_label(b, e->line, top);
                asm_line(b, e->line, "LLGC %%r%d,0(%%r%d)", chr, addr);
                asm_line(b, e->line, "LTR %%r%d,%%r%d", chr, chr);
                asm_line(b, e->line, "BRCL 8,%s", end);
                asm_line(b, e->line, "AGHI %%r%d,1", addr);
                asm_line(b, e->line, "AGHI %%r0,1");
                asm_line(b, e->line, "BRCL 15,%s", top);
                emit_label(b, e->line, end);
                free(top); free(end);
            }
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(64, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__popcnt") || !strcmp(e->name, "__popcnt4") ||
            !strcmp(e->name, "__popcnt8")) {
            if (!eval(cg, e->a, &sa)) return 0;
            load_slot(b, e->line, sa, e->a->type, 0);
            asm_line(b, e->line, "POPCNT %%r1,%%r0");
            res = alloc_slot(cg);
            store_slot(b, e->line, res, e->type, 1);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__lrv") || !strcmp(e->name, "__lrvh") ||
            !strcmp(e->name, "__lrvg")) {
            int addr;
            if (!eval(cg, e->a, &sa)) return 0;
            addr = pick_addr(b, 0, 0);
            if (addr < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "%s needs a nonzero scratch register", e->name);
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), addr);
            asm_line(b, e->line, !strcmp(e->name, "__lrv") ? "LRV %%r0,0(%%r%d)" :
                               !strcmp(e->name, "__lrvh") ? "LRVH %%r0,0(%%r%d)" :
                                                              "LRVG %%r0,0(%%r%d)", addr);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, e->type, 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__strv") || !strcmp(e->name, "__strvg") ||
            !strcmp(e->name, "__strvh")) {
            int addr;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            addr = pick_addr(b, 0, 0);
            if (addr < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "%s needs a nonzero scratch register", e->name);
                return 0;
            }
            load_slot(b, e->line, sa, e->a->type, 0);
            load_slot(b, e->line, sp, type_ptr(), addr);
            asm_line(b, e->line, !strcmp(e->name, "__strv") ? "STRV %%r0,0(%%r%d)" :
                               !strcmp(e->name, "__strvh") ? "STRVH %%r0,0(%%r%d)" :
                                                              "STRVG %%r0,0(%%r%d)", addr);
            res = alloc_slot(cg);
            asm_line(b, e->line, "LGHI %%r0,0");
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__stck") || !strcmp(e->name, "__stcke") ||
            !strcmp(e->name, "__stckf")) {
            int addr;
            if (!eval(cg, e->a, &sa)) return 0;
            addr = pick_addr(b, 0, 0);
            if (addr < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "%s needs a nonzero scratch register", e->name);
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), addr);
            if (!strcmp(e->name, "__stck")) asm_line(b, e->line, "STCK 0(%%r%d)", addr);
            else if (!strcmp(e->name, "__stcke")) asm_line(b, e->line, "STCKE 0(%%r%d)", addr);
            else asm_line(b, e->line, "STCKF 0(%%r%d)", addr);
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__lad") || !strcmp(e->name, "__ladl") ||
            !strcmp(e->name, "__lan") || !strcmp(e->name, "__lax") ||
            !strcmp(e->name, "__lao") || !strcmp(e->name, "__ladg") ||
            !strcmp(e->name, "__ladlg") || !strcmp(e->name, "__lang") ||
            !strcmp(e->name, "__laxg") || !strcmp(e->name, "__laog")) {
            int ra, rb;
            const char *op;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sl))
                return 0;
            ra = pick_addr(b, 1, 0);       /* keep r1 for the old value */
            rb = pick_addr(b, 1, ra);
            if (ra < 0 || rb < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "%s needs two nonzero address registers", e->name);
                return 0;
            }
            if (!strcmp(e->name, "__lad")) op = "LAA";
            else if (!strcmp(e->name, "__ladl")) op = "LAAL";
            else if (!strcmp(e->name, "__lan")) op = "LAN";
            else if (!strcmp(e->name, "__lax")) op = "LAX";
            else if (!strcmp(e->name, "__lao")) op = "LAO";
            else if (!strcmp(e->name, "__ladg")) op = "LAAG";
            else if (!strcmp(e->name, "__ladlg")) op = "LAALG";
            else if (!strcmp(e->name, "__lang")) op = "LANG";
            else if (!strcmp(e->name, "__laxg")) op = "LAXG";
            else op = "LAOG";
            load_slot(b, e->line, sa, type_ptr(), ra);
            load_slot(b, e->line, sp, e->b->type, 0);
            load_slot(b, e->line, sl, type_ptr(), rb);
            asm_line(b, e->line, "%s %%r1,%%r0,0(%%r%d)", op, rb);
            if (!strcmp(op, "LAA") || !strcmp(op, "LAAL") || !strcmp(op, "LAN") ||
                !strcmp(op, "LAX") || !strcmp(op, "LAO"))
                asm_line(b, e->line, "ST %%r1,0(%%r%d)", ra);
            else
                asm_line(b, e->line, "STG %%r1,0(%%r%d)", ra);
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__lpd") || !strcmp(e->name, "__lpdg")) {
            int pair, second, ra, rb, sd;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) ||
                !eval(cg, e->c, &sl) || !eval(cg, e->d, &sd))
                return 0;
            pair = pick_pair(b, 0, 0, &second);
            if (pair < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "%s needs an even-odd cscratch pair", e->name);
                return 0;
            }
            ra = pick_addr(b, pair, second);
            rb = pick_addr3(b, pair, second, ra);
            if (ra < 0 || rb < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "%s needs two address registers", e->name);
                return 0;
            }
            load_slot(b, e->line, sl, type_ptr(), ra);
            load_slot(b, e->line, sd, type_ptr(), rb);
            asm_line(b, e->line, !strcmp(e->name, "__lpd") ? "LPD %%r%d,0(%%r%d),0(%%r%d)" :
                                                              "LPDG %%r%d,0(%%r%d),0(%%r%d)", pair, ra, rb);
            load_slot(b, e->line, sa, type_ptr(), ra);
            load_slot(b, e->line, sp, type_ptr(), rb);
            if (!strcmp(e->name, "__lpd")) {
                asm_line(b, e->line, "ST %%r%d,0(%%r%d)", pair, ra);
                asm_line(b, e->line, "ST %%r%d,0(%%r%d)", second, rb);
            } else {
                asm_line(b, e->line, "STG %%r%d,0(%%r%d)", pair, ra);
                asm_line(b, e->line, "STG %%r%d,0(%%r%d)", second, rb);
            }
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__csg")) {
            int ra, rb, rc;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sl)) return 0;
            ra = pick_addr3(b, 1, 0, 0);
            rb = pick_addr3(b, 1, ra, 0);
            rc = pick_addr3(b, 1, ra, rb);
            if (ra < 0 || rb < 0 || rc < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "__csg needs three nonzero address registers");
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), ra);
            load_slot(b, e->line, sp, type_ptr(), rb);
            load_slot(b, e->line, sl, type_ptr(), rc);
            asm_line(b, e->line, "LG %%r0,0(%%r%d)", ra);
            asm_line(b, e->line, "LG %%r1,0(%%r%d)", rc);
            asm_line(b, e->line, "CSG %%r0,%%r1,0(%%r%d)", rb);
            asm_line(b, e->line, "STG %%r0,0(%%r%d)", ra);
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__ap") || !strcmp(e->name, "__cp") ||
            !strcmp(e->name, "__sp") || !strcmp(e->name, "__mp") ||
            !strcmp(e->name, "__dp") || !strcmp(e->name, "__zap") ||
            !strcmp(e->name, "__pack") || !strcmp(e->name, "__unpk") ||
            !strcmp(e->name, "__nc") || !strcmp(e->name, "__oc") ||
            !strcmp(e->name, "__xc") || !strcmp(e->name, "__tr") ||
            !strcmp(e->name, "__ed") || !strcmp(e->name, "__tp") ||
            !strcmp(e->name, "__srp") || !strcmp(e->name, "__cvb") ||
            !strcmp(e->name, "__cvd")) {
            int64_t l1, l2 = 0;
            int ra, rb;
            const char *op = NULL;
            if (!strcmp(e->name, "__cvb")) {
                if (!eval(cg, e->a, &sa)) return 0;
                ra = pick_addr(b, 0, 0);
                if (ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005", "__cvb needs a nonzero scratch register");
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), ra);
                asm_line(b, e->line, "CVB %%r0,0(%%r%d)", ra);
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__cvd")) {
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
                ra = pick_addr(b, 0, 0);
                if (ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005", "__cvd needs a nonzero scratch register");
                    return 0;
                }
                load_slot(b, e->line, sa, e->a->type, 0);
                load_slot(b, e->line, sp, type_ptr(), ra);
                asm_line(b, e->line, "CVD %%r0,0(%%r%d)", ra);
                res = alloc_slot(cg);
                asm_line(b, e->line, "LGHI %%r0,0");
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__tp")) {
                if (!const_eval(e->b, &l1) || l1 < 0 || l1 > 15) {
                    cb_error(b, e->line, e->col, "ZA5005", "__tp length must be a constant 0 to 15");
                    return 0;
                }
                if (!eval(cg, e->a, &sa)) return 0;
                ra = pick_addr(b, 0, 0);
                if (ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005", "__tp needs a nonzero scratch register");
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), ra);
                asm_line(b, e->line, "TP 0(%lld,%%r%d)", (long long)l1, ra);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__srp")) {
                int64_t sh, pat;
                if (!const_eval(e->b, &l1) || l1 < 0 || l1 > 15 ||
                    !const_eval(e->c, &sh) || sh < -128 || sh > 127 ||
                    !const_eval(e->d, &pat) || pat < 0 || pat > 255) {
                    cb_error(b, e->line, e->col, "ZA5005", "__srp needs constant length, shift and pattern");
                    return 0;
                }
                if (!eval(cg, e->a, &sa)) return 0;
                ra = pick_addr(b, 0, 0);
                if (ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005", "__srp needs a nonzero scratch register");
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), ra);
                asm_line(b, e->line, "SRP 0(%lld,%%r%d),%lld,%lld", (long long)l1, ra,
                         (long long)sh, (long long)pat);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__nc") || !strcmp(e->name, "__oc") ||
                !strcmp(e->name, "__xc") || !strcmp(e->name, "__tr") ||
                !strcmp(e->name, "__ed")) {
                if (!const_eval(e->c, &l1) || l1 < 0 || l1 > 15) {
                    cb_error(b, e->line, e->col, "ZA5005", "%s length must be a constant 0 to 15", e->name);
                    return 0;
                }
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
                ra = pick_addr(b, 0, 0);
                rb = pick_addr(b, ra, 0);
                if (ra < 0 || rb < 0) {
                    cb_error(b, e->line, e->col, "ZA5005", "%s needs two address registers", e->name);
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), ra);
                load_slot(b, e->line, sp, type_ptr(), rb);
                if (!strcmp(e->name, "__ed")) {
                    asm_line(b, e->line, "ED 0(%lld,%%r%d),0(%%r%d)", (long long)l1, ra, rb);
                    asm_line(b, e->line, "IPM %%r0");
                    asm_line(b, e->line, "SRL %%r0,28");
                    res = alloc_slot(cg);
                    store_slot(b, e->line, res, type_of(32, 0), 0);
                    *slotp = res;
                    return 1;
                }
                if (!strcmp(e->name, "__tr")) {
                    asm_line(b, e->line, "TR 0(%lld,%%r%d),0(%%r%d)", (long long)l1, ra, rb);
                    res = alloc_slot(cg);
                    asm_line(b, e->line, "LGHI %%r0,0");
                    store_slot(b, e->line, res, type_of(32, 0), 0);
                    *slotp = res;
                    return 1;
                }
                op = !strcmp(e->name, "__nc") ? "NC" : !strcmp(e->name, "__oc") ? "OC" : "XC";
                asm_line(b, e->line, "%s 0(%lld,%%r%d),0(%%r%d)", op, (long long)l1, ra, rb);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!const_eval(e->b, &l1) || l1 < 0 || l1 > 15 ||
                !const_eval(e->d, &l2) || l2 < 0 || l2 > 15) {
                cb_error(b, e->line, e->col, "ZA5005", "%s lengths must be constants 0 to 15", e->name);
                return 0;
            }
            if (!eval(cg, e->a, &sa) || !eval(cg, e->c, &sl)) return 0;
            ra = pick_addr(b, 0, 0);
            rb = pick_addr(b, ra, 0);
            if (ra < 0 || rb < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "%s needs two address registers", e->name);
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), ra);
            load_slot(b, e->line, sl, type_ptr(), rb);
            if (!strcmp(e->name, "__ap")) op = "AP";
            else if (!strcmp(e->name, "__cp")) op = "CP";
            else if (!strcmp(e->name, "__sp")) op = "SP";
            else if (!strcmp(e->name, "__mp")) op = "MP";
            else if (!strcmp(e->name, "__dp")) op = "DP";
            else if (!strcmp(e->name, "__zap")) op = "ZAP";
            else if (!strcmp(e->name, "__pack")) op = "PACK";
            else op = "UNPK";
            asm_line(b, e->line, "%s 0(%lld,%%r%d),0(%lld,%%r%d)", op,
                     (long long)l1, ra, (long long)l2, rb);
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__cdsg")) {
            int p1, p2, s1, s2, ra;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sl))
                return 0;
            p1 = pick_pair(b, 0, 0, &s1);
            p2 = pick_pair(b, p1, s1, &s2);
            ra = pick_addr4(b, p1, s1, p2, s2);
            if (p1 < 0 || p2 < 0 || ra < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "__cdsg needs two even-odd pairs and an address register");
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), ra);
            asm_line(b, e->line, "LG %%r%d,0(%%r%d)", p1, ra);
            asm_line(b, e->line, "LG %%r%d,8(%%r%d)", s1, ra);
            load_slot(b, e->line, sl, type_ptr(), ra);
            asm_line(b, e->line, "LG %%r%d,0(%%r%d)", p2, ra);
            asm_line(b, e->line, "LG %%r%d,8(%%r%d)", s2, ra);
            load_slot(b, e->line, sp, type_ptr(), ra);
            asm_line(b, e->line, "CDSG %%r%d,%%r%d,0(%%r%d)", p1, p2, ra);
            load_slot(b, e->line, sa, type_ptr(), ra);
            asm_line(b, e->line, "STG %%r%d,0(%%r%d)", p1, ra);
            asm_line(b, e->line, "STG %%r%d,8(%%r%d)", s1, ra);
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__cvbg")) {
            int ra;
            if (!eval(cg, e->a, &sa)) return 0;
            ra = pick_addr(b, 0, 0);
            if (ra < 0) { cb_error(b, e->line, e->col, "ZA5005", "__cvbg needs a scratch register"); return 0; }
            load_slot(b, e->line, sa, type_ptr(), ra);
            asm_line(b, e->line, "CVBG %%r0,0(%%r%d)", ra);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(64, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__cvdg")) {
            int ra;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            ra = pick_addr(b, 0, 0);
            if (ra < 0) { cb_error(b, e->line, e->col, "ZA5005", "__cvdg needs a scratch register"); return 0; }
            load_slot(b, e->line, sa, e->a->type, 0);
            load_slot(b, e->line, sp, type_ptr(), ra);
            asm_line(b, e->line, "CVDG %%r0,0(%%r%d)", ra);
            res = alloc_slot(cg);
            asm_line(b, e->line, "LGHI %%r0,0");
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__pka") || !strcmp(e->name, "__pku") ||
            !strcmp(e->name, "__unpka") || !strcmp(e->name, "__unpku") ||
            !strcmp(e->name, "__mvcrl")) {
            int64_t len;
            int ra, rb;
            if (!strcmp(e->name, "__unpka") || !strcmp(e->name, "__unpku")) {
                if (!const_eval(e->b, &len) || len < 0 || len > 255) {
                    cb_error(b, e->line, e->col, "ZA5005", "%s length must be a constant 0 to 255", e->name);
                    return 0;
                }
                if (!eval(cg, e->a, &sa) || !eval(cg, e->c, &sl)) return 0;
            } else {
                if (!const_eval(e->c, &len) || len < 0 || len > 255) {
                    cb_error(b, e->line, e->col, "ZA5005", "%s length must be a constant 0 to 255", e->name);
                    return 0;
                }
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
                sl = sp;
            }
            ra = pick_addr(b, 0, 0);
            rb = pick_addr(b, ra, 0);
            if (ra < 0 || rb < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "%s needs two address registers", e->name);
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), ra);
            load_slot(b, e->line, sl, type_ptr(), rb);
            if (!strcmp(e->name, "__pka")) asm_line(b, e->line, "PKA 0(%%r%d),0(%lld,%%r%d)", ra, (long long)len, rb);
            else if (!strcmp(e->name, "__pku")) asm_line(b, e->line, "PKU 0(%%r%d),0(%lld,%%r%d)", ra, (long long)len, rb);
            else if (!strcmp(e->name, "__unpka")) asm_line(b, e->line, "UNPKA 0(%lld,%%r%d),0(%%r%d)", (long long)len, ra, rb);
            else if (!strcmp(e->name, "__unpku")) asm_line(b, e->line, "UNPKU 0(%lld,%%r%d),0(%%r%d)", (long long)len, ra, rb);
            else asm_line(b, e->line, "MVCRL 0(%lld,%%r%d),0(%%r%d)", (long long)len, ra, rb);
            res = alloc_slot(cg);
            asm_line(b, e->line, "LGHI %%r0,0");
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_round") || !strcmp(e->name, "__vec_roundc") ||
            !strcmp(e->name, "__vec_roundm") || !strcmp(e->name, "__vec_roundp") ||
            !strcmp(e->name, "__vec_roundz") || !strcmp(e->name, "__vec_trunc") ||
            !strcmp(e->name, "__vec_ceil") || !strcmp(e->name, "__vec_floor")) {
            int vr, va, mode;
            if (!eval(cg, e->a, &sa)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            if (vr < 0 || va < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs two cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            if (!strcmp(e->name, "__vec_round")) mode = 1;
            else if (!strcmp(e->name, "__vec_roundc")) mode = 0;
            else if (!strcmp(e->name, "__vec_roundm") || !strcmp(e->name, "__vec_floor")) mode = 7;
            else if (!strcmp(e->name, "__vec_roundp") || !strcmp(e->name, "__vec_ceil")) mode = 6;
            else mode = 5;
            asm_line(b, e->line, "VFI %%v%d,%%v%d,%d,0,%d", vr, va,
                     e->a->type.vbits == 32 ? 2 : 3, mode);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strncmp(e->name, "__vec_gather_element_", 21) ||
            !strncmp(e->name, "__vec_scatter_element_", 22)) {
            int vr, vi, ra;
            int64_t mask;
            int gather = !strncmp(e->name, "__vec_gather_element_", 21);
            int is64 = strstr(e->name, "_s8") || strstr(e->name, "_u8") ||
                       strstr(e->name, "_b8") || strstr(e->name, "_f8");
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sc))
                return 0;
            if (!const_eval(e->d, &mask) || mask < 0 || mask > 3) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs a constant mask 0 to 3", e->name);
                return 0;
            }
            vr = pick_vr4(b, -1, -1, -1, -1);
            vi = pick_vr4(b, vr, -1, -1, -1);
            ra = pick_addr(b, 0, 0);
            if (vr < 0 || vi < 0 || ra < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs two vector and one address register", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sp, vi);
            load_slot(b, e->line, sc, type_ptr(), ra);
            if (gather) {
                load_vec_slot(b, e->line, sa, vr);
                asm_line(b, e->line, "%s %%v%d,0(%%v%d,%%r%d),%lld",
                         is64 ? "VGEG" : "VGEF", vr, vi, ra, (long long)mask);
                res = alloc_slots(cg, 2);
                store_vec_slot(b, e->line, res, vr);
            } else {
                load_vec_slot(b, e->line, sa, vr);
                asm_line(b, e->line, "%s %%v%d,0(%%v%d,%%r%d),%lld",
                         is64 ? "VSCEG" : "VSCEF", vr, vi, ra, (long long)mask);
                res = alloc_slot(cg);
                asm_line(b, e->line, "LGHI %%r0,0");
                store_slot(b, e->line, res, type_of(32, 0), 0);
            }
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_msum_u128")) {
            int vr, va, vb, vacc;
            int64_t mask;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) ||
                !eval(cg, e->c, &sc)) return 0;
            if (!const_eval(e->d, &mask) || mask < 0 || mask > 3) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__vec_msum_u128 needs a constant mask 0 to 3");
                return 0;
            }
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            vacc = pick_vr4(b, vr, va, vb, -1);
            if (vr < 0 || va < 0 || vb < 0 || vacc < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__vec_msum_u128 needs four cscratch vector registers");
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            load_vec_slot(b, e->line, sc, vacc);
            asm_line(b, e->line, "VMSL %%v%d,%%v%d,%%v%d,%%v%d,3,%lld",
                     vr, va, vb, vacc, (long long)mask);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_sum_u128")) {
            int vr, va, vb;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__vec_sum_u128 needs three cscratch vector registers");
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            asm_line(b, e->line, "VSUMQ %%v%d,%%v%d,%%v%d,%d",
                     vr, va, vb, e->a->type.vbits == 32 ? 2 : 3);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_sum2") || !strcmp(e->name, "__vec_sum4")) {
            int vr, va, vb;
            int m4;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            if (!strcmp(e->name, "__vec_sum2")) {
                m4 = e->a->type.vbits == 16 ? 1 : 2;
                asm_line(b, e->line, "VSUMG %%v%d,%%v%d,%%v%d,%d", vr, va, vb, m4);
            } else {
                m4 = e->a->type.vbits == 8 ? 0 : 1;
                asm_line(b, e->line, "VSUM %%v%d,%%v%d,%%v%d,%d", vr, va, vb, m4);
            }
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_gfmsum_accum")) {
            int vr, va, vb, vacc;
            int m4 = e->a->type.vbits == 8 ? 0 : e->a->type.vbits == 16 ? 1 : 2;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sc))
                return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            vacc = pick_vr4(b, vr, va, vb, -1);
            if (vr < 0 || va < 0 || vb < 0 || vacc < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__vec_gfmsum_accum needs four cscratch vector registers");
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            load_vec_slot(b, e->line, sc, vacc);
            if (e->a->type.vbits == 64)
                asm_line(b, e->line, "VGFMAG %%v%d,%%v%d,%%v%d,%%v%d",
                         vr, va, vb, vacc);
            else
                asm_line(b, e->line, "VGFMA %%v%d,%%v%d,%%v%d,%%v%d,%d",
                         vr, va, vb, vacc, m4);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strncmp(e->name, "__vec_splat__", 13)) {
            int vr, va, elem;
            int64_t idx;
            const char *suffix = e->name + 13;
            if (!vec_element_type(suffix, &e->type)) {
                cb_error(b, e->line, e->col, "ZA5002", "unsupported splat suffix");
                return 0;
            }
            if (!eval(cg, e->a, &sa)) return 0;
            if (!const_eval(e->b, &idx) || idx < 0 || idx > 15) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs a constant index 0 to 15", e->name);
                return 0;
            }
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            if (vr < 0 || va < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs two cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            elem = e->type.vbits == 8 ? 0 : e->type.vbits == 16 ? 1 :
                   e->type.vbits == 32 ? 2 : 3;
            asm_line(b, e->line, "VREP %%v%d,%%v%d,%lld,%d", vr, va,
                     (long long)idx, elem);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_double") || !strcmp(e->name, "__vec_float") ||
            !strcmp(e->name, "__vec_signed") || !strcmp(e->name, "__vec_unsigned")) {
            int vr, va;
            if (!eval(cg, e->a, &sa)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            if (vr < 0 || va < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs two cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            if (!strcmp(e->name, "__vec_double"))
                asm_line(b, e->line, "VCDG %%v%d,%%v%d,3,0,0", vr, va);
            else if (!strcmp(e->name, "__vec_float"))
                asm_line(b, e->line, "VCEFB %%v%d,%%v%d,0,0", vr, va);
            else if (!strcmp(e->name, "__vec_signed"))
                asm_line(b, e->line, "VCGD %%v%d,%%v%d,3,0,0", vr, va);
            else
                asm_line(b, e->line, "VCLGD %%v%d,%%v%d,3,0,0", vr, va);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_doublee") || !strcmp(e->name, "__vec_floate")) {
            int vr, va;
            if (!eval(cg, e->a, &sa)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            if (vr < 0 || va < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs two cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            if (!strcmp(e->name, "__vec_doublee"))
                asm_line(b, e->line, "VFLLS %%v%d,%%v%d", vr, va);
            else
                asm_line(b, e->line, "VFLRD %%v%d,%%v%d,0,0", vr, va);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_fp_test_data_class")) {
            int vr, va, ra;
            int64_t mask;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->c, &sc)) return 0;
            if (!const_eval(e->b, &mask) || mask < 0 || mask > 4095) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__vec_fp_test_data_class needs a constant mask 0 to 4095");
                return 0;
            }
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            ra = pick_addr(b, 0, 0);
            if (vr < 0 || va < 0 || ra < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__vec_fp_test_data_class needs a vector and an address register");
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            asm_line(b, e->line, "VFTCI %%v%d,%%v%d,%lld,%d,0", vr, va,
                     (long long)mask, e->a->type.vbits == 32 ? 2 : 3);
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            load_slot(b, e->line, sc, type_ptr(), ra);
            asm_line(b, e->line, "ST %%r0,0(%%r%d)", ra);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_genmask") || !strcmp(e->name, "__vec_genmasks_8") ||
            !strcmp(e->name, "__vec_genmasks_16") || !strcmp(e->name, "__vec_genmasks_32") ||
            !strcmp(e->name, "__vec_genmasks_64") || !strcmp(e->name, "__vec_test_mask")) {
            int vr, va, vb;
            int64_t x = 0, y = 0;
            if (!strcmp(e->name, "__vec_test_mask")) {
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
                va = pick_vr4(b, -1, -1, -1, -1);
                vb = pick_vr4(b, va, -1, -1, -1);
                if (va < 0 || vb < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "__vec_test_mask needs two cscratch vector registers");
                    return 0;
                }
                load_vec_slot(b, e->line, sa, va);
                load_vec_slot(b, e->line, sp, vb);
                asm_line(b, e->line, "VTM %%v%d,%%v%d", va, vb);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!const_eval(e->a, &x) || x < 0 ||
                (!strcmp(e->name, "__vec_genmask") ? x > 65535 : x > 255)) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs a constant immediate", e->name);
                return 0;
            }
            vr = pick_vr4(b, -1, -1, -1, -1);
            if (vr < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs a cscratch vector register", e->name);
                return 0;
            }
            if (!strcmp(e->name, "__vec_genmask"))
                asm_line(b, e->line, "VGBM %%v%d,%lld", vr, (long long)x);
            else {
                if (!const_eval(e->b, &y) || y < 0 || y > 255) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs two byte immediates", e->name);
                    return 0;
                }
                asm_line(b, e->line, "VGM %%v%d,%lld,%lld,%d", vr, (long long)x,
                         (long long)y, !strcmp(e->name, "__vec_genmasks_8") ? 0 :
                                         !strcmp(e->name, "__vec_genmasks_16") ? 1 :
                                         !strcmp(e->name, "__vec_genmasks_32") ? 2 : 3);
            }
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_xld2") || !strcmp(e->name, "__vec_xlw4") ||
            !strcmp(e->name, "__vec_xstd2") || !strcmp(e->name, "__vec_xstw4")) {
            int vr, addr, off;
            int load = !strcmp(e->name, "__vec_xld2") || !strcmp(e->name, "__vec_xlw4");
            if (load) {
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
                vr = pick_vr4(b, -1, -1, -1, -1);
                addr = pick_addr(b, 0, 0);
                off = pick_addr(b, addr, 0);
                if (vr < 0 || addr < 0 || off < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs a vector and two address registers", e->name);
                    return 0;
                }
                load_slot(b, e->line, sp, type_ptr(), addr);
                load_slot(b, e->line, sa, e->a->type, off);
                asm_line(b, e->line, "AGR %%r%d,%%r%d", addr, off);
                asm_line(b, e->line, "VL %%v%d,0(%%r%d)", vr, addr);
                res = alloc_slots(cg, 2);
                store_vec_slot(b, e->line, res, vr);
            } else {
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sc))
                    return 0;
                vr = pick_vr4(b, -1, -1, -1, -1);
                addr = pick_addr(b, 0, 0);
                off = pick_addr(b, addr, 0);
                if (vr < 0 || addr < 0 || off < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs a vector and two address registers", e->name);
                    return 0;
                }
                load_slot(b, e->line, sc, type_ptr(), addr);
                load_slot(b, e->line, sp, e->b->type, off);
                load_vec_slot(b, e->line, sa, vr);
                asm_line(b, e->line, "AGR %%r%d,%%r%d", addr, off);
                asm_line(b, e->line, "VST %%v%d,0(%%r%d)", vr, addr);
                res = alloc_slot(cg);
                asm_line(b, e->line, "LGHI %%r0,0");
                store_slot(b, e->line, res, type_of(32, 0), 0);
            }
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_extend_s64")) {
            int vr, va, tmp;
            int srcbits = e->a->type.vbits;
            if (!eval(cg, e->a, &sa)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            tmp = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || (srcbits != 32 && tmp < 0)) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__vec_extend_s64 needs enough cscratch vector registers");
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            if (srcbits == 8) {
                asm_line(b, e->line, "VUPH %%v%d,%%v%d,0", tmp, va);
                asm_line(b, e->line, "VUPH %%v%d,%%v%d,1", vr, tmp);
                asm_line(b, e->line, "VUPH %%v%d,%%v%d,2", vr, vr);
            } else if (srcbits == 16) {
                asm_line(b, e->line, "VUPH %%v%d,%%v%d,1", vr, va);
                asm_line(b, e->line, "VUPH %%v%d,%%v%d,2", vr, vr);
            } else {
                asm_line(b, e->line, "VUPH %%v%d,%%v%d,2", vr, va);
            }
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_all_eq") || !strcmp(e->name, "__vec_all_eq_8") ||
            !strcmp(e->name, "__vec_all_ne") || !strcmp(e->name, "__vec_all_ne_8") ||
            !strcmp(e->name, "__vec_any_eq") || !strcmp(e->name, "__vec_any_eq_8") ||
            !strcmp(e->name, "__vec_any_ne") || !strcmp(e->name, "__vec_any_ne_8")) {
            int vr, va, vb;
            int elem;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            elem = e->a->type.vbits == 8 ? 0 : e->a->type.vbits == 16 ? 1 :
                   e->a->type.vbits == 32 ? 2 : 3;
            asm_line(b, e->line, "VCEQ %%v%d,%%v%d,%%v%d,%d,1", vr, va, vb, elem);
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            {
                char *out = new_label(b);
                if (!strcmp(e->name, "__vec_all_eq") || !strcmp(e->name, "__vec_all_eq_8")) {
                    asm_line(b, e->line, "CLFI %%r0,0");
                    asm_line(b, e->line, "LHI %%r0,0");
                    asm_line(b, e->line, "BRCL 7,%s", out);
                    asm_line(b, e->line, "LHI %%r0,1");
                } else if (!strcmp(e->name, "__vec_all_ne") || !strcmp(e->name, "__vec_all_ne_8")) {
                    asm_line(b, e->line, "CLFI %%r0,3");
                    asm_line(b, e->line, "LHI %%r0,0");
                    asm_line(b, e->line, "BRCL 7,%s", out);
                    asm_line(b, e->line, "LHI %%r0,1");
                } else if (!strcmp(e->name, "__vec_any_eq") || !strcmp(e->name, "__vec_any_eq_8")) {
                    asm_line(b, e->line, "CLFI %%r0,3");
                    asm_line(b, e->line, "LHI %%r0,1");
                    asm_line(b, e->line, "BRCL 7,%s", out);
                    asm_line(b, e->line, "LHI %%r0,0");
                } else {
                    asm_line(b, e->line, "CLFI %%r0,0");
                    asm_line(b, e->line, "LHI %%r0,1");
                    asm_line(b, e->line, "BRCL 7,%s", out);
                    asm_line(b, e->line, "LHI %%r0,0");
                }
                emit_label(b, e->line, out);
                free(out);
            }
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_addc_u128") || !strcmp(e->name, "__vec_subc_u128") ||
            !strcmp(e->name, "__vec_subec_u128")) {
            int vr, va, vb, vc;
            int hascarry = !strcmp(e->name, "__vec_subec_u128");
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) ||
                (hascarry && !eval(cg, e->c, &sc))) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            vc = pick_vr4(b, vr, va, vb, -1);
            if (vr < 0 || va < 0 || vb < 0 || vc < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs four cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            if (hascarry) {
                load_vec_slot(b, e->line, sc, vc);
                asm_line(b, e->line, "VSBCBI %%v%d,%%v%d,%%v%d,%%v%d,3",
                         vr, va, vb, vc);
            } else {
                asm_line(b, e->line, "%s %%v%d,%%v%d,%%v%d",
                         !strcmp(e->name, "__vec_addc_u128") ? "VACCQ" : "VSCBIQ",
                         vr, va, vb);
            }
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_add_u128") || !strcmp(e->name, "__vec_adde_u128") ||
            !strcmp(e->name, "__vec_sub_u128") || !strcmp(e->name, "__vec_sube_u128")) {
            int vr, va, vb, vc;
            int hascarry = !strcmp(e->name, "__vec_adde_u128") ||
                           !strcmp(e->name, "__vec_sube_u128");
            int sub = !strcmp(e->name, "__vec_sub_u128") ||
                      !strcmp(e->name, "__vec_sube_u128");
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) ||
                (hascarry && !eval(cg, e->c, &sc))) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            vc = pick_vr4(b, vr, va, vb, -1);
            if (vr < 0 || va < 0 || vb < 0 || vc < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs four cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            if (hascarry) load_vec_slot(b, e->line, sc, vc);
            else {
                asm_line(b, e->line, "VGBM %%v%d,0", vc);
            }
            asm_line(b, e->line, "%s %%v%d,%%v%d,%%v%d,%%v%d,3",
                     sub ? "VSBI" : "VAC", vr, va, vb, vc);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_abs") || !strcmp(e->name, "__vec_abs_8") ||
            !strcmp(e->name, "__vec_nabs")) {
            int vr, va, vt;
            if (!eval(cg, e->a, &sa)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vt = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vt < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            if (e->a->type.vfloat) {
                asm_line(b, e->line, "VFPSO %%v%d,%%v%d,%d,0,2", vr, va,
                         e->a->type.vbits == 32 ? 2 : 3);
                if (!strcmp(e->name, "__vec_nabs"))
                    asm_line(b, e->line, "VFPSO %%v%d,%%v%d,%d,0,1", vr, vr,
                             e->a->type.vbits == 32 ? 2 : 3);
            } else {
                int elem = e->a->type.vbits == 8 ? 0 : e->a->type.vbits == 16 ? 1 :
                           e->a->type.vbits == 32 ? 2 : 3;
                asm_line(b, e->line, "VGBM %%v%d,0", vt);
                asm_line(b, e->line, "VS %%v%d,%%v%d,%%v%d,%d", vt, vt, va, elem);
                asm_line(b, e->line, "VMX %%v%d,%%v%d,%%v%d,%d", vr, va, vt, elem);
            }
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_ctd") || !strcmp(e->name, "__vec_ctsl") ||
            !strcmp(e->name, "__vec_ctul")) {
            int vr, va;
            int64_t mode;
            if (!eval(cg, e->a, &sa)) return 0;
            if (!const_eval(e->b, &mode) || mode < 0 || mode > 7) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs a constant rounding mode 0 to 7", e->name);
                return 0;
            }
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            if (vr < 0 || va < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs two cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            asm_line(b, e->line, "%s %%v%d,%%v%d,3,%lld,0",
                     !strcmp(e->name, "__vec_ctd") ? "VCDG" :
                     !strcmp(e->name, "__vec_ctsl") ? "VCGD" : "VCLGD",
                     vr, va, (long long)mode);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_splat_s8_") || !strcmp(e->name, "__vec_splat_s16_") ||
            !strcmp(e->name, "__vec_splat_s32_") || !strcmp(e->name, "__vec_splat_s64_") ||
            !strcmp(e->name, "__vec_splat_u8_") || !strcmp(e->name, "__vec_splat_u16_") ||
            !strcmp(e->name, "__vec_splat_u32_") || !strcmp(e->name, "__vec_splat_u64_")) {
            int vr, elem;
            int64_t imm;
            if (!const_eval(e->a, &imm) || imm < -32768 || imm > 65535) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs a constant immediate", e->name);
                return 0;
            }
            elem = e->type.vbits == 8 ? 0 : e->type.vbits == 16 ? 1 :
                   e->type.vbits == 32 ? 2 : 3;
            vr = pick_vr4(b, -1, -1, -1, -1);
            if (vr < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs a cscratch vector register", e->name);
                return 0;
            }
            asm_line(b, e->line, "VREPI %%v%d,%lld,%d", vr, (long long)imm, elem);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_popcnt")) {
            int vr, va, elem;
            if (!eval(cg, e->a, &sa)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            if (vr < 0 || va < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__vec_popcnt needs two cscratch vector registers");
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            elem = e->a->type.vbits == 8 ? 0 : e->a->type.vbits == 16 ? 1 :
                   e->a->type.vbits == 32 ? 2 : 3;
            asm_line(b, e->line, "VPOPCT %%v%d,%%v%d,%d", vr, va, elem);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strncmp(e->name, "__vec_search_string_cc", 22) ||
            !strncmp(e->name, "__vec_search_string_until_zero_cc", 33)) {
            int va, vb, vc, ra;
            int until = !strncmp(e->name, "__vec_search_string_until_zero_cc", 33);
            int isw = strstr(e->name, "ccw4") != NULL;
            int ish = strstr(e->name, "cch8") != NULL;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) ||
                !eval(cg, e->c, &sc) || !eval(cg, e->d, &sd))
                return 0;
            va = pick_vr4(b, -1, -1, -1, -1);
            vb = pick_vr4(b, va, -1, -1, -1);
            vc = pick_vr4(b, va, vb, -1, -1);
            ra = pick_addr(b, 0, 0);
            if (va < 0 || vb < 0 || vc < 0 || ra < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three vectors and an address register", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            load_vec_slot(b, e->line, sc, vc);
            if (until) {
                asm_line(b, e->line, "%s %%v%d,%%v%d,%%v%d,%%v%d",
                         isw ? "VSTRSZF" : ish ? "VSTRSZH" : "VSTRSZB",
                         va, va, vb, vc);
            } else {
                asm_line(b, e->line, "%s %%v%d,%%v%d,%%v%d,%%v%d,0",
                         isw ? "VSTRSF" : ish ? "VSTRSH" : "VSTRSB",
                         va, va, vb, vc);
            }
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            load_slot(b, e->line, sd, type_ptr(), ra);
            asm_line(b, e->line, "ST %%r0,0(%%r%d)", ra);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, va);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_all_ge") || !strcmp(e->name, "__vec_all_ge_8") ||
            !strcmp(e->name, "__vec_all_gt") || !strcmp(e->name, "__vec_all_gt_8") ||
            !strcmp(e->name, "__vec_all_le") || !strcmp(e->name, "__vec_all_le_8") ||
            !strcmp(e->name, "__vec_all_lt") || !strcmp(e->name, "__vec_all_lt_8") ||
            !strcmp(e->name, "__vec_all_nge") || !strcmp(e->name, "__vec_all_ngt") ||
            !strcmp(e->name, "__vec_all_nle") || !strcmp(e->name, "__vec_all_nlt") ||
            !strcmp(e->name, "__vec_any_ge") || !strcmp(e->name, "__vec_any_ge_8") ||
            !strcmp(e->name, "__vec_any_gt") || !strcmp(e->name, "__vec_any_gt_8") ||
            !strcmp(e->name, "__vec_any_le") || !strcmp(e->name, "__vec_any_le_8") ||
            !strcmp(e->name, "__vec_any_lt") || !strcmp(e->name, "__vec_any_lt_8") ||
            !strcmp(e->name, "__vec_any_nge") || !strcmp(e->name, "__vec_any_ngt") ||
            !strcmp(e->name, "__vec_any_nle") || !strcmp(e->name, "__vec_any_nlt")) {
            int vr, va, vb;
            int isall = !strncmp(e->name, "__vec_all_", 10);
            int isge = strstr(e->name, "ge") != NULL && !strstr(e->name, "nge");
            int isle = strstr(e->name, "le") != NULL && !strstr(e->name, "nle");
            int islt = strstr(e->name, "lt") != NULL && !strstr(e->name, "nlt");
            int target;
            if (strstr(e->name, "nge")) islt = 1;
            if (strstr(e->name, "ngt")) isle = 1;
            if (strstr(e->name, "nlt")) isge = 1;
            char *out;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            if (e->a->type.vfloat) {
                int fmt = e->a->type.vbits == 32 ? 2 : 3;
                if (isge || isle)
                    asm_line(b, e->line, "VFCHE %%v%d,%%v%d,%%v%d,%d,0,1",
                             vr, isle ? vb : va, isle ? va : vb, fmt);
                else
                    asm_line(b, e->line, "VFCH %%v%d,%%v%d,%%v%d,%d,0,1",
                             vr, islt ? vb : va, islt ? va : vb, fmt);
                target = 0;
            } else {
                int elem = e->a->type.vbits == 8 ? 0 : e->a->type.vbits == 16 ? 1 :
                           e->a->type.vbits == 32 ? 2 : 3;
                asm_line(b, e->line, "%s %%v%d,%%v%d,%%v%d,%d,1",
                         e->a->type.vunsig ? "VCHL" : "VCH", vr,
                         (isge || islt) ? vb : va, (isge || islt) ? va : vb, elem);
                target = (isge || isle) && isall ? 3 : 0;
            }
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            out = new_label(b);
            if (isall) {
                asm_line(b, e->line, "CLFI %%r0,%d", target);
                asm_line(b, e->line, "LHI %%r0,0");
                asm_line(b, e->line, "BRCL 7,%s", out);
                asm_line(b, e->line, "LHI %%r0,1");
            } else {
                asm_line(b, e->line, "CLFI %%r0,%d",
                         e->a->type.vfloat ? 3 : (isge || isle ? 0 : 3));
                asm_line(b, e->line, "LHI %%r0,1");
                asm_line(b, e->line, "BRCL 7,%s", out);
                asm_line(b, e->line, "LHI %%r0,0");
            }
            emit_label(b, e->line, out);
            free(out);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_meadd") || !strcmp(e->name, "__vec_mhadd") ||
            !strcmp(e->name, "__vec_mladd") || !strcmp(e->name, "__vec_moadd")) {
            int vr, va, vb, vc;
            int m4 = e->a->type.vbits == 8 ? 0 : e->a->type.vbits == 16 ? 1 :
                     e->a->type.vbits == 32 ? 2 : 3;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sc))
                return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            vc = pick_vr4(b, vr, va, vb, -1);
            if (vr < 0 || va < 0 || vb < 0 || vc < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs four cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            load_vec_slot(b, e->line, sc, vc);
            asm_line(b, e->line, "%s %%v%d,%%v%d,%%v%d,%%v%d,%d",
                     !strcmp(e->name, "__vec_meadd") ? "VMAE" :
                     !strcmp(e->name, "__vec_moadd") ? "VMAO" :
                     !strcmp(e->name, "__vec_mhadd") ? "VMAH" : "VMAL",
                     vr, va, vb, vc, m4);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_madd") || !strcmp(e->name, "__vec_msub") ||
            !strcmp(e->name, "__vec_nmadd") || !strcmp(e->name, "__vec_nmsub")) {
            int vr, va, vb, vc;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sc))
                return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            vc = pick_vr4(b, vr, va, vb, -1);
            if (vr < 0 || va < 0 || vb < 0 || vc < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs four cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            load_vec_slot(b, e->line, sc, vc);
            asm_line(b, e->line, "%s %%v%d,%%v%d,%%v%d,%%v%d,0,%d",
                     !strcmp(e->name, "__vec_madd") ? "VFMA" :
                     !strcmp(e->name, "__vec_msub") ? "VFMS" :
                     !strcmp(e->name, "__vec_nmadd") ? "VFNMA" : "VFNMS",
                     vr, va, vb, vc, e->a->type.vbits == 32 ? 2 : 3);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_mule") || !strcmp(e->name, "__vec_mulh") ||
            !strcmp(e->name, "__vec_mulo")) {
            int vr, va, vb;
            int m4 = e->a->type.vbits == 8 ? 0 : e->a->type.vbits == 16 ? 1 : 2;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            asm_line(b, e->line, "%s %%v%d,%%v%d,%%v%d,%d",
                     !strcmp(e->name, "__vec_mule") ? (e->a->type.vunsig ? "VMLE" : "VME") :
                     !strcmp(e->name, "__vec_mulh") ? (e->a->type.vunsig ? "VMLH" : "VMH") :
                                                       (e->a->type.vunsig ? "VMLO" : "VMO"),
                     vr, va, vb, m4);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_addc") || !strcmp(e->name, "__vec_subc")) {
            int vr, va, vb;
            int elem;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            elem = e->a->type.vbits == 8 ? 0 : e->a->type.vbits == 16 ? 1 :
                   e->a->type.vbits == 32 ? 2 : 3;
            asm_line(b, e->line, "%s %%v%d,%%v%d,%%v%d,%d",
                     !strcmp(e->name, "__vec_addc") ? "VACC" : "VSCBI",
                     vr, va, vb, elem);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_gfmsum")) {
            int vr, va, vb;
            int m4 = e->a->type.vbits == 8 ? 0 : e->a->type.vbits == 16 ? 1 : 2;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__vec_gfmsum needs three cscratch vector registers");
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            if (e->a->type.vbits == 64)
                asm_line(b, e->line, "VGFMG %%v%d,%%v%d,%%v%d", vr, va, vb);
            else
                asm_line(b, e->line, "VGFM %%v%d,%%v%d,%%v%d,%d", vr, va, vb, m4);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_load_pair_s8") || !strcmp(e->name, "__vec_load_pair_u8")) {
            int vr, tmp;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            tmp = alloc_slots(cg, 2);
            if (vr < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs a vector register", e->name);
                return 0;
            }
            load_slot(b, e->line, sa, e->a->type, 0);
            asm_line(b, e->line, "STG %%r0,%ld(%%r%d)", slot_off(b, tmp), b->temp_reg);
            load_slot(b, e->line, sp, e->b->type, 0);
            asm_line(b, e->line, "STG %%r0,%ld(%%r%d)", slot_off(b, tmp + 1), b->temp_reg);
            asm_line(b, e->line, "VL %%v%d,%ld(%%r%d)", vr, slot_off(b, tmp), b->temp_reg);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_bperm")) {
            int vr, va, vb;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__vec_bperm needs three cscratch vector registers");
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            asm_line(b, e->line, "VBPERM %%v%d,%%v%d,%%v%d", vr, va, vb);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_sqrt")) {
            int vr, va;
            if (!eval(cg, e->a, &sa)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            if (vr < 0 || va < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__vec_sqrt needs two cscratch vector registers");
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            asm_line(b, e->line, e->a->type.vbits == 32 ? "VFSQSB %%v%d,%%v%d" :
                                                          "VFSQDB %%v%d,%%v%d", vr, va);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_andc") || !strcmp(e->name, "__vec_andc_8") ||
            !strcmp(e->name, "__vec_nand") || !strcmp(e->name, "__vec_nor") ||
            !strcmp(e->name, "__vec_nor_8") || !strcmp(e->name, "__vec_orc") ||
            !strcmp(e->name, "__vec_eqv")) {
            int vr, va, vb;
            const char *op;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            if (!strcmp(e->name, "__vec_andc") || !strcmp(e->name, "__vec_andc_8")) op = "VNC";
            else if (!strcmp(e->name, "__vec_nand")) op = "VNN";
            else if (!strcmp(e->name, "__vec_nor") || !strcmp(e->name, "__vec_nor_8")) op = "VNO";
            else if (!strcmp(e->name, "__vec_orc")) op = "VOC";
            else op = "VNX";
            asm_line(b, e->line, "%s %%v%d,%%v%d,%%v%d", op, vr, va, vb);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_srdb")) {
            int vr, va, vb;
            int64_t imm;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            if (!const_eval(e->c, &imm) || imm < 0 || imm > 15) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__vec_srdb needs a constant byte count 0 to 15");
                return 0;
            }
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__vec_srdb needs three cscratch vector registers");
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            asm_line(b, e->line, "VSRD %%v%d,%%v%d,%%v%d,%lld", vr, va, vb,
                     (long long)imm * 8);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_sldw")) {
            int vr, va, vb;
            int64_t imm;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            if (!const_eval(e->c, &imm) || imm < 0 || imm > 3) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__vec_sldw needs a constant word count 0 to 3");
                return 0;
            }
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "__vec_sldw needs three cscratch vector registers");
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            asm_line(b, e->line, "VSLDB %%v%d,%%v%d,%%v%d,%lld", vr, va, vb,
                     (long long)imm * 4);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_sld") || !strcmp(e->name, "__vec_sldb")) {
            int vr, va, vb;
            int64_t imm;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            if (!const_eval(e->c, &imm) || imm < 0 ||
                (!strcmp(e->name, "__vec_sld") ? imm > 127 : imm > 15)) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs a constant count in range", e->name);
                return 0;
            }
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            asm_line(b, e->line, !strcmp(e->name, "__vec_sld") ? "VSLD %%v%d,%%v%d,%%v%d,%lld" :
                                                              "VSLDB %%v%d,%%v%d,%%v%d,%lld",
                     vr, va, vb, (long long)imm);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_rl") || !strcmp(e->name, "__vec_rl_8") ||
            !strcmp(e->name, "__vec_rli") || !strcmp(e->name, "__vec_rl_mask")) {
            int vr, va, vb;
            int elem;
            int64_t imm = 0;
            if (!eval(cg, e->a, &sa)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            elem = e->a->type.vbits == 8 ? 0 : e->a->type.vbits == 16 ? 1 :
                   e->a->type.vbits == 32 ? 2 : 3;
            if (!strcmp(e->name, "__vec_rli")) {
                if (!const_eval(e->b, &imm) || imm < 0 || imm > 255) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "__vec_rli needs a constant count 0 to 255");
                    return 0;
                }
                asm_line(b, e->line, "VERLL %%v%d,%%v%d,%lld,%d", vr, va,
                         (long long)imm, elem);
            } else {
                if (!eval(cg, e->b, &sp)) return 0;
                load_vec_slot(b, e->line, sp, vb);
                if (!strcmp(e->name, "__vec_rl_mask")) {
                    if (!const_eval(e->c, &imm) || imm < 0 || imm > 255) {
                        cb_error(b, e->line, e->col, "ZA5005",
                                 "__vec_rl_mask needs a constant mask 0 to 255");
                        return 0;
                    }
                    asm_line(b, e->line, "VERIM %%v%d,%%v%d,%%v%d,%lld,%d",
                             vr, va, vb, (long long)imm, elem);
                } else asm_line(b, e->line, "VERLLV %%v%d,%%v%d,%%v%d,%d", vr, va, vb, elem);
            }
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_sll") || !strcmp(e->name, "__vec_srl") ||
            !strcmp(e->name, "__vec_sral") || !strcmp(e->name, "__vec_slb") ||
            !strcmp(e->name, "__vec_srab")) {
            int vr, va, vb;
            const char *op;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            if (!strcmp(e->name, "__vec_sll")) op = "VSL";
            else if (!strcmp(e->name, "__vec_srl")) op = "VSRL";
            else if (!strcmp(e->name, "__vec_sral")) op = "VSRA";
            else if (!strcmp(e->name, "__vec_slb")) op = "VSLB";
            else op = "VSRAB";
            asm_line(b, e->line, "%s %%v%d,%%v%d,%%v%d", op, vr, va, vb);
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_mergeh") || !strcmp(e->name, "__vec_mergeh_8") ||
            !strcmp(e->name, "__vec_mergel") || !strcmp(e->name, "__vec_mergel_8") ||
            !strcmp(e->name, "__vec_checksum") ||
            !strcmp(e->name, "__vec_cntlz") || !strcmp(e->name, "__vec_cnttz")) {
            int vr, va, vb;
            if (!eval(cg, e->a, &sa) || (e->b && !eval(cg, e->b, &sp))) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || (e->b && vb < 0)) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            if (!strcmp(e->name, "__vec_cntlz") || !strcmp(e->name, "__vec_cnttz")) {
                int elem = e->a->type.vbits == 8 ? 0 : e->a->type.vbits == 16 ? 1 :
                           e->a->type.vbits == 32 ? 2 : 3;
                asm_line(b, e->line, !strcmp(e->name, "__vec_cntlz") ? "VCLZ %%v%d,%%v%d,%d" :
                                                                 "VCTZ %%v%d,%%v%d,%d", vr, va, elem);
            } else if (!strcmp(e->name, "__vec_checksum")) {
                load_vec_slot(b, e->line, sp, vb);
                asm_line(b, e->line, "VCKSM %%v%d,%%v%d,%%v%d", vr, va, vb);
            } else {
                int elem = e->a->type.vbits == 8 ? 0 : e->a->type.vbits == 16 ? 1 :
                           e->a->type.vbits == 32 ? 2 : 3;
                int isl = strstr(e->name, "mergel") != NULL;
                load_vec_slot(b, e->line, sp, vb);
                asm_line(b, e->line, e->a->type.vbits == 64 ?
                         (isl ? "VMRLG %%v%d,%%v%d,%%v%d" : "VMRHG %%v%d,%%v%d,%%v%d") :
                         (isl ? "VMRL %%v%d,%%v%d,%%v%d,%d" : "VMRH %%v%d,%%v%d,%%v%d,%d"),
                         vr, va, vb, elem);
            }
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_pack") || !strcmp(e->name, "__vec_pack_8") ||
            !strcmp(e->name, "__vec_packs") || !strcmp(e->name, "__vec_packs_8") ||
            !strcmp(e->name, "__vec_packs_cc") || !strcmp(e->name, "__vec_packsu") ||
            !strcmp(e->name, "__vec_packsu_8") || !strcmp(e->name, "__vec_packsu_cc") ||
            !strncmp(e->name, "__vec_unpackh_", 14) ||
            !strncmp(e->name, "__vec_unpackl_", 14)) {
            int vr, va, vb, addr;
            int unpack = !strncmp(e->name, "__vec_unpackh_", 14) ||
                         !strncmp(e->name, "__vec_unpackl_", 14);
            int m4 = e->a->type.vbits == 16 ? 1 : e->a->type.vbits == 32 ? 2 : 3;
            if (!eval(cg, e->a, &sa) || (!unpack && !eval(cg, e->b, &sp))) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            if (unpack) {
                int isl = !strncmp(e->name, "__vec_unpackl_", 14);
                int isu = e->name[14] == 'u';
                int srcbits = e->a->type.vbits;
                int sm4 = srcbits == 8 ? 0 : srcbits == 16 ? 1 : 2;
                asm_line(b, e->line, "%s %%v%d,%%v%d,%d",
                         isu ? (isl ? "VUPLL" : "VUPLH") : (isl ? "VUPL" : "VUPH"),
                         vr, va, sm4);
            } else {
                load_vec_slot(b, e->line, sp, vb);
                if (!strcmp(e->name, "__vec_pack") || !strcmp(e->name, "__vec_pack_8"))
                    asm_line(b, e->line, "VPK %%v%d,%%v%d,%%v%d,%d", vr, va, vb, m4);
                else if (!strcmp(e->name, "__vec_packsu") || !strcmp(e->name, "__vec_packsu_8"))
                    asm_line(b, e->line, "VPKLS %%v%d,%%v%d,%%v%d,%d,0", vr, va, vb, m4);
                else if (!strcmp(e->name, "__vec_packsu_cc"))
                    asm_line(b, e->line, "VPKLS %%v%d,%%v%d,%%v%d,%d,1", vr, va, vb, m4);
                else if (!strcmp(e->name, "__vec_packs_cc"))
                    asm_line(b, e->line, "VPKS %%v%d,%%v%d,%%v%d,%d,1", vr, va, vb, m4);
                else
                    asm_line(b, e->line, "VPKS %%v%d,%%v%d,%%v%d,%d,0", vr, va, vb, m4);
            }
            if (e->c) {
                if (!eval(cg, e->c, &sc)) return 0;
                addr = pick_addr(b, 0, 0);
                if (addr < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs an address register", e->name);
                    return 0;
                }
                load_slot(b, e->line, sc, type_ptr(), addr);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                asm_line(b, e->line, "ST %%r0,0(%%r%d)", addr);
            }
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strncmp(e->name, "__vec_extract_", 14) ||
            !strncmp(e->name, "__vec_insert_and_zero_", 22) ||
            !strncmp(e->name, "__vec_insert_", 13) ||
            !strncmp(e->name, "__vec_promote_", 14)) {
            const char *suffix;
            int kind, vr, addr, elem;
            int64_t idx = 0;
            if (!strncmp(e->name, "__vec_extract_", 14)) {
                kind = 0; suffix = e->name + 14;
            } else if (!strncmp(e->name, "__vec_insert_and_zero_", 22)) {
                kind = 3; suffix = e->name + 22;
            } else if (!strncmp(e->name, "__vec_insert_", 13)) {
                kind = 1; suffix = e->name + 13;
            } else {
                kind = 2; suffix = e->name + 14;
            }
            elem = vec_suffix_code(suffix);
            if (kind == 3) {
                if (!eval(cg, e->a, &sa)) return 0;
                vr = pick_vr4(b, -1, -1, -1, -1);
                addr = pick_addr(b, 0, 0);
                if (vr < 0 || addr < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs a vector and an address register", e->name);
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), addr);
                asm_line(b, e->line, "VLLEZ %%v%d,0(%%r%d),%d", vr, addr, elem);
                res = alloc_slots(cg, 2);
                store_vec_slot(b, e->line, res, vr);
                *slotp = res;
                return 1;
            }
            if (kind == 0) {
                const char *op = elem == 0 ? "VSTEB" : elem == 1 ? "VSTEH" :
                                 elem == 2 ? "VSTEF" : "VSTEG";
                if (!const_eval(e->b, &idx) || idx < 0 || idx > 15) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "__vec_extract needs a constant index 0 to 15");
                    return 0;
                }
                if (!eval(cg, e->a, &sa)) return 0;
                vr = pick_vr4(b, -1, -1, -1, -1);
                addr = pick_addr(b, 0, 0);
                res = alloc_slot(cg);
                if (vr < 0 || addr < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "__vec_extract needs a vector and an address register");
                    return 0;
                }
                load_vec_slot(b, e->line, sa, vr);
                asm_line(b, e->line, "LA %%r%d,%ld(%%r%d)", addr, slot_off(b, res), b->temp_reg);
                asm_line(b, e->line, "%s %%v%d,0(%%r%d),%lld", op, vr, addr, (long long)idx);
                if (e->type.flt) {
                    asm_line(b, e->line, e->type.bits == 32 ? "LE %%f0,%ld(%%r%d)" : "LD %%f0,%ld(%%r%d)",
                             slot_off(b, res), b->temp_reg);
                    store_fp_slot(b, e->line, res, e->type, 0);
                } else if (e->type.bits == 64) {
                    asm_line(b, e->line, "LG %%r0,%ld(%%r%d)", slot_off(b, res), b->temp_reg);
                    store_slot(b, e->line, res, e->type, 0);
                } else {
                    asm_line(b, e->line, elem == 0 ? "LGB %%r0,%ld(%%r%d)" :
                             elem == 1 ? "LGH %%r0,%ld(%%r%d)" : "LGF %%r0,%ld(%%r%d)",
                             slot_off(b, res), b->temp_reg);
                    store_slot(b, e->line, res, e->type, 0);
                }
                *slotp = res;
                return 1;
            }
            if (!const_eval(kind == 1 ? e->c : e->b, &idx) || idx < 0 || idx > 15) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs a constant index 0 to 15", e->name);
                return 0;
            }
            if (kind == 1) {
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
                vr = pick_vr4(b, -1, -1, -1, -1);
                addr = pick_addr(b, 0, 0);
                if (vr < 0 || addr < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "__vec_insert needs a vector and an address register");
                    return 0;
                }
                load_vec_slot(b, e->line, sp, vr);
                asm_line(b, e->line, "LA %%r%d,%ld(%%r%d)", addr, slot_off(b, sa), b->temp_reg);
                asm_line(b, e->line, "VLE%s %%v%d,0(%%r%d),%lld",
                         elem == 0 ? "B" : elem == 1 ? "H" : elem == 2 ? "F" : "G",
                         vr, addr, (long long)idx);
            } else {
                if (!eval(cg, e->a, &sa)) return 0;
                vr = pick_vr4(b, -1, -1, -1, -1);
                addr = pick_addr(b, 0, 0);
                if (vr < 0 || addr < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "__vec_promote needs a vector and an address register");
                    return 0;
                }
                asm_line(b, e->line, "VGBM %%v%d,0", vr);
                asm_line(b, e->line, "LA %%r%d,%ld(%%r%d)", addr, slot_off(b, sa), b->temp_reg);
                asm_line(b, e->line, "VLE%s %%v%d,0(%%r%d),%lld",
                         elem == 0 ? "B" : elem == 1 ? "H" : elem == 2 ? "F" : "G",
                         vr, addr, (long long)idx);
            }
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_max") || !strcmp(e->name, "__vec_max_8") ||
            !strcmp(e->name, "__vec_min") || !strcmp(e->name, "__vec_min_8") ||
            !strcmp(e->name, "__vec_avg")) {
            int vr, va, vb;
            int elem, ismax, isavg, isunsigned;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            elem = e->a->type.vbits == 8 ? 0 : e->a->type.vbits == 16 ? 1 :
                   e->a->type.vbits == 32 ? 2 : 3;
            ismax = !strcmp(e->name, "__vec_max") || !strcmp(e->name, "__vec_max_8");
            isavg = !strcmp(e->name, "__vec_avg");
            isunsigned = e->a->type.vunsig;
            if (e->a->type.vfloat) {
                if (isavg)
                    asm_line(b, e->line, e->a->type.vbits == 32 ? "VAVGF %%v%d,%%v%d,%%v%d" :
                                                                "VAVGG %%v%d,%%v%d,%%v%d", vr, va, vb);
                else if (ismax)
                    asm_line(b, e->line, e->a->type.vbits == 32 ? "VMXF %%v%d,%%v%d,%%v%d" :
                                                                "VMXG %%v%d,%%v%d,%%v%d", vr, va, vb);
                else
                    asm_line(b, e->line, e->a->type.vbits == 32 ? "VMNF %%v%d,%%v%d,%%v%d" :
                                                                "VMNG %%v%d,%%v%d,%%v%d", vr, va, vb);
            } else {
                asm_line(b, e->line, "%s %%v%d,%%v%d,%%v%d,%d",
                         isavg ? (isunsigned ? "VAVGL" : "VAVG") :
                         ismax ? (isunsigned ? "VMXL" : "VMX") :
                                 (isunsigned ? "VMNL" : "VMN"), vr, va, vb, elem);
            }
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_cmpge") || !strcmp(e->name, "__vec_cmpge_8") ||
            !strcmp(e->name, "__vec_cmpgt") || !strcmp(e->name, "__vec_cmpgt_8") ||
            !strcmp(e->name, "__vec_cmple") || !strcmp(e->name, "__vec_cmple_8") ||
            !strcmp(e->name, "__vec_cmplt") || !strcmp(e->name, "__vec_cmplt_8")) {
            int vr, va, vb, vzero;
            int elem, invert, swap;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            vzero = e->a->type.vfloat ? -1 : pick_vr4(b, vr, va, vb, -1);
            if (vr < 0 || va < 0 || vb < 0 || (!e->a->type.vfloat && vzero < 0)) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three or four cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            swap = !strcmp(e->name, "__vec_cmplt") || !strcmp(e->name, "__vec_cmplt_8") ||
                   !strcmp(e->name, "__vec_cmple") || !strcmp(e->name, "__vec_cmple_8");
            if (e->a->type.vfloat) {
                int isge = !strcmp(e->name, "__vec_cmpge") ||
                           !strcmp(e->name, "__vec_cmpge_8") ||
                           !strcmp(e->name, "__vec_cmple") ||
                           !strcmp(e->name, "__vec_cmple_8");
                asm_line(b, e->line, "%s %%v%d,%%v%d,%%v%d,%d,0,0",
                         isge ? "VFCHE" : "VFCH", vr,
                         swap ? vb : va, swap ? va : vb,
                         e->a->type.vbits == 32 ? 2 : 3);
            } else {
                elem = e->a->type.vbits == 8 ? 0 : e->a->type.vbits == 16 ? 1 :
                       e->a->type.vbits == 32 ? 2 : 3;
                invert = !strcmp(e->name, "__vec_cmpge") || !strcmp(e->name, "__vec_cmpge_8") ||
                         !strcmp(e->name, "__vec_cmple") || !strcmp(e->name, "__vec_cmple_8");
                asm_line(b, e->line, "%s %%v%d,%%v%d,%%v%d,%d,0",
                         e->a->type.vunsig ? "VCHL" : "VCH", vr,
                         swap ? vb : va, swap ? va : vb, elem);
                if (invert) {
                    asm_line(b, e->line, "VGBM %%v%d,0", vzero);
                    asm_line(b, e->line, "VNO %%v%d,%%v%d,%%v%d", vr, vr, vzero);
                }
            }
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_cmpeq") || !strcmp(e->name, "__vec_cmpeq_8")) {
            int vr, va, vb;
            int elem;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            if (vr < 0 || va < 0 || vb < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs three cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            if (e->a->type.vfloat)
                asm_line(b, e->line, "VFCE %%v%d,%%v%d,%%v%d,%d,0,0", vr, va, vb,
                         e->a->type.vbits == 32 ? 2 : 3);
            else {
                elem = e->a->type.vbits == 8 ? 0 : e->a->type.vbits == 16 ? 1 :
                       e->a->type.vbits == 32 ? 2 : 3;
                asm_line(b, e->line, "VCEQ %%v%d,%%v%d,%%v%d,%d,0", vr, va, vb, elem);
            }
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__vec_sel") || !strcmp(e->name, "__vec_sel_8") ||
            !strcmp(e->name, "__vec_perm") || !strcmp(e->name, "__vec_permi")) {
            int vr, va, vb, vm;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sc))
                return 0;
            vr = pick_vr4(b, -1, -1, -1, -1);
            va = pick_vr4(b, vr, -1, -1, -1);
            vb = pick_vr4(b, vr, va, -1, -1);
            vm = pick_vr4(b, vr, va, vb, -1);
            if (vr < 0 || va < 0 || vb < 0 || vm < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs four cscratch vector registers", e->name);
                return 0;
            }
            load_vec_slot(b, e->line, sa, va);
            load_vec_slot(b, e->line, sp, vb);
            if (!strcmp(e->name, "__vec_sel") || !strcmp(e->name, "__vec_sel_8")) {
                load_vec_slot(b, e->line, sc, vm);
                asm_line(b, e->line, "VSEL %%v%d,%%v%d,%%v%d,%%v%d", vr, va, vb, vm);
            } else if (!strcmp(e->name, "__vec_perm")) {
                load_vec_slot(b, e->line, sc, vm);
                asm_line(b, e->line, "VPERM %%v%d,%%v%d,%%v%d,%%v%d", vr, va, vb, vm);
            } else {
                int64_t imm;
                if (!const_eval(e->c, &imm) || imm < 0 || imm > 15) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "__vec_permi needs a constant selector 0 to 15");
                    return 0;
                }
                asm_line(b, e->line, "VPDI %%v%d,%%v%d,%%v%d,%lld", vr, va, vb,
                         (long long)imm);
            }
            res = alloc_slots(cg, 2);
            store_vec_slot(b, e->line, res, vr);
            *slotp = res;
            return 1;
        }
        if (!strncmp(e->name, "__vec_load_len_", 15) ||
            !strncmp(e->name, "__vec_load_bndry_", 17) ||
            !strncmp(e->name, "__vec_splats_", 13) ||
            !strcmp(e->name, "__vec_store_len") || !strcmp(e->name, "__vec_store_len_r")) {
            int vr, addr, len;
            if (!strncmp(e->name, "__vec_splats_", 13)) {
                int elem = vec_suffix_code(e->name + 13);
                if (!eval(cg, e->a, &sa)) return 0;
                vr = pick_vr(b, -1, -1);
                addr = pick_addr(b, 0, 0);
                if (vr < 0 || addr < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "a vector splat needs a vector and an address register");
                    return 0;
                }
                asm_line(b, e->line, "LA %%r%d,%ld(%%r%d)", addr, slot_off(b, sa), b->temp_reg);
                asm_line(b, e->line, "VLREP %%v%d,0(%%r%d),%d", vr, addr, elem);
                res = alloc_slots(cg, 2);
                store_vec_slot(b, e->line, res, vr);
                *slotp = res;
                return 1;
            }
            if (!strncmp(e->name, "__vec_load_bndry_", 17)) {
                if (!eval(cg, e->a, &sa)) return 0;
                vr = pick_vr(b, -1, -1);
                addr = pick_addr(b, 0, 0);
                if (vr < 0 || addr < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "vector load needs a vector and an address register");
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), addr);
                asm_line(b, e->line, "VL %%v%d,0(%%r%d)", vr, addr);
                res = alloc_slots(cg, 2);
                store_vec_slot(b, e->line, res, vr);
                *slotp = res;
                return 1;
            }
            if (!strncmp(e->name, "__vec_load_len_", 15)) {
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sl)) return 0;
                vr = pick_vr(b, -1, -1);
                addr = pick_addr(b, 0, 0);
                len = pick_addr(b, addr, 0);
                if (vr < 0 || addr < 0 || len < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "vector load needs a vector and two address registers");
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), addr);
                load_slot(b, e->line, sl, e->b->type, len);
                asm_line(b, e->line, "VLL %%v%d,%%r%d,0(%%r%d)", vr, len, addr);
                res = alloc_slots(cg, 2);
                store_vec_slot(b, e->line, res, vr);
                *slotp = res;
                return 1;
            }
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sl))
                return 0;
            vr = pick_vr(b, -1, -1);
            addr = pick_addr(b, 0, 0);
            len = pick_addr(b, addr, 0);
            if (vr < 0 || addr < 0 || len < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "vector store needs a vector and two address registers");
                return 0;
            }
            load_slot(b, e->line, sp, type_ptr(), addr);
            load_slot(b, e->line, sl, e->c->type, len);
            load_vec_slot(b, e->line, sa, vr);
            asm_line(b, e->line, "VSTL %%v%d,%%r%d,0(%%r%d)", vr, len, addr);
            res = alloc_slot(cg);
            asm_line(b, e->line, "LGHI %%r0,0");
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__lper") || !strcmp(e->name, "__lpdr") ||
            !strcmp(e->name, "__lner") || !strcmp(e->name, "__lndr") ||
            !strcmp(e->name, "__sqer") || !strcmp(e->name, "__sqdr") ||
            !strcmp(e->name, "__fier") || !strcmp(e->name, "__fidr") ||
            !strcmp(e->name, "__fixr") || !strcmp(e->name, "__cfer") ||
            !strcmp(e->name, "__cfdr") || !strcmp(e->name, "__cfxr") ||
            !strcmp(e->name, "__lpxr") || !strcmp(e->name, "__lnxr") ||
            !strcmp(e->name, "__sqxr") || !strcmp(e->name, "__cger") ||
            !strcmp(e->name, "__cgdr") || !strcmp(e->name, "__cgxr")) {
            int fa, ra, sb = -1, sc = -1;
            int64_t m = 0;
            if (!strcmp(e->name, "__lper") || !strcmp(e->name, "__lpdr") ||
                !strcmp(e->name, "__lner") || !strcmp(e->name, "__lndr")) {
                int isd = !strcmp(e->name, "__lpdr") || !strcmp(e->name, "__lndr");
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sb)) return 0;
                fa = pick_fpr(b, -1, -1);
                ra = pick_addr(b, 0, 0);
                if (fa < 0 || ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs one FPR and one address register", e->name);
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), ra);
                load_fp_slot(b, e->line, sb, e->b->type, fa);
                asm_line(b, e->line, !strcmp(e->name, "__lper") ? "LPER %%f%d,%%f%d" :
                                       !strcmp(e->name, "__lpdr") ? "LPDR %%f%d,%%f%d" :
                                       !strcmp(e->name, "__lner") ? "LNER %%f%d,%%f%d" :
                                                                    "LNDR %%f%d,%%f%d", fa, fa);
                asm_line(b, e->line, isd ? "STD %%f%d,0(%%r%d)" : "STE %%f%d,0(%%r%d)", fa, ra);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__sqer") || !strcmp(e->name, "__sqdr") ||
                !strcmp(e->name, "__sqxr")) {
                int isx = !strcmp(e->name, "__sqxr");
                if (!eval(cg, e->a, &sa)) return 0;
                fa = isx ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
                if (fa < 0) {
                    cb_error(b, e->line, e->col, "ZA5005", "%s needs an FPR%s", e->name,
                             isx ? " pair" : "");
                    return 0;
                }
                load_fp_slot(b, e->line, sa, e->a->type, fa);
                asm_line(b, e->line, isx ? "SQXR %%f%d,%%f%d" :
                         !strcmp(e->name, "__sqer") ? "SQER %%f%d,%%f%d" : "SQDR %%f%d,%%f%d", fa, fa);
                res = alloc_slots(cg, isx ? 2 : 1);
                store_fp_slot(b, e->line, res, e->type, fa);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__fier") || !strcmp(e->name, "__fidr") ||
                !strcmp(e->name, "__fixr")) {
                int isx = !strcmp(e->name, "__fixr");
                if (!eval(cg, e->a, &sa)) return 0;
                fa = isx ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
                if (fa < 0) {
                    cb_error(b, e->line, e->col, "ZA5005", "%s needs an FPR%s", e->name,
                             isx ? " pair" : "");
                    return 0;
                }
                load_fp_slot(b, e->line, sa, e->a->type, fa);
                asm_line(b, e->line, isx ? "FIXR %%f%d,%%f%d" :
                         !strcmp(e->name, "__fier") ? "FIER %%f%d,%%f%d" : "FIDR %%f%d,%%f%d", fa, fa);
                res = alloc_slots(cg, isx ? 2 : 1);
                store_fp_slot(b, e->line, res, e->type, fa);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__lpxr") || !strcmp(e->name, "__lnxr")) {
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sb)) return 0;
                fa = pick_fpr_pair(b, -1, -1);
                ra = pick_addr(b, 0, 0);
                if (fa < 0 || ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs an FPR pair and an address register", e->name);
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), ra);
                load_fp_slot(b, e->line, sb, e->b->type, fa);
                asm_line(b, e->line, !strcmp(e->name, "__lpxr") ? "LPXR %%f%d,%%f%d" :
                                                              "LNXR %%f%d,%%f%d", fa, fa);
                store_fp_mem(b, e->line, e->b->type, fa, ra);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!const_eval(e->b, &m) || m < 0 || m > 15) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs a constant rounding mode 0 to 15", e->name);
                return 0;
            }
            if (!eval(cg, e->a, &sa) || !eval(cg, e->c, &sc)) return 0;
            fa = (!strcmp(e->name, "__cfxr") || !strcmp(e->name, "__cgxr"))
                     ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
            ra = pick_addr(b, 0, 0);
            if (fa < 0 || ra < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "%s needs one FPR%s and one address register", e->name,
                         (!strcmp(e->name, "__cfxr") || !strcmp(e->name, "__cgxr")) ? " pair" : "");
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), ra);
            load_fp_slot(b, e->line, sc, e->c->type, fa);
            if (!strcmp(e->name, "__cfer")) asm_line(b, e->line, "CFER %%r0,%lld,%%f%d", (long long)m, fa);
            else if (!strcmp(e->name, "__cfdr")) asm_line(b, e->line, "CFDR %%r0,%lld,%%f%d", (long long)m, fa);
            else if (!strcmp(e->name, "__cfxr")) asm_line(b, e->line, "CFXR %%r0,%lld,%%f%d", (long long)m, fa);
            else if (!strcmp(e->name, "__cger")) asm_line(b, e->line, "CGER %%r0,%lld,%%f%d", (long long)m, fa);
            else if (!strcmp(e->name, "__cgdr")) asm_line(b, e->line, "CGDR %%r0,%lld,%%f%d", (long long)m, fa);
            else asm_line(b, e->line, "CGXR %%r0,%lld,%%f%d", (long long)m, fa);
            asm_line(b, e->line, (!strcmp(e->name, "__cger") || !strcmp(e->name, "__cgdr") ||
                                  !strcmp(e->name, "__cgxr")) ? "STG %%r0,0(%%r%d)" : "ST %%r0,0(%%r%d)", ra);
            asm_line(b, e->line, "IPM %%r0");
            asm_line(b, e->line, "SRL %%r0,28");
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__efpc") || !strcmp(e->name, "__sfpc") ||
            !strcmp(e->name, "__fiebr") || !strcmp(e->name, "__fidbr") ||
            !strcmp(e->name, "__lpebr") || !strcmp(e->name, "__lpdbr") ||
            !strcmp(e->name, "__lnebr") || !strcmp(e->name, "__lndbr") ||
            !strcmp(e->name, "__sqebr") || !strcmp(e->name, "__sqdbr") ||
            !strcmp(e->name, "__cfebr") || !strcmp(e->name, "__cfdbr") ||
            !strcmp(e->name, "__cgebr") || !strcmp(e->name, "__cgdbr") ||
            !strcmp(e->name, "__maebr") || !strcmp(e->name, "__madbr") ||
            !strcmp(e->name, "__msebr") || !strcmp(e->name, "__msdbr") ||
            !strcmp(e->name, "__fmadd") || !strcmp(e->name, "__fmadds") ||
            !strcmp(e->name, "__fmsub") || !strcmp(e->name, "__fmsubs") ||
            !strcmp(e->name, "__sqxbr") || !strcmp(e->name, "__fixbr") ||
            !strcmp(e->name, "__lpxbr") || !strcmp(e->name, "__lnxbr") ||
            !strcmp(e->name, "__cfxbr") || !strcmp(e->name, "__cgxbr") ||
            !strcmp(e->name, "__tceb") || !strcmp(e->name, "__tcdb") ||
            !strcmp(e->name, "__tcxb") || !strcmp(e->name, "__srnm") ||
            !strcmp(e->name, "__thder") || !strcmp(e->name, "__thdr") ||
            !strcmp(e->name, "__tbedr") || !strcmp(e->name, "__tbdr") ||
            !strcmp(e->name, "__diebr") || !strcmp(e->name, "__didbr") ||
            !strcmp(e->name, "__cdzt") || !strcmp(e->name, "__cxzt") ||
            !strcmp(e->name, "__czdt") || !strcmp(e->name, "__czxt")) {
            int fa, fb, fc, ra, rb, sb = -1, sc = -1, sd = -1;
            int64_t m = 0;
            if (!strcmp(e->name, "__efpc")) {
                asm_line(b, e->line, "EFPC %%r0");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__sfpc")) {
                if (!eval(cg, e->a, &sa)) return 0;
                load_slot(b, e->line, sa, e->a->type, 0);
                asm_line(b, e->line, "SFPC %%r0");
                res = alloc_slot(cg);
                asm_line(b, e->line, "LGHI %%r0,0");
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__fiebr") || !strcmp(e->name, "__fidbr")) {
                if (!const_eval(e->a, &m) || m < 0 || m > 7) {
                    cb_error(b, e->line, e->col, "ZA5005", "%s needs a constant rounding mode 0 to 7",
                             e->name);
                    return 0;
                }
                fa = pick_fpr(b, -1, -1);
                fb = pick_fpr(b, fa, -1);
                if (fa < 0 || fb < 0 || !load_fp_expr(cg, e->b, fb)) {
                    cb_error(b, e->line, e->col, "ZA5005", "%s needs two cscratch FPRs", e->name);
                    return 0;
                }
                asm_line(b, e->line, !strcmp(e->name, "__fiebr") ? "FIEBR %%f%d,%lld,%%f%d" :
                                                              "FIDBR %%f%d,%lld,%%f%d",
                         fa, (long long)m, fb);
                res = alloc_slot(cg);
                store_fp_slot(b, e->line, res, e->type, fa);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__lpebr") || !strcmp(e->name, "__lpdbr") ||
                !strcmp(e->name, "__lnebr") || !strcmp(e->name, "__lndbr")) {
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sb)) return 0;
                fa = pick_fpr(b, -1, -1);
                fb = pick_fpr(b, fa, -1);
                ra = pick_addr(b, 0, 0);
                if (fa < 0 || fb < 0 || ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005", "%s needs two FPRs and one address register",
                             e->name);
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), ra);
                load_fp_slot(b, e->line, sb, e->b->type, fb);
                asm_line(b, e->line, !strcmp(e->name, "__lpebr") ? "LPEBR %%f%d,%%f%d" :
                                       !strcmp(e->name, "__lpdbr") ? "LPDBR %%f%d,%%f%d" :
                                       !strcmp(e->name, "__lnebr") ? "LNEBR %%f%d,%%f%d" :
                                                                     "LNDBR %%f%d,%%f%d", fa, fb);
                asm_line(b, e->line, !strcmp(e->name, "__lpebr") || !strcmp(e->name, "__lnebr")
                                       ? "STE %%f%d,0(%%r%d)" : "STD %%f%d,0(%%r%d)", fa, ra);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__cdzt") || !strcmp(e->name, "__cxzt")) {
                int isx = !strcmp(e->name, "__cxzt");
                int64_t len, mask;
                if (!const_eval(e->b, &len) || len < 0 || len > 255 ||
                    !const_eval(e->c, &mask) || mask < 0 || mask > 15) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs constant length 0 to 255 and mask 0 to 15", e->name);
                    return 0;
                }
                if (!eval(cg, e->a, &sa)) return 0;
                fa = isx ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
                ra = pick_addr(b, 0, 0);
                if (fa < 0 || ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs one FPR%s and one address register", e->name,
                             isx ? " pair" : "");
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), ra);
                asm_line(b, e->line, isx ? "CXZT %%f%d,0(%lld,%%r%d),%lld" :
                                            "CDZT %%f%d,0(%lld,%%r%d),%lld",
                         fa, (long long)len, ra, (long long)mask);
                res = alloc_slots(cg, isx ? 2 : 1);
                store_fp_slot(b, e->line, res, e->type, fa);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__czdt") || !strcmp(e->name, "__czxt")) {
                int isx = !strcmp(e->name, "__czxt");
                int64_t len, mask;
                if (!const_eval(e->c, &len) || len < 0 || len > 255 ||
                    !const_eval(e->d, &mask) || mask < 0 || mask > 15) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs constant length 0 to 255 and mask 0 to 15", e->name);
                    return 0;
                }
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
                fa = isx ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
                ra = pick_addr(b, 0, 0);
                if (fa < 0 || ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs one FPR%s and one address register", e->name,
                             isx ? " pair" : "");
                    return 0;
                }
                load_slot(b, e->line, sp, type_ptr(), ra);
                load_fp_slot(b, e->line, sa, e->a->type, fa);
                asm_line(b, e->line, isx ? "CZXT %%f%d,0(%lld,%%r%d),%lld" :
                                            "CZDT %%f%d,0(%lld,%%r%d),%lld",
                         fa, (long long)len, ra, (long long)mask);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__thder") || !strcmp(e->name, "__thdr")) {
                int isl = !strcmp(e->name, "__thdr");
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sb)) return 0;
                fa = pick_fpr(b, -1, -1);
                ra = pick_addr(b, 0, 0);
                if (fa < 0 || ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs one FPR and one address register", e->name);
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), ra);
                load_fp_slot(b, e->line, sb, e->b->type, fa);
                asm_line(b, e->line, isl ? "THDR %%f%d,%%f%d" : "THDER %%f%d,%%f%d", fa, fa);
                asm_line(b, e->line, isl ? "STD %%f%d,0(%%r%d)" : "STE %%f%d,0(%%r%d)", fa, ra);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__tbedr") || !strcmp(e->name, "__tbdr")) {
                int isl = !strcmp(e->name, "__tbdr");
                if (!const_eval(e->b, &m) || m < 0 || m > 15) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs a constant rounding mode 0 to 15", e->name);
                    return 0;
                }
                if (!eval(cg, e->a, &sa) || !eval(cg, e->c, &sc)) return 0;
                fa = pick_fpr(b, -1, -1);
                ra = pick_addr(b, 0, 0);
                if (fa < 0 || ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs one FPR and one address register", e->name);
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), ra);
                load_fp_slot(b, e->line, sc, e->c->type, fa);
                asm_line(b, e->line, isl ? "TBDR %%f%d,%lld,%%f%d" : "TBEDR %%f%d,%lld,%%f%d",
                         fa, (long long)m, fa);
                asm_line(b, e->line, isl ? "STD %%f%d,0(%%r%d)" : "STE %%f%d,0(%%r%d)", fa, ra);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__diebr") || !strcmp(e->name, "__didbr")) {
                int isl = !strcmp(e->name, "__didbr");
                if (!const_eval(e->arg5, &m) || m < 0 || m > 15) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs a constant rounding mode 0 to 15", e->name);
                    return 0;
                }
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) ||
                    !eval(cg, e->c, &sc) || !eval(cg, e->d, &sd))
                    return 0;
                fa = pick_fpr(b, -1, -1);
                fb = pick_fpr(b, fa, -1);
                fc = pick_fpr(b, fa, fb);
                ra = pick_addr(b, 0, 0);
                rb = pick_addr(b, ra, 0);
                if (fa < 0 || fb < 0 || fc < 0 || ra < 0 || rb < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs three FPRs and two address registers", e->name);
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), ra);
                load_slot(b, e->line, sp, type_ptr(), rb);
                load_fp_slot(b, e->line, sc, e->c->type, fa);
                load_fp_slot(b, e->line, sd, e->d->type, fb);
                asm_line(b, e->line, isl ? "DIDBR %%f%d,%%f%d,%%f%d,%lld" :
                                            "DIEBR %%f%d,%%f%d,%%f%d,%lld",
                         fa, fc, fb, (long long)m);
                asm_line(b, e->line, isl ? "STD %%f%d,0(%%r%d)" : "STE %%f%d,0(%%r%d)", fa, ra);
                asm_line(b, e->line, isl ? "STD %%f%d,0(%%r%d)" : "STE %%f%d,0(%%r%d)", fc, rb);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__srnm")) {
                if (!eval(cg, e->a, &sa)) return 0;
                ra = pick_addr(b, 0, 0);
                if (ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005", "__srnm needs a nonzero address register");
                    return 0;
                }
                load_slot(b, e->line, sa, e->a->type, ra);
                asm_line(b, e->line, "SRNM 0(%%r%d)", ra);
                res = alloc_slot(cg);
                asm_line(b, e->line, "LGHI %%r0,0");
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__tceb") || !strcmp(e->name, "__tcdb") ||
                !strcmp(e->name, "__tcxb")) {
                int isx = !strcmp(e->name, "__tcxb");
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sb)) return 0;
                fa = isx ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
                ra = pick_addr(b, 0, 0);
                if (fa < 0 || ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs one FPR%s and one address register", e->name,
                             isx ? " pair" : "");
                    return 0;
                }
                load_fp_slot(b, e->line, sa, e->a->type, fa);
                load_slot(b, e->line, sb, e->b->type, ra);
                asm_line(b, e->line, isx ? "TCXB %%f%d,0(%%r%d)" :
                         !strcmp(e->name, "__tceb") ? "TCEB %%f%d,0(%%r%d)" :
                                                       "TCDB %%f%d,0(%%r%d)", fa, ra);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__sqxbr")) {
                fa = pick_fpr_pair(b, -1, -1);
                if (fa < 0 || !eval(cg, e->a, &sa)) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "__sqxbr needs a cscratch FPR pair");
                    return 0;
                }
                load_fp_from_slot(b, e->line, sa, e->a->type, fa, 128);
                asm_line(b, e->line, "SQXBR %%f%d,%%f%d", fa, fa);
                res = alloc_slots(cg, 2);
                store_fp_slot(b, e->line, res, e->type, fa);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__fixbr")) {
                if (!const_eval(e->a, &m) || m < 0 || m > 7) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "__fixbr needs a constant rounding mode 0 to 7");
                    return 0;
                }
                fa = pick_fpr_pair(b, -1, -1);
                fb = pick_fpr_pair(b, fa, -1);
                if (fa < 0 || fb < 0 || !eval(cg, e->b, &sb)) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "__fixbr needs two cscratch FPR pairs");
                    return 0;
                }
                load_fp_from_slot(b, e->line, sb, e->b->type, fb, 128);
                asm_line(b, e->line, "FIXBR %%f%d,%lld,%%f%d", fa, (long long)m, fb);
                res = alloc_slots(cg, 2);
                store_fp_slot(b, e->line, res, e->type, fa);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__lpxbr") || !strcmp(e->name, "__lnxbr")) {
                if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sb)) return 0;
                fa = pick_fpr_pair(b, -1, -1);
                ra = pick_addr(b, 0, 0);
                if (fa < 0 || ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs a cscratch FPR pair and an address register", e->name);
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), ra);
                load_fp_from_slot(b, e->line, sb, e->b->type, fa, 128);
                asm_line(b, e->line, !strcmp(e->name, "__lpxbr") ? "LPXBR %%f%d,%%f%d" :
                                                              "LNXBR %%f%d,%%f%d", fa, fa);
                store_fp_mem(b, e->line, e->b->type, fa, ra);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__cfxbr") || !strcmp(e->name, "__cgxbr")) {
                int is64 = !strcmp(e->name, "__cgxbr");
                if (!const_eval(e->b, &m) || m < 0 || m > 15) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs a constant rounding mode 0 to 15", e->name);
                    return 0;
                }
                if (!eval(cg, e->a, &sa) || !eval(cg, e->c, &sc)) return 0;
                fa = pick_fpr_pair(b, -1, -1);
                ra = pick_addr(b, 0, 0);
                if (fa < 0 || ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005",
                             "%s needs a cscratch FPR pair and an address register", e->name);
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), ra);
                load_fp_from_slot(b, e->line, sc, e->c->type, fa, 128);
                asm_line(b, e->line, is64 ? "CGXBR %%r0,%lld,%%f%d" : "CFXBR %%r0,%lld,%%f%d",
                         (long long)m, fa);
                asm_line(b, e->line, is64 ? "STG %%r0,0(%%r%d)" : "ST %%r0,0(%%r%d)", ra);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__sqebr") || !strcmp(e->name, "__sqdbr")) {
                fa = pick_fpr(b, -1, -1);
                fb = pick_fpr(b, fa, -1);
                if (fa < 0 || fb < 0 || !load_fp_expr(cg, e->a, fb)) {
                    cb_error(b, e->line, e->col, "ZA5005", "%s needs two cscratch FPRs", e->name);
                    return 0;
                }
                asm_line(b, e->line, !strcmp(e->name, "__sqebr") ? "SQEBR %%f%d,%%f%d" :
                                                              "SQDBR %%f%d,%%f%d", fa, fb);
                res = alloc_slot(cg);
                store_fp_slot(b, e->line, res, e->type, fa);
                *slotp = res;
                return 1;
            }
            if (!strcmp(e->name, "__cfebr") || !strcmp(e->name, "__cfdbr") ||
                !strcmp(e->name, "__cgebr") || !strcmp(e->name, "__cgdbr")) {
                int is64 = !strcmp(e->name, "__cgebr") || !strcmp(e->name, "__cgdbr");
                int isshort = !strcmp(e->name, "__cfebr") || !strcmp(e->name, "__cgebr");
                if (!const_eval(e->b, &m) || m < 0 || m > 15) {
                    cb_error(b, e->line, e->col, "ZA5005", "%s needs a constant rounding mode 0 to 15",
                             e->name);
                    return 0;
                }
                if (!eval(cg, e->a, &sa) || !eval(cg, e->c, &sc)) return 0;
                fa = pick_fpr(b, -1, -1);
                ra = pick_addr(b, 0, 0);
                if (fa < 0 || ra < 0) {
                    cb_error(b, e->line, e->col, "ZA5005", "%s needs one FPR and one address register",
                             e->name);
                    return 0;
                }
                load_slot(b, e->line, sa, type_ptr(), ra);
                load_fp_slot(b, e->line, sc, e->c->type, fa);
                asm_line(b, e->line, isshort ? (is64 ? "CGEBR %%r0,%lld,%%f%d" : "CFEBR %%r0,%lld,%%f%d")
                                             : (is64 ? "CGDBR %%r0,%lld,%%f%d" : "CFDBR %%r0,%lld,%%f%d"),
                         (long long)m, fa);
                asm_line(b, e->line, is64 ? "STG %%r0,0(%%r%d)" : "ST %%r0,0(%%r%d)", ra);
                asm_line(b, e->line, "IPM %%r0");
                asm_line(b, e->line, "SRL %%r0,28");
                res = alloc_slot(cg);
                store_slot(b, e->line, res, type_of(32, 0), 0);
                *slotp = res;
                return 1;
            }
            /* three-operand multiply-add/subtract */
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sb) || !eval(cg, e->c, &sc))
                return 0;
            fa = pick_fpr(b, -1, -1);
            fb = pick_fpr(b, fa, -1);
            fc = pick_fpr(b, fa, fb);
            if (fa < 0 || fb < 0 || fc < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "%s needs three cscratch FPRs", e->name);
                return 0;
            }
            load_fp_from_slot(b, e->line, sa, e->a->type, fa, e->type.bits);
            load_fp_from_slot(b, e->line, sb, e->b->type, fb, e->type.bits);
            load_fp_from_slot(b, e->line, sc, e->c->type, fc, e->type.bits);
            asm_line(b, e->line, !strcmp(e->name, "__maebr") || !strcmp(e->name, "__fmadds")
                                   ? "MAEBR %%f%d,%%f%d,%%f%d" :
                 !strcmp(e->name, "__msebr") || !strcmp(e->name, "__fmsubs")
                                   ? "MSEBR %%f%d,%%f%d,%%f%d" :
                 !strcmp(e->name, "__madbr") || !strcmp(e->name, "__fmadd")
                                   ? "MADBR %%f%d,%%f%d,%%f%d" : "MSDBR %%f%d,%%f%d,%%f%d",
                     fa, fb, fc);
            res = alloc_slot(cg);
            store_fp_slot(b, e->line, res, e->type, fa);
            *slotp = res;
            return 1;
        }
        if (!strcmp(e->name, "__strcpy")) {
            int ra, rb, saved;
            char *top, *end;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            ra = pick_addr(b, 0, 0);
            rb = pick_addr(b, ra, 0);
            if (ra < 0 || rb < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "__strcpy needs two nonzero scratch registers");
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), ra);
            load_slot(b, e->line, sp, type_ptr(), rb);
            saved = alloc_slot(cg);
            store_slot(b, e->line, saved, type_ptr(), ra);
            top = new_label(b); end = new_label(b);
            emit_label(b, e->line, top);
            asm_line(b, e->line, "MVC 0(1,%%r%d),0(%%r%d)", ra, rb);
            asm_line(b, e->line, "LLGC %%r0,0(%%r%d)", rb);
            asm_line(b, e->line, "LTR %%r0,%%r0");
            asm_line(b, e->line, "BRCL 8,%s", end);
            asm_line(b, e->line, "AGHI %%r%d,1", ra);
            asm_line(b, e->line, "AGHI %%r%d,1", rb);
            asm_line(b, e->line, "BRCL 15,%s", top);
            emit_label(b, e->line, end);
            *slotp = saved;
            free(top); free(end);
            return 1;
        }
        if (!strcmp(e->name, "__memcmp")) {
            int ra, rb;
            char *top, *eq, *less, *greater, *end;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sl))
                return 0;
            ra = pick_addr(b, 0, 0);
            rb = pick_addr(b, ra, 0);
            if (ra < 0 || rb < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "__memcmp needs two nonzero scratch registers");
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), ra);
            load_slot(b, e->line, sp, type_ptr(), rb);
            load_slot(b, e->line, sl, e->c->type, 0);
            top = new_label(b); eq = new_label(b); less = new_label(b);
            greater = new_label(b); end = new_label(b);
            asm_line(b, e->line, "LTGR %%r0,%%r0");
            asm_line(b, e->line, "BRCL 8,%s", eq);
            emit_label(b, e->line, top);
            asm_line(b, e->line, "LTGR %%r0,%%r0");
            asm_line(b, e->line, "BRCL 8,%s", eq);
            asm_line(b, e->line, "CLC 0(1,%%r%d),0(%%r%d)", ra, rb);
            asm_line(b, e->line, "BRCL 4,%s", less);
            asm_line(b, e->line, "BRCL 2,%s", greater);
            asm_line(b, e->line, "AGHI %%r%d,1", ra);
            asm_line(b, e->line, "AGHI %%r%d,1", rb);
            asm_line(b, e->line, "AGHI %%r0,-1");
            asm_line(b, e->line, "BRCL 15,%s", top);
            emit_label(b, e->line, eq);
            asm_line(b, e->line, "LGHI %%r0,0");
            asm_line(b, e->line, "BRCL 15,%s", end);
            emit_label(b, e->line, less);
            asm_line(b, e->line, "LGHI %%r0,-1");
            asm_line(b, e->line, "BRCL 15,%s", end);
            emit_label(b, e->line, greater);
            asm_line(b, e->line, "LGHI %%r0,1");
            emit_label(b, e->line, end);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            free(top); free(eq); free(less); free(greater); free(end);
            return 1;
        }
        if (!strcmp(e->name, "__strcmp")) {
            int ra, rb;
            char *top, *eq, *less, *greater, *end;
            if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
            ra = pick_addr(b, 0, 0);
            rb = pick_addr(b, ra, 0);
            if (ra < 0 || rb < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "__strcmp needs two nonzero scratch registers");
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), ra);
            load_slot(b, e->line, sp, type_ptr(), rb);
            top = new_label(b); eq = new_label(b); less = new_label(b);
            greater = new_label(b); end = new_label(b);
            emit_label(b, e->line, top);
            asm_line(b, e->line, "CLC 0(1,%%r%d),0(%%r%d)", ra, rb);
            asm_line(b, e->line, "BRCL 4,%s", less);
            asm_line(b, e->line, "BRCL 2,%s", greater);
            asm_line(b, e->line, "LLGC %%r0,0(%%r%d)", ra);
            asm_line(b, e->line, "LTR %%r0,%%r0");
            asm_line(b, e->line, "BRCL 8,%s", eq);
            asm_line(b, e->line, "AGHI %%r%d,1", ra);
            asm_line(b, e->line, "AGHI %%r%d,1", rb);
            asm_line(b, e->line, "BRCL 15,%s", top);
            emit_label(b, e->line, eq);
            asm_line(b, e->line, "LGHI %%r0,0");
            asm_line(b, e->line, "BRCL 15,%s", end);
            emit_label(b, e->line, less);
            asm_line(b, e->line, "LGHI %%r0,-1");
            asm_line(b, e->line, "BRCL 15,%s", end);
            emit_label(b, e->line, greater);
            asm_line(b, e->line, "LGHI %%r0,1");
            emit_label(b, e->line, end);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_of(32, 0), 0);
            *slotp = res;
            free(top); free(eq); free(less); free(greater); free(end);
            return 1;
        }
        if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sl)) return 0;
        {
            int addr = pick_addr(b, 0, 0), pat = pick_addr(b, addr, 0), chr = pick_addr(b, addr, pat);
            char *top, *found, *notf, *end;
            if (addr < 0 || pat < 0 || chr < 0) {
                cb_error(b, e->line, e->col, "ZA5005", "__memchr needs three nonzero scratch registers");
                return 0;
            }
            load_slot(b, e->line, sa, type_ptr(), addr);
            load_slot(b, e->line, sp, e->b->type, pat);
            asm_line(b, e->line, "NILF %%r%d,255", pat);
            load_slot(b, e->line, sl, e->c->type, 0);
            top = new_label(b); found = new_label(b); notf = new_label(b); end = new_label(b);
            asm_line(b, e->line, "LTGR %%r0,%%r0");
            asm_line(b, e->line, "BRCL 8,%s", notf);
            emit_label(b, e->line, top);
            asm_line(b, e->line, "LLGC %%r%d,0(%%r%d)", chr, addr);
            asm_line(b, e->line, "CLR %%r%d,%%r%d", chr, pat);
            asm_line(b, e->line, "BRCL 8,%s", found);
            asm_line(b, e->line, "AGHI %%r%d,1", addr);
            asm_line(b, e->line, "AGHI %%r0,-1");
            asm_line(b, e->line, "BRCL 7,%s", top);
            emit_label(b, e->line, notf);
            asm_line(b, e->line, "LGHI %%r0,0");
            asm_line(b, e->line, "BRCL 15,%s", end);
            emit_label(b, e->line, found);
            asm_line(b, e->line, "LGR %%r0,%%r%d", addr);
            emit_label(b, e->line, end);
            res = alloc_slot(cg);
            store_slot(b, e->line, res, type_ptr(), 0);
            *slotp = res;
            free(top); free(found); free(notf); free(end);
            return 1;
        }
    }
    case E_BIN:
        return eval_bin(cg, e, slotp);
    }
    return 0;
}

static int codegen_stmt(Cg *cg, CBStmt *st);
static int codegen_block(Cg *cg, CBStmtVec *v);

/* What a comparison to zero means for one CLC result:
 * cc is 0 (equal), 1 (first less) or 2 (first greater). */
static int cmp_cc_false(int op, int cc) {
    switch (op) {
    case OP_EQ: return cc != 0;
    case OP_NE: return cc == 0;
    case '<': return cc != 1;
    case OP_LE: return cc == 2;
    case '>': return cc != 2;
    case OP_GE: return cc == 1;
    default: return 1;
    }
}

static int cmp_eval(int op, int64_t a, int64_t b) {
    switch (op) {
    case OP_EQ: return a == b;
    case OP_NE: return a != b;
    case '<': return a < b;
    case OP_LE: return a <= b;
    case '>': return a > b;
    case OP_GE: return a >= b;
    default: return 0;
    }
}

/* The result of __memcmp/__strcmp is always -1, 0 or 1. Return -1 when the
 * comparison is always false, 1 when it is always true, 0 when either is
 * possible. */
static int cmp_range_truth(int op, int64_t value) {
    int any = 0, all = 1, v;
    for (v = -1; v <= 1; v++) {
        if (cmp_eval(op, v, value)) any = 1;
        else all = 0;
    }
    return !any ? -1 : all ? 1 : 0;
}

static int cmp_cond_call(CBExpr *e, CBExpr **call, int *op, int64_t *value) {
    if (e->kind == E_CALL && (!strcmp(e->name, "__memcmp") || !strcmp(e->name, "__strcmp"))) {
        *call = e;
        *op = OP_NE;
        *value = 0;
        return 1;
    }
    if (e->kind == E_UN && e->op == '!' && e->a->kind == E_CALL) {
        *call = e->a;
        *op = OP_EQ;
        *value = 0;
        return 1;
    }
    if (e->kind == E_BIN && (e->op == OP_EQ || e->op == OP_NE || e->op == '<' ||
                             e->op == '>' || e->op == OP_LE || e->op == OP_GE)) {
        int o = e->op;
        int64_t v = 0;
        if (e->a->kind == E_CALL && e->b->kind == E_NUM) {
            *call = e->a;
            v = e->b->num;
        } else if (e->b->kind == E_CALL && e->a->kind == E_NUM) {
            *call = e->b;
            v = e->a->num;
            if (o == '<') o = '>';
            else if (o == '>') o = '<';
            else if (o == OP_LE) o = OP_GE;
            else if (o == OP_GE) o = OP_LE;
        } else {
            return 0;
        }
        *op = o;
        *value = v;
        return 1;
    }
    return 0;
}

static int emit_memcmp_cond(Cg *cg, CBExpr *e, int op, const char *false_label) {
    CBlock *b = cg->b;
    int sa, sp, sl, ra, rb;
    char *top, *less, *greater, *equal, *truth;
    if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp) || !eval(cg, e->c, &sl))
        return 0;
    ra = pick_addr(b, 0, 0);
    rb = pick_addr(b, ra, 0);
    if (ra < 0 || rb < 0) {
        cb_error(b, e->line, e->col, "ZA5005", "__memcmp needs two nonzero scratch registers");
        return 0;
    }
    load_slot(b, e->line, sa, type_ptr(), ra);
    load_slot(b, e->line, sp, type_ptr(), rb);
    load_slot(b, e->line, sl, e->c->type, 0);
    top = new_label(b); less = new_label(b); greater = new_label(b);
    equal = new_label(b); truth = new_label(b);
    emit_label(b, e->line, top);
    asm_line(b, e->line, "LTGR %%r0,%%r0");
    asm_line(b, e->line, "BRCL 8,%s", equal);
    asm_line(b, e->line, "CLC 0(1,%%r%d),0(%%r%d)", ra, rb);
    asm_line(b, e->line, "BRCL 4,%s", less);
    asm_line(b, e->line, "BRCL 2,%s", greater);
    asm_line(b, e->line, "AGHI %%r%d,1", ra);
    asm_line(b, e->line, "AGHI %%r%d,1", rb);
    asm_line(b, e->line, "AGHI %%r0,-1");
    asm_line(b, e->line, "BRCL 15,%s", top);
    emit_label(b, e->line, less);
    if (cmp_cc_false(op, 1)) asm_line(b, e->line, "BRCL 15,%s", false_label);
    asm_line(b, e->line, "BRCL 15,%s", truth);
    emit_label(b, e->line, greater);
    if (cmp_cc_false(op, 2)) asm_line(b, e->line, "BRCL 15,%s", false_label);
    asm_line(b, e->line, "BRCL 15,%s", truth);
    emit_label(b, e->line, equal);
    if (cmp_cc_false(op, 0)) asm_line(b, e->line, "BRCL 15,%s", false_label);
    emit_label(b, e->line, truth);
    free(top); free(less); free(greater); free(equal); free(truth);
    return 1;
}

static int emit_strcmp_cond(Cg *cg, CBExpr *e, int op, const char *false_label) {
    CBlock *b = cg->b;
    int sa, sp, ra, rb;
    char *top, *less, *greater, *equal, *truth;
    if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
    ra = pick_addr(b, 0, 0);
    rb = pick_addr(b, ra, 0);
    if (ra < 0 || rb < 0) {
        cb_error(b, e->line, e->col, "ZA5005", "__strcmp needs two nonzero scratch registers");
        return 0;
    }
    load_slot(b, e->line, sa, type_ptr(), ra);
    load_slot(b, e->line, sp, type_ptr(), rb);
    top = new_label(b); less = new_label(b); greater = new_label(b);
    equal = new_label(b); truth = new_label(b);
    emit_label(b, e->line, top);
    asm_line(b, e->line, "CLC 0(1,%%r%d),0(%%r%d)", ra, rb);
    asm_line(b, e->line, "BRCL 4,%s", less);
    asm_line(b, e->line, "BRCL 2,%s", greater);
    asm_line(b, e->line, "LLGC %%r0,0(%%r%d)", ra);
    asm_line(b, e->line, "LTR %%r0,%%r0");
    asm_line(b, e->line, "BRCL 8,%s", equal);
    asm_line(b, e->line, "AGHI %%r%d,1", ra);
    asm_line(b, e->line, "AGHI %%r%d,1", rb);
    asm_line(b, e->line, "BRCL 15,%s", top);
    emit_label(b, e->line, less);
    if (cmp_cc_false(op, 1)) asm_line(b, e->line, "BRCL 15,%s", false_label);
    asm_line(b, e->line, "BRCL 15,%s", truth);
    emit_label(b, e->line, greater);
    if (cmp_cc_false(op, 2)) asm_line(b, e->line, "BRCL 15,%s", false_label);
    asm_line(b, e->line, "BRCL 15,%s", truth);
    emit_label(b, e->line, equal);
    if (cmp_cc_false(op, 0)) asm_line(b, e->line, "BRCL 15,%s", false_label);
    emit_label(b, e->line, truth);
    free(top); free(less); free(greater); free(equal); free(truth);
    return 1;
}

static int emit_facility_cond(Cg *cg, CBExpr *e, const char *zero) {
    CBlock *b = cg->b;
    int sa, sp, base, count, bitr, shift, tmp;
    if (!strcmp(e->name, "__facility")) {
        char *loop = new_label(b), *done = new_label(b), *no = new_label(b);
        if (!eval(cg, e->a, &sa)) { free(loop); free(done); free(no); return 0; }
        base = pick_addr(b, 0, 0);
        count = pick_addr(b, base, 0);
        bitr = pick_addr3(b, base, count, 0);
        shift = pick_addr4(b, base, count, bitr, 0);
        if (base < 0 || count < 0 || bitr < 0 || shift < 0) {
            cb_error(b, e->line, e->col, "ZA5005", "__facility needs four scratch registers");
            free(loop); free(done); free(no);
            return 0;
        }
        tmp = alloc_slots(cg, 4);
        load_slot(b, e->line, sa, e->a->type, bitr);
        asm_line(b, e->line, "CGHI %%r%d,255", bitr);
        asm_line(b, e->line, "BRCL 2,%s", zero);
        asm_line(b, e->line, "LGR %%r0,%%r%d", bitr);
        asm_line(b, e->line, "SRL %%r0,6");
        asm_line(b, e->line, "AHI %%r0,1");
        asm_line(b, e->line, "LGR %%r%d,%%r0", count);
        asm_line(b, e->line, "LA %%r%d,%ld(%%r%d)", base, slot_off(b, tmp), b->temp_reg);
        emit_label(b, e->line, loop);
        asm_line(b, e->line, "STFLE 0(%%r%d)", base);
        asm_line(b, e->line, "IPM %%r0");
        asm_line(b, e->line, "SRL %%r0,28");
        asm_line(b, e->line, "AHI %%r%d,-1", count);
        asm_line(b, e->line, "BRCL 8,%s", done);
        asm_line(b, e->line, "LTR %%r0,%%r0");
        asm_line(b, e->line, "BRCL 8,%s", no);
        asm_line(b, e->line, "AHI %%r%d,8", base);
        asm_line(b, e->line, "BRCL 15,%s", loop);
        emit_label(b, e->line, no);
        asm_line(b, e->line, "BRCL 15,%s", zero);
        emit_label(b, e->line, done);
        asm_line(b, e->line, "NILF %%r%d,63", bitr);
        asm_line(b, e->line, "LGHI %%r%d,63", shift);
        asm_line(b, e->line, "SGR %%r%d,%%r%d", shift, bitr);
        asm_line(b, e->line, "LGHI %%r%d,1", bitr);
        asm_line(b, e->line, "SLLG %%r%d,%%r%d,0(%%r%d)", bitr, bitr, shift);
        asm_line(b, e->line, "LG %%r0,0(%%r%d)", base);
        asm_line(b, e->line, "NGR %%r0,%%r%d", bitr);
        asm_line(b, e->line, "BRCL 8,%s", zero);
        free(loop); free(done); free(no);
        return 1;
    }
    if (!strcmp(e->name, "__facility_test")) {
        if (!eval(cg, e->a, &sa) || !eval(cg, e->b, &sp)) return 0;
        base = pick_addr(b, 0, 0);
        bitr = pick_addr(b, base, 0);
        shift = pick_addr3(b, base, bitr, 0);
        if (base < 0 || bitr < 0 || shift < 0) {
            cb_error(b, e->line, e->col, "ZA5005",
                     "__facility_test needs three scratch registers");
            return 0;
        }
        load_slot(b, e->line, sa, type_ptr(), base);
        load_slot(b, e->line, sp, e->b->type, bitr);
        asm_line(b, e->line, "LGR %%r0,%%r%d", bitr);
        asm_line(b, e->line, "SRL %%r0,6");
        asm_line(b, e->line, "SLLG %%r0,%%r0,3");
        asm_line(b, e->line, "AGR %%r%d,%%r0", base);
        asm_line(b, e->line, "NILF %%r%d,63", bitr);
        asm_line(b, e->line, "LGHI %%r%d,63", shift);
        asm_line(b, e->line, "SGR %%r%d,%%r%d", shift, bitr);
        asm_line(b, e->line, "LGHI %%r%d,1", bitr);
        asm_line(b, e->line, "SLLG %%r%d,%%r%d,0(%%r%d)", bitr, bitr, shift);
        asm_line(b, e->line, "LG %%r0,0(%%r%d)", base);
        asm_line(b, e->line, "NGR %%r0,%%r%d", bitr);
        asm_line(b, e->line, "BRCL 8,%s", zero);
        return 1;
    }
    return 0;
}

static int emit_cond_zero(Cg *cg, CBExpr *e, const char *label) {
    CBlock *b = cg->b;
    int s, op, truth;
    int64_t value = 0;
    CBExpr *call = NULL;
    if (e->kind == E_CALL &&
        (!strcmp(e->name, "__facility") || !strcmp(e->name, "__facility_test")))
        return emit_facility_cond(cg, e, label);
    if (cmp_cond_call(e, &call, &op, &value)) {
        truth = cmp_range_truth(op, value);
        if (truth < 0) {
            asm_line(b, e->line, "BRCL 15,%s", label);
            return 1;
        }
        if (truth > 0) return 1;
        if (value == 0) {
            if (!strcmp(call->name, "__memcmp")) return emit_memcmp_cond(cg, call, op, label);
            if (!strcmp(call->name, "__strcmp")) return emit_strcmp_cond(cg, call, op, label);
        }
    }
    if (e->type.dfp) {
        int fp, abits = e->type.bits == 32 ? 64 : e->type.bits;
        fp = abits == 128 ? pick_fpr_pair(b, -1, -1) : pick_fpr(b, -1, -1);
        if (fp < 0) {
            cb_error(b, e->line, e->col, "ZA5005",
                     "a decimal condition needs a cscratch FPR");
            return 0;
        }
        if (!eval(cg, e, &s)) return 0;
        load_dfp_from_slot(b, e->line, s, e->type, fp, abits);
        asm_line(b, e->line, abits == 128 ? "LTXTR %%f%d,%%f%d" : "LTDTR %%f%d,%%f%d", fp, fp);
        asm_line(b, e->line, "BRCL 8,%s", label);
        return 1;
    }
    if (e->type.flt) {
        int fp, zero = -1;
        if (e->type.bits == 128) {
            fp = pick_fpr_pair(b, -1, -1);
            zero = pick_fpr_pair(b, fp, -1);
            if (fp < 0 || zero < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "a long double condition needs two cscratch FPR pairs");
                return 0;
            }
            if (!eval(cg, e, &s)) return 0;
            load_fp_slot(b, e->line, s, e->type, fp);
            asm_line(b, e->line, "LZXR %%f%d", zero);
            asm_line(b, e->line, "CXBR %%f%d,%%f%d", fp, zero);
        } else {
            fp = pick_fpr(b, -1, -1);
            zero = pick_fpr(b, fp, -1);
            if (fp < 0 || zero < 0) {
                cb_error(b, e->line, e->col, "ZA5005",
                         "a floating condition needs two cscratch FPRs");
                return 0;
            }
            if (!eval(cg, e, &s)) return 0;
            load_fp_slot(b, e->line, s, e->type, fp);
            asm_line(b, e->line, e->type.bits == 64 ? "LZDR %%f%d" : "LZER %%f%d", zero);
            asm_line(b, e->line, e->type.bits == 64 ? "CDBR %%f%d,%%f%d" : "CEBR %%f%d,%%f%d", fp, zero);
        }
        asm_line(b, e->line, "BRCL 8,%s", label);
        return 1;
    }
    if (!eval(cg, e, &s)) return 0;
    load_slot(b, e->line, s, e->type, 0);
    emit_test(b, e->line, e->type);
    asm_line(b, e->line, "BRCL 8,%s", label);
    return 1;
}

static int codegen_block(Cg *cg, CBStmtVec *v) {
    size_t i;
    for (i = 0; i < v->n; i++)
        if (!codegen_stmt(cg, &v->v[i])) return 0;
    return 1;
}

/* pick a nonzero register for an address operand; avoids r0 and used */
static int pick_addr(CBlock *b, int used1, int used2) {
    int i;
    for (i = 0; i < b->nscratch; i++)
        if (b->scratch[i] != 0 && b->scratch[i] != used1 && b->scratch[i] != used2)
            return b->scratch[i];
    if (used1 != 1 && used2 != 1) return 1;
    return -1;
}

/* choose an even-odd general-register pair, both listed in cscratch */
static int pick_addr3(CBlock *b, int used1, int used2, int used3) {
    int i;
    for (i = 0; i < b->nscratch; i++)
        if (b->scratch[i] != 0 && b->scratch[i] != used1 &&
            b->scratch[i] != used2 && b->scratch[i] != used3)
            return b->scratch[i];
    if (used1 != 1 && used2 != 1 && used3 != 1) return 1;
    return -1;
}

static int pick_addr4(CBlock *b, int used1, int used2, int used3, int used4) {
    int i;
    for (i = 0; i < b->nscratch; i++)
        if (b->scratch[i] != 0 && b->scratch[i] != used1 && b->scratch[i] != used2 &&
            b->scratch[i] != used3 && b->scratch[i] != used4)
            return b->scratch[i];
    if (used1 != 1 && used2 != 1 && used3 != 1 && used4 != 1) return 1;
    return -1;
}

static int pick_fpr(CBlock *b, int used1, int used2) {
    int i;
    for (i = 0; i < b->nfscratch; i++) {
        int r = b->fscratch[i];
        if (r != used1 && r != used2) return r;
    }
    return -1;
}

static int pick_fpr_pair(CBlock *b, int used1, int used2) {
    int i, j;
    for (i = 0; i < b->nfscratch; i++) {
        int r = b->fscratch[i];
        if ((r & 2) != 0 || r == used1 || r == used2) continue;
        for (j = 0; j < b->nfscratch; j++)
            if (b->fscratch[j] == r + 2) return r;
    }
    return -1;
}

static int pick_vr(CBlock *b, int used1, int used2) {
    int i;
    for (i = 0; i < b->nvscratch; i++) {
        int r = b->vscratch[i];
        if (r != used1 && r != used2) return r;
    }
    return -1;
}

static int pick_vr4(CBlock *b, int u1, int u2, int u3, int u4) {
    int i;
    for (i = 0; i < b->nvscratch; i++) {
        int r = b->vscratch[i];
        if (r != u1 && r != u2 && r != u3 && r != u4) return r;
    }
    return -1;
}

static int pick_addr5(CBlock *b, int u1, int u2, int u3, int u4, int u5) {
    int i;
    for (i = 0; i < b->nscratch; i++) {
        int r = b->scratch[i];
        if (r != 0 && r != u1 && r != u2 && r != u3 && r != u4 && r != u5) return r;
    }
    return -1;
}

static int pick_pair(CBlock *b, int used1, int used2, int *second) {
    int i;
    for (i = 0; i < b->nscratch; i++) {
        int r = b->scratch[i];
        if (r <= 1 || (r & 1)) continue;
        if (r == used1 || r == used2) continue;
        if (!has_scratch(b, r + 1)) continue;
        *second = r + 1;
        return r;
    }
    return -1;
}

static int codegen_intrinsic(Cg *cg, CBStmt *st) {
    CBlock *b = cg->b;
    int sd, ss, sl = -1, addr_d, addr_s = -1;
    int64_t len_cv = 0, pat_cv = 0;
    int have_len = const_eval(st->cond, &len_cv);
    int have_pat = st->kind == S_MEMSET ? const_eval(st->step, &pat_cv) : 0;
    int64_t pat = pat_cv;
    if (!eval(cg, st->expr, &sd)) return 0;
    if (st->kind != S_MEMSET && !eval(cg, st->step, &ss)) return 0;
    if (st->kind == S_MEMSET && !eval(cg, st->step, &ss)) return 0;
    if (!have_len && !eval(cg, st->cond, &sl)) return 0;
    if (have_len && len_cv < 0) {
        cb_error(b, st->line, 1, "ZA5003", "the length of a memory intrinsic is negative");
        return 0;
    }
    if (st->kind == S_MEMSET) {
        addr_d = pick_addr(b, 0, 1);
        if (addr_d < 0) {
            cb_error(b, st->line, 1, "ZA5005", "memset needs a nonzero scratch address register");
            return 0;
        }
        load_slot(b, st->line, sd, type_ptr(), addr_d);
        if (!have_pat) {
            load_slot(b, st->line, ss, st->step->type, 0);
        } else {
            asm_line(b, st->line, "LHI %%r0,%d", (int)(pat & 0xFF));
        }
        if (!have_len) load_slot(b, st->line, sl, st->cond->type, 1);
        else asm_line(b, st->line, "LGHI %%r1,%lld", (long long)len_cv);
        if (have_len && len_cv == 0) return 1;
        if (have_len && have_pat && (pat & 0xFF) == 0 && len_cv <= 4096) {
            int64_t off;
            for (off = 0; off < len_cv; off += 256) {
                int64_t n = len_cv - off > 256 ? 256 : len_cv - off;
                asm_line(b, st->line, "XC %lld(%lld,%%r%d),%lld(%%r%d)", (long long)off,
                         (long long)n, addr_d, (long long)off, addr_d);
            }
            return 1;
        }
        {
            char *top = new_label(b), *end = new_label(b);
            asm_line(b, st->line, "LTGR %%r1,%%r1");
            asm_line(b, st->line, "BRCL 8,%s", end);
            emit_label(b, st->line, top);
            asm_line(b, st->line, "STC %%r0,0(%%r%d)", addr_d);
            asm_line(b, st->line, "AGHI %%r%d,1", addr_d);
            asm_line(b, st->line, "AGHI %%r1,-1");
            asm_line(b, st->line, "BRCL 7,%s", top);
            emit_label(b, st->line, end);
            free(top); free(end);
        }
        return 1;
    }
    addr_d = pick_addr(b, 0, 1);
    addr_s = pick_addr(b, addr_d, 0);
    if (addr_d < 0 || addr_s < 0) {
        cb_error(b, st->line, 1, "ZA5005",
                 "%s needs two nonzero scratch address registers", st->kind == S_MEMCPY ? "__memcpy" : "__memmove");
        return 0;
    }
    load_slot(b, st->line, sd, type_ptr(), addr_d);
    load_slot(b, st->line, ss, type_ptr(), addr_s);
    if (!have_len) load_slot(b, st->line, sl, st->cond->type, 0);
    else asm_line(b, st->line, "LGHI %%r0,%lld", (long long)len_cv);
    if (have_len && len_cv == 0) return 1;
    if (st->kind == S_MEMCPY && have_len && len_cv <= 4096) {
        int64_t off;
        for (off = 0; off < len_cv; off += 256) {
            int64_t n = len_cv - off > 256 ? 256 : len_cv - off;
            asm_line(b, st->line, "MVC %lld(%lld,%%r%d),%lld(%%r%d)", (long long)off,
                     (long long)n, addr_d, (long long)off, addr_s);
        }
        return 1;
    }
    {
        char *top = new_label(b), *end = new_label(b);
        if (st->kind == S_MEMMOVE) {
            char *back = new_label(b), *btop = new_label(b);
            asm_line(b, st->line, "LTGR %%r0,%%r0");
            asm_line(b, st->line, "BRCL 8,%s", end);
            asm_line(b, st->line, "CLGR %%r%d,%%r%d", addr_d, addr_s);
            asm_line(b, st->line, "BRCL 2,%s", back);
            emit_label(b, st->line, top);
            asm_line(b, st->line, "MVC 0(1,%%r%d),0(%%r%d)", addr_d, addr_s);
            asm_line(b, st->line, "AGHI %%r%d,1", addr_d);
            asm_line(b, st->line, "AGHI %%r%d,1", addr_s);
            asm_line(b, st->line, "AGHI %%r0,-1");
            asm_line(b, st->line, "BRCL 7,%s", top);
            asm_line(b, st->line, "BRCL 15,%s", end);
            emit_label(b, st->line, back);
            asm_line(b, st->line, "AGR %%r%d,%%r0", addr_d);
            asm_line(b, st->line, "AGR %%r%d,%%r0", addr_s);
            emit_label(b, st->line, btop);
            asm_line(b, st->line, "AGHI %%r%d,-1", addr_d);
            asm_line(b, st->line, "AGHI %%r%d,-1", addr_s);
            asm_line(b, st->line, "MVC 0(1,%%r%d),0(%%r%d)", addr_d, addr_s);
            asm_line(b, st->line, "AGHI %%r0,-1");
            asm_line(b, st->line, "BRCL 7,%s", btop);
            free(back); free(btop);
        } else {
            char *chunk = new_label(b), *after = new_label(b);
            asm_line(b, st->line, "CGHI %%r0,256");
            asm_line(b, st->line, "BRCL 4,%s", after);
            emit_label(b, st->line, chunk);
            asm_line(b, st->line, "MVC 0(256,%%r%d),0(%%r%d)", addr_d, addr_s);
            asm_line(b, st->line, "AGHI %%r%d,256", addr_d);
            asm_line(b, st->line, "AGHI %%r%d,256", addr_s);
            asm_line(b, st->line, "AGHI %%r0,-256");
            asm_line(b, st->line, "CGHI %%r0,256");
            asm_line(b, st->line, "BRCL 10,%s", chunk);
            emit_label(b, st->line, after);
            asm_line(b, st->line, "LTGR %%r0,%%r0");
            asm_line(b, st->line, "BRCL 8,%s", end);
            {
                int treg = pick_addr(b, addr_d, addr_s);
                if (treg < 0) {
                    /* no third scratch register: copy the tail one byte at a time */
                    emit_label(b, st->line, top);
                    asm_line(b, st->line, "MVC 0(1,%%r%d),0(%%r%d)", addr_d, addr_s);
                    asm_line(b, st->line, "AGHI %%r%d,1", addr_d);
                    asm_line(b, st->line, "AGHI %%r%d,1", addr_s);
                    asm_line(b, st->line, "AGHI %%r0,-1");
                    asm_line(b, st->line, "BRCL 7,%s", top);
                } else {
                    char *tmpl = new_label(b), *ex = new_label(b);
                    /* EX ORs the low byte of a nonzero register into the MVC
                       length byte. R0 cannot be used: EX with an R1 field of
                       zero performs no ORing at all. */
                    asm_line(b, st->line, "AGHI %%r0,-1");
                    asm_line(b, st->line, "BRCL 15,%s", ex);
                    emit_label(b, st->line, tmpl);
                    asm_line(b, st->line, "MVC 0(0,%%r%d),0(%%r%d)", addr_d, addr_s);
                    emit_label(b, st->line, ex);
                    asm_line(b, st->line, "LARL %%r%d,%s", treg, tmpl);
                    asm_line(b, st->line, "EX %%r%d,0(%%r%d)", treg, treg);
                    free(tmpl); free(ex);
                }
            }
            free(chunk); free(after);
        }
        emit_label(b, st->line, end);
        free(top); free(end);
    }
    return 1;
}

static int codegen_stmt(Cg *cg, CBStmt *st) {
    CBlock *b = cg->b;
    int s;
    cg->slot_top = cg->base_slots;
    if (st->kind == S_IF) {
        char *els = new_label(b), *end = NULL;
        if (!emit_cond_zero(cg, st->cond, els)) { free(els); return 0; }
        if (!codegen_block(cg, &st->body)) { free(els); return 0; }
        if (st->other.n) {
            end = new_label(b);
            asm_line(b, st->line, "BRCL 15,%s", end);
            emit_label(b, st->line, els);
            if (!codegen_block(cg, &st->other)) { free(els); free(end); return 0; }
            emit_label(b, st->line, end);
            free(end);
        } else {
            emit_label(b, st->line, els);
        }
        free(els);
        return 1;
    }
    if (st->kind == S_SWITCH) {
        char *end = new_label(b);
        const char *def = NULL;
        size_t i;
        int s;
        for (i = 0; i < st->body.n; i++)
            if (st->body.v[i].kind == S_DEFAULT) def = st->body.v[i].label;
        if (!eval(cg, st->cond, &s)) { free(end); return 0; }
        for (i = 0; i < st->body.n; i++) {
            CBStmt *c = &st->body.v[i];
            if (c->kind != S_CASE) continue;
            load_slot(b, c->line, s, st->cond->type, 0);
            load_imm(b, c->line, c->num, st->cond->type, 1);
            asm_line(b, c->line, st->cond->type.bits == 64
                                    ? (st->cond->type.unsig ? "CLGR %%r0,%%r1" : "CGR %%r0,%%r1")
                                    : (st->cond->type.unsig ? "CLR %%r0,%%r1" : "CR %%r0,%%r1"));
            asm_line(b, c->line, "BRCL 8,%s", c->label);
        }
        asm_line(b, st->line, "BRCL 15,%s", def ? def : end);
        if (cg->ctrl_depth >= 16) {
            cb_error(b, st->line, 1, "ZA5008", "control flow nested too deeply");
            free(end);
            return 0;
        }
        cg->ctrl_break[cg->ctrl_depth] = end;
        cg->ctrl_continue[cg->ctrl_depth] = "";
        cg->ctrl_loop[cg->ctrl_depth] = 0;
        cg->ctrl_depth++;
        for (i = 0; i < st->body.n; i++) {
            CBStmt *c = &st->body.v[i];
            if (c->kind == S_CASE || c->kind == S_DEFAULT) emit_label(b, c->line, c->label);
            else if (!codegen_stmt(cg, c)) {
                cg->ctrl_depth--;
                free(end);
                return 0;
            }
        }
        cg->ctrl_depth--;
        emit_label(b, st->line, end);
        free(end);
        return 1;
    }
    if (st->kind == S_WHILE) {
        char *top = new_label(b), *end = new_label(b);
        if (cg->ctrl_depth >= 16) {
            cb_error(b, st->line, 1, "ZA5008", "loops nested too deeply");
            free(top); free(end);
            return 0;
        }
        cg->ctrl_break[cg->ctrl_depth] = end;
        cg->ctrl_continue[cg->ctrl_depth] = top;
        cg->ctrl_loop[cg->ctrl_depth] = 1;
        cg->ctrl_depth++;
        emit_label(b, st->line, top);
        if (!emit_cond_zero(cg, st->cond, end)) goto while_fail;
        if (!codegen_block(cg, &st->body)) goto while_fail;
        asm_line(b, st->line, "BRCL 15,%s", top);
        emit_label(b, st->line, end);
        cg->ctrl_depth--;
        free(top); free(end);
        return 1;
while_fail:
        cg->ctrl_depth--;
        free(top); free(end);
        return 0;
    }
    if (st->kind == S_DO) {
        char *top = new_label(b), *cont = new_label(b), *end = new_label(b);
        if (cg->ctrl_depth >= 16) {
            cb_error(b, st->line, 1, "ZA5008", "loops nested too deeply");
            free(top); free(cont); free(end);
            return 0;
        }
        cg->ctrl_break[cg->ctrl_depth] = end;
        cg->ctrl_continue[cg->ctrl_depth] = cont;
        cg->ctrl_loop[cg->ctrl_depth] = 1;
        cg->ctrl_depth++;
        emit_label(b, st->line, top);
        if (!codegen_block(cg, &st->body)) goto do_fail;
        emit_label(b, st->line, cont);
        {
            int s;
            if (!eval(cg, st->cond, &s)) goto do_fail;
            load_slot(b, st->cond->line, s, st->cond->type, 0);
            emit_test(b, st->cond->line, st->cond->type);
            asm_line(b, st->line, "BRCL 7,%s", top);
        }
        emit_label(b, st->line, end);
        cg->ctrl_depth--;
        free(top); free(cont); free(end);
        return 1;
do_fail:
        cg->ctrl_depth--;
        free(top); free(cont); free(end);
        return 0;
    }
    if (st->kind == S_FOR) {
        char *top = new_label(b), *step = new_label(b), *end = new_label(b);
        if (!codegen_block(cg, &st->init)) { free(top); free(step); free(end); return 0; }
        if (cg->ctrl_depth >= 16) {
            cb_error(b, st->line, 1, "ZA5008", "loops nested too deeply");
            free(top); free(step); free(end);
            return 0;
        }
        cg->ctrl_break[cg->ctrl_depth] = end;
        cg->ctrl_continue[cg->ctrl_depth] = step;
        cg->ctrl_loop[cg->ctrl_depth] = 1;
        cg->ctrl_depth++;
        emit_label(b, st->line, top);
        if (st->cond && !emit_cond_zero(cg, st->cond, end)) goto for_fail;
        if (!codegen_block(cg, &st->body)) goto for_fail;
        emit_label(b, st->line, step);
        if (st->step) {
            int s;
            if (!eval(cg, st->step, &s)) goto for_fail;
        }
        asm_line(b, st->line, "BRCL 15,%s", top);
        emit_label(b, st->line, end);
        cg->ctrl_depth--;
        free(top); free(step); free(end);
        return 1;
for_fail:
        cg->ctrl_depth--;
        free(top); free(step); free(end);
        return 0;
    }
    if (st->kind == S_BREAK || st->kind == S_CONTINUE) {
        if (cg->ctrl_depth <= 0) {
            cb_error(b, st->line, 1, "ZA5008", "break/continue outside a loop");
            return 0;
        }
        {
            int k = cg->ctrl_depth - 1;
            if (st->kind == S_CONTINUE) {
                while (k >= 0 && !cg->ctrl_loop[k]) k--;
                if (k < 0) {
                    cb_error(b, st->line, 1, "ZA5008", "continue outside a loop");
                    return 0;
                }
                asm_line(b, st->line, "BRCL 15,%s", cg->ctrl_continue[k]);
            } else {
                asm_line(b, st->line, "BRCL 15,%s", cg->ctrl_break[k]);
            }
        }
        return 1;
    }
    if (st->kind == S_MEMCPY || st->kind == S_MEMMOVE || st->kind == S_MEMSET)
        return codegen_intrinsic(cg, st);
    if (st->kind == S_LABEL) {
        emit_label(b, st->line, st->label);
        return 1;
    }
    if (st->kind == S_GOTO) {
        const char *lab = strmap_get(&b->labels, st->label);
        if (!lab) {
            cb_error(b, st->line, 1, "ZA5004", "goto target \"%s\" is not defined", st->label);
            return 0;
        }
        asm_line(b, st->line, "BRCL 15,%s", lab);
        return 1;
    }
    if (st->kind == S_DECL) {
        if (st->expr) {
            if (!eval(cg, st->expr, &s)) return 0;
            copy_slot(b, st->line, st->var->slot, s, st->var->type);
        } else {
            asm_line(b, st->line, "LGHI %%r0,0");
            if (st->var->type.vec || st->var->type.bits == 128) {
                asm_line(b, st->line, "STG %%r0,%ld(%%r%d)", slot_off(b, st->var->slot), b->temp_reg);
                asm_line(b, st->line, "STG %%r0,%ld(%%r%d)", slot_off(b, st->var->slot + 1), b->temp_reg);
            } else store_slot(b, st->line, st->var->slot, st->var->type, 0);
        }
        return 1;
    }
    if (st->kind == S_ASSIGN) {
        if (st->lval) {
            int sa, rs, addr;
            if (st->op != '=') {
                CBExpr bin;
                memset(&bin, 0, sizeof bin);
                bin.kind = E_BIN;
                bin.type = st->lval->type;
                bin.op = st->op;
                bin.a = st->lval;
                bin.b = st->expr;
                bin.line = st->line;
                if (!eval(cg, &bin, &rs)) return 0;
            } else if (!eval(cg, st->expr, &rs)) return 0;
            if (!eval(cg, st->lval->a, &sa)) return 0;
            addr = pick_addr(b, 0, 0);
            if (addr < 0) {
                cb_error(b, st->line, 1, "ZA5005", "a dereference assignment needs a scratch register");
                return 0;
            }
            load_slot(b, st->line, sa, type_ptr(), addr);
            if (st->lval->type.bits == 128) {
                int fp = pick_fpr_pair(b, -1, -1);
                if (fp < 0) {
                    cb_error(b, st->line, 1, "ZA5005",
                             "a long double dereference needs a cscratch FPR pair");
                    return 0;
                }
                load_fp_slot(b, st->line, rs, st->lval->type, fp);
                asm_line(b, st->line, "STD %%f%d,0(%%r%d)", fp, addr);
                asm_line(b, st->line, "STD %%f%d,8(%%r%d)", fp + 2, addr);
            } else {
                load_slot(b, st->line, rs, st->lval->type, 0);
                if (st->lval->type.bits == 64) asm_line(b, st->line, "STG %%r0,0(%%r%d)", addr);
                else if (st->lval->type.bits == 32) asm_line(b, st->line, "ST %%r0,0(%%r%d)", addr);
                else if (st->lval->type.bits == 16) asm_line(b, st->line, "STH %%r0,0(%%r%d)", addr);
                else asm_line(b, st->line, "STC %%r0,0(%%r%d)", addr);
            }
            return 1;
        }
        if (st->op != '=') {
            CBExpr lhs, bin;
            memset(&lhs, 0, sizeof lhs);
            lhs.kind = E_VAR;
            lhs.type = st->var->type;
            lhs.var = st->var;
            lhs.line = st->line;
            memset(&bin, 0, sizeof bin);
            bin.kind = E_BIN;
            bin.type = st->var->type;
            bin.op = st->op;
            bin.a = &lhs;
            bin.b = st->expr;
            bin.line = st->line;
            if (!eval(cg, &bin, &s)) return 0;
            copy_slot(b, st->line, st->var->slot, s, st->var->type);
            return 1;
        }
        if (!eval(cg, st->expr, &s)) return 0;
        copy_slot(b, st->line, st->var->slot, s, st->var->type);
        return 1;
    }
    if (st->kind == S_EXPR) {
        if (!eval(cg, st->expr, &s)) return 0;
        return 1;
    }
    return 1;
}

/* ---- block compile ------------------------------------------------------ */

static void free_block(CBlock *b) {
    size_t i;
    for (i = 0; i < b->vars.n; i++) {
        free(b->vars.v[i]->name);
        free(b->vars.v[i]->lit);
        free(b->vars.v[i]);
    }
    free(b->vars.v);
    for (i = 0; i < b->byname.cap; i++) free(b->byname.keys[i]);
    free(b->byname.keys);
    free(b->byname.vals);
    for (i = 0; i < b->labels.cap; i++) {
        free(b->labels.keys[i]);
        free(b->labels.vals[i]);
    }
    free(b->labels.keys);
    free(b->labels.vals);
    for (i = 0; i < b->ptypenames.n; i++) free(b->ptypenames.v[i]);
    free(b->ptypenames.v);
    for (i = 0; i < b->nlits; i++) {
        free(b->lits[i].data);
        free(b->lits[i].label);
        free(b->lits[i].dc);
    }
    free(b->lits);
    free(b->lit_end);
    free(b->name);
}

void cblock_debug_free(CBlockDebugVec *v) {
    size_t i, j;
    if (!v) return;
    for (i = 0; i < v->n; i++) {
        CBlockDebug *d = &v->v[i];
        free(d->file);
        free(d->name);
        free(d->start_label);
        free(d->end_label);
        for (j = 0; j < d->nvars; j++) free(d->vars[j].name);
        free(d->vars);
    }
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

int cblock_compile(const char *file, int line, const char *head, const CBlockLine *body,
                   size_t n, Reporter *rep, SrcVec *out, int *label_counter,
                   CBlockDebugVec *debug, int elf) {
    CBlock b;
    Parser p;
    CBStmtVec stmts = {0};
    size_t i, body_start = n;
    int ok = 1;
    size_t out_start = out->n;
    memset(&b, 0, sizeof b);
    memset(&p, 0, sizeof p);
    b.file = file;
    b.line = line;
    b.rep = rep;
    b.out = out;
    b.body = body;
    b.body_n = n;
    b.temp_reg = -1;
    b.elf = elf;
    b.name = xstrdup(head && *head ? head : "cblock");
    b.temp_down = 1;
    b.debug = debug;
    if (label_counter) {
        b.label_no = *label_counter;
        b.label_seq = *label_counter;
    }
    p.b = &b;
    if (b.debug) {
        b.start_label = new_label(&b);
        b.end_label = new_label(&b);
        emit_label(&b, line, b.start_label);
    }
    /* metadata */
    for (i = 0; i < n; i++) {
        char *w = NULL, *rest = NULL;
        const char *s = body[i].text;
        const char *q = s;
        while (*q == ' ' || *q == '\t') q++;
        if (!*q) continue;
        if (!pragma_split(s, &w, &rest)) {
            cb_error(&b, body[i].line, 1, "ZA5001", "expected #pragma metadata before #pragma cbegin");
            ok = 0;
            continue;
        }
        if (!strcmp(w, "cbegin")) {
            free(w);
            free(rest);
            body_start = i + 1;
            break;
        }
        if (!strcmp(w, "cin")) ok = binding_parse(&b, rest, body[i].line, V_CIN) && ok;
        else if (!strcmp(w, "cout")) ok = binding_parse(&b, rest, body[i].line, V_COUT) && ok;
        else if (!strcmp(w, "cinout")) ok = binding_parse(&b, rest, body[i].line, V_CINOUT) && ok;
        else if (!strcmp(w, "cscratch")) {
            char *s2 = xstrdup(rest), *pp = s2;
            for (;;) {
                char *comma = strchr(pp, ',');
                char *part, *dash;
                int r1 = -1, r2 = -1;
                if (comma) *comma = 0;
                part = trim(pp);
                dash = strchr(part, '-');
                if (dash) {
                    *dash = 0;
                    if (!reg_parse(trim(dash + 1), &r2) &&
                        !freg_parse(trim(dash + 1), &r2)) r2 = -1;
                }
                if (vreg_parse(trim(part), &r1)) {
                    if (dash) {
                        cb_error(&b, body[i].line, 1, "ZA5005", "cscratch vector ranges are not supported");
                        ok = 0;
                    } else add_vscratch(&b, r1, body[i].line);
                } else if (freg_parse(trim(part), &r1)) {
                    if (dash) {
                        cb_error(&b, body[i].line, 1, "ZA5005", "cscratch floating-point ranges are not supported");
                        ok = 0;
                    } else add_fscratch(&b, r1, body[i].line);
                } else if (!reg_parse(trim(part), &r1)) {
                    cb_error(&b, body[i].line, 1, "ZA5005", "\"%s\" is not a register", part);
                    ok = 0;
                } else if (dash && (r2 < r1)) {
                    cb_error(&b, body[i].line, 1, "ZA5005", "invalid cscratch register range");
                    ok = 0;
                } else if (dash) {
                    int r;
                    for (r = r1; r <= r2; r++) add_scratch(&b, r, body[i].line);
                } else add_scratch(&b, r1, body[i].line);
                if (!comma) break;
                pp = comma + 1;
            }
            free(s2);
        } else if (!strcmp(w, "ctemp")) ok = ctemp_parse(&b, rest, body[i].line) && ok;
        else {
            cb_error(&b, body[i].line, 1, "ZA5005", "unknown cblock pragma \"%s\"", w);
            ok = 0;
        }
        free(w);
        free(rest);
    }
    if (!ok) goto done;
    if (body_start == n) {
        cb_error(&b, line, 1, "ZA5001", "a cblock needs #pragma cbegin");
        goto done;
    }
    if (b.temp_reg < 0) {
        cb_error(&b, line, 1, "ZA5006", "cblock needs a ctemp area for its variables and values");
        goto done;
    }
    for (i = 0; i < b.vars.n; i++) {
        if (!b.vars.v[i]->type.vec && b.vars.v[i]->type.bits == 128 &&
            (b.vars.v[i]->reg & 2) != 0) {
            cb_error(&b, line, 1, "ZA5005",
                     "long double binding %%f%d is not the base of an FPR pair", b.vars.v[i]->reg);
            goto done;
        }
        if (!b.vars.v[i]->type.flt && !b.vars.v[i]->type.vec &&
            (b.vars.v[i]->reg == 0 || b.vars.v[i]->reg == 1)) {
            cb_error(&b, line, 1, "ZA5005", "register %%r%d cannot be a cin/cout binding",
                     b.vars.v[i]->reg);
            goto done;
        }
        if (!b.vars.v[i]->type.flt && !b.vars.v[i]->type.vec &&
            b.vars.v[i]->reg == b.temp_reg) {
            cb_error(&b, line, 1, "ZA5005", "bound register %%r%d is also the ctemp base",
                     b.vars.v[i]->reg);
            goto done;
        }
    }
    for (i = 0; i < (size_t)b.nfscratch; i++) {
        size_t j;
        for (j = 0; j < b.vars.n; j++) {
            CBVar *v = b.vars.v[j];
            if (!v->type.vec && v->type.flt &&
                (v->reg == b.fscratch[i] ||
                 (v->type.bits == 128 && v->reg + 2 == b.fscratch[i]))) {
                cb_error(&b, line, 1, "ZA5005", "bound register %%f%d also appears in cscratch",
                         v->reg);
                goto done;
            }
        }
    }
    for (i = 0; i < (size_t)b.nscratch; i++) {
        size_t j;
        if (b.scratch[i] == b.temp_reg) {
            cb_error(&b, line, 1, "ZA5005", "the ctemp base register cannot also be cscratch");
            goto done;
        }
        for (j = 0; j < b.vars.n; j++) {
            CBVar *v = b.vars.v[j];
            if (!v->type.flt && !v->type.vec && v->reg >= 0 && v->reg == b.scratch[i]) {
                cb_error(&b, line, 1, "ZA5005", "bound register %%r%d also appears in cscratch", v->reg);
                goto done;
            }
        }
    }
    /* source text and per-byte line map */
    {
        Buf src = {0};
        int *lmap = NULL;
        size_t cap = 0, used = 0;
        for (i = body_start; i < n; i++) {
            size_t k, ln = strlen(body[i].text);
            if (used + ln + 2 > cap) {
                cap = (used + ln + 2) * 2;
                lmap = xrealloc(lmap, cap * sizeof *lmap);
            }
            for (k = 0; k < ln; k++) { buf_putc(&src, body[i].text[k]); lmap[used++] = body[i].line; }
            buf_putc(&src, '\n');
            lmap[used++] = body[i].line;
        }
        lmap = xrealloc(lmap, (used + 1) * sizeof *lmap);
        lmap[used] = line;
        memset(&p.l, 0, sizeof p.l);
        p.l.b = &b;
        p.l.src = buf_cstr(&src);
        p.l.line_at = lmap;
        p.l.n = used;
        lex_next(&p.l);
        while (p.l.tok.kind != TK_EOF && !p.err) parse_stmt(&p, &stmts);
        if (p.err) ok = 0;
        if (ok && !check_gotos(&b, &stmts)) ok = 0;
        if (ok) {
            Cg cg;
            b.base_slots = b.next_var_slot;
            b.save_r0 = b.save_r1 = -1;
            if (b.temp_reg >= 0) {
                int need0 = !has_scratch(&b, 0), need1 = !has_scratch(&b, 1);
                if (need0 && need1 && b.temp_down) {
                    /* keep R0 below R1 in memory so STMG 0,1 stores them
                     * in register order */
                    b.save_r1 = b.base_slots++;
                    b.save_r0 = b.base_slots++;
                } else {
                    if (need0) b.save_r0 = b.base_slots++;
                    if (need1) b.save_r1 = b.base_slots++;
                }
            }
            memset(&cg, 0, sizeof cg);
            cg.b = &b;
            cg.base_slots = b.base_slots;
            cg.slot_top = b.base_slots;
            cg.max_slot = b.base_slots;
            asm_line(&b, line, "* cblock %s", b.name);
            if (b.save_r0 >= 0 && b.save_r1 >= 0)
                asm_line(&b, line, "STMG %%r0,%%r1,%ld(%%r%d)", slot_off(&b, b.save_r0), b.temp_reg);
            else {
                if (b.save_r0 >= 0) store_slot(&b, line, b.save_r0, type_of(64, 1), 0);
                if (b.save_r1 >= 0) store_slot(&b, line, b.save_r1, type_of(64, 1), 1);
            }
            for (i = 0; i < b.vars.n; i++) {
                CBVar *v = b.vars.v[i];
                if (v->kind == V_CIN || v->kind == V_CINOUT) {
                    if (v->type.vec) store_vec_slot(&b, line, v->slot, v->reg);
                    else if (v->type.flt) store_fp_slot(&b, line, v->slot, v->type, v->reg);
                    else store_slot(&b, line, v->slot, v->type, v->reg);
                }
            }
            for (i = 0; i < stmts.n && ok; i++) ok = codegen_stmt(&cg, &stmts.v[i]);
            if (ok && cg.max_slot * 8 > b.temp_size) {
                cb_error(&b, line, 1, "ZA5006", "cblock temp area needs %d bytes, but ctemp declares %ld",
                         cg.max_slot * 8, b.temp_size);
                ok = 0;
            }
            if (ok) {
                for (i = 0; i < b.vars.n; i++) {
                    CBVar *v = b.vars.v[i];
                    if (v->kind == V_COUT || v->kind == V_CINOUT) {
                        if (v->type.vec) load_vec_slot(&b, line, v->slot, v->reg);
                        else if (v->type.flt) load_fp_slot(&b, line, v->slot, v->type, v->reg);
                        else load_slot(&b, line, v->slot, v->type, v->reg);
                    }
                }
                if (b.save_r0 >= 0 && b.save_r1 >= 0)
                    asm_line(&b, line, "LMG %%r0,%%r1,%ld(%%r%d)", slot_off(&b, b.save_r0), b.temp_reg);
                else {
                    if (b.save_r1 >= 0) load_slot(&b, line, b.save_r1, type_of(64, 1), 1);
                    if (b.save_r0 >= 0) load_slot(&b, line, b.save_r0, type_of(64, 1), 0);
                }
            }
            if (ok && b.nlits) {
                b.lit_end = new_label(&b);
                asm_line(&b, line, "BRCL 15,%s", b.lit_end);
            }
            if (b.debug) {
                emit_label(&b, line, b.end_label);
                if (ok) {
                    CBlockDebug cd;
                    size_t nv = 0, vi;
                    memset(&cd, 0, sizeof cd);
                    cd.file = xstrdup(file);
                    cd.name = xstrdup(b.name);
                    cd.start_label = b.start_label;
                    cd.end_label = b.end_label;
                    cd.line = line;
                    cd.temp_reg = b.temp_reg;
                    b.start_label = b.end_label = NULL;
                    for (vi = 0; vi < b.vars.n; vi++)
                        if (b.vars.v[vi]->kind == V_LOCAL) nv++;
                    if (nv) cd.vars = xcalloc(nv * sizeof *cd.vars);
                    for (vi = 0; vi < b.vars.n; vi++) {
                        CBVar *v = b.vars.v[vi];
                        CBlockVarDebug *cv;
                        if (v->kind != V_LOCAL) continue;
                        cv = &cd.vars[cd.nvars++];
                        cv->name = xstrdup(v->name);
                        cv->bits = v->type.ptr ? 0 : v->type.bits;
                        cv->unsig = v->type.unsig;
                        cv->ptr = v->type.ptr;
                        cv->off = slot_off(&b, v->slot);
                    }
                    vec_push(b.debug, cd);
                }
            }
            if (ok && b.nlits) emit_literal_data(&b, line);
        }
        for (i = 0; i < p.l.ids.n; i++) free(p.l.ids.v[i]);
        free(p.l.ids.v);
        free(lmap);
        free(src.p);
    }
done:
    free(b.start_label);
    free(b.end_label);
    if (label_counter) {
        int used = b.label_no > b.label_seq ? b.label_no : b.label_seq;
        if (used > *label_counter) *label_counter = used;
    }
    for (i = 0; i < stmts.n; i++) {
        cblock_expr_free(stmts.v[i].expr);
        cblock_expr_free(stmts.v[i].lval);
    }
    if (!ok) {
        size_t k;
        for (k = out_start; k < out->n; k++) free((char *)out->v[k].text);
        out->n = out_start;
    }
    free(stmts.v);
    free_block(&b);
    return ok ? 0 : 1;
}
