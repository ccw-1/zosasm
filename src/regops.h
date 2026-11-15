/* regops.h - walk the register positions of a statement's operands.
 *
 * Used by -disasm -strict-registers (numbers to %r1) and by hlasm2za
 * (analysis and rewriting of HLASM register operands). */
#ifndef ZA_REGOPS_H
#define ZA_REGOPS_H

typedef enum {
    RO_INSN,  /* a machine instruction: the register slots of the chosen variant */
    RO_USING, /* USING: every operand but the first is a base register */
    RO_DROP   /* DROP: every operand is a base register */
} RegOpsKind;

/* Called for the text of every register position (an operand or one part of
 * an address such as the X of D(X,B)); flags is the F_GPR/F_FPR/F_AR/F_CR/F_VR
 * flag of the field. Returns the replacement text (heap) or NULL to keep the
 * text. The text is not trimmed and may be empty (D(,B)). */
typedef char *(*RegVisit)(void *ctx, const char *text, unsigned flags);

/* The operands (comma separated text) with every register position passed
 * through fn. Returns the new operand text, or NULL when the statement has
 * no variant for this number of operands (op is the mnemonic for RO_INSN and
 * ignored otherwise). Literals (=...) are skipped. */
char *regops_map(const char *op, RegOpsKind kind, const char *operands, RegVisit fn, void *ctx);

/* the register letter of operand flags: 'r', 'f', 'a', 'c' or 'v' */
char regops_letter(unsigned flags);

#endif
