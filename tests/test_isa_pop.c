/* test_isa_pop.c - the instruction table (generated from the Principles of
 * Operation) and instruction encodings written out by hand from the POP
 * instruction formats. */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "testlib.h"

/* encode "T CSECT / <src>"; compare the code bytes with the hex string */
static void asm_case(const char *src, const char *want) {
    Reporter *rep = rep_new(0);
    char *text = xasprintf("T CSECT\n %s\n", src);
    Module *m = asm_assemble_text("t.za", text, strlen(text), rep);
    char got[64] = "";
    if (!m) {
        printf("assembly of \"%s\" failed\n", src);
        rep_print(rep, stdout, 0);
        CHECK(0);
        return;
    }
    {
        Class *k = m->sections.v[0]->classes.v[0];
        int64_t i;
        for (i = 0; i < k->len && i < 20; i++) sprintf(got + 2 * i, "%02X", k->data[i]);
    }
    CHECK_MSG(!strcmp(got, want), "%-28s = %s, want %s", src, got, want);
}

static void test_encodings(void) {
    static const struct {
        const char *src, *want;
    } c[] = {
        /* RR */
        {"LR 1,2", "1812"},
        {"AR 15,0", "1AF0"},
        {"SR 1,1", "1B11"},
        {"MR 2,5", "1C25"},
        {"BR 14", "07FE"},
        {"BCR 8,14", "078E"},
        {"BER 14", "078E"},
        {"NOPR 0", "0700"},
        /* RRE */
        {"LGR 3,4", "B9040034"},
        /* RX-a, RX-b */
        {"L 1,4(2,3)", "58123004"},
        {"L 1,4(,3)", "58103004"},
        {"LA 2,4095(2)", "41220FFF"},
        {"BE 4(,12)", "47 80C004"},
        {"B 4(,12)", "47F0C004"},
        /* RXY-a: long displacement, DL then DH */
        {"LY 1,5(2,3)", "E31230050058"},
        {"LY 1,-1(,3)", "E3103FFFFF58"},
        {"LY 1,74565(2,3)", "E31233451258"},
        {"STG 1,-8(,13)", "E310DFF8FF24"},
        /* RS-a, RS-b, RSY-a, RSY-b */
        {"SLL 1,3", "89100003"},
        {"LM 14,12,12(13)", "98ECD00C"},
        {"STM 14,12,12(13)", "90ECD00C"},
        {"LAM 1,2,0(3)", "9A123000"},
        {"LCTL 0,1,0(3)", "B7013000"},
        {"LMG 14,12,8(13)", "EBECD0080004"},
        {"SLLG 1,2,3", "EB120003000D"},
        /* SI, SS */
        {"MVI 4(1),X'FF'", "92FF1004"},
        {"TM 4(1),X'80'", "91801004"},
        {"MVC 0(8,1),0(2)", "D20710002000"},
        {"CLC 0(4,1),0(2)", "D50310002000"},
        /* S */
        {"STCK 0(1)", "B2051000"},
        /* RI-a, RI-b, RI-c, RIL-a, RIL-b */
        {"LHI 1,-1", "A718FFFF"},
        {"LGHI 1,5", "A7190005"},
        {"BRAS 14,*+8", "A7E50004"},
        {"J *+4", "A7F40002"},
        {"JE *+6", "A7840003"},
        {"JLU *+6", "C0F400000003"},
        {"LARL 1,*+8", "C01000000004"},
        /* vector: RXB carries the fifth register bit */
        {"VL 1,0(,2)", "E71020000006"},
        {"VL 17,0(,2)", "E71020000806"},
        {"VST 1,0(,2)", "E7102000000E"},
        {"VLR 1,2", "E71200000056"},
        {"VAB 1,2,3", "E712300000F3"},
        {"VAF 1,2,3", "E712300020F3"},
        /* extended mnemonics that OR a fixed value into an optional operand */
        {"VFAEB 1,2,3", "E71230000082"},
        {"VFAEBS 1,2,3", "E71230100082"},
        {"VFAEBS 1,2,3,2", "E71230300082"},
    };
    size_t i;
    for (i = 0; i < sizeof c / sizeof c[0]; i++) {
        char want[40], *w = want;
        const char *p;
        /* "47 80C004" style spaces are only for readability */
        for (p = c[i].want; *p; p++)
            if (*p != ' ') *w++ = *p;
        *w = 0;
        asm_case(c[i].src, want);
    }
}

/* every table entry must be internally consistent */
static void test_table(void) {
    size_t i, j, nfac = 0;
    int dup = 0;
    for (i = 0; i < isa_inst_count; i++) {
        const IsaInst *in = &isa_inst_tab[i];
        size_t nops = isa_nops(in);
        unsigned char fields[6] = {0};
        int ok = 1;
        if (in->length != 2 && in->length != 4 && in->length != 6) ok = 0;
        for (j = 0; ok && j < nops; j++) {
            const IsaOperand *op = isa_op(in, j);
            int b, bits = op->bits;
            if (op->shift + bits > in->length * 8) ok = 0;
            for (b = 0; ok && b < bits; b++) {
                int pos = op->shift + b;
                if (fields[pos / 8] & (0x80 >> (pos % 8))) ok = 0; /* overlap */
                fields[pos / 8] |= (unsigned char)(0x80 >> (pos % 8));
            }
        }
        /* the fixed opcode bits never lie inside an operand field, except that
         * extended mnemonics may fix a field that is not an operand */
        for (j = 0; ok && j < 6; j++)
            if (in->opcode[j] & fields[j]) ok = 0;
        CHECK_MSG(ok, "table entry %s (%s) is inconsistent", in->mnemonic, in->format);
        CHECK_MSG(in->opt <= nops, "%s: opt %d > operands", in->mnemonic, in->opt);
        if (in->facility[0]) nfac++;
        if (i && !strcmp(in->mnemonic, isa_inst_tab[i - 1].mnemonic) &&
            !memcmp(in->opcode, isa_inst_tab[i - 1].opcode, 6) &&
            !memcmp(in->orv, isa_inst_tab[i - 1].orv, 6) &&
            !strcmp(in->format, isa_inst_tab[i - 1].format))
            dup++;
    }
    CHECK_MSG(dup == 0, "%d duplicate table entries", dup);
    CHECK_MSG(isa_inst_count == 2369, "instruction count is %d, expected 2369", (int)isa_inst_count);
    CHECK(nfac > 500);
}

int main(void) {
    test_encodings();
    test_table();
    TEST_MAIN_END();
}
