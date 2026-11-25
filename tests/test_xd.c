/* test_xd.c - DXD, CXD, Q, J and R constants, XATTR REFERENCE(...,INDIRECT)
 * (DESIGN.md 4.4).  The RLD encodings are not verified against an IBM
 * object; these tests pin down what zosasm writes. */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "../src/ebcdic.h"
#include "../src/goff.h"
#include "testlib.h"

static GoffModule *emit_src(const char *src) {
    Reporter *rep = rep_new(0);
    Module *m = asm_assemble_text("t.za", src, strlen(src), rep);
    char err[256];
    size_t n;
    uint8_t *obj;
    const char *perr = NULL;
    GoffModule *g;
    if (!m) {
        rep_print(rep, stdout, 0);
        CHECK_MSG(0, "assembly failed");
        return NULL;
    }
    obj = module_emit(m, &n, err, sizeof err);
    CHECK_MSG(obj != NULL, "emit: %s", err);
    if (!obj) return NULL;
    g = goff_parse(obj, n, &perr);
    CHECK_MSG(g != NULL, "parse: %s", perr ? perr : "?");
    return g;
}

static const GoffESD *find_esd(const GoffModule *g, GoffESDType type, const char *name, uint32_t *id) {
    size_t i;
    for (i = 0; i < g->esds.n; i++) {
        const GoffESD *e = g->esds.v[i];
        char *nm;
        int ok;
        if (e->type != type) continue;
        nm = ebcdic_decode(e->name, e->name_len);
        ok = !strcmp(nm, name);
        free(nm);
        if (ok) {
            if (id) *id = (uint32_t)i + 1;
            return e;
        }
    }
    return NULL;
}

static const GoffReloc *reloc_at(const GoffModule *g, uint32_t off) {
    size_t i;
    for (i = 0; i < g->relocs.n; i++)
        if (g->relocs.v[i].offset == off) return &g->relocs.v[i];
    return NULL;
}

static void test_structure(void) {
    GoffModule *g = emit_src("T CSECT\n"
                             " DC Q(WORK),J(WORK),J(DSA),Q(DSA)\n"
                             "CX CXD\n"
                             "WORK DXD CL100,F\n"
                             "DSA DSECT\n"
                             " DS CL20,D\n");
    uint32_t prv_id = 0, work_id = 0, dsa_id = 0;
    const GoffESD *prv, *work, *dsa;
    const GoffReloc *r;
    if (!g) return;
    prv = find_esd(g, GOFF_ED, "B_PRV", &prv_id);
    work = find_esd(g, GOFF_PR, "WORK", &work_id);
    dsa = find_esd(g, GOFF_PR, "DSA", &dsa_id);
    CHECK(prv && work && dsa);
    if (!prv || !work || !dsa) return;
    CHECK(prv->attr.bind_algorithm == GOFF_BIND_MERGE);
    CHECK(work->parent == prv_id && dsa->parent == prv_id);
    CHECK(work->name_space == GOFF_NS_PSEUDO);
    CHECK(work->length == 104 && work->attr.alignment == GOFF_ALIGN_WORD);
    CHECK(dsa->length == 32 && dsa->attr.alignment == GOFF_ALIGN_DBL);
    CHECK(g->relocs.n == 5);
    r = reloc_at(g, 0);
    CHECK(r && r->rid == work_id && r->reference == GOFF_REF_OFFSET && !r->store && r->length == 4);
    r = reloc_at(g, 4);
    CHECK(r && r->rid == work_id && r->reference == GOFF_REF_LENGTH &&
          r->referent == GOFF_REFERENT_PART && r->store);
    r = reloc_at(g, 8);
    CHECK(r && r->rid == dsa_id && r->reference == GOFF_REF_LENGTH);
    r = reloc_at(g, 12);
    CHECK(r && r->rid == dsa_id && r->reference == GOFF_REF_OFFSET);
    r = reloc_at(g, 16);
    CHECK(r && r->rid == prv_id && r->reference == GOFF_REF_LENGTH &&
          r->referent == GOFF_REFERENT_CLASS && r->store && r->length == 4);
}

static void test_r_and_indirect(void) {
    GoffModule *g = emit_src("T CSECT\n"
                             " ENTRY LAB\n"
                             "LAB DS 0H\n"
                             " DC R(LAB),R(EXT1),RD(T)\n"
                             " EXTRN EXT1\n"
                             "EXT1 XATTR REFERENCE(DATA,INDIRECT)\n"
                             "LAB XATTR REFERENCE(INDIRECT)\n");
    uint32_t lab_id = 0, ext_id = 0, t_id = 0, labr_id = 0;
    const GoffESD *lab, *ext;
    const GoffReloc *r;
    if (!g) return;
    lab = find_esd(g, GOFF_LD, "LAB", &lab_id);
    ext = find_esd(g, GOFF_ER, "EXT1", &ext_id);
    /* an R-type constant naming a local entry refers to an ER of the same name (IBM HLASM, probe st6) */
    CHECK(find_esd(g, GOFF_ER, "LAB", &labr_id) != NULL);
    CHECK(find_esd(g, GOFF_ER, "T", &t_id) != NULL);
    CHECK(lab && ext);
    if (!lab || !ext) return;
    CHECK(ext->attr.indirect && ext->attr.executable == GOFF_EXEC_DATA);
    CHECK(lab->attr.indirect);
    r = reloc_at(g, 0);
    CHECK(r && r->rid == labr_id && lab_id && r->reference == GOFF_REF_RTYPE && r->store && r->length == 4);
    r = reloc_at(g, 4);
    CHECK(r && r->rid == ext_id && r->reference == GOFF_REF_RTYPE);
    r = reloc_at(g, 8);
    CHECK(r && r->rid == t_id && r->length == 8);
    CHECK(!find_esd(g, GOFF_ED, "B_PRV", NULL));
}

static void test_cxd_only(void) {
    GoffModule *g = emit_src("T CSECT\n CXD\n END\n");
    if (!g) return;
    CHECK(find_esd(g, GOFF_ED, "B_PRV", NULL) != NULL);
    CHECK(g->relocs.n == 1);
}

/* the codes of the diagnostics of a source, space separated */
static void expect_codes(const char *name, const char *src, const char *want) {
    Reporter *rep = rep_new(0);
    Buf b = {0};
    size_t i, n;
    const Diag **d;
    asm_assemble_text("t.za", src, strlen(src), rep);
    d = rep_sorted(rep, &n);
    for (i = 0; i < n; i++) {
        if (i) buf_putc(&b, ' ');
        buf_puts(&b, d[i]->code);
    }
    CHECK_MSG(!strcmp(buf_cstr(&b), want), "%s: codes \"%s\", want \"%s\"", name, buf_cstr(&b), want);
}

static void test_errors(void) {
    expect_codes("CXD is no constant", "T CSECT\nW DXD F\n DC Q(W),J(W),R(T),CXD\n", "ZA2014");
    expect_codes("Q length", "T CSECT\nW DXD F\n DC QL5(W)\n", "ZA2016");
    expect_codes("QD length", "T CSECT\nW DXD F\n DC QDL8(W),QL4(W)\n", "");
    expect_codes("J length", "T CSECT\nW DXD F\n DC JL1(W)\n", "ZA2029");
    expect_codes("R length", "T CSECT\n DC RL2(T)\n", "ZA2016");
    expect_codes("Q unknown", "T CSECT\n DC Q(NOPE)\n", "ZA2060");
    expect_codes("Q label", "T CSECT\nL1 DS 0H\n DC Q(L1)\n", "ZA2060");
    expect_codes("R no entry", "T CSECT\nL1 DS 0H\n DC R(L1)\n", "ZA2060");
    expect_codes("R dsect", "T CSECT\n DC R(D)\nD DSECT\n DS F\n", "ZA2060");
    expect_codes("DXD no name", "T CSECT\n DXD F\n", "ZA2061");
    expect_codes("DXD in dsect", "D DSECT\nW DXD F\n", "ZA2063");
    expect_codes("DXD as address", "T CSECT\nW DXD F\n DC A(W)\n", "ZA2062");
    expect_codes("DXD twice", "T CSECT\nW DXD F\nW DXD F\n", "ZA1020");
    expect_codes("REFERENCE twice",
                 "T CSECT\n EXTRN E\nE XATTR REFERENCE(CODE,DATA)\n", "ZA3037");
    expect_codes("REFERENCE twice 2",
                 "T CSECT\n EXTRN E\nE XATTR REFERENCE(DIRECT,INDIRECT)\n", "ZA3037");
    expect_codes("REFERENCE bad", "T CSECT\n EXTRN E\nE XATTR REFERENCE(NEAR)\n", "ZA3037");
    expect_codes("REFERENCE three", "T CSECT\n EXTRN E\nE XATTR REFERENCE(CODE,DIRECT,X)\n",
                 "ZA3031");
    expect_codes("REFERENCE direct", "T CSECT\n EXTRN E\nE XATTR REFERENCE(DIRECT)\n", "");
}

static void test_qy(void) {
    GoffModule *g = emit_src("T CSECT\n"
                             " DC QY(WORK),H'7'\n"
                             " DC AL1(L'Q1)\n"
                             "Q1 DC QY(WORK)\n"
                             "WORK DXD F\n");
    uint32_t work_id = 0;
    const GoffReloc *r;
    if (!g) return;
    CHECK(find_esd(g, GOFF_PR, "WORK", &work_id) != NULL);
    CHECK(g->relocs.n == 2);
    r = reloc_at(g, 0);
    CHECK(r && r->rid == work_id && r->reference == GOFF_REF_LONGDISP && !r->store &&
          r->length == 2 && r->bit_length == 4 && r->bit_offset == 4);
    /* QY is 3 bytes and, like SY, is not aligned by HLASM */
    r = reloc_at(g, 7);
    CHECK(r && r->reference == GOFF_REF_LONGDISP);
    expect_codes("QY length", "T CSECT\n DC QYL4(W)\nW DXD F\n", "ZA2016");
}

/* XATTR PSECT: the LD of an entry points its ADA at the PSECT class ED
 * (HLASM R6.0 probe pps.asm); the ER made for R(entry) does not. */
static void test_psect(void) {
    GoffModule *g = emit_src("Q CSECT\n"
                             "CODE CATTR ALIGN(2)\n"
                             "ENT LR 1,2\n"
                             " ENTRY ENT\n"
                             "DATA CATTR ALIGN(3)\n"
                             "PSTART DS 0D\n"
                             "PS2 DS CL8\n"
                             "ENT XATTR LINKAGE(XPLINK),PSECT(PSTART)\n"
                             " DC R(ENT)\n");
    uint32_t data_id = 0, ld_id = 0, er_id = 0;
    const GoffESD *data, *ld, *er;
    if (!g) return;
    data = find_esd(g, GOFF_ED, "DATA", &data_id);
    ld = find_esd(g, GOFF_LD, "ENT", &ld_id);
    er = find_esd(g, GOFF_ER, "ENT", &er_id);
    CHECK(data && ld && er);
    if (data && ld && er) {
        CHECK(ld->ada_id == data_id);
        CHECK(er->ada_id == 0);
    }
}

int main(void) {
    test_psect();
    test_qy();
    test_structure();
    test_r_and_indirect();
    test_cxd_only();
    test_errors();
    TEST_MAIN_END();
}
