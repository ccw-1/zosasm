/* test_oracle.c - rules and object details that were found by comparing
 * zosasm with IBM HLASM (tools/oracle*.sh, tools/oracle_rules.py): register
 * pairs, lengths, immediate windows, masks, term limits, entry AMODE/RMODE,
 * relative immediates to external symbols and GOFF flags. */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "../src/ebcdic.h"
#include "../src/goff.h"
#include "testlib.h"

static Module *assemble(const char *src, Reporter **repp) {
    Reporter *rep = rep_new(0);
    Module *m = asm_assemble_text("t.za", src, strlen(src), rep);
    if (repp) *repp = rep;
    return m;
}

static int has_code(const Reporter *rep, const char *code) {
    size_t i;
    for (i = 0; i < rep_count(rep); i++)
        if (!strcmp(rep_get(rep, i)->code, code)) return 1;
    return 0;
}

/* Assemble "T CSECT / EXTRN EXTA / <src>"; fill hex with the code bytes. The
 * return value is the reporter, NULL when assembly failed. */
static Reporter *run(const char *src, char *hex, size_t hexsz) {
    Reporter *rep;
    char *text = xasprintf("T CSECT\n EXTRN EXTA\n %s\n", src);
    Module *m = assemble(text, &rep);
    hex[0] = 0;
    if (!m) return rep;
    {
        Class *k = m->sections.v[0]->classes.v[0];
        int64_t i;
        for (i = 0; i < k->len && 2 * (size_t)i + 3 < hexsz; i++) sprintf(hex + 2 * i, "%02X", k->data[i]);
    }
    return rep;
}

static void ok(const char *src, const char *want) {
    char hex[64];
    Reporter *rep = run(src, hex, sizeof hex);
    CHECK_MSG(rep_error_count(rep) == 0, "%s: unexpected error", src);
    CHECK_MSG(!strcmp(hex, want), "%-26s = %s, want %s", src, hex, want);
}

static void warns(const char *src, const char *want) {
    char hex[64];
    Reporter *rep = run(src, hex, sizeof hex);
    CHECK_MSG(rep_error_count(rep) == 0 && has_code(rep, "ZA2081"), "%s: want warning ZA2081", src);
    CHECK_MSG(!strcmp(hex, want), "%-26s = %s, want %s", src, hex, want);
}

static void fails(const char *src, const char *code) {
    char hex[64];
    Reporter *rep = run(src, hex, sizeof hex);
    CHECK_MSG(rep_error_count(rep) > 0 && has_code(rep, code), "%s: want error %s", src, code);
}

static void test_pairs(void) {
    /* general register pairs must be even, floating-point pairs 0,1,4,5,8,9,12,13 */
    fails("CDS 1,2,0(3)", "ZA2080");
    ok("CDS 0,2,0(3)", "BB023000");
    fails("CDSG 3,4,0(3)", "ZA2080");
    fails("AXR 2,0", "ZA2080");
    fails("AXR 0,6", "ZA2080");
    ok("AXR 1,0", "3610");
    ok("AXR 13,4", "36D4");
    fails("LXR 3,0", "ZA2080");
}

static void test_lengths(void) {
    /* length 0 is accepted and encoded as field 0 */
    ok("MVC 0(0,1),0(2)", "D20010002000");
    ok("PKA 0(1),0(32,2)", "E91F10002000");
    fails("PKA 0(1),0(33,2)", "ZA2039");
    ok("CVBG 1,0(2)", "E3120000000E");
    /* DP and MP: the divisor/multiplier must be shorter than the first operand */
    ok("DP 0(4,1),0(2,2)", "FD3110002000");
    fails("DP 0(4,1),0(4,2)", "ZA2084");
    fails("MP 0(2,1),0(3,2)", "ZA2084");
}

static void test_immediates(void) {
    /* outside the window: warning ASMA320W, low bits are used */
    warns("AHI 1,40000", "A71A9C40");
    warns("AHI 1,-32769", "A71A7FFF");
    ok("AHI 1,32767", "A71A7FFF");
    ok("AHI 1,-32768", "A71A8000");
    ok("LHI 1,65535 - 65536", "A718FFFF");
    /* masks: values HLASM refuses */
    fails("CDFBRA 1,2,3,0", "ZA2082");
    ok("CDFBRA 1,3,3,0", "B3953013");
    fails("MC 0(1),16", "ZA2082");
    ok("MC 0(1),15", "AF0F1000");
    fails("NIAI 16,0", "ZA2082");
    fails("SRP 0(1,1),0(1),10", "ZA2082");
    ok("SRP 0(1,1),0(1),9", "F00910001000");
}

static void test_terms(void) {
    char hex[64];
    Reporter *rep = run("DC F'-2147483648'\n DC F'2147483647'\nX EQU 2147483647\nY EQU -2147483648", hex,
                        sizeof hex);
    CHECK(rep_error_count(rep) == 0);
    rep = run("DS 0H\nZ EQU 2147483648", hex, sizeof hex);
    CHECK(has_code(rep, "ZA1004"));
    rep = run("DS 0H\nZ EQU X'FFFFFFFF'", hex, sizeof hex);
    CHECK(rep_error_count(rep) == 0);
    rep = run("DS 0H\nZ EQU X'100000000'", hex, sizeof hex);
    CHECK(has_code(rep, "ZA1007"));
}

static void test_relimm_extrn(void) {
    /* a relative immediate to an external symbol carries only the offset
     * from the symbol, in halfwords; the linker adds the distance */
    ok("LARL 1,EXTA", "C01000000000");
    ok("LARL 1,EXTA+4", "C01000000002");
    ok("LARL 1,EXTA-6", "C010FFFFFFFD");
    ok("BRASL 14,EXTA", "C0E500000000");
}

static GoffModule *emit_src(const char *src) {
    Reporter *rep;
    Module *m = assemble(src, &rep);
    char err[256];
    size_t n;
    uint8_t *obj;
    const char *perr = NULL;
    if (!m) {
        rep_print(rep, stdout, 0);
        CHECK_MSG(0, "assembly failed");
        return NULL;
    }
    obj = module_emit(m, &n, err, sizeof err);
    CHECK_MSG(obj != NULL, "emit: %s", err);
    return obj ? goff_parse(obj, n, &perr) : NULL;
}

static const GoffESD *find_esd(const GoffModule *g, GoffESDType type, const char *name) {
    size_t i;
    for (i = 0; i < g->esds.n; i++) {
        const GoffESD *e = g->esds.v[i];
        char *nm;
        int same;
        if (e->type != type) continue;
        nm = ebcdic_decode(e->name, e->name_len);
        same = !strcmp(nm, name);
        free(nm);
        if (same) return e;
    }
    return NULL;
}

static void test_goff_flags(void) {
    GoffModule *g = emit_src("S1       CSECT\n"
                             "S1       AMODE 31\n"
                             "S1       RMODE ANY\n"
                             "         ENTRY ENT1,ENT2,ENT3\n"
                             "ENT1     BR    14\n"
                             "ENT2     BR    14\n"
                             "ENT3     BR    14\n"
                             "ENT1     AMODE 31\n"
                             "ENT2     RMODE ANY\n"
                             "         EXTRN EXTA\n"
                             "         DC    A(ENT1),A(EXTA)\n"
                             "         END\n");
    const GoffESD *sd, *ed, *ld, *er;
    if (!g) return;
    sd = find_esd(g, GOFF_SD, "S1");
    CHECK(sd && sd->name_space == GOFF_NS_NORMAL);
    ed = find_esd(g, GOFF_ED, "B_TEXT");
    CHECK(ed && ed->attr.executable == GOFF_EXEC_UNSPEC);
    /* the section name LD carries the section modes */
    ld = find_esd(g, GOFF_LD, "S1");
    CHECK(ld && ld->attr.amode == GOFF_AMODE_31 && ld->attr.rmode == GOFF_RMODE_31);
    CHECK(ld && ld->attr.scope == GOFF_SCOPE_UNSPEC && ld->attr.executable == GOFF_EXEC_UNSPEC);
    /* an ENTRY symbol has no modes unless an AMODE/RMODE statement names it */
    ld = find_esd(g, GOFF_LD, "ENT1");
    CHECK(ld && ld->attr.amode == GOFF_AMODE_31 && ld->attr.rmode == GOFF_RMODE_NONE);
    ld = find_esd(g, GOFF_LD, "ENT2");
    CHECK(ld && ld->attr.amode == GOFF_AMODE_31 && ld->attr.rmode == GOFF_RMODE_31);
    ld = find_esd(g, GOFF_LD, "ENT3");
    CHECK(ld && ld->attr.amode == GOFF_AMODE_NONE && ld->attr.rmode == GOFF_RMODE_NONE);
    /* ER: no mode, no scope; a local A-con is a label relocation */
    er = find_esd(g, GOFF_ER, "EXTA");
    CHECK(er && er->attr.amode == GOFF_AMODE_NONE && er->attr.scope == GOFF_SCOPE_UNSPEC);
    CHECK(g->relocs.n == 2 && g->relocs.v[0].referent == GOFF_REFERENT_LABEL);
}

static uint64_t be(const uint8_t *p, int n) {
    uint64_t v = 0;
    int i;
    for (i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

static void test_elf_relimm(void) {
    /* ELF: the RELA addend of a relative immediate to an external symbol is
     * 2*F + 2 (F = the halfword offset in the instruction) */
    Reporter *rep;
    Module *m = assemble("T CSECT\n EXTRN EXTA\n LARL 1,EXTA+4\n BRASL 14,EXTA\n END\n", &rep);
    char err[256];
    size_t n, i, k, shoff, shnum;
    uint8_t *o;
    int64_t add[4];
    int na = 0;
    CHECK(m != NULL);
    if (!m) return;
    o = module_emit_elf(m, &n, err, sizeof err);
    CHECK_MSG(o != NULL, "elf: %s", err);
    if (!o) return;
    shoff = (size_t)be(o + 40, 8);
    shnum = (size_t)be(o + 60, 2);
    for (i = 1; i < shnum; i++) {
        const uint8_t *h = o + shoff + i * 64;
        if (be(h + 4, 4) == 4) { /* SHT_RELA */
            const uint8_t *r = o + be(h + 24, 8);
            size_t cnt = (size_t)(be(h + 32, 8) / 24);
            for (k = 0; k < cnt && na < 4; k++) add[na++] = (int64_t)be(r + k * 24 + 16, 8);
        }
    }
    CHECK_MSG(na == 2, "%d relocations", na);
    if (na == 2) CHECK_MSG(add[0] == 6 && add[1] == 2, "addends %lld %lld", (long long)add[0], (long long)add[1]);
}

static void any_error(const char *src) {
    char hex[64];
    Reporter *rep = run(src, hex, sizeof hex);
    CHECK_MSG(rep_error_count(rep) > 0, "%s: want an error", src);
}

/* DC/DS rules of HLASM (probes dc*.asm, dcl.asm, dsl.asm, pz.asm, blk*.asm) */
static void test_dc_rules(void) {
    char hex[64];
    Reporter *rep;
    /* lengths: 0 is an error, windows per type */
    any_error("DC CL0'A'");
    any_error("DC XL0'1'");
    any_error("DS CL0");
    any_error("DC XL257'1'");
    rep = run("DS XL65535", hex, sizeof hex);
    CHECK_MSG(rep_error_count(rep) == 0, "DS XL65535: unexpected error");
    any_error("DS XL65536");
    any_error("DC PL17'1'");
    any_error("DC FL9'1'");
    any_error("DC AL5(1)");
    any_error("DC VL8(EXTA)");
    any_error("DC EDL5'1'");
    any_error("DC SYL2(1)");
    any_error("DC C''");
    any_error("DC XL2''");
    ok("DC CL2''", "4040");
    /* values: out of range is an error, U prefix and absolute A truncate */
    any_error("DC F'4294967295'");
    ok("DC F'U4294967295'", "FFFFFFFF");
    any_error("DC H'65535'");
    ok("DC H'U65535'", "FFFF");
    ok("DC AL1(256)", "00");
    ok("DC Y(-32768)", "8000");
    ok("DC Y(65535)", "FFFF");
    any_error("DC Y(65536)");
    any_error("DC Y(-32769)");
    /* a 2-byte relocatable address constant is a warning */
    rep = run("DC AL2(EXTA)", hex, sizeof hex);
    CHECK_MSG(rep_error_count(rep) == 0 && has_code(rep, "ZA2086"), "AL2(EXTA): want warning ZA2086");
    rep = run("DC AL3(EXTA)", hex, sizeof hex);
    CHECK_MSG(rep_error_count(rep) == 0 && !has_code(rep, "ZA2086"), "AL3(EXTA): no warning");
    any_error("DC AL1(EXTA)");
    /* modifier order */
    any_error("DC FE2S1'1'");
    /* decimal: digit limits, silent left truncation */
    ok("DC PL2'12345'", "345C");
    ok("DC ZL2'12345'", "F4C5");
    ok("DC P'1234567890123456789012345678901'", "1234567890123456789012345678901C");
    any_error("DC P'12345678901234567890123456789012'");
    any_error("DC PL16'12345678901234567890123456789012'");
    any_error("DC Z'12345678901234567'");
    /* blanks inside numeric nominals are ignored */
    ok("DC X'12 34'", "1234");
    ok("DC B'1 0'", "02");
    ok("DC F'1 2'", "0000000C");
    ok("DC P'1 2'", "012C");
    ok("DC X'1 2,3 4'", "1234");
    /* floating point lengths */
    any_error("DC EL1'1'");
    ok("DC EL3'1'", "411000");
}

int main(void) {
    test_pairs();
    test_lengths();
    test_immediates();
    test_terms();
    test_relimm_extrn();
    test_goff_flags();
    test_elf_relimm();
    test_dc_rules();
    TEST_MAIN_END();
}
