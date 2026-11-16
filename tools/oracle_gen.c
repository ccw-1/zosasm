/* oracle_gen.c - write HLASM statements for the instruction table, with sample
 * operands, for a comparison with the real HLASM (see tools/oracle.sh).
 *
 *   oracle_gen [-i] [-p reg|range] [first last] > instr.asm
 *
 * Without -p one statement per table entry with valid operands (register
 * numbers that are valid register pair starts). Lines 1 and 2 are "T CSECT" and
 * "DS 0H"; table entry n is source line n + 3, which is also its HLASM
 * statement number. With -p reg or -p range every statement changes one operand
 * of one entry to a boundary or invalid value (register numbers, immediates,
 * displacements, lengths, branch offsets), all other operands stay valid.
 * -i  write "line index mnemonic format operand-kind value" lines instead of
 *     the source (same arguments).
 *
 * Build: cc -std=c99 -Isrc -o build/oracle_gen tools/oracle_gen.c \
 *        src/isa.c src/isa_tab.c src/util.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "isa.h"

static const int gpr_vals[] = {4, 6, 8, 10};
static const int fpr_vals[] = {0, 4, 8, 12};
static const int u4_vals[] = {5, 2, 9, 6, 3};

/* the pieces of one assembler operand: up to 3 strings (D, X/L/R, B) */
typedef struct {
    char p[3][40];
    char kind; /* R register, I immediate, J relative, A address */
    const IsaOperand *op[3];
    int has_mid, has_base;
    char mid_kind;
} Piece;

static void default_value(const IsaOperand *op, int *ng, int *nf, int *nu, char *buf) {
    unsigned f = op->flags;
    if (f & F_GPR) sprintf(buf, "%d", gpr_vals[(*ng)++ & 3]);
    else if (f & F_FPR) sprintf(buf, "%d", fpr_vals[(*nf)++ & 3]);
    else if (f & F_VR) {
        int v = op->shift == 8 ? 17 : op->shift == 12 ? 3 : op->shift == 16 ? 20 : 9;
        sprintf(buf, "%d", v);
    } else if (f & (F_AR | F_CR)) sprintf(buf, "%d", gpr_vals[(*ng)++ & 3]);
    else if (f & F_PCREL) strcpy(buf, "*+14");
    else if (op->name[0] == 'U') sprintf(buf, "%d", u4_vals[(*nu)++ % 5]);
    else if (op->name[0] == 'I') {
        int bits = op->bits;
        if (bits <= 4) strcpy(buf, "5");
        else if (bits <= 8) strcpy(buf, "60");
        else if (bits <= 12) strcpy(buf, "291");
        else if (bits <= 16) strcpy(buf, "4660");
        else strcpy(buf, "305419896");
    } else strcpy(buf, "1");
}

static int build(const IsaInst *in, const IsaSlots *sl, Piece *pc) {
    int k, ng = 0, nf = 0, nu = 0, nl = 0;
    for (k = 0; k < sl->n; k++) {
        const IsaSlot *s = &sl->s[k];
        Piece *p = &pc[k];
        memset(p, 0, sizeof *p);
        if (s->kind == SK_ADDR) {
            const IsaOperand *d = isa_op(in, (size_t)s->field);
            p->kind = 'A';
            p->op[0] = d;
            sprintf(p->p[0], "%d", (d->flags & F_SIGNED) ? 74565 : 291);
            if (s->mid >= 0) {
                const IsaOperand *m = isa_op(in, (size_t)s->mid);
                p->has_mid = 1;
                p->mid_kind = s->mid_kind;
                p->op[1] = m;
                if (s->mid_kind == 'X') strcpy(p->p[1], (m->flags & F_VR) ? "21" : "4");
                else if (s->mid_kind == 'L') {
                    /* the second length smaller than the first (DP, MP), at most 16 (CDPT) */
                    strcpy(p->p[1], nl++ ? "4" : "8");
                }
                else default_value(m, &ng, &nf, &nu, p->p[1]);
            }
            if (s->base >= 0) {
                p->has_base = 1;
                p->op[2] = isa_op(in, (size_t)s->base);
                strcpy(p->p[2], "6");
            }
        } else {
            const IsaOperand *o = isa_op(in, (size_t)s->field);
            p->kind = (s->kind == SK_REG) ? 'R' : (s->kind == SK_REL) ? 'J' : 'I';
            p->op[0] = o;
            default_value(o, &ng, &nf, &nu, p->p[0]);
        }
    }
    return sl->n;
}

static void render(const IsaInst *in, const Piece *pc, int n, char *line, size_t sz) {
    int k;
    snprintf(line, sz, "         %-5s ", in->mnemonic);
    for (k = 0; k < n; k++) {
        const Piece *p = &pc[k];
        if (k) strcat(line, ",");
        strcat(line, p->p[0]);
        if (p->kind == 'A' && (p->has_mid || p->has_base)) {
            strcat(line, "(");
            if (p->has_mid) strcat(line, p->p[1]);
            if (p->has_base) {
                if (p->has_mid) strcat(line, ",");
                strcat(line, p->p[2]);
            }
            strcat(line, ")");
        }
    }
}

static long out_line;
static int info;

static void put(size_t idx, const IsaInst *in, const Piece *pc, int n, const char *what,
                const char *val) {
    char line[200];
    render(in, pc, n, line, sizeof line);
    if (strlen(line) > 71) {
        fprintf(stderr, "line too long: %zu\n", idx);
        exit(1);
    }
    if (info) printf("%ld %zu %s %s %s %s\n", out_line, idx, in->mnemonic, in->format, what, val);
    else printf("%s\n", line);
    out_line++;
}

static void probe_reg(size_t idx, const IsaInst *in, Piece *pc, int n, int k) {
    Piece *p = &pc[k];
    const IsaOperand *o = p->op[0];
    unsigned f = o->flags;
    static const char *gp[] = {"5", "15", "16", NULL};
    static const char *fp[] = {"1", "2", "3", "5", "15", "16", NULL};
    static const char *vp[] = {"31", "32", NULL};
    static const char *ap[] = {"15", "16", NULL};
    const char **v = (f & F_GPR) ? gp : (f & F_FPR) ? fp : (f & F_VR) ? vp : ap;
    char save[40], what[16];
    strcpy(save, p->p[0]);
    sprintf(what, "R%d", k);
    for (; *v; v++) {
        strcpy(p->p[0], *v);
        put(idx, in, pc, n, what, *v);
    }
    strcpy(p->p[0], save);
}

static void probe_range(size_t idx, const IsaInst *in, Piece *pc, int n, int k) {
    Piece *p = &pc[k];
    char save[3][40], what[16], val[40];
    memcpy(save, p->p, sizeof save);
    if (p->kind == 'I') {
        const IsaOperand *o = p->op[0];
        long long mx = (1LL << o->bits) - 1, h = 1LL << (o->bits - 1);
        long long vs[32];
        int i, nv = 0;
        sprintf(what, "I%d", k);
        if (o->name[0] == 'U') { /* masks: every value */
            for (i = 0; i <= 16; i++) vs[nv++] = i;
            vs[nv++] = -1;
        } else {
            vs[nv++] = 0;
            vs[nv++] = mx;
            vs[nv++] = mx + 1;
            vs[nv++] = mx + 2;
            vs[nv++] = 2 * mx + 1;
            vs[nv++] = 2 * mx + 2;
            vs[nv++] = -1;
            vs[nv++] = -h;
            vs[nv++] = -h - 1;
            vs[nv++] = -2 * h;
            vs[nv++] = -2 * h - 1;
        }
        for (i = 0; i < nv; i++) {
            if (vs[i] > 2147483647LL || vs[i] < -2147483648LL) continue; /* not a decimal term */
            sprintf(val, "%lld", vs[i]);
            strcpy(p->p[0], val);
            put(idx, in, pc, n, what, val);
        }
    } else if (p->kind == 'J') {
        static const char *vs[] = {"*+2", "*+3", "*-2", "*+0", NULL};
        int i;
        sprintf(what, "J%d", k);
        for (i = 0; vs[i]; i++) {
            strcpy(p->p[0], vs[i]);
            put(idx, in, pc, n, what, vs[i]);
        }
    } else if (p->kind == 'A') {
        static const char *d12[] = {"4095", "4096", "-1", NULL};
        static const char *d20[] = {"524287", "524288", "-524288", "-524289", NULL};
        static const char *lens4[] = {"0", "1", "2", "15", "16", "17", NULL};
        static const char *lens8[] = {"0",  "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",  "9",
                                      "10", "11", "12", "13", "14", "15", "16", "17", "18", "19",
                                      "20", "24", "30", "31", "32", "33", "34", "40", "48", "63",
                                      "64", "65", "66", "128", "129", "255", "256", "257", NULL};
        static const char *rb[] = {"15", "16", NULL};
        const char **v;
        sprintf(what, "D%d", k);
        for (v = (p->op[0]->flags & F_SIGNED) ? d20 : d12; *v; v++) {
            strcpy(p->p[0], *v);
            put(idx, in, pc, n, what, *v);
        }
        strcpy(p->p[0], save[0]);
        if (p->has_mid) {
            sprintf(what, "M%d", k);
            if (p->mid_kind == 'L') v = p->op[1]->bits == 4 ? lens4 : lens8;
            else v = rb;
            for (; *v; v++) {
                strcpy(p->p[1], *v);
                put(idx, in, pc, n, what, *v);
            }
            strcpy(p->p[1], save[1]);
        }
        if (p->has_base) {
            sprintf(what, "B%d", k);
            for (v = rb; *v; v++) {
                strcpy(p->p[2], *v);
                put(idx, in, pc, n, what, *v);
            }
            strcpy(p->p[2], save[2]);
        }
    }
    memcpy(p->p, save, sizeof save);
}

int main(int argc, char **argv) {
    size_t first = 0, last = isa_inst_count, i;
    int a = 1, mode = 0; /* 0 base, 1 reg, 2 range */
    while (a < argc) {
        if (!strcmp(argv[a], "-i")) {
            info = 1;
            a++;
        } else if (!strcmp(argv[a], "-p") && a + 1 < argc) {
            mode = !strcmp(argv[a + 1], "reg") ? 1 : 2;
            a += 2;
        } else break;
    }
    if (a + 1 < argc) {
        first = (size_t)atol(argv[a]);
        last = (size_t)atol(argv[a + 1]);
        if (last > isa_inst_count) last = isa_inst_count;
    }
    if (!info) {
        printf("T        CSECT\n");
        printf("         DS    0H\n");
    }
    out_line = 3;
    for (i = first; i < last; i++) {
        const IsaInst *in = &isa_inst_tab[i];
        IsaSlots sl;
        Piece pc[6];
        int n, k;
        /* macros in the HLASM system library: their expansion shifts the statement numbers */
        if (!strcmp(in->mnemonic, "pgin") || !strcmp(in->mnemonic, "pgout")) continue;
        isa_slots(in, &sl);
        n = build(in, &sl, pc);
        if (mode == 0) {
            put(i, in, pc, n, "-", "-");
            continue;
        }
        for (k = 0; k < n; k++) {
            if (mode == 1 && pc[k].kind == 'R') probe_reg(i, in, pc, n, k);
            if (mode == 2 && pc[k].kind != 'R') probe_range(i, in, pc, n, k);
        }
    }
    if (!info) printf("         END\n");
    return 0;
}
