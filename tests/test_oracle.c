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

/* the object conventions found with tools/oracle_obj.sh (probes cat1, cn1, com1, am1,
 * lc1, st5, st6, st8, st9) */
static void test_object_rules(void) {
    GoffModule *g;
    const GoffESD *e, *prv, *pr;
    char *hex = NULL;
    Reporter *rep;
    Module *m;
    /* CATTR classes: ALIGN(3), RMODE(24), NONREUS, AMODE none; PART = merge, no PR fill */
    g = emit_src("S CSECT\nS AMODE 31\nS RMODE 31\n DC F'1'\n"
                 "C1 CATTR\n DC F'2'\n"
                 "C2 CATTR PART(PP),FILL(0),REMOVABLE,NOTEXECUTABLE\n DC F'3'\n END\n");
    if (g) {
        e = find_esd(g, GOFF_ED, "C1");
        CHECK(e && e->attr.amode == 0 && e->attr.rmode == GOFF_RMODE_24 && e->attr.alignment == 3 &&
              e->attr.tasking == GOFF_TASK_NONREUS);
        e = find_esd(g, GOFF_ED, "C2");
        CHECK(e && e->has_fill && e->removable && e->attr.bind_algorithm == GOFF_BIND_MERGE);
        pr = find_esd(g, GOFF_PR, "PP");
        CHECK(pr && !pr->has_fill && pr->attr.amode == 0 && pr->attr.rmode == GOFF_RMODE_24 &&
              pr->attr.bind_algorithm == GOFF_BIND_MERGE);
        e = find_esd(g, GOFF_ED, "B_TEXT");
        CHECK(e && e->attr.amode == GOFF_AMODE_31 && e->attr.rmode == GOFF_RMODE_31);
    }
    /* one-sided AMODE and RMODE */
    g = emit_src("A CSECT\nA RMODE ANY\n DC F'1'\nB CSECT\nB RMODE 64\n DC F'1'\n"
                 "C CSECT\nC AMODE 64\n DC F'1'\nD CSECT\nD RMODE 24\n DC F'1'\n END\n");
    if (g) {
        size_t i, seen = 0;
        for (i = 0; i < g->esds.n; i++) {
            const GoffESD *x = g->esds.v[i];
            char *nm;
            if (x->type != GOFF_ED) continue;
            nm = ebcdic_decode(g->esds.v[x->parent - 1]->name, g->esds.v[x->parent - 1]->name_len);
            if (!strcmp(nm, "A")) { CHECK(x->attr.amode == GOFF_AMODE_31 && x->attr.rmode == GOFF_RMODE_31); seen++; }
            if (!strcmp(nm, "B")) { CHECK(x->attr.amode == GOFF_AMODE_64 && x->attr.rmode == GOFF_RMODE_64); seen++; }
            if (!strcmp(nm, "C")) { CHECK(x->attr.amode == GOFF_AMODE_64 && x->attr.rmode == GOFF_RMODE_31); seen++; }
            if (!strcmp(nm, "D")) { CHECK(x->attr.amode == 0 && x->attr.rmode == GOFF_RMODE_24); seen++; }
            free(nm);
        }
        CHECK(seen == 4);
    }
    /* COM: no common flag on the class, no text */
    g = emit_src("S CSECT\n DC A(CM)\nCM COM\n DS CL16\n END\n");
    if (g) {
        size_t i, txt = 0;
        e = find_esd(g, GOFF_SD, "CM");
        CHECK(e && e->attr.common);
        for (i = 0; i < g->esds.n; i++)
            if (g->esds.v[i]->type == GOFF_ED && !g->esds.v[i]->attr.common) txt++;
        CHECK(txt > 0);
        for (i = 0; i < g->esds.n; i++) CHECK(!(g->esds.v[i]->type == GOFF_ED && g->esds.v[i]->attr.common));
        CHECK(g->texts.n == 1);
    }
    /* pseudo registers: B_PRV is namespace 2, NOLOAD, length 0; the PR is weak, amode 0;
     * Q constants refer to the class */
    g = emit_src("S CSECT\n DC Q(DX)\n DC QY(DX)\nDX DXD F\n END\n");
    if (g) {
        prv = find_esd(g, GOFF_ED, "B_PRV");
        CHECK(prv && prv->name_space == GOFF_NS_PSEUDO && prv->length == 0 && prv->attr.loading == GOFF_NOLOAD &&
              prv->attr.amode == 0 && prv->attr.bind_algorithm == GOFF_BIND_MERGE);
        pr = find_esd(g, GOFF_PR, "DX");
        CHECK(pr && pr->attr.amode == 0 && pr->attr.bind_strength == GOFF_STRENGTH_WEAK && pr->attr.executable == 0);
        CHECK(g->relocs.n == 2 && g->relocs.v[0].referent == GOFF_REFERENT_CLASS &&
              g->relocs.v[1].referent == GOFF_REFERENT_CLASS);
    }
    /* CNOP pads with 0700 and 47000700 (probe cn1) */
    m = assemble("S CSECT\n DC X'11'\n DS 0H\n CNOP 0,4\n DC X'22'\n DS 0H\n CNOP 0,8\n DC X'33'\n END\n", &rep);
    CHECK(m && rep_error_count(rep) == 0);
    if (m) {
        Class *k = m->sections.v[0]->classes.v[0];
        int64_t i;
        hex = xmalloc((size_t)k->len * 2 + 1);
        for (i = 0; i < k->len; i++) sprintf(hex + 2 * i, "%02X", k->data[i]);
        CHECK_MSG(!strncmp(hex, "110007002200070033", 18), "%s", hex);
    }
    /* LOCTR pieces start at the next byte, items align to the final address, CSECT
     * resumes the last location counter of B_TEXT (probes lc1, st5) */
    m = assemble("Q1 CSECT\n DC X'01'\nL1 LOCTR\n DC X'02'\nA1 DC F'3'\n DC X'04'\nA2 DC D'5'\n DC X'09'\n"
                 "L2 LOCTR\n DC X'06'\nA3 DC H'7'\n DC X'0A'\nL3 LOCTR\n DC X'0B'\nA4 DC X'0C'\n"
                 "Q2 CSECT\n DC X'08'\nQ1 CSECT\n DC X'0D'\n END\n", &rep);
    CHECK(m && rep_error_count(rep) == 0);
    if (m) {
        Class *k = m->sections.v[0]->classes.v[0];
        int64_t i;
        hex = xmalloc((size_t)k->len * 2 + 1);
        for (i = 0; i < k->len; i++) sprintf(hex + 2 * i, "%02X", k->data[i]);
        CHECK_MSG(!strncmp(hex, "01020000000000030400000000000000415000000000000009060007" "0A0B0C0D", 62), "%s", hex);
    }
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
    test_object_rules();
    TEST_MAIN_END();
}
