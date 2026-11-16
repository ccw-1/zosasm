/* dirctl.c - assembler controls: ACONTROL, PUSH, POP, CEJECT, EXITCTL, REPRO.
 * Only what makes sense for a cross assembler has an effect (DESIGN.md 4.7):
 * AFPR and TYPECHECK(REGISTER) change the register checks, PUSH and POP save
 * the PRINT (listing), USING and ACONTROL states.  The other ACONTROL
 * options are checked and ignored; EXITCTL is checked and ignored. */
#include <stdlib.h>
#include <string.h>

#include "asm.h"

/* the text of an operand as upper case keyword and its parenthesized argument
 * (NULL without one); returns 0 when the shape is wrong */
static int split_item(const char *t, char **word, char **arg) {
    size_t n = 0, len = strlen(t);
    while (n < len && t[n] != '(') n++;
    *word = za_strupper(str_trim(xstrndup(t, n)));
    *arg = NULL;
    if (n == len) return 1;
    if (t[len - 1] != ')') return 0;
    *arg = za_strupper(str_trim(xstrndup(t + n + 1, len - n - 2)));
    return 1;
}

static int in_list(const char *w, const char *const *list) {
    size_t i;
    for (i = 0; list[i]; i++)
        if (!strcmp(w, list[i])) return 1;
    return 0;
}

static size_t list_len(const char *const *list) {
    size_t n = 0;
    while (list[n]) n++;
    return n;
}

static void bad_word(Asm *a, Stmt *st, Operand o, const char *code, const char *what, const char *w,
                     const char *const *cands) {
    Diag *d = rep_errorf(a->rep, code, asm_op_span(st, o), "unknown %s \"%s\"", what, w);
    const char *c = diag_closest(w, cands, list_len(cands));
    if (c) diag_help(d, "did you mean \"%s\"?", c);
    st->bad = 1;
}

/* a comma separated list of keywords inside parentheses */
static void check_keywords(Asm *a, Stmt *st, Operand o, const char *opt, const char *arg,
                           const char *const *kw) {
    OperandVec items = {0};
    size_t i;
    if (!arg || !*arg) {
        rep_errorf(a->rep, "ZA2072", asm_op_span(st, o), "%s needs a list in parentheses", opt);
        st->bad = 1;
        return;
    }
    split_operands(&items, arg);
    for (i = 0; i < items.n; i++) {
        char *w = str_trim(items.v[i].text);
        if (!*w || (!in_list(w, kw) && !za_isdigit((unsigned char)w[0]))) {
            bad_word(a, st, o, "ZA2072", "keyword in ACONTROL", w, kw);
            return;
        }
    }
}

void dir_acontrol(Asm *a, Stmt *st) {
    static const char *const sel[] = {"AFPR",  "NOAFPR", "CA",     "CE",      "CU",     "COMPAT",
                                      "NOCOMPAT", "FLAG", "ILMA",  "NOILMA",  "LIBMAC", "NOLIBMAC",
                                      "MACHINE", "OPTABLE", "RA2", "NORA2", "TYPECHECK", "NOTYPECHECK",
                                      NULL};
    static const char *const compat[] = {"CASE",      "NOCASE",   "CPAT",    "LITTYPE", "NOLITTYPE",
                                         "MACROCASE", "NOMACROCASE", "SYSLIST", "NOSYSLIST",
                                         "TRANSDT",   "NOTRANSDT", NULL};
    static const char *const typecheck[] = {"MAGNITUDE", "NOMAGNITUDE", "REGISTER", "NOREGISTER", NULL};
    static const char *const flag[] = {"ALIGN", "NOALIGN", "CONT", "NOCONT", "EXLITW", "NOEXLITW",
                                       "IMPLEN", "NOIMPLEN", "LONGER", "NOLONGER", "PAGE0",
                                       "NOPAGE0", "PUSH", "NOPUSH", "RECORD", "NORECORD", "SUBSTR",
                                       "NOSUBSTR", "USING0", "NOUSING0", NULL};
    OperandVec ops = {0};
    size_t i;
    if (!asm_ops(a, st, 1, -1, &ops)) return;
    for (i = 0; i < ops.n; i++) {
        char *w, *arg;
        char *t = str_trim(ops.v[i].text);
        int paren;
        if (!split_item(t, &w, &arg)) {
            if (a->pass == 1) {
                rep_errorf(a->rep, "ZA2072", asm_op_span(st, ops.v[i]), "\"%s\" is not an ACONTROL selection", t);
                st->bad = 1;
            }
            continue;
        }
        paren = arg != NULL;
        if (!in_list(w, sel)) {
            if (a->pass == 1) bad_word(a, st, ops.v[i], "ZA2072", "ACONTROL selection", w, sel);
            continue;
        }
        if (!strcmp(w, "AFPR")) {
            a->afpr = 1;
        } else if (!strcmp(w, "NOAFPR")) {
            a->afpr = 0;
        } else if (!strcmp(w, "NOTYPECHECK")) {
            a->strict = 0;
        } else if (!strcmp(w, "TYPECHECK")) {
            if (a->pass == 1) check_keywords(a, st, ops.v[i], w, arg, typecheck);
            if (arg) {
                OperandVec items = {0};
                size_t k;
                split_operands(&items, arg);
                for (k = 0; k < items.n; k++) {
                    char *kw = str_trim(items.v[k].text);
                    if (!strcmp(kw, "REGISTER")) a->strict = 1;
                    if (!strcmp(kw, "NOREGISTER")) a->strict = 0;
                }
            }
        } else if (a->pass == 1) {
            if (!strcmp(w, "COMPAT"))
                check_keywords(a, st, ops.v[i], w, arg, compat);
            else if (!strcmp(w, "FLAG"))
                check_keywords(a, st, ops.v[i], w, arg, flag);
            else if (!strcmp(w, "CA") || !strcmp(w, "CE")) {
                int ca = w[1] == 'A';
                long v = arg ? strtol(arg, NULL, 10) : -1;
                int ok = arg && (!strcmp(arg, "LOCAL") || v == (ca ? 819 : 1047));
                if (!ok) {
                    diag_help(rep_errorf(a->rep, "ZA2073", asm_op_span(st, ops.v[i]),
                                         "zosasm writes %s constants in %s only", ca ? "CA" : "CE",
                                         ca ? "ISO 8859-1 (CCSID 819)" : "IBM-1047 (CCSID 1047)"),
                              "use %s(%s) or %s(LOCAL)", w, ca ? "819" : "1047", w);
                    st->bad = 1;
                }
            } else if ((!strcmp(w, "CU") || !strcmp(w, "MACHINE") || !strcmp(w, "OPTABLE")) && !paren) {
                rep_errorf(a->rep, "ZA2072", asm_op_span(st, ops.v[i]), "%s needs a value in parentheses", w);
                st->bad = 1;
            }
        }
    }
}

/* ---- PUSH and POP ------------------------------------------------------ */

typedef struct {
    int print, using, acontrol, noprint;
} PushOps;

static int push_ops(Asm *a, Stmt *st, PushOps *p) {
    OperandVec ops = {0};
    size_t i;
    memset(p, 0, sizeof *p);
    if (!asm_ops(a, st, 1, 4, &ops)) return 0;
    for (i = 0; i < ops.n; i++) {
        char *w = za_strupper(str_trim(ops.v[i].text));
        int *flag = !strcmp(w, "PRINT")      ? &p->print
                    : !strcmp(w, "USING")    ? &p->using
                    : !strcmp(w, "ACONTROL") ? &p->acontrol
                    : !strcmp(w, "NOPRINT")  ? &p->noprint
                                             : NULL;
        if (!flag) {
            static const char *const kw[] = {"PRINT", "USING", "ACONTROL", "NOPRINT", NULL};
            if (a->pass == 1) bad_word(a, st, ops.v[i], "ZA2074", "operand of PUSH/POP", w, kw);
            st->bad = 1;
            return 0;
        }
        if (*flag) {
            if (a->pass == 1) {
                rep_errorf(a->rep, "ZA2074", asm_op_span(st, ops.v[i]), "%s is given twice", w);
                st->bad = 1;
            }
            return 0;
        }
        *flag = 1;
    }
    if (!p->print && !p->using && !p->acontrol) {
        if (a->pass == 1) {
            diag_help(rep_errorf(a->rep, "ZA2074", st->op_span, "%s needs PRINT, USING or ACONTROL",
                                 st->op),
                      "for example: %s USING", st->op);
            st->bad = 1;
        }
        return 0;
    }
    return 1;
}

static UsingVec copy_usings(const UsingVec *u) {
    UsingVec c = {0};
    size_t i;
    for (i = 0; i < u->n; i++) vec_push(&c, u->v[i]);
    return c;
}

void dir_pushpop(Asm *a, Stmt *st) {
    int push = za_streqi(st->op, "PUSH");
    PushOps p;
    if (!push_ops(a, st, &p)) return;
    if (p.print) {
        if (push) {
            a->print_depth++;
        } else if (a->print_depth == 0) {
            if (a->pass == 1) {
                rep_errorf(a->rep, "ZA2075", st->op_span, "POP PRINT without a PUSH PRINT");
                st->bad = 1;
            }
        } else {
            a->print_depth--;
        }
    }
    if (p.acontrol) {
        if (push) {
            AcState s;
            s.afpr = a->afpr;
            s.strict = a->strict;
            vec_push(&a->ac_stack, s);
        } else if (a->ac_stack.n == 0) {
            if (a->pass == 1) {
                rep_errorf(a->rep, "ZA2075", st->op_span, "POP ACONTROL without a PUSH ACONTROL");
                st->bad = 1;
            }
        } else {
            AcState s = a->ac_stack.v[--a->ac_stack.n];
            a->afpr = s.afpr;
            a->strict = s.strict;
        }
    }
    if (p.using && a->pass == 2) {
        if (push) {
            vec_push(&a->using_stack, copy_usings(&a->usings));
        } else if (a->using_stack.n == 0) {
            rep_errorf(a->rep, "ZA2075", st->op_span, "POP USING without a PUSH USING");
            st->bad = 1;
        } else {
            a->usings = a->using_stack.v[--a->using_stack.n];
        }
    }
}

/* ---- the others -------------------------------------------------------- */

/* CEJECT [lines]: only the listing has an effect (listing.c) */
void dir_ceject(Asm *a, Stmt *st) {
    OperandVec ops = {0};
    int64_t n;
    if (!asm_ops(a, st, 0, 1, &ops)) return;
    if (ops.n == 1 && a->pass == 1) asm_abs_expr(a, st, ops.v[0], "the number of lines", &n);
}

/* EXITCTL type,value[,value...]: user exits do not exist, the statement is checked */
void dir_exitctl(Asm *a, Stmt *st) {
    static const char *const types[] = {"SOURCE", "LIBRARY", "LISTING", "PUNCH", "OBJECT", "ADATA", "TRACE", NULL};
    OperandVec ops = {0};
    size_t i;
    if (!asm_ops(a, st, 2, 5, &ops)) return;
    if (a->pass != 1) return;
    {
        char *w = za_strupper(str_trim(ops.v[0].text));
        if (!in_list(w, types)) bad_word(a, st, ops.v[0], "ZA2076", "EXITCTL exit type", w, types);
    }
    for (i = 1; i < ops.n; i++) {
        int64_t v;
        asm_abs_expr(a, st, ops.v[i], "the exit control value", &v);
    }
}

/* REPRO: the next record would be copied into the object deck. GOFF objects have
 * no such records; the reader skips the record (reader.c) and this says so. */
void dir_repro(Asm *a, Stmt *st) {
    if (a->pass != 1) return;
    diag_help(rep_warnf(a->rep, "ZA2077", st->op_span,
                        "REPRO has no effect: a GOFF object has no card image records"),
              "the record that follows is skipped");
}
