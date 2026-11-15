/* asm.h - the assembler proper: statements, expressions, the object model
 * (sections, classes, location counters, symbols) and the assembler state.
 *
 * Memory: the assembler is a short lived process, so objects are allocated
 * and never freed. */
#ifndef ZA_ASM_H
#define ZA_ASM_H

#include <stdint.h>

#include "diag.h"
#include "isa.h"
#include "util.h"

/* ---- object model ---------------------------------------------------- */

typedef struct Piece Piece;
typedef struct Class Class;
typedef struct Section Section;
typedef struct Symbol Symbol;
typedef struct Stmt Stmt;
typedef struct Expr Expr;
typedef struct Asm Asm;

typedef enum { K_CSECT, K_RSECT, K_DSECT, K_COM } SectKind;
const char *sectkind_name(SectKind k);

typedef struct {
    int align; /* log2, -1 = unset */
    uint8_t rmode;
    int executable; /* GOFF_EXEC_* */
    int readonly, noload, defload;
    int fill;     /* -1 = none */
    int priority; /* -1 = none */
    int merge;
    int tasking;
} ClassAttr;

typedef enum { REL_ADDR, REL_V, REL_Q, REL_CXD, REL_RI, REL_LEN } RelocKind;

typedef struct {
    int64_t off; /* offset in the class */
    int len;
    Symbol *target; /* external symbol (ER), or NULL */
    Class *cls;     /* class origin when target is NULL */
    RelocKind kind;
    int amode;
} Reloc;

/* One location counter (LOCTR) of a class; the first piece of a class
 * carries the section/class name. */
struct Piece {
    char *name;
    Class *cls;
    int64_t loc;   /* current location counter, relative to start */
    int64_t max;   /* highest location reached (ORG can move back) */
    int64_t start; /* offset in the class after layout */
};

int64_t piece_len(const Piece *p);

typedef Vec(Piece *) PieceVec;
typedef Vec(Reloc) RelocVec;
typedef Vec(Class *) ClassVec;

/* A GOFF class (ED) of a section; every section has B_TEXT. */
struct Class {
    char *name;
    Section *sect;
    PieceVec pieces;
    ClassAttr attr;
    char *part; /* PR name when PART(...) was given, else NULL */
    int64_t len;
    uint8_t *data;
    uint32_t esd; /* ED esdid */
    uint32_t pr;  /* PR esdid when part != NULL */
    RelocVec relocs;
};

struct Section {
    char *name;
    SectKind kind;
    Span def;
    ClassVec classes;
    uint8_t amode, rmode; /* GOFF_AMODE_* / GOFF_RMODE_*, 0 = unset */
    Piece *cur;           /* location counter to resume with */
    uint32_t esd;
};

typedef enum { B_NONE = 0, B_PIECE, B_CLASS, B_SYM } BaseKind;

/* What a relocatable value is relative to: a piece (before layout, or in
 * a DSECT), a class (after layout) or an external symbol. */
typedef struct {
    BaseKind kind;
    void *p;
} Base;

Base base_piece(Piece *p);
Base base_class(Class *c);
Base base_sym(Symbol *s);
int base_eq(Base a, Base b);

typedef struct {
    Base base;
    int coef;
} Term;

/* abs + sum(coef * base) */
typedef struct {
    int64_t abs;
    Term *terms;
    int nterms;
    int defined; /* 0 when a forward reference was seen (pass 1) */
} Value;

Value value_abs(int64_t n);
int value_is_abs(const Value *v);
/* "base + constant" with a single positive term */
int value_simple(const Value *v, Base *b);
Value value_add(Value a, Value b, int sign);
Value value_neg(Value v);

typedef enum { SYM_LABEL, SYM_ABS, SYM_REL, SYM_EXT, SYM_SECT } SymKind;

typedef struct {
    int linkage; /* GOFF_LINKAGE_*, -1 = unset */
    int scope;   /* GOFF_SCOPE_*, -1 = unset */
    char *reference; /* "CODE", "DATA" or NULL */
    char *psect;
    Span def;
} XAttr;

struct Symbol {
    char *name;
    SymKind kind;
    Span def;
    Piece *piece; /* SYM_LABEL, SYM_SECT */
    int64_t off;  /* offset in piece (SYM_LABEL) */
    Value val;    /* SYM_ABS, SYM_REL */
    int64_t len;  /* length attribute */
    char type;    /* type attribute (C, X, F, ...), 0 = U */
    int scale;    /* scale attribute */
    size_t def_idx; /* index of the defining statement */
    int weak;     /* WXTRN */
    int entry;    /* ENTRY */
    Section *sect;
    XAttr *xattr;
    int used;
    uint32_t esd;    /* ER esdid */
    uint32_t ld_esd; /* LD esdid */
};

typedef Vec(Section *) SectionVec;
typedef Vec(Symbol *) SymbolVec;

typedef struct {
    char *name;
    SectionVec sections;
    StrMap symbols;
    SymbolVec sym_order;
    char *entry; /* END operand, or NULL */
    Span entry_def;
} Module;

Symbol *module_symbol(const Module *m, const char *name);

/* Writes the module as a GOFF object (malloc'ed). On failure returns NULL
 * and describes the problem in err. */
uint8_t *module_emit(Module *m, size_t *len, char *err, size_t errsz);

/* ---- source lines and statements ------------------------------------ */

/* One logical input line as supplied by the preprocessor (or the plain
 * file reader): text without the line end, in UTF-8. */
typedef struct {
    const char *file;
    int line; /* 1-based */
    const char *text;
} SrcLine;

typedef Vec(SrcLine) SrcVec;

/* Convert text to UTF-8 (input that is not valid UTF-8 is taken as
 * ISO-8859-1) and split it into lines, appending to out. */
void src_split(SrcVec *out, const char *file, const char *text, size_t len);

typedef struct {
    int off; /* offset in Operands where this piece starts */
    int line;
    int col; /* 1-based byte column of the piece's first byte */
} Seg;

/* One logical statement (continuation lines joined). */
struct Stmt {
    const char *file;
    int line; /* first physical line */
    char *name;
    int name_quoted; /* written as "...": name is the decoded text */
    Span name_span;
    char *op; /* as written */
    Span op_span;
    char *operands; /* joined operand text */
    Seg *segs;
    int nsegs;
    char *remark;
    int bad; /* an error was reported; skip in pass 2 */
};

typedef Vec(Stmt *) StmtVec;

/* One top-level comma separated operand. */
typedef struct {
    char *text;
    int off; /* offset in Stmt.operands */
} Operand;

typedef Vec(Operand) OperandVec;

Span stmt_operand_span(const Stmt *s, int off, int n);
Span stmt_span(const Stmt *s); /* the operation field, or column 1 */

/* Split the lines into statements; errors go to rep. */
void read_statements(StmtVec *out, const SrcLine *lines, size_t n, Reporter *rep);
void split_operands(OperandVec *out, const char *s);

int is_blank(int c);
int is_sym_start(int c);
int is_sym_char(int c);
int is_symbol_like(const char *s);
/* is the quote at s[i] part of an attribute reference such as L'SYM? */
int is_attr_quote(const char *s, size_t i);
/* index of the ')' closing the '(' at s[i] (quotes skipped), or -1 */
long match_paren(const char *s, size_t i);
/* end of the operand field starting at start: the first blank outside
 * quotes and parentheses; returns 0 if a quote or parenthesis is left open */
int scan_operand_end(const char *s, size_t start, size_t *end);
/* index of the quote closing the one at s[i], or -1 */
long close_quote(const char *s, size_t i);
/* quoted symbols: "any characters", with \" \\ and \xHH escapes (a source
 * character U+0001 - U+00FF). close_dquote: index of the '"' closing the one
 * at s[i], or -1. */
long close_dquote(const char *s, size_t i);
/* the text of a quoted symbol: s[0..n) are the characters between the quotes;
 * NULL and *err when they are not valid */
char *sym_decode(const char *s, size_t n, const char **err);
/* a symbol written as an operand: plain or quoted, no blanks around; the
 * name, or NULL and *err */
char *sym_text(const char *tok, const char **err);
/* the source form of an EBCDIC name: the plain symbol, else "quoted"; NULL
 * if it holds a NUL (not expressible) */
char *sym_source(const unsigned char *ebcdic, size_t n);
/* a name field is a valid symbol name (decoded when quoted) */
int asm_valid_stmt_name(const Stmt *st);

/* ---- expressions ----------------------------------------------------- */

typedef enum { EX_NUM, EX_SYM, EX_LOC, EX_ATTR, EX_BIN, EX_UN } ExprKind;

struct Expr {
    ExprKind kind;
    Span sp;
    int64_t num;
    char *name; /* EX_SYM, EX_ATTR */
    char attr;  /* 'L', 'T', ... */
    int loc;    /* L'* */
    char op;    /* EX_BIN, EX_UN */
    Expr *l, *r;
};

/* Parse text (an operand or part of one); off is its offset in
 * st->operands for diagnostics. Returns NULL after reporting an error. */
Expr *parse_expr(Stmt *st, const char *text, int off, Reporter *rep);
/* short description of e for messages */
char *expr_describe(const Expr *e);

/* ---- the assembler --------------------------------------------------- */

typedef struct {
    Base key; /* class (also for dummy sections); B_NONE = absolute */
    int64_t off;
    int reg;
    Span sp;
} Using;

typedef Vec(Using) UsingVec;

typedef struct {
    char *name;
    Span sp;
} EntryRef;

typedef Vec(EntryRef) EntryVec;

typedef struct {
    Stmt *st;
    char *name;
    XAttr attr;
} XattrStmt;

typedef Vec(XattrStmt *) XattrVec;

typedef struct {
    char *key;  /* operand text including '=' */
    char *name; /* generated label */
    Span sp;
    Stmt *st; /* synthetic DC statement */
} Literal;

typedef struct {
    Vec(Literal *) lits;
    StrMap by_key;
    int sorted;
} LitPool;

typedef Vec(LitPool *) PoolVec;

struct Asm {
    Reporter *rep;
    Reporter *scratch; /* silent reporter for pass 2 re-parsing */
    Module *mod;
    StmtVec stmts;
    int pass;
    int laid; /* layout done (pass 2) */
    Piece *cur;
    Section *sect;
    int ended;
    UsingVec usings;
    char *end_entry;
    Span end_span;
    XattrVec xattrs;
    EntryVec entries;
    StrMap secs;
    PoolVec pools;
    int pool_no;
    size_t cur_idx;   /* index of the statement being assembled */
    int64_t star_len; /* L'* of the current statement, 0 = unknown */
};

/* Assemble the lines; returns NULL when errors were reported. The caller
 * registers file texts with rep_add_source for diagnostic excerpts. */
Module *asm_assemble(const SrcLine *lines, size_t n, const char *name, Reporter *rep);
/* Convenience: split text, register it and assemble. */
Module *asm_assemble_text(const char *file, const char *text, size_t len, Reporter *rep);

typedef void (*Handler)(Asm *a, Stmt *st);

/* assembler.c */
Symbol *asm_lookup(Asm *a, const char *name);
Symbol *asm_define(Asm *a, Stmt *st, const char *name, Span sp, int *created);
void asm_define_label(Asm *a, Stmt *st, int64_t length, char typ);
int asm_valid_name(const char *n);
int asm_need_section(Asm *a, Stmt *st);
int asm_ops(Asm *a, Stmt *st, int min, int max, OperandVec *out);
Expr *asm_expr(Asm *a, Stmt *st, Operand o);
Span asm_op_span(Stmt *st, Operand o);
int asm_abs_expr(Asm *a, Stmt *st, Operand o, const char *what, int64_t *out);
int asm_eval(Asm *a, Stmt *st, const Expr *e, int need, Value *out);
void asm_expr_err(Asm *a, Stmt *st, Span sp, const char *code, const char *fmt, ...);
void asm_stmt(Asm *a, Stmt *st);
void asm_align(Asm *a, int64_t n);
void asm_advance(Asm *a, int64_t n);
void asm_put(Asm *a, const uint8_t *b, size_t n);
void asm_reloc(Asm *a, int n, Symbol *target, Class *cls, RelocKind kind);
Value asm_addr_value(Asm *a, Piece *p, int64_t off);
int amode_byte(const char *s, uint8_t *out);
int rmode_byte(const char *s, uint8_t *out);
char *str_trim(const char *s);
int parse_i64(const char *s, int64_t *out);

/* directives.c */
void dir_section(Asm *a, Stmt *st);
void dir_loctr(Asm *a, Stmt *st);
void dir_cattr(Asm *a, Stmt *st);
void dir_xattr(Asm *a, Stmt *st);
void dir_mode(Asm *a, Stmt *st);
void dir_entry(Asm *a, Stmt *st);
void dir_extrn(Asm *a, Stmt *st);
void dir_equ(Asm *a, Stmt *st);
void dir_org(Asm *a, Stmt *st);
void dir_cnop(Asm *a, Stmt *st);
void dir_end(Asm *a, Stmt *st);
void dir_copy(Asm *a, Stmt *st);
void dir_ignore(Asm *a, Stmt *st);
size_t asm_directive_names(const char *const **names);

/* dc.c */
typedef struct {
    int64_t dup;
    char typ; /* upper case type letter */
    char ext; /* 'A'/'E' (character), 'D' (F, A), 'B'/'H' (floating point) */
    int64_t length;
    int has_len, has_nom, quoted;
    char *nom;  /* nominal text without the delimiters */
    int nom_off;
    int off, n; /* the operand inside Stmt.operands */
} DcSpec;

typedef struct {
    uint8_t *b;
    size_t n;
    int has_rel;
    Symbol *target;
    Class *cls;
    RelocKind kind;
} DcVal;

typedef Vec(DcVal) DcVec;

int64_t dc_alignment(const DcSpec *sp);
int64_t dc_deflen(const DcSpec *sp);
int dc_parse(Asm *a, Stmt *st, Operand o, DcSpec *sp);
Diag *dc_err(Asm *a, Stmt *st, const char *code, int off, int n, const char *fmt, ...);
void split_nominal(const DcSpec *sp, OperandVec *out);
void be_bytes(uint64_t v, int n, uint8_t *out);
void dir_dc(Asm *a, Stmt *st);

/* float.c: E, D and L constants; fills out (n bytes) or describes the
 * problem in err. ieee selects binary instead of hexadecimal floating point. */
int float_bytes(const char *text, int ieee, int n, uint8_t *out, const char **err);
int dc_float(Asm *a, Stmt *st, const DcSpec *sp, DcVec *out);

/* instr.c */
void asm_instr(Asm *a, Stmt *st);
void dir_using(Asm *a, Stmt *st);
void dir_drop(Asm *a, Stmt *st);

/* literal.c */
void dir_ltorg(Asm *a, Stmt *st);
void asm_end_pool(Asm *a);
void asm_literal_operands(Asm *a, Stmt *st, OperandVec *ops);
void asm_close_pool(Asm *a);

#endif
