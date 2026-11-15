/* isagen - build src/isa_tab.c from isa/isa.json.
 *
 *   cc -std=c99 -o isagen tools/isagen.c src/util.c
 *   ./isagen isa/isa.json > src/isa_tab.c
 *
 * isa/isa.json is the source of truth for the instruction set; it is
 * produced from the Principles of Operation by tools/pop2json.py. The
 * generator checks operand names, lengths and hex strings and exits with a
 * message on any error.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/util.h"

/* ---- minimal JSON reader ---------------------------------------------- */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct J {
    JType t;
    double num;
    char *str;       /* J_STR */
    size_t n;        /* items of J_ARR / J_OBJ */
    struct J **v;    /* values */
    char **k;        /* keys (J_OBJ) */
} J;

typedef struct {
    const char *p, *end, *file;
} Parser;

static void fail(Parser *ps, const char *msg) {
    int line = 1;
    const char *q;
    (void)q;
    fprintf(stderr, "isagen: %s: %s (near offset %ld)\n", ps->file, msg, (long)(ps->p - ps->file));
    (void)line;
    exit(1);
}

static void ws(Parser *ps) {
    while (ps->p < ps->end && isspace((unsigned char)*ps->p)) ps->p++;
}

static J *jnew(JType t) {
    J *j = xcalloc(sizeof *j);
    j->t = t;
    return j;
}

static void jadd(J *a, char *key, J *v) {
    a->v = xrealloc(a->v, (a->n + 1) * sizeof *a->v);
    a->v[a->n] = v;
    if (a->t == J_OBJ) {
        a->k = xrealloc(a->k, (a->n + 1) * sizeof *a->k);
        a->k[a->n] = key;
    }
    a->n++;
}

static char *pstring(Parser *ps) {
    Buf b = {0};
    ps->p++; /* opening quote */
    while (ps->p < ps->end && *ps->p != '"') {
        if (*ps->p == '\\') {
            ps->p++;
            if (ps->p >= ps->end) fail(ps, "unterminated string");
            switch (*ps->p) {
            case 'n': buf_putc(&b, '\n'); break;
            case 't': buf_putc(&b, '\t'); break;
            case 'r': buf_putc(&b, '\r'); break;
            case 'b': buf_putc(&b, '\b'); break;
            case 'f': buf_putc(&b, '\f'); break;
            case 'u': {
                unsigned v = 0;
                int i;
                for (i = 1; i <= 4; i++) {
                    if (ps->p + i >= ps->end || !isxdigit((unsigned char)ps->p[i])) fail(ps, "bad \\u escape");
                    v = v * 16 + (unsigned)(isdigit((unsigned char)ps->p[i]) ? ps->p[i] - '0'
                                                                             : (tolower((unsigned char)ps->p[i]) - 'a' + 10));
                }
                ps->p += 4;
                buf_putc(&b, v < 128 ? (int)v : '?'); /* tables are ASCII */
                break;
            }
            default: buf_putc(&b, *ps->p);
            }
            ps->p++;
        } else
            buf_putc(&b, *ps->p++);
    }
    if (ps->p >= ps->end) fail(ps, "unterminated string");
    ps->p++;
    return (char *)buf_cstr(&b);
}

static J *pvalue(Parser *ps) {
    J *j;
    ws(ps);
    if (ps->p >= ps->end) fail(ps, "unexpected end of file");
    switch (*ps->p) {
    case '{':
        j = jnew(J_OBJ);
        ps->p++;
        ws(ps);
        if (*ps->p == '}') {
            ps->p++;
            return j;
        }
        for (;;) {
            char *key;
            ws(ps);
            if (*ps->p != '"') fail(ps, "object key expected");
            key = pstring(ps);
            ws(ps);
            if (*ps->p++ != ':') fail(ps, "':' expected");
            jadd(j, key, pvalue(ps));
            ws(ps);
            if (*ps->p == ',') {
                ps->p++;
                continue;
            }
            if (*ps->p == '}') {
                ps->p++;
                return j;
            }
            fail(ps, "',' or '}' expected");
        }
    case '[':
        j = jnew(J_ARR);
        ps->p++;
        ws(ps);
        if (*ps->p == ']') {
            ps->p++;
            return j;
        }
        for (;;) {
            jadd(j, NULL, pvalue(ps));
            ws(ps);
            if (*ps->p == ',') {
                ps->p++;
                continue;
            }
            if (*ps->p == ']') {
                ps->p++;
                return j;
            }
            fail(ps, "',' or ']' expected");
        }
    case '"':
        j = jnew(J_STR);
        j->str = pstring(ps);
        return j;
    default:
        if (!strncmp(ps->p, "true", 4) || !strncmp(ps->p, "false", 5)) {
            j = jnew(J_BOOL);
            j->num = ps->p[0] == 't';
            ps->p += j->num ? 4 : 5;
            return j;
        }
        if (!strncmp(ps->p, "null", 4)) {
            ps->p += 4;
            return jnew(J_NULL);
        }
        {
            char *e;
            j = jnew(J_NUM);
            j->num = strtod(ps->p, &e);
            if (e == ps->p) fail(ps, "value expected");
            ps->p = e;
            return j;
        }
    }
}

static J *get(const J *o, const char *key) {
    size_t i;
    if (o->t != J_OBJ) return NULL;
    for (i = 0; i < o->n; i++)
        if (!strcmp(o->k[i], key)) return o->v[i];
    return NULL;
}

static J *need(const J *o, const char *key, JType t, const char *ctx) {
    J *v = get(o, key);
    if (!v || v->t != t) {
        fprintf(stderr, "isagen: %s: missing or wrong type of \"%s\"\n", ctx, key);
        exit(1);
    }
    return v;
}

/* ---- generator ------------------------------------------------------------ */

static void cstr(const char *s) {
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') putchar('\\');
        putchar(*s);
    }
    putchar('"');
}

static char *upper(const char *s) { return za_strupper(s); }

static int op_index(const J *ops, const char *s) {
    size_t i;
    for (i = 0; i < ops->n; i++) {
        const J *nm = get(ops->v[i], "name");
        if (nm && nm->t == J_STR && !strcmp(nm->str, s)) return (int)i;
    }
    return -1;
}

static int hexval(char c) {
    return isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10;
}

/* hex string -> 6 bytes, zero padded */
static void hex6(const char *s, const char *ctx, const char *what, int length, unsigned char out[6]) {
    size_t n = strlen(s), i;
    memset(out, 0, 6);
    if (n != (size_t)length * 2) {
        fprintf(stderr, "isagen: %s: \"%s\" needs %d hex digits\n", ctx, what, length * 2);
        exit(1);
    }
    for (i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)s[i])) {
            fprintf(stderr, "isagen: %s: bad hex digit in \"%s\"\n", ctx, what);
            exit(1);
        }
        out[i / 2] = (unsigned char)(out[i / 2] * 16 + hexval(s[i]));
    }
}

typedef struct {
    const J *inst;
    char *mn, *key;
    size_t order;
} Ent;

static int cmp_ent(const void *a, const void *b) {
    const Ent *x = a, *y = b;
    int c = strcmp(x->mn, y->mn);
    if (c) return c;
    c = strcmp(x->key, y->key);
    if (c) return c;
    return x->order < y->order ? -1 : x->order > y->order;
}

int main(int argc, char **argv) {
    size_t len, i, k;
    char *text;
    Parser ps;
    J *root, *ops, *insts;
    Ent *ents;
    if (argc != 2) {
        fprintf(stderr, "usage: isagen isa/isa.json > src/isa_tab.c\n");
        return 2;
    }
    text = read_file(argv[1], &len);
    if (!text) {
        perror(argv[1]);
        return 1;
    }
    ps.p = text;
    ps.end = text + len;
    ps.file = argv[1];
    root = pvalue(&ps);
    ops = need(root, "operands", J_ARR, "top level");
    insts = need(root, "instructions", J_ARR, "top level");

    printf("/* Generated by tools/isagen.c from isa/isa.json; DO NOT EDIT. */\n\n#include \"isa.h\"\n\n");
    printf("const IsaOperand isa_operand_tab[] = {\n");
    for (i = 0; i < ops->n; i++) {
        const J *o = ops->v[i];
        const J *fl = need(o, "flags", J_ARR, "operand");
        printf("    {");
        cstr(need(o, "name", J_STR, "operand")->str);
        printf(", %d, %d, ", (int)need(o, "bits", J_NUM, "operand")->num,
               (int)need(o, "shift", J_NUM, "operand")->num);
        if (!fl->n) printf("0");
        for (k = 0; k < fl->n; k++) {
            char *u = upper(fl->v[k]->str);
            printf("%sF_%s", k ? " | " : "", u);
            free(u);
        }
        printf("},\n");
    }
    printf("};\n\n");

    ents = xcalloc(insts->n * sizeof *ents);
    for (i = 0; i < insts->n; i++) {
        const J *in = insts->v[i];
        ents[i].inst = in;
        ents[i].mn = need(in, "mnemonic", J_STR, "instruction")->str;
        ents[i].key = need(in, "opcode", J_STR, "instruction")->str;
        ents[i].order = i;
    }
    qsort(ents, insts->n, sizeof *ents, cmp_ent);

    printf("const IsaInst isa_inst_tab[] = {\n");
    for (i = 0; i < insts->n; i++) {
        const J *in = ents[i].inst;
        const char *mn = ents[i].mn;
        const J *names = need(in, "operands", J_ARR, mn);
        int length = (int)need(in, "length", J_NUM, mn)->num;
        int opt = (int)need(in, "opt", J_NUM, mn)->num;
        const J *orj = get(in, "or");
        unsigned char opc[6], orv[6];
        if (length != 2 && length != 4 && length != 6) {
            fprintf(stderr, "isagen: %s: length must be 2, 4 or 6\n", mn);
            return 1;
        }
        if (names->n > 6) {
            fprintf(stderr, "isagen: %s: more than 6 operands\n", mn);
            return 1;
        }
        if (opt < 0 || (size_t)opt > names->n) {
            fprintf(stderr, "isagen: %s: bad \"opt\"\n", mn);
            return 1;
        }
        hex6(need(in, "opcode", J_STR, mn)->str, mn, "opcode", length, opc);
        if (orj && orj->t == J_STR)
            hex6(orj->str, mn, "or", length, orv);
        else
            memset(orv, 0, sizeof orv);
        printf("    {");
        cstr(mn);
        printf(", ");
        cstr(need(in, "format", J_STR, mn)->str);
        printf(", {");
        for (k = 0; k < 6; k++) printf("%s0x%02X", k ? ", " : "", opc[k]);
        printf("}, {");
        for (k = 0; k < 6; k++) printf("%s0x%02X", k ? ", " : "", orv[k]);
        printf("}, %d, {", length);
        for (k = 0; k < 6; k++) {
            int idx = 0;
            if (k < names->n) {
                idx = op_index(ops, names->v[k]->str);
                if (idx < 0) {
                    fprintf(stderr, "isagen: %s: unknown operand \"%s\"\n", mn, names->v[k]->str);
                    return 1;
                }
            }
            printf("%s%d", k ? ", " : "", idx);
        }
        printf("}, %d, ", opt);
        cstr(need(in, "facility", J_STR, mn)->str);
        printf(", ");
        cstr(need(in, "desc", J_STR, mn)->str);
        printf("},\n");
    }
    printf("};\n\nconst size_t isa_inst_count = sizeof isa_inst_tab / sizeof isa_inst_tab[0];\n");
    return 0;
}
