/* directives.c - section, class, symbol and location counter directives. */
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "ebcdic.h"
#include "goff.h"

/* parse a whole decimal number (optional sign); 0 on failure or overflow */
int parse_i64(const char *s, int64_t *out) {
    int neg = 0;
    uint64_t v = 0;
    if (*s == '+' || *s == '-') neg = *s++ == '-';
    if (!*s) return 0;
    for (; *s; s++) {
        unsigned d;
        if (!za_isdigit((unsigned char)*s)) return 0;
        d = (unsigned)(*s - '0');
        if (v > (UINT64_MAX - d) / 10) return 0;
        v = v * 10 + d;
    }
    if (neg ? v > (uint64_t)INT64_MAX + 1 : v > (uint64_t)INT64_MAX) return 0;
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return 1;
}

/* ---- sections ---------------------------------------------------------- */

static ClassAttr new_class_attr(void) {
    ClassAttr c;
    memset(&c, 0, sizeof c);
    c.align = -1;
    c.fill = -1;
    c.priority = -1;
    return c;
}

/* the start the piece had in the previous assembly run (asm_assemble_all) */
static void piece_hint(Asm *a, Piece *p) {
    char *key;
    int64_t *v;
    if (!a->hints || !p->cls->sect) return;
    key = xasprintf("%s\x01%s\x01%s", p->cls->sect->name, p->cls->name, p->name);
    v = strmap_get(a->hints, key);
    if (v) p->hint = *v;
    free(key);
}

static Class *new_class(Asm *a, const char *name, Section *sect) {
    Class *c = xcalloc(sizeof *c);
    Piece *p = xcalloc(sizeof *p);
    c->name = xstrdup(name);
    c->sect = sect;
    c->attr = new_class_attr();
    p->name = xstrdup(c->name);
    p->cls = c;
    vec_push(&c->pieces, p);
    piece_hint(a, p);
    return c;
}

void dir_section(Asm *a, Stmt *st) {
    char *op = za_strupper(st->op);
    SectKind kind;
    Section *sec;
    char *genname = NULL;
    const char *secname = st->name;
    int unnamed = 0;
    int64_t start_addr = 0;
    if (!strcmp(op, "CSECT") || !strcmp(op, "START"))
        kind = K_CSECT;
    else if (!strcmp(op, "RSECT"))
        kind = K_RSECT;
    else if (!strcmp(op, "DSECT"))
        kind = K_DSECT;
    else
        kind = K_COM;
    if (!st->name[0] && kind == K_DSECT) {
        /* HLASM allows an unnamed dummy section; give the internal section
         * model a name derived from the statement index, the same in pass 1
         * and pass 2, and define no symbol */
        unnamed = 1;
        genname = xasprintf("$DSECT%06zu", a->cur_idx);
        secname = genname;
    }
    if (a->pass == 1) {
        if (!unnamed && st->name[0] && !asm_valid_stmt_name(st)) {
            rep_errorf(a->rep, "ZA1021", st->name_span, "\"%s\" is not a valid section name",
                       st->name);
            st->bad = 1;
            return;
        }
        if (!strcmp(op, "START") && st->operands[0]) {
            OperandVec ops = {0};
            int64_t v;
            if (!asm_ops(a, st, 0, 1, &ops)) return;
            if (ops.n == 1 && ops.v[0].text[0]) {
                if (!asm_abs_expr(a, st, ops.v[0], "the START address", &v)) return;
                if (v < 0 || v > ZA_MAX_LOC) {
                    rep_errorf(a->rep, "ZA3056", asm_op_span(st, ops.v[0]),
                               "the START address %lld is outside 0 to %lld", (long long)v,
                               (long long)ZA_MAX_LOC);
                    st->bad = 1;
                    return;
                }
                start_addr = v;
            }
        }
    }
    sec = strmap_get(&a->secs, secname);
    if (!sec) {
        Class *c;
        if (a->pass != 1) {
            free(genname);
            return;
        }
        sec = xcalloc(sizeof *sec);
        sec->name = xstrdup(secname);
        sec->kind = kind;
        sec->start_addr = kind == K_DSECT ? 0 : start_addr;
        sec->def = stmt_span(st);
        if (st->name[0] && !unnamed) {
            int created;
            Symbol *sym = asm_define(a, st, st->name, st->name_span, &created);
            if (!created) return; /* duplicate symbol, already reported */
            sym->kind = SYM_SECT;
            sym->sect = sec;
        }
        c = new_class(a, "B_TEXT", sec);
        if (kind == K_RSECT) c->attr.readonly = 1;
        free(c->pieces.v[0]->name);
        c->pieces.v[0]->name = xstrdup(sec->name);
        vec_push(&sec->classes, c);
        sec->cur = sec->text_cur = c->pieces.v[0];
        if (st->name[0] && !unnamed) asm_lookup(a, st->name)->piece = c->pieces.v[0];
        strmap_put(&a->secs, secname, sec);
        if (kind == K_DSECT) {
            size_t q;
            for (q = a->mod->sections.n; q > 0; q--)
                if (a->mod->sections.v[q - 1]->kind != K_DSECT) {
                    sec->prv_owner = a->mod->sections.v[q - 1];
                    break;
                }
        }
        vec_push(&a->mod->sections, sec); /* dsects are laid out but never emitted */
    } else if (sec->kind != kind && !(sec->kind == K_CSECT && kind == K_RSECT) && a->pass == 1) {
        diag_note(rep_errorf(a->rep, "ZA3003", st->op_span,
                             "\"%s\" was started as a %s and cannot be continued as a %s", st->name,
                             sectkind_name(sec->kind), sectkind_name(kind)),
                  sec->def, "\"%s\" was started here", st->name);
        st->bad = 1;
        return;
    }
    a->sect = sec;
    a->cur = sec->text_cur;
    free(genname);
    free(op);
}

void dir_loctr(Asm *a, Stmt *st) {
    Class *cl;
    Piece *p = NULL;
    size_t i;
    if (!asm_need_section(a, st)) return;
    if (!st->name[0]) {
        if (a->pass == 1) {
            rep_errorf(a->rep, "ZA3006", st->op_span, "LOCTR needs a name");
            st->bad = 1;
        }
        return;
    }
    cl = a->cur->cls;
    for (i = 0; i < cl->pieces.n; i++)
        if (!strcmp(cl->pieces.v[i]->name, st->name)) p = cl->pieces.v[i];
    if (!p) {
        int created;
        Symbol *s;
        if (a->pass != 1) return;
        p = xcalloc(sizeof *p);
        p->name = xstrdup(st->name);
        p->cls = cl;
        vec_push(&cl->pieces, p);
        piece_hint(a, p);
        s = asm_define(a, st, st->name, st->name_span, &created);
        if (created) {
            s->kind = SYM_SECT;
            s->piece = p;
            s->sect = a->sect;
        }
    }
    a->cur = p;
    a->sect->cur = p;
    if (cl == a->sect->classes.v[0]) a->sect->text_cur = p;
}

/* ---- CATTR / XATTR ---------------------------------------------------- */

typedef Vec(char *) StrVec;
typedef Vec(Span) SpanVec;

typedef struct {
    char *key;
    StrVec args;
    Span sp;
    SpanVec asp;
} AttrArg;

typedef Vec(AttrArg) AttrVec;

/* split NAME or NAME(a,b) operands */
static void parse_attrs(Asm *a, Stmt *st, const OperandVec *ops, AttrVec *out) {
    size_t k;
    for (k = 0; k < ops->n; k++) {
        const Operand *o = &ops->v[k];
        const char *t = o->text;
        size_t tl = strlen(t);
        AttrArg at;
        const char *par;
        memset(&at, 0, sizeof at);
        if (!tl) {
            if (a->pass == 1) {
                rep_errorf(a->rep, "ZA0011", asm_op_span(st, *o), "empty attribute");
                st->bad = 1;
            }
            continue;
        }
        at.sp = stmt_operand_span(st, o->off, (int)tl);
        par = strchr(t, '(');
        if (par) {
            size_t i = (size_t)(par - t), j;
            char *inner;
            OperandVec in = {0};
            if (t[tl - 1] != ')') {
                if (a->pass == 1) {
                    rep_errorf(a->rep, "ZA0012", at.sp,
                               "attribute \"%s\" is missing its closing parenthesis", t);
                    st->bad = 1;
                }
                continue;
            }
            {
                char *k0 = xstrndup(t, i);
                at.key = za_strupper(k0);
                free(k0);
            }
            at.sp = stmt_operand_span(st, o->off, (int)i);
            inner = xstrndup(t + i + 1, tl - i - 2);
            split_operands(&in, inner);
            for (j = 0; j < in.n; j++) {
                Span sp = stmt_operand_span(st, o->off + (int)i + 1 + in.v[j].off,
                                            (int)strlen(in.v[j].text));
                vec_push(&at.args, str_trim(in.v[j].text));
                vec_push(&at.asp, sp);
            }
            free(inner);
        } else {
            at.key = za_strupper(t);
        }
        vec_push(out, at);
    }
}

static void bad_attr(Asm *a, Stmt *st, const AttrArg *at, const char *const *valid, size_t n) {
    Diag *d = rep_errorf(a->rep, "ZA3030", at->sp, "unknown attribute \"%s\"", at->key);
    const char *c = diag_closest(at->key, valid, n);
    if (c) {
        diag_help(d, "did you mean \"%s\"?", c);
    } else {
        Buf b = {0};
        size_t i;
        for (i = 0; i < n; i++) {
            if (i) buf_puts(&b, ", ");
            buf_puts(&b, valid[i]);
        }
        diag_help(d, "valid attributes: %s", buf_cstr(&b));
    }
    st->bad = 1;
}

static int want_args(Asm *a, Stmt *st, const AttrArg *at, size_t n, const char *usage) {
    if (at->args.n == n && (n == 0 || at->args.v[0][0])) return 1;
    rep_errorf(a->rep, "ZA3031", at->sp, "attribute %s needs %d argument(s): %s", at->key, (int)n,
               usage);
    st->bad = 1;
    return 0;
}

static const char *const cattr_keys[] = {"ALIGN",    "DEFLOAD",  "EXECUTABLE",    "FILL", "MERGE",
                                         "NOLOAD",   "NOTEXECUTABLE", "PART",   "PRIORITY",
                                         "READONLY", "RENT",     "REUS",          "NONREUS", "NOTREUS", "MOVABLE",
                                         "REMOVABLE", "RMODE", "TASKING"};

static int attr_is_none(const char *v) {
    char *u = za_strupper(v);
    int r = !strcmp(u, "NONE");
    free(u);
    return r;
}

static int attr_num(Asm *a, Stmt *st, const AttrArg *at, int64_t lo, int64_t hi, int *out) {
    char *usage;
    int64_t n;
    int ok;
    usage = xasprintf("%s(n)", at->key);
    ok = want_args(a, st, at, 1, usage);
    free(usage);
    if (!ok) return 0;
    if (!parse_i64(at->args.v[0], &n) || n < lo || n > hi) {
        rep_errorf(a->rep, "ZA3033", at->asp.v[0], "%s needs a number from %lld to %lld", at->key,
                   (long long)lo, (long long)hi);
        st->bad = 1;
        return 0;
    }
    *out = (int)n;
    return 1;
}

static void apply_cattr(Asm *a, Stmt *st, Class *cl, const AttrArg *at) {
    const char *k = at->key;
    int n;
    if (!strcmp(k, "ALIGN")) {
        if (attr_num(a, st, at, 0, 12, &n)) cl->attr.align = n;
    } else if (!strcmp(k, "FILL")) {
        if (attr_num(a, st, at, 0, 255, &n)) cl->attr.fill = n;
    } else if (!strcmp(k, "PRIORITY")) {
        if (attr_num(a, st, at, 0, 1 << 30, &n)) cl->attr.priority = n;
    } else if (!strcmp(k, "RMODE")) {
        uint8_t r;
        if (!want_args(a, st, at, 1, "RMODE(24|31|64|ANY|NONE)")) return;
        if (attr_is_none(at->args.v[0])) { /* zosasm extension: no RMODE in the object */
            cl->attr.rmode = CLASS_RMODE_NONE;
            return;
        }
        if (!rmode_byte(at->args.v[0], &r)) {
            diag_help(rep_errorf(a->rep, "ZA3034", at->asp.v[0], "invalid RMODE \"%s\"",
                                 at->args.v[0]),
                      "use 24, 31, 64, ANY or NONE");
            st->bad = 1;
            return;
        }
        cl->attr.rmode = r;
    } else if (!strcmp(k, "TASKING")) { /* zosasm extension: TASKING(NONE) = unspecified */
        if (!want_args(a, st, at, 1, "TASKING(NONE)")) return;
        if (!attr_is_none(at->args.v[0])) {
            diag_help(rep_errorf(a->rep, "ZA3034", at->asp.v[0], "invalid TASKING \"%s\"",
                                 at->args.v[0]),
                      "use TASKING(NONE), or one of RENT, REUS and NONREUS");
            st->bad = 1;
            return;
        }
        cl->attr.tasking = GOFF_TASK_UNSPEC;
    } else if (!strcmp(k, "EXECUTABLE")) {
        cl->attr.executable = GOFF_EXEC_CODE;
    } else if (!strcmp(k, "NOTEXECUTABLE")) {
        cl->attr.executable = GOFF_EXEC_DATA;
    } else if (!strcmp(k, "READONLY")) {
        cl->attr.readonly = 1;
    } else if (!strcmp(k, "NOLOAD")) {
        cl->attr.noload = 1;
    } else if (!strcmp(k, "DEFLOAD")) {
        cl->attr.defload = 1;
    } else if (!strcmp(k, "MERGE")) {
        cl->attr.merge = 1;
    } else if (!strcmp(k, "RENT")) {
        cl->attr.tasking = GOFF_TASK_RENT;
    } else if (!strcmp(k, "REUS")) {
        cl->attr.tasking = GOFF_TASK_REUS;
    } else if (!strcmp(k, "MOVABLE")) {
        cl->attr.movable = 1;
    } else if (!strcmp(k, "REMOVABLE")) {
        cl->attr.removable = 1;
    } else if (!strcmp(k, "NONREUS") || !strcmp(k, "NOTREUS")) {
        cl->attr.tasking = GOFF_TASK_NONREUS;
    } else if (!strcmp(k, "PART")) {
        int created;
        Symbol *s;
        const char *why;
        if (!want_args(a, st, at, 1, "PART(name)")) return;
        why = NULL;
        cl->part = sym_text(at->args.v[0], &why);
        if (!cl->part) {
            diag_help(rep_errorf(a->rep, "ZA1021", at->asp.v[0], "\"%s\" is not a valid part name",
                                 at->args.v[0]),
                      "%s", why);
            st->bad = 1;
            return;
        }
        s = asm_define(a, st, cl->part, at->asp.v[0], &created);
        if (created) {
            s->kind = SYM_SECT;
            s->piece = cl->pieces.v[0];
            s->sect = cl->sect;
        }
    } else {
        bad_attr(a, st, at, cattr_keys, sizeof cattr_keys / sizeof cattr_keys[0]);
    }
}

void dir_cattr(Asm *a, Stmt *st) {
    OperandVec ops = {0};
    Class *cl = NULL;
    size_t i;
    if (!asm_need_section(a, st)) return;
    if (!st->name[0]) {
        if (a->pass == 1) {
            diag_help(rep_errorf(a->rep, "ZA3007", st->op_span,
                                 "CATTR needs a class name in the name field"),
                      "for example `D_INFO   CATTR ALIGN(3),NOLOAD`");
            st->bad = 1;
        }
        return;
    }
    if (a->sect->kind == K_DSECT) {
        if (a->pass == 1) {
            rep_errorf(a->rep, "ZA3008", st->op_span, "CATTR is not allowed in a DSECT");
            st->bad = 1;
        }
        return;
    }
    for (i = 0; i < a->sect->classes.n; i++)
        if (!strcmp(a->sect->classes.v[i]->name, st->name)) cl = a->sect->classes.v[i];
    asm_ops(a, st, 0, -1, &ops);
    if (cl && a->pass == 1 && ops.n > 0) {
        rep_errorf(a->rep, "ZA3009", st->name_span,
                   "class \"%s\" already exists in section \"%s\"; CATTR can only resume it "
                   "without attributes",
                   st->name, a->sect->name);
        st->bad = 1;
        return;
    }
    if (!cl) {
        AttrVec ats = {0};
        if (a->pass != 1) return;
        if (strlen(st->name) > 16) {
            rep_errorf(a->rep, "ZA3032", st->name_span,
                       "class name \"%s\" is longer than 16 characters", st->name);
            st->bad = 1;
            return;
        }
        cl = new_class(a, st->name, a->sect);
        cl->attr.cattr = 1;
        cl->attr.rmode = 1; /* RMODE(24) is the default of CATTR */
        cl->attr.tasking = GOFF_TASK_NONREUS;
        parse_attrs(a, st, &ops, &ats);
        for (i = 0; i < ats.n; i++) apply_cattr(a, st, cl, &ats.v[i]);
        if (st->bad) return;
        vec_push(&a->sect->classes, cl);
    }
    a->cur = cl->pieces.v[0];
    a->sect->cur = a->cur;
}

static const char *const xattr_keys[] = {"LINKAGE", "SCOPE", "REFERENCE", "PSECT", "ATTRIBUTES"};

void dir_xattr(Asm *a, Stmt *st) {
    OperandVec ops = {0};
    AttrVec ats = {0};
    XattrStmt *x;
    size_t i;
    if (a->pass != 1) return;
    if (!st->name[0]) {
        diag_help(rep_errorf(a->rep, "ZA3021", st->op_span,
                             "XATTR needs the symbol it describes in the name field"),
                  "for example `PARTNAME XATTR LINKAGE(OS),SCOPE(SECTION)`");
        st->bad = 1;
        return;
    }
    asm_ops(a, st, 1, -1, &ops);
    x = xcalloc(sizeof *x);
    x->st = st;
    x->name = xstrdup(st->name);
    x->attr.linkage = -1;
    x->attr.scope = -1;
    x->attr.indirect = -1;
    x->attr.def = stmt_span(st);
    parse_attrs(a, st, &ops, &ats);
    for (i = 0; i < ats.n; i++) {
        AttrArg *at = &ats.v[i];
        if (!strcmp(at->key, "LINKAGE")) {
            char *v;
            if (!want_args(a, st, at, 1, "LINKAGE(OS|XPLINK)")) continue;
            v = za_strupper(at->args.v[0]);
            if (!strcmp(v, "OS"))
                x->attr.linkage = GOFF_LINKAGE_OS;
            else if (!strcmp(v, "XPLINK"))
                x->attr.linkage = GOFF_LINKAGE_XPLINK;
            else {
                diag_help(rep_errorf(a->rep, "ZA3035", at->asp.v[0], "invalid LINKAGE \"%s\"",
                                     at->args.v[0]),
                          "use OS or XPLINK");
                st->bad = 1;
            }
        } else if (!strcmp(at->key, "SCOPE")) {
            char *v;
            int sc = -1;
            if (!want_args(a, st, at, 1, "SCOPE(SECTION|MODULE|LIBRARY|EXPORT|IMPORT)")) continue;
            v = za_strupper(at->args.v[0]);
            if (!strcmp(v, "SECTION"))
                sc = GOFF_SCOPE_SECTION;
            else if (!strcmp(v, "MODULE"))
                sc = GOFF_SCOPE_MODULE;
            else if (!strcmp(v, "LIBRARY"))
                sc = GOFF_SCOPE_LIBRARY;
            else if (!strcmp(v, "EXPORT") || !strcmp(v, "IMPORT"))
                sc = GOFF_SCOPE_IMPORT_EXPORT;
            if (sc < 0) {
                diag_help(rep_errorf(a->rep, "ZA3036", at->asp.v[0], "invalid SCOPE \"%s\"",
                                     at->args.v[0]),
                          "use SECTION, MODULE, LIBRARY, EXPORT or IMPORT");
                st->bad = 1;
                continue;
            }
            x->attr.scope = sc;
        } else if (!strcmp(at->key, "REFERENCE")) {
            size_t k;
            if (at->args.n < 1 || at->args.n > 2) {
                want_args(a, st, at, 1, "REFERENCE(CODE|DATA[,DIRECT|INDIRECT])");
                continue;
            }
            for (k = 0; k < at->args.n; k++) {
                char *r = za_strupper(at->args.v[k]);
                if (!strcmp(r, "CODE") || !strcmp(r, "DATA")) {
                    if (x->attr.reference) {
                        rep_errorf(a->rep, "ZA3037", at->asp.v[k],
                                   "REFERENCE names CODE or DATA twice");
                        st->bad = 1;
                    } else {
                        x->attr.reference = r;
                    }
                } else if (!strcmp(r, "DIRECT") || !strcmp(r, "INDIRECT")) {
                    if (x->attr.indirect >= 0) {
                        rep_errorf(a->rep, "ZA3037", at->asp.v[k],
                                   "REFERENCE names DIRECT or INDIRECT twice");
                        st->bad = 1;
                    } else {
                        x->attr.indirect = r[0] == 'I';
                    }
                } else {
                    diag_help(rep_errorf(a->rep, "ZA3037", at->asp.v[k],
                                         "invalid REFERENCE \"%s\"", at->args.v[k]),
                              "use CODE or DATA, and DIRECT or INDIRECT");
                    st->bad = 1;
                }
            }
        } else if (!strcmp(at->key, "PSECT")) {
            if (want_args(a, st, at, 1, "PSECT(name)")) x->attr.psect = xstrdup(at->args.v[0]);
        } else if (!strcmp(at->key, "ATTRIBUTES")) {
            if (want_args(a, st, at, 1, "ATTRIBUTES(label)"))
                x->attr.attributes = xstrdup(at->args.v[0]);
        } else {
            bad_attr(a, st, at, xattr_keys, sizeof xattr_keys / sizeof xattr_keys[0]);
        }
    }
    for (i = 0; i < ats.n; i++) {
        AttrArg *at = &ats.v[i];
        free(at->key);
        size_t kk;
        for (kk = 0; kk < at->args.n; kk++) free(at->args.v[kk]);
        free(at->args.v);
        free(at->asp.v);
    }
    free(ats.v);
    if (!st->bad) vec_push(&a->xattrs, x);
}

/* ---- AMODE / RMODE ----------------------------------------------------- */

void dir_mode(Asm *a, Stmt *st) {
    OperandVec ops = {0};
    int is_a = za_streqi(st->op, "AMODE");
    char *up;
    Section *sec;
    const char *val;
    uint8_t v;
    if (a->pass != 1) return;
    if (!asm_ops(a, st, 1, 1, &ops)) return;
    up = za_strupper(st->op);
    sec = a->sect;
    if (st->name[0]) {
        sec = strmap_get(&a->secs, st->name);
        if (!sec) { /* an ENTRY symbol, checked when pass 1 ends */
            SymMode sm;
            val = ops.v[0].text;
            if (is_a ? !amode_byte(val, &v) : !rmode_byte(val, &v)) {
                diag_help(rep_errorf(a->rep, is_a ? "ZA3041" : "ZA3042", asm_op_span(st, ops.v[0]),
                                     "invalid %s \"%s\"", up, val),
                          is_a ? "use 24, 31, 64, ANY or MIN" : "use 24, 31, 64 or ANY");
                st->bad = 1;
                free(up);
        return;
            }
            sm.name = xstrdup(st->name);
            sm.sp = st->name_span;
            sm.is_amode = is_a;
            sm.val = v;
            vec_push(&a->symmodes, sm);
            free(up);
        return;
        }
    }
    if (!sec) {
        diag_help(rep_errorf(a->rep, "ZA3001", st->op_span, "%s has no section to apply to", up),
                  "name the section in the name field or place the statement after a CSECT");
        st->bad = 1;
        free(up);
        return;
    }
    val = ops.v[0].text;
    if (is_a) {
        if (!amode_byte(val, &v)) {
            diag_help(rep_errorf(a->rep, "ZA3041", asm_op_span(st, ops.v[0]),
                                 "invalid AMODE \"%s\"", val),
                      "use 24, 31, 64, ANY or MIN");
            st->bad = 1;
            free(up);
        return;
        }
        sec->amode = v;
        free(up);
        return;
    }
    if (!rmode_byte(val, &v)) {
        diag_help(rep_errorf(a->rep, "ZA3042", asm_op_span(st, ops.v[0]), "invalid RMODE \"%s\"",
                             val),
                  "use 24, 31, 64 or ANY");
        st->bad = 1;
        free(up);
        return;
    }
    sec->rmode = v;
    free(up);
}

/* ---- ENTRY / EXTRN / WXTRN --------------------------------------------- */

void dir_entry(Asm *a, Stmt *st) {
    OperandVec ops = {0};
    size_t i;
    if (a->pass != 1) return;
    if (!asm_ops(a, st, 1, -1, &ops)) return;
    for (i = 0; i < ops.n; i++) {
        Operand o = ops.v[i];
        EntryRef r;
        const char *why = NULL;
        r.name = sym_text(o.text, &why);
        if (!r.name) {
            diag_help(rep_errorf(a->rep, "ZA1021", asm_op_span(st, o),
                                 "\"%s\" is not a valid symbol name", o.text),
                      "%s", why);
            st->bad = 1;
            continue;
        }
        /* HLASM rejects ENTRY of a symbol in a dummy section (DSECT). */
        Symbol *es = asm_lookup(a, r.name);
        if (es && es->sect && es->sect->kind == K_DSECT) {
            diag_help(rep_errorf(a->rep, "ZA3010", asm_op_span(st, o),
                                 "ENTRY of \"%s\" is not allowed: the symbol is in a DSECT",
                                 r.name),
                      "ENTRY applies to symbols in CSECT/RSECT/COM sections, not dummy sections");
            st->bad = 1;
            free(r.name);
            continue;
        }
        r.sp = asm_op_span(st, o);
        vec_push(&a->entries, r);
    }
}

void dir_extrn(Asm *a, Stmt *st) {
    OperandVec ops = {0};
    int weak = za_streqi(st->op, "WXTRN");
    size_t i;
    if (a->pass != 1) return;
    if (!asm_ops(a, st, 1, -1, &ops)) return;
    for (i = 0; i < ops.n; i++) {
        Operand o = ops.v[i];
        Symbol *old;
        int created;
        const char *why = NULL;
        char *nm = sym_text(o.text, &why);
        if (!nm) {
            diag_help(rep_errorf(a->rep, "ZA1021", asm_op_span(st, o),
                                 "\"%s\" is not a valid symbol name", o.text),
                      "%s", why);
            st->bad = 1;
            continue;
        }
        old = asm_lookup(a, nm);
        if (old && old->kind == SYM_EXT) {
            if (old->weak && !weak) old->weak = 0; /* a strong declaration wins */
            continue;
        }
        old = asm_define(a, st, nm, asm_op_span(st, o), &created);
        if (created) {
            old->kind = SYM_EXT;
            old->weak = weak;
            old->ext_sect = a->sect;
        }
    }
}

/* ---- EQU / ORG / CNOP / END -------------------------------------------- */

void dir_equ(Asm *a, Stmt *st) {
    OperandVec ops = {0};
    Expr *e = NULL;
    Value v;
    int64_t length = 1, type = 0;
    int have_len = 0, have_type = 0;
    Symbol *s;
    int created;
    char regkind = 0;
    if (a->pass != 1) return;
    if (!st->name[0]) {
        rep_errorf(a->rep, "ZA1022", st->op_span, "EQU needs a symbol in the name field");
        st->bad = 1;
        return;
    }
    if (!asm_ops(a, st, 1, 5, &ops)) return;
    {
        char *t0 = str_trim(ops.v[0].text);
        if (t0[0] == '%') { /* NAME EQU %r12: a register name */
            int num;
            if (!asm_parse_pct(t0, &regkind, &num) || num > asm_reg_max(regkind)) {
                diag_help(rep_errorf(a->rep, "ZA2054", asm_op_span(st, ops.v[0]),
                                     "\"%s\" is not a register name", t0),
                          "GNU style registers are %%r0-%%r15, %%f0-%%f15, %%a0-%%a15, "
                          "%%c0-%%c15 and %%v0-%%v31");
                free(t0);
                st->bad = 1;
                return;
            }
            v = value_abs(num);
        } else {
            e = asm_expr(a, st, ops.v[0]);
            if (!e) {
                free(t0);
                return;
            }
            {
                /* A missing symbol makes the EQU deferred; a real error (a
                 * CSECT size that would have to size storage, for example)
                 * must be reported now.  Try with need=1 in a scratch
                 * reporter and look at the codes. */
                Reporter *tr = rep_new(0), *save = a->rep;
                int ok0, bad0 = st->bad, real = 0;
                size_t di;
                a->rep = tr;
                a->trial = 1;
                ok0 = asm_eval(a, st, e, 1, &v);
                a->trial = 0;
                a->rep = save;
                st->bad = bad0;
                for (di = 0; di < rep_count(tr); di++) {
                    const Diag *d = rep_get(tr, di);
                    if (strcmp(d->code, "ZA1010") != 0) real = 1;
                }
                rep_free(tr);
                if (!ok0 || !v.defined) {
                    if (real) {
                        asm_eval(a, st, e, 1, &v);
                        st->bad = 1;
                        free(t0);
                        return;
                    }
                    v = value_abs(0);
                    v.defined = 0; /* defer */
                }
            }
        }
        free(t0);
    }
    if (ops.n > 1 && ops.v[1].text[0]) {
        if (!asm_abs_expr(a, st, ops.v[1], "the EQU length attribute", &length)) return;
        if (length < 0 || length > 65535) {
            rep_errorf(a->rep, "ZA1034", asm_op_span(st, ops.v[1]),
                       "the EQU length attribute must be 0 to 65535, not %lld",
                       (long long)length);
            st->bad = 1;
            return;
        }
        have_len = 1;
    }
    if (!asm_valid_stmt_name(st)) {
        rep_errorf(a->rep, "ZA1021", st->name_span, "\"%s\" is not a valid symbol name", st->name);
        st->bad = 1;
        return;
    }
    if (ops.n > 2 && ops.v[2].text[0]) {
        if (!asm_abs_expr(a, st, ops.v[2], "the EQU type attribute", &type)) return;
        if (type < 0 || type > 255) {
            rep_errorf(a->rep, "ZA1033", asm_op_span(st, ops.v[2]),
                       "the EQU type attribute must be a character code from 0 to 255");
            st->bad = 1;
            return;
        }
        have_type = 1;
    }
    if (ops.n > 3 && ops.v[3].text[0]) {
        int64_t prog;
        if (!asm_abs_expr(a, st, ops.v[3], "the EQU program type", &prog)) return;
        if (prog < 0 || prog > 0xFFFFFFFFLL) {
            rep_errorf(a->rep, "ZA1033", asm_op_span(st, ops.v[3]),
                       "the EQU program type must fit 32 bits");
            st->bad = 1;
            return;
        }
    }
    if (ops.n > 4 && ops.v[4].text[0]) {
        char *t = str_trim(ops.v[4].text), *u = za_strupper(t);
        if (strcmp(u, "AR") && strcmp(u, "CR") && strcmp(u, "CR32") && strcmp(u, "CR64") &&
            strcmp(u, "FPR") && strcmp(u, "GR") && strcmp(u, "GR32") && strcmp(u, "GR64") &&
            strcmp(u, "VR")) {
            rep_errorf(a->rep, "ZA1033", asm_op_span(st, ops.v[4]),
                       "the EQU assembler type must be AR, CR, CR32, CR64, FPR, GR, GR32, GR64 or VR");
            free(t);
            free(u);
            st->bad = 1;
            return;
        }
        free(t);
        free(u);
    }
    if (!have_len) length = regkind ? 1 : asm_equ_left_len(a, e);
    s = asm_define(a, st, st->name, st->name_span, &created);
    if (created) {
        s->len = length;
        s->has_len = 1;
        s->fixed_len = (unsigned)have_len;
        s->has_type = (unsigned)have_type;
        s->type = have_type ? (za_ascii ? (char)type : (char)ZA_A(type)) : 0;
        s->regkind = regkind;
        if (v.defined) {
            s->val = v;
            s->kind = value_is_abs(&v) ? SYM_ABS : SYM_REL;
        } else {
            s->kind = SYM_DEFERRED;
            s->deferred = e;
            s->def_stmt = st;
            s->val = value_abs(0);
        }
    }
}

/* ASSERT condition[,'message']: the assembly fails when the condition is 0.
 * It is checked in pass 2, so sizes and forward references can be used. */
void dir_assert(Asm *a, Stmt *st) {
    OperandVec ops = {0};
    Expr *e;
    Value v;
    char *msg = NULL;
    if (!asm_ops(a, st, 1, 2, &ops)) return;
    if (a->pass == 1 && st->name[0]) {
        diag_help(rep_errorf(a->rep, "ZA1053", st->name_span, "ASSERT cannot have a name"),
                  "remove the name; the statement defines nothing");
        st->bad = 1;
        return;
    }
    e = asm_expr(a, st, ops.v[0]);
    if (!e) return;
    if (ops.n == 2) {
        char *t = str_trim(ops.v[1].text);
        size_t n = strlen(t), i;
        Buf b = {0};
        int ok = n >= 2 && t[0] == '\'' && t[n - 1] == '\'';
        for (i = 1; ok && i + 1 < n; i++) {
            if (t[i] == '\'') {
                if (i + 2 < n && t[i + 1] == '\'')
                    i++;
                else
                    ok = 0;
            }
            if (ok) buf_putc(&b, t[i]);
        }
        if (!ok) {
            if (a->pass == 1) {
                diag_help(rep_errorf(a->rep, "ZA1051", asm_op_span(st, ops.v[1]),
                                     "the ASSERT message must be a quoted text"),
                          "for example: ASSERT sizeof(S)<=256,'S does not fit in 256 bytes'");
                st->bad = 1;
            }
            return;
        }
        msg = xstrdup(buf_cstr(&b));
    }
    if (a->pass == 1) return;
    if (!asm_eval(a, st, e, 1, &v)) return;
    if (!value_is_abs(&v)) {
        diag_label(rep_errorf(a->rep, "ZA1013", e->sp,
                              "the ASSERT condition must be an absolute value"),
                   "this is a relocatable address");
        st->bad = 1;
        return;
    }
    if (v.abs == 0) {
        Span sp = asm_op_span(st, ops.v[0]);
        if (msg)
            diag_help(rep_errorf(a->rep, "ZA1052", sp, "assertion failed: %s", msg),
                      "the condition was: %s", ops.v[0].text);
        else
            rep_errorf(a->rep, "ZA1052", sp, "assertion failed: %s", ops.v[0].text);
        st->bad = 1;
    }
}

void dir_org(Asm *a, Stmt *st) {
    OperandVec ops = {0};
    Expr *e = NULL;
    Value v;
    int64_t off, boundary = 0, add = 0;
    Base b;
    int is_rel = 0, ok_kind = 0;
    if (!asm_need_section(a, st)) return;
    if (!asm_ops(a, st, 0, 3, &ops)) return;
    asm_define_label(a, st, 1, 0);
    if (ops.n == 0 || !ops.v[0].text[0]) {
        /* no expression: the next available location for this counter */
        off = a->cur->max;
        ok_kind = 1;
    } else {
        e = asm_expr(a, st, ops.v[0]);
        if (!e) return;
        if (!asm_eval(a, st, e, 1, &v)) return;
        off = v.abs;
        is_rel = value_simple(&v, &b);
        if (!is_rel && value_is_abs(&v)) {
            ok_kind = 1; /* absolute: offset from the start of the current piece */
        } else if (is_rel &&
                   (base_eq(b, base_piece(a->cur)) || base_eq(b, base_class(a->cur->cls)))) {
            if (b.kind == B_CLASS)
                off -= a->cur->start + a->cur->cls->base;
            else
                off -= a->cur->cls->sect->start_addr;
            ok_kind = 1;
        }
    }
    if (!ok_kind) {
        if (a->pass == 1 || a->laid) {
            rep_errorf(a->rep, "ZA3050", e->sp,
                       "ORG operand must be an address in the current location counter or an "
                       "absolute offset");
            st->bad = 1;
        }
        return;
    }
    if (ops.n > 1 && ops.v[1].text[0]) {
        if (!asm_abs_expr(a, st, ops.v[1], "the ORG boundary", &boundary)) return;
        if (boundary < 2 || boundary > 4096 || (boundary & (boundary - 1)) != 0) {
            rep_errorf(a->rep, "ZA3055", asm_op_span(st, ops.v[1]),
                       "the ORG boundary must be a power of 2 from 2 to 4096, not %lld",
                       (long long)boundary);
            st->bad = 1;
            return;
        }
    }
    if (ops.n > 2 && ops.v[2].text[0]) {
        if (!asm_abs_expr(a, st, ops.v[2], "the ORG offset", &add)) return;
    }
    if (boundary) {
        int64_t r = off % boundary;
        if (r) off += boundary - r;
    }
    if ((add > 0 && off > ZA_MAX_LOC - add) || (add < 0 && off < -add)) {
        rep_errorf(a->rep, "ZA3053", st->op_span,
                   "ORG would move the location counter outside the section");
        st->bad = 1;
        return;
    }
    off += add;
    if (off < 0) {
        rep_errorf(a->rep, "ZA3051", st->op_span,
                   "ORG would move the location counter before the start of the section");
        st->bad = 1;
        return;
    }
    if (off > ZA_MAX_LOC) {
        rep_errorf(a->rep, "ZA3053", st->op_span,
                   "ORG would move the location counter to %lld; the limit for a section is "
                   "%lld bytes",
                   (long long)off, (long long)ZA_MAX_LOC);
        st->bad = 1;
        return;
    }
    a->cur->loc = off;
    if (off > a->cur->max) a->cur->max = off;
}

void dir_cnop(Asm *a, Stmt *st) {
    OperandVec ops = {0};
    int64_t b = 0, n = 0;
    int ok1, ok2;
    /* HLASM pads with BCR 0,0 for a gap of 2 bytes and then as many
     * 47000700 as fit (oracle: gap 6 = 0700 47000700) */
    static const uint8_t nop[2] = {0x07, 0x00}, nop4[4] = {0x47, 0x00, 0x07, 0x00};
    if (!asm_need_section(a, st)) return;
    if (!asm_ops(a, st, 2, 2, &ops)) return;
    ok1 = asm_abs_expr(a, st, ops.v[0], "the CNOP byte offset", &b);
    ok2 = asm_abs_expr(a, st, ops.v[1], "the CNOP boundary", &n);
    if (!ok1 || !ok2) return;
    if (n < 4 || n > 4096 || (n & (n - 1)) != 0 || b < 0 || b >= n || b % 2 != 0) {
        if (a->pass == 1) {
            diag_help(rep_errorf(a->rep, "ZA3052", asm_op_span(st, ops.v[1]),
                                 "CNOP needs an even byte offset below a power-of-2 boundary "
                                 "from 4 to 4096"),
                      "for example `CNOP 0,8` or `CNOP 2,4`");
            st->bad = 1;
        }
        return;
    }
    asm_align(a, 2);
    asm_define_label(a, st, 2, 0);
    if ((a->cur->hint + a->cur->loc) % n != b &&
        ((b - (a->cur->hint + a->cur->loc) % n + n) % n) % 4 == 2)
        asm_put(a, nop, 2);
    while ((a->cur->hint + a->cur->loc) % n != b) asm_put(a, nop4, 4);
}

void dir_end(Asm *a, Stmt *st) {
    if (a->pass == 1) {
        OperandVec ops = {0};
        if (asm_ops(a, st, 0, 1, &ops) && ops.n == 1 && ops.v[0].text[0]) {
            const char *why = NULL;
            a->end_entry = sym_text(ops.v[0].text, &why);
            a->end_span = asm_op_span(st, ops.v[0]);
            if (!a->end_entry) {
                diag_help(rep_errorf(a->rep, "ZA1021", a->end_span,
                                     "\"%s\" is not a valid symbol name", ops.v[0].text),
                          "%s", why);
                st->bad = 1;
            }
        }
    }
    a->ended = 1;
}
