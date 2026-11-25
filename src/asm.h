/* asm.h - the assembler proper: statements, expressions, the object model
 * (sections, classes, location counters, symbols) and the assembler state.
 *
 * Memory: objects are owned by the Module that holds them. When zosasm is
 * used as a one-shot process the OS reclaims memory at exit; when used as
 * a library, call asm_free() to release a Module and everything it owns.
 * Value terms live in an Arena owned by the Module and are released by
 * asm_free(). A few buffers of the literal-pool alignment path are still
 * process-lifetime; a long-lived caller should use one process per assembly
 * or the CLI/MCP subprocess model (DESIGN.md section 7). */
#ifndef ZA_ASM_H
#define ZA_ASM_H

#include <stdint.h>

#include "diag.h"
#include "isa.h"
#include "util.h"
#include "goff.h"
#include "cblock_dbg.h"

/* ---- object model ---------------------------------------------------- */

typedef struct Piece Piece;
typedef struct Class Class;
typedef struct Section Section;
typedef struct Symbol Symbol;
typedef struct Stmt Stmt;
typedef struct Expr Expr;
typedef struct Asm Asm;
typedef Vec(Expr *) ExprVec;

typedef enum { K_CSECT, K_RSECT, K_DSECT, K_COM } SectKind;
const char *sectkind_name(SectKind k);

#define CLASS_RMODE_NONE 0xFF /* CATTR RMODE(NONE): the class ED carries no RMODE */

/* GOFF stores class lengths and text offsets in 32 bits (DESIGN.md 3), so the
 * location counter is capped below that: a DS/DC/ORG beyond the limit is a
 * diagnostic (ZA2087/ZA3053/ZA3054), not an out-of-memory exit. */
#define ZA_MAX_LOC INT64_C(0x7fffffff)
typedef struct {
    int align; /* log2, -1 = unset */
    uint8_t rmode;
    int executable; /* GOFF_EXEC_* */
    int readonly, noload, defload;
    int fill;     /* -1 = none */
    int priority; /* -1 = none */
    int merge;
    int tasking;
    int movable, removable;
    int cattr; /* made by CATTR: no AMODE, defaults of CATTR (oracle) */
} ClassAttr;

typedef enum { REL_ADDR, REL_V, REL_Q, REL_CXD, REL_RI, REL_LEN, REL_R, REL_QY } RelocKind;

/* the GOFF flag pattern each relocation kind writes (emit.c defines them);
 * the disassembler recognizes foreign records with the same table */
const GoffRelocPat *reloc_pattern(RelocKind k);

typedef struct {
    int64_t off; /* offset in the class */
    int len;
    Symbol *target; /* external symbol (ER), or NULL */
    Class *cls;     /* class origin when target is NULL */
    RelocKind kind;
    int amode;
    int sub; /* the target is subtracted (RLD action SUB) */
} Reloc;

/* One location counter (LOCTR) of a class; the first piece of a class
 * carries the section/class name. */
struct Piece {
    char *name;
    Class *cls;
    int64_t loc;   /* current location counter, relative to start */
    int64_t max;   /* highest location reached (ORG can move back) */
    int64_t start; /* offset in the class after layout */
    int64_t hint;  /* start found by the previous assembly run: items align to hint + loc */
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
    int64_t base; /* class start in the section location space (0 unless START had an address) */
    int64_t len;
    uint8_t *data;
    uint32_t esd; /* ED esdid */
    uint32_t pr;  /* PR esdid when part != NULL */
    RelocVec relocs;
    int zid; /* scope number in the ZSYM block */
};

struct Section {
    char *name;
    SectKind kind;
    Span def;
    ClassVec classes;
    uint8_t amode, rmode; /* GOFF_AMODE_* / GOFF_RMODE_*, 0 = unset */
    Piece *cur;           /* location counter to resume with */
    Section *prv_owner;   /* DSECT: the section started last when it was defined (owns its pseudo register) */
    Piece *text_cur;      /* last location counter of the first class (B_TEXT): CSECT resumes there */
    int64_t start_addr;   /* START address: the section location space starts here */
    uint32_t esd;
    int max_align;        /* strictest alignment of the data (bytes) */
    int prv;              /* needs the pseudo register class B_PRV */
    uint32_t prv_esd;     /* ED esdid of B_PRV */
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
void value_set_arena(Arena *a);
int value_is_abs(const Value *v);
/* "base + constant" with a single positive term */
int value_simple(const Value *v, Base *b);
Value value_add(Value a, Value b, int sign);
Value value_neg(Value v);

typedef enum { SYM_LABEL, SYM_ABS, SYM_REL, SYM_EXT, SYM_SECT, SYM_DXD, SYM_DEFERRED } SymKind;

typedef struct {
    int linkage; /* GOFF_LINKAGE_*, -1 = unset */
    int scope;   /* GOFF_SCOPE_*, -1 = unset */
    char *reference; /* "CODE", "DATA" or NULL */
    int indirect;    /* REFERENCE(INDIRECT) = 1, REFERENCE(DIRECT) = 0, -1 = unset */
    char *psect;
    Class *psect_cls;   /* resolved PSECT class (ED), or NULL */
    Symbol *psect_sym;  /* resolved PSECT DXD (PR), or NULL */
    char *attributes;   /* XATTR ATTRIBUTES(label) */
    Class *attr_cls;    /* the class of that label */
    int64_t attr_off;   /* its offset in the class */
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
    unsigned has_len;  /* the length attribute is explicit or derived (not "unset") */
    int64_t count; /* SYM_LABEL: number of items of the first DC/DS operand (countof) */
    int align;     /* SYM_LABEL: alignment of the item in bytes (alignof) */
    char type;    /* type attribute (C, X, F, ...); 0 is a valid type when has_type is set */
    unsigned has_type; /* the type attribute is set (an EQU third operand or a label type) */
    unsigned fixed_len; /* an explicit EQU length operand was given */
    /* a forward-reference EQU: the value expression is owned by Asm.exprs
     * (freed when the assembly state is released) and resolved lazily */
    Expr *deferred;
    Stmt *def_stmt;
    int resolving;
    int failed; /* a reference already reported that this EQU does not resolve */
    int scale;    /* scale attribute */
    size_t def_idx; /* index of the defining statement */
    int weak;     /* WXTRN */
    int entry;    /* ENTRY */
    uint8_t amode, rmode; /* AMODE/RMODE statement naming an ENTRY symbol, 0 = none */
    Section *sect;
    XAttr *xattr;
    int used;
    uint32_t esd;    /* ER esdid */
    uint32_t ld_esd; /* LD esdid */
    Section *ext_sect; /* EXTRN/WXTRN: the section current at the statement (parent of its ER) */
    uint32_t r_er;   /* ER esdid made for an R-type constant naming this local symbol */
    /* external dummy section (SYM_DXD, or a DSECT used in Q/J: xd) */
    int64_t xd_len;
    int xd_align;     /* bytes */
    int xd;           /* the symbol is an external dummy section */
    Section *xd_sect; /* its B_PRV is in this section */
    int zid;          /* symbol number in the ZSYM block */
    Vec(Pos) refs;    /* where the symbol was used (pass 2, for the listing) */
    char regkind;     /* EQU %r12: a register name: 'R', 'F', 'A', 'C' or 'V'; 0 otherwise */
};

typedef struct {
    char *name;
    Span sp;
    int is_amode;
    uint8_t val;
} SymMode;
typedef Vec(SymMode) SymModeVec;
typedef Vec(Stmt *) StmtVec;
typedef Vec(Section *) SectionVec;
typedef Vec(Symbol *) SymbolVec;

/* One assembled statement for ZSYM: where its bytes are (final layout). */
typedef struct {
    Stmt *st;
    Class *cls; /* NULL before the first section */
    int64_t off, len;
    int64_t pad; /* alignment bytes at the start of off..off+len (the listing starts after them) */
} ZRec;

typedef Vec(ZRec) ZRecVec;

/* the original text of a source file, kept in the object at -gz2 (zsrc.h) */
typedef struct {
    char *name;
    char *text;
    size_t len;
} ZSrcIn;

/* a literal of a pool as the listing shows it */
typedef struct {
    Class *cls;
    int64_t off, len;
    char *key;    /* the literal as written, with the '=' */
    size_t owner; /* index in zrecs of the statement that holds the pool (LTORG or END) */
} LitRec;

/* one USING as the listing shows it (a copy made when it was established) */
typedef struct {
    Span sp;       /* the USING statement */
    char *label;   /* or NULL */
    Base key;      /* class of the domain (B_NONE: absolute) */
    int64_t off, lo, hi, end;
    int reg, dependent;
    int64_t bias;  /* Using.bias: the register holds the address of off minus bias */
    int id;        /* Using.id, to follow DROP */
    Pos dropped;   /* line that ended it; file NULL while active */
    size_t zbeg, zend; /* index in Module.zrecs of the USING and of the statement that ended it (-1: still active) */
} UsingRec;

typedef struct {
    char *name;
    SectionVec sections;
    StrMap symbols;
    SymbolVec sym_order;
    char *entry; /* END operand, or NULL */
    Arena *arena; /* values and other transient data kept with the module */
    Span entry_def;
    ZRecVec zrecs; /* the statements, in source order (ZSYM.md) */
    int zlevel;    /* ZSYM level written by module_emit: 0 none, 1 symbols and statements,
                    * 2 also the original sources (zsrc) */
    Vec(ZSrcIn) zsrc;    /* at level 2: the main file first, then the included files */
    Vec(char *) zdefs;   /* at level 2: the -D options given ("NAME" or "NAME=value") */
    /* for the listing (listing.c) */
    StmtVec stmts;       /* every statement read, in source order */
    StmtVec synth;       /* synthetic statements of literals, owned here */
    Vec(LitRec) lits;
    Vec(UsingRec) usings;
    CBlockDebugVec cblocks; /* -g: CBLOCK locals and their ctemp ranges */
} Module;

Symbol *module_symbol(const Module *m, const char *name);
/* Release a Module and everything it owns (sections, classes, pieces,
 * symbols, statements, literals, USINGs, zsrc, zdefs, ...). After this
 * call the Module pointer must not be used. */
void asm_free(Module *m);

/* Writes the module as a GOFF object (malloc'ed). On failure returns NULL
 * and describes the problem in err. */
uint8_t *module_emit(Module *m, size_t *len, char *err, size_t errsz);
int module_validate(const Module *m, char *err, size_t errsz); /* 32-bit limits, relocs inside the class */
/* The same as a relocatable ELF64 object for Linux on Z (src/elf.c). */
uint8_t *module_emit_elf(Module *m, size_t *len, char *err, size_t errsz);

/* ---- source lines and statements ------------------------------------ */

/* One logical input line as supplied by the preprocessor (or the plain
 * file reader): text without the line end, in UTF-8. */
typedef struct {
    const char *file;
    int line; /* 1-based physical line of the first segment */
    const char *text;
    const char *orig; /* the line as written when block comments were blanked in text, else NULL */
    const int *plines; /* physical line per segment, or NULL when 1:1 */
    const size_t *poffs; /* byte offset in text per segment, or NULL */
    size_t nplines;
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
    size_t li; /* index of its first line in the line array given to read_statements */
    int nli;   /* the number of lines it uses (continuation) */
};


/* One top-level comma separated operand. */
typedef struct {
    char *text;
    int off; /* offset in Stmt.operands */
} Operand;

typedef Vec(Operand) OperandVec;
/* release the operand texts and the vector itself */
void operands_free(OperandVec *v);

Span stmt_operand_span(const Stmt *s, int off, int n);
Span stmt_span(const Stmt *s); /* the operation field, or column 1 */

/* Split the lines into statements; errors go to rep. */
void read_statements(StmtVec *out, const SrcLine *lines, size_t n, Reporter *rep);
void split_operands(OperandVec *out, const char *s);

int is_blank(int c);
/* Blank the C-style block comments of one line in place (every byte of a comment
 * and its delimiters becomes a blank, so columns stay). *in_comment carries a comment that is
 * open at the end of the line over to the next one. Not scanned: comment lines (first non-blank
 * character * or ;), quoted
 * strings and text after "//". Returns nonzero when the line touches a comment. */
int bc_line(char *s, int *in_comment);
/* bc_line over all lines of one file (text is replaced, orig keeps the line as written); an
 * unclosed comment is the error ZA4036. */
void src_blank_comments(SrcVec *lines, Reporter *rep);
/* The column anchor: a line whose first non-blank character is '^' is read as if the text after
 * the '^' started in column 1 ("    ^LB1  DC  AD(0)"). Returns that text, or s itself when the
 * line has no anchor. */
const char *caret_rest(const char *s);
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

typedef enum { EX_NUM, EX_SYM, EX_LOC, EX_ATTR, EX_BIN, EX_UN, EX_FUNC } ExprKind;

struct Expr {
    ExprKind kind;
    Span sp;
    int64_t num;
    char *name; /* EX_SYM, EX_ATTR */
    char *qual; /* USING label of a qualified symbol (LABEL.NAME), else NULL */
    char attr;  /* 'L', 'T', ... */
    int loc;    /* L'* */
    char op;    /* EX_BIN, EX_UN; EX_FUNC: 's' sizeof, 'o' offsetof, 'a' alignof, 'c' countof */
    Expr *l, *r;
};

/* Parse text (an operand or part of one); off is its offset in
 * st->operands for diagnostics. Returns NULL after reporting an error. */
Expr *parse_expr(Stmt *st, const char *text, int off, Reporter *rep);
/* the built-in functions sizeof, offsetof, alignof and countof: n bytes at s
 * (not followed by a quote) name one */
int expr_is_func_name(const char *s, size_t n);
/* short description of e for messages */
char *expr_describe(const Expr *e);
/* release a tree returned by parse_expr()/asm_expr() */
void expr_free(Expr *e);

/* ---- the assembler --------------------------------------------------- */

/* One base register assignment of a USING (DESIGN.md 4.3).  Offsets are in
 * the section of key (absolute when key.kind is B_NONE).  The register holds
 * the address of offset off minus bias; a dependent USING gets its register
 * and bias from the USING that resolves its supporting address. */
typedef struct {
    char *label;       /* USING label (qualifier), NULL when unlabeled */
    Base key;          /* class (also for dummy sections); B_NONE = absolute */
    int64_t off;       /* base address */
    int64_t lo, hi;    /* addressable offsets lo <= t < hi */
    int64_t end;       /* end limit; applies to unsigned displacements only */
    int reg;
    int64_t bias;
    int dependent;
    int id;   /* serial number of the USING (per register) */
    int root; /* id of the USING that supplies the register; own id if not dependent */
    int dead; /* being removed by DROP */
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
    char *name;
    Span sp;
    Section *sect; /* the section of the constant */
} XdRef;

typedef Vec(XdRef) XdRefVec;

/* sizeof or alignof of a DSECT used in pass 1: the value must still be right
 * when the DSECT is complete */
typedef struct {
    Section *sect;
    char fn; /* 's' or 'a' */
    int64_t val;
    Span sp;
} FuncUse;

typedef Vec(FuncUse) FuncUseVec;

typedef struct {
    char *key;  /* operand text including '=' */
    char *shown; /* the text as written when key differs (* terms), else NULL */
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

/* the state ACONTROL changes (and PUSH ACONTROL saves) */
typedef struct {
    int afpr, strict;
} AcState;

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
    int ended_warned;
    ExprVec exprs;        /* every tree made by asm_expr(), freed by asm_state_free */
    Arena *arena;         /* Value terms; moved to the Module when it is kept */
    Vec(OperandVec) opvecs; /* operand vectors made by asm_ops(), freed by asm_state_free */
    Vec(char *) using_labels; /* all label strings of USINGs (owned here) */
    UsingVec usings;
    int using_id;
    char *end_entry;
    Span end_span;
    XattrVec xattrs;
    SymModeVec symmodes; /* AMODE/RMODE statements naming no section yet */
    EntryVec entries;
    StrMap secs;
    PoolVec pools;
    int pool_no;
    int star_no; /* literals with a location counter reference in this pass */
    XdRefVec xdrefs;  /* names used by Q and J constants, resolved after pass 1 */
    FuncUseVec fuses; /* sizeof/alignof of DSECTs seen in pass 1 */
    size_t cur_idx;   /* index of the statement being assembled */
    int64_t star_len; /* L'* of the current statement, 0 = unknown */
    int64_t loc_bias; /* temporary offset for * while a duplicated constant is evaluated */
    int saw_loc;      /* the last dc_values evaluation used the location counter */
    int trial;        /* a speculative evaluation: do not set statement/symbol errors */
    Piece *pool_piece; /* the literal pool of END was written here, from pool_loc (ZSYM) */
    int64_t pool_loc;
    /* alignment bytes before the first byte of the statement (ZRec.pad) */
    int lead_open, in_align;
    int64_t lead_pad;
    size_t lit_owner; /* LitRec.owner of the literals being assembled */
    int using_logged; /* highest Using.id copied to Module.usings */
    int strict;       /* registers only as %r<n> or a name made with EQU %r<n> */
    StrMap *hints;    /* piece key -> int64_t start of the previous run (LOCTR alignment) */
    StrMap opsyn;     /* OPSYN: upper case operation code -> its meaning ("" = deleted) */
    int afpr;         /* ACONTROL AFPR: FP registers 1, 3, 5, 7 to 15 are allowed */
    int print_depth;  /* PUSH PRINT entries (the listing keeps the saved states) */
    Vec(UsingVec) using_stack; /* PUSH USING */
    Vec(AcState) ac_stack;     /* PUSH ACONTROL */
};

/* Assemble the lines; returns NULL when errors were reported. The caller
 * registers file texts with rep_add_source for diagnostic excerpts. */
Module *asm_assemble(const SrcLine *lines, size_t n, const char *name, Reporter *rep);
/* the same, but the module is returned even when errors were reported (for
 * the listing); pass 2 may be incomplete then */
Module *asm_assemble_all(const SrcLine *lines, size_t n, const char *name, Reporter *rep);
/* Convenience: split text, register it and assemble. */
Module *asm_assemble_text(const char *file, const char *text, size_t len, Reporter *rep);

/* Strict registers (-strict-registers): in a register position only %r1 (any
 * case, also %f %a %c %v) and names made with `NAME EQU %r1` are accepted, not
 * plain numbers or other symbols. Default 0. Read when assembling starts. */
extern ZA_TLS int za_strict_registers;

typedef void (*Handler)(Asm *a, Stmt *st);

/* assembler.c */
Symbol *asm_lookup(Asm *a, const char *name);
Symbol *asm_define(Asm *a, Stmt *st, const char *name, Span sp, int *created);
/* returns the new symbol in pass 1 (count 1, alignment 1), else NULL */
Symbol *asm_define_label(Asm *a, Stmt *st, int64_t length, char typ);
int asm_valid_name(const char *n);
/* name is the label of an active labeled USING */
int asm_using_label_active(Asm *a, const char *name);
int asm_need_section(Asm *a, Stmt *st);
int asm_ops(Asm *a, Stmt *st, int min, int max, OperandVec *out);
Expr *asm_expr(Asm *a, Stmt *st, Operand o);
Span asm_op_span(Stmt *st, Operand o);
int asm_abs_expr(Asm *a, Stmt *st, Operand o, const char *what, int64_t *out);
int asm_eval(Asm *a, Stmt *st, const Expr *e, int need, Value *out);
/* the length attribute of the leftmost term of an EQU value (HLASM rule) */
int64_t asm_equ_left_len(Asm *a, const Expr *e);
/* resolve a forward-reference EQU; 1 on success (the symbol is now ABS or REL) */
int asm_resolve_equ(Asm *a, Symbol *s, Stmt *st, int need);
void asm_expr_err(Asm *a, Stmt *st, Span sp, const char *code, const char *fmt, ...);
void asm_stmt(Asm *a, Stmt *st);
void asm_align(Asm *a, int64_t n);
void asm_advance(Asm *a, int64_t n);
void asm_put(Asm *a, const uint8_t *b, size_t n);
void asm_reloc(Asm *a, int n, Symbol *target, Class *cls, RelocKind kind, int sub);
Value asm_addr_value(Asm *a, Piece *p, int64_t off);
void asm_note_ref(Asm *a, Symbol *s, Span sp);
/* %r1 %f0 %a2 %c3 %v31 (any case): the kind (upper case letter) and the number; 0 if t is not that */
int asm_parse_pct(const char *t, char *kind, int *num);
/* the largest register number of a kind letter: 15, or 31 for V */
int asm_reg_max(char kind);
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
void dir_assert(Asm *a, Stmt *st);
void dir_org(Asm *a, Stmt *st);
void dir_cnop(Asm *a, Stmt *st);
void dir_end(Asm *a, Stmt *st);
void dir_dxd(Asm *a, Stmt *st);
void dir_ccw(Asm *a, Stmt *st);
void dir_cxd(Asm *a, Stmt *st);
void dir_copy(Asm *a, Stmt *st);
void dir_ignore(Asm *a, Stmt *st);
void dir_acontrol(Asm *a, Stmt *st);
void dir_ceject(Asm *a, Stmt *st);
void dir_exitctl(Asm *a, Stmt *st);
void dir_pushpop(Asm *a, Stmt *st);
void dir_repro(Asm *a, Stmt *st);
void dir_opsyn(Asm *a, Stmt *st);
int asm_is_op(const char *upper_op); /* a directive or instruction */
int asm_is_directive(const char *upper_op);
void dir_punch(Asm *a, Stmt *st);
size_t asm_directive_names(const char *const **names);

/* dc.c */
typedef struct {
    int64_t dup;
    char typ; /* upper case type letter */
    char ext; /* 'A'/'E' (character), 'D' (F, A), 'B'/'H' (floating point) */
    int64_t length; /* bytes; with a bit length: bytes needed to hold it */
    int64_t bits;   /* the bit length of L.n (0 = none) */
    int has_len, has_nom, quoted;
    int has_scale, has_exp; /* Sn and En modifiers */
    int64_t scale, exp10;
    int scale_off, exp_off; /* positions inside the operand */
    char *nom;  /* nominal text without the delimiters */
    int nom_off;
    int off, n; /* the operand inside Stmt.operands */
} DcSpec;

/* a further relocation item on the same field (A(X-Y), A(X+Y-Z)) */
typedef struct {
    Symbol *target;
    Class *cls;
    int sub;
} DcRelTerm;

typedef struct {
    uint8_t *b;
    size_t n;
    int has_rel;
    Symbol *target;
    Class *cls;
    RelocKind kind;
    int sub;
    DcRelTerm more[4];
    int nmore;
} DcVal;

typedef Vec(DcVal) DcVec;
/* release the byte buffers of a DC value vector (dc.c) */
void dc_vec_free(DcVec *v);
void dc_spec_free(DcSpec *sp);

void asm_put_val(Asm *a, const DcVal *v);


int64_t dc_alignment(const DcSpec *sp);
int64_t dc_deflen(const DcSpec *sp);
int dc_parse(Asm *a, Stmt *st, Operand o, DcSpec *sp);
int dc_values(Asm *a, Stmt *st, const DcSpec *sp, int is_ds, int64_t base_off, DcVec *vals);
Diag *dc_err(Asm *a, Stmt *st, const char *code, int off, int n, const char *fmt, ...);
void split_nominal(const DcSpec *sp, OperandVec *out);
void be_bytes(uint64_t v, int n, uint8_t *out);
void dir_dc(Asm *a, Stmt *st);

/* float.c: E, D and L constants; fills out (n bytes) or describes the
 * problem in err. ieee selects binary instead of hexadecimal floating point. */
int float_bytes(const char *text, int ieee, int n, uint8_t *out, const char **err);
/* E, D and L constants of every kind: ext is 0, 'H', 'Q', 'B' or 'D'; with
 * an exponent modifier (power of 10) and a scale modifier (hexadecimal
 * digits). 1 ok, 2 ok with a warning in *warn, 0 not a number, -1 error. */
void float_set_type(char typ); /* E, D or L for binary floating point of unusual length, else 0 */
/* Big temporaries are allocated from this arena (the Module arena), freed by asm_free */
void big_set_arena(Arena *a);
int float_const(const char *text, char ext, int n, long exp10, long scale, uint8_t *out,
                const char **err, const char **warn);
/* F and H: whole number result of text * 10^exp10 * 2^scale, rounded */
int fixed_bytes(const char *text, int n, long exp10, long scale, uint8_t *out, const char **err);
int dc_float(Asm *a, Stmt *st, const DcSpec *sp, DcVec *out);

/* instr.c */
void asm_instr(Asm *a, Stmt *st);
/* an operand split into displacement text and the parenthesized part */
typedef struct {
    Operand disp; /* text before the parenthesis (the whole operand if none) */
    OperandVec inner;
    int parens;
} AddrOp;
AddrOp asm_split_addr(Operand o);
void addr_op_free(AddrOp *r);
int asm_base_disp(Asm *a, Stmt *st, Operand o, int longdisp, int64_t *base, int64_t *disp);
/* the variant of an instruction (isa_lookup) that takes n operands, or NULL */
const IsaInst *asm_choose_variant(const IsaInst *vars, size_t nv, int n);
void dir_using(Asm *a, Stmt *st);
void dir_drop(Asm *a, Stmt *st);
void dir_cfi(Asm *a, Stmt *st);
/* CFI operands (instr.c): CFA reg,off | SAVE reg,off | SAME reg; the offset is a decimal number.
 * Returns 0 when malformed. The debug information reads them again from the statement. */
typedef enum { CFI_CFA, CFI_SAVE, CFI_SAME, CFI_UNDEF } CfiKind;
typedef struct {
    CfiKind kind;
    int reg;
    int64_t off;
} CfiOp;
int cfi_operands(const char *operands, CfiOp *out);

/* literal.c */
void dir_ltorg(Asm *a, Stmt *st);
void asm_end_pool(Asm *a);
void asm_literal_operands(Asm *a, Stmt *st, OperandVec *ops);
void asm_close_pool(Asm *a);

#endif
