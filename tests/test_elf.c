/* test_elf.c - ELF64 output (-elf) and the ASCII character set. */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "../src/dbg.h"
#include "../src/ebcdic.h"
#include "testlib.h"

typedef struct {
    const uint8_t *b;
    size_t n;
    size_t shoff, shnum, shstr;
} Elf;

static uint64_t be(const uint8_t *p, int n) {
    uint64_t v = 0;
    int i;
    for (i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

/* section header fields */
static uint64_t sh(const Elf *e, size_t i, int field) {
    static const int off[] = {0, 4, 8, 16, 24, 32, 40, 44, 48, 56}; /* name type flags addr offset size link info align entsize */
    static const int len[] = {4, 4, 8, 8, 8, 8, 4, 4, 8, 8};
    return be(e->b + e->shoff + i * 64 + (size_t)off[field], len[field]);
}
enum { F_NAME, F_TYPE, F_FLAGS, F_ADDR, F_OFF, F_SIZE, F_LINK, F_INFO, F_ALIGN, F_ENT };

static const char *sh_name(const Elf *e, size_t i) {
    return (const char *)e->b + sh(e, e->shstr, F_OFF) + sh(e, i, F_NAME);
}

static long find_sec(const Elf *e, const char *name) {
    size_t i;
    for (i = 1; i < e->shnum; i++)
        if (!strcmp(sh_name(e, i), name)) return (long)i;
    return -1;
}

static const uint8_t *sec_data(const Elf *e, size_t i) { return e->b + sh(e, i, F_OFF); }

static int parse(Elf *e, const uint8_t *b, size_t n) {
    memset(e, 0, sizeof *e);
    e->b = b;
    e->n = n;
    if (n < 64 || memcmp(b, "\x7f" "ELF", 4) != 0) return 0;
    e->shoff = (size_t)be(b + 40, 8);
    e->shnum = (size_t)be(b + 60, 2);
    e->shstr = (size_t)be(b + 62, 2);
    return e->shoff + e->shnum * 64 <= n;
}

/* the symbol called name: *shndx, *value, *info; 0 when absent */
static int find_sym(const Elf *e, const char *name, unsigned *shndx, uint64_t *value, unsigned *info) {
    long st = find_sec(e, ".symtab");
    size_t i, nsym;
    const char *strs;
    if (st < 0) return 0;
    strs = (const char *)sec_data(e, (size_t)sh(e, (size_t)st, F_LINK));
    nsym = (size_t)(sh(e, (size_t)st, F_SIZE) / 24);
    for (i = 0; i < nsym; i++) {
        const uint8_t *s = sec_data(e, (size_t)st) + i * 24;
        if (!strcmp(strs + be(s, 4), name) && name[0]) {
            *info = s[4];
            *shndx = (unsigned)be(s + 6, 2);
            *value = be(s + 8, 8);
            return 1;
        }
    }
    return 0;
}

/* name of the symbol with index idx */
static const char *sym_name(const Elf *e, size_t idx) {
    long st = find_sec(e, ".symtab");
    const char *strs = (const char *)sec_data(e, (size_t)sh(e, (size_t)st, F_LINK));
    return strs + be(sec_data(e, (size_t)st) + idx * 24, 4);
}

static uint8_t *build(const char *src, int ascii, size_t *n, char *err) {
    Reporter *rep = rep_new(0);
    Module *m;
    uint8_t *obj;
    za_ascii = ascii;
    m = asm_assemble_text("t.za", src, strlen(src), rep);
    if (!m) {
        rep_print(rep, stdout, 0);
        err[0] = 0;
        return NULL;
    }
    obj = module_emit_elf(m, n, err, 256);
    return obj;
}

static void test_basic(void) {
    const char *src = "* a program\n"
                      "T        CSECT\n"
                      "         ENTRY T2\n"
                      "         LARL  %r1,MSG\n"
                      "         BRASL %r14,EXT\n"
                      "         BRAS  %r14,T2\n"
                      "T2       BR    %r14\n"
                      "MSG      DC    C'Hi',X'0A'\n"
                      "PTR      DC    AD(MSG+1)\n"
                      "WPTR     DC    AD(WK)\n"
                      "         EXTRN EXT\n"
                      "         WXTRN WK\n"
                      "         END\n";
    char err[256];
    size_t n;
    uint8_t *o = build(src, 1, &n, err);
    Elf e;
    unsigned shndx, info;
    uint64_t val;
    long t, rt;
    CHECK_MSG(o != NULL, "emit: %s", err);
    if (!o) return;
    CHECK(parse(&e, o, n));
    CHECK(o[4] == 2 && o[5] == 2);              /* ELFCLASS64, big endian */
    CHECK(be(o + 16, 2) == 1 && be(o + 18, 2) == 22); /* ET_REL, EM_S390 */
    t = find_sec(&e, ".text.T");
    CHECK(t > 0);
    CHECK(find_sec(&e, ".note.GNU-stack") > 0);
    if (t <= 0) return;
    CHECK(sh(&e, (size_t)t, F_TYPE) == 1 && sh(&e, (size_t)t, F_FLAGS) == 6); /* PROGBITS, AX */
    CHECK(sh(&e, (size_t)t, F_ALIGN) == 8);
    /* the characters are ASCII, the addends are cleared */
    {
        const uint8_t *d = sec_data(&e, (size_t)t);
        size_t sz = (size_t)sh(&e, (size_t)t, F_SIZE);
        CHECK(sz >= 32);
        CHECK(memcmp(d + 18, "Hi\n", 3) == 0);
        CHECK(d[24] == 0 && d[31] == 0); /* the addend of AD(MSG+1) is in the RELA entry */
    }
    rt = find_sec(&e, ".rela.text.T");
    CHECK(rt > 0);
    if (rt > 0) {
        const uint8_t *r = sec_data(&e, (size_t)rt);
        size_t cnt = (size_t)(sh(&e, (size_t)rt, F_SIZE) / 24), i;
        int seen_plt = 0, seen_64 = 0, seen_weak = 0;
        CHECK(sh(&e, (size_t)rt, F_INFO) == (uint64_t)t);
        for (i = 0; i < cnt; i++) {
            uint32_t type = (uint32_t)(be(r + i * 24 + 8, 8) & 0xFFFFFFFFu);
            size_t sidx = (size_t)(be(r + i * 24 + 8, 8) >> 32);
            int64_t add = (int64_t)be(r + i * 24 + 16, 8);
            if (type == 20 && !strcmp(sym_name(&e, sidx), "EXT")) seen_plt = (add == 2);
            if (type == 22 && !strcmp(sym_name(&e, sidx), "WK")) seen_weak = 1;
            if (type == 22 && add == 19) seen_64 = 1;
        }
        CHECK(seen_plt && seen_weak);
        CHECK_MSG(seen_64, "AD(MSG+1) is section + label offset + 1");
        (void)seen_64;
    }
    /* symbols */
    CHECK(find_sym(&e, "T", &shndx, &val, &info) && info == ((1 << 4) | 2) && shndx == (unsigned)t && val == 0);
    CHECK(find_sym(&e, "T2", &shndx, &val, &info) && (info >> 4) == 1 && val == 16);
    CHECK(find_sym(&e, "MSG", &shndx, &val, &info) && (info >> 4) == 0);
    CHECK(find_sym(&e, "EXT", &shndx, &val, &info) && shndx == 0 && (info >> 4) == 1);
    CHECK(find_sym(&e, "WK", &shndx, &val, &info) && shndx == 0 && (info >> 4) == 2);
    /* the symbol table has its locals first and sh_info = first global */
    {
        long st = find_sec(&e, ".symtab");
        size_t i, nsym = (size_t)(sh(&e, (size_t)st, F_SIZE) / 24), first = (size_t)sh(&e, (size_t)st, F_INFO);
        int ok = first > 0 && first <= nsym;
        for (i = 0; ok && i < nsym; i++) {
            int bind = sec_data(&e, (size_t)st)[i * 24 + 4] >> 4;
            if (i < first && bind != 0) ok = 0;
            if (i >= first && bind == 0) ok = 0;
        }
        CHECK(ok);
    }
    free(o);
}

static void test_classes(void) {
    const char *src = "M        CSECT\n"
                      "         LARL  %r1,V1\n"
                      "         BR    %r14\n"
                      "RO       CATTR ALIGN(4),READONLY\n"
                      "R1       DC    C'abc'\n"
                      "DATA     CATTR ALIGN(3)\n"
                      "V1       DC    AD(R1)\n"
                      "BSS      CATTR ALIGN(3),NOLOAD\n"
                      "B1       DS    CL100\n"
                      "CM       COM\n"
                      "         DS    CL64\n"
                      "         END\n";
    char err[256];
    size_t n;
    uint8_t *o = build(src, 1, &n, err);
    Elf e;
    long ro, da, bs;
    unsigned shndx, info;
    uint64_t val;
    CHECK_MSG(o != NULL, "emit: %s", err);
    if (!o) return;
    CHECK(parse(&e, o, n));
    ro = find_sec(&e, ".rodata.M.RO");
    da = find_sec(&e, ".data.M.DATA");
    bs = find_sec(&e, ".bss.M.BSS");
    CHECK(ro > 0 && da > 0 && bs > 0);
    if (ro > 0) CHECK(sh(&e, (size_t)ro, F_FLAGS) == 2 && sh(&e, (size_t)ro, F_ALIGN) == 16);
    if (da > 0) CHECK(sh(&e, (size_t)da, F_FLAGS) == 3);
    if (bs > 0) CHECK(sh(&e, (size_t)bs, F_TYPE) == 8 && sh(&e, (size_t)bs, F_SIZE) == 100);
    CHECK(find_sym(&e, "CM", &shndx, &val, &info) && shndx == 0xfff2 && val == 8);
    CHECK(find_sym(&e, "B1", &shndx, &val, &info) && shndx == (unsigned)bs);
    CHECK(find_sec(&e, ".rela.data.M.DATA") > 0 && find_sec(&e, ".rela.text.M") > 0);
    free(o);
}

static void test_rejected(void) {
    char err[256];
    size_t n;
    uint8_t *o;
    o = build("T CSECT\n DC AD(A-B)\n EXTRN A,B\n END\n", 1, &n, err);
    CHECK(o == NULL && strstr(err, "subtracted") != NULL);
    o = build("T CSECT\n DC AD(A+B)\n EXTRN A,B\n END\n", 1, &n, err);
    CHECK(o == NULL && strstr(err, "several") != NULL);
    o = build("T CSECT\n DC AL3(A)\n EXTRN A\n END\n", 1, &n, err);
    CHECK(o == NULL);
    o = build("T CSECT\nW DXD F\n DC Q(W)\n END\n", 1, &n, err);
    CHECK(o == NULL && strstr(err, "Q-type") != NULL);
    o = build("D DSECT\n DS F\n END\n", 1, &n, err);
    CHECK(o == NULL && strstr(err, "no control section") != NULL);
    za_ascii = 0;
}

static void test_charset(void) {
    Reporter *rep = rep_new(0);
    const char *src = "T CSECT\n DC C'AB',CL4'x'\n LA %r1,C'A'\n END\n";
    Module *m;
    uint8_t *d;
    za_ascii = 1;
    m = asm_assemble_text("t.za", src, strlen(src), rep);
    CHECK(m != NULL);
    if (m) {
        d = m->sections.v[0]->classes.v[0]->data;
        CHECK(d[0] == 'A' && d[1] == 'B' && d[2] == 'x' && d[3] == ' ' && d[5] == ' ');
        CHECK(d[6] == 0x41 && d[7] == 0x10 && d[8] == 0x00 && d[9] == 0x41); /* LA 1,65 */
    }
    za_ascii = 0;
    rep = rep_new(0);
    m = asm_assemble_text("t.za", src, strlen(src), rep);
    CHECK(m != NULL);
    if (m) {
        d = m->sections.v[0]->classes.v[0]->data;
        CHECK(d[0] == 0xC1 && d[1] == 0xC2 && d[2] == 0xA7 && d[3] == 0x40);
    }
}

/* -elf -g: the DWARF sections, their relocations as RELA entries against the .text sections */
static void test_dwarf(void) {
    const char *src = "A        CSECT\n"
                      "         BR    %r14\n"
                      "         BR    %r14\n"
                      "B        CSECT\n"
                      "         DC    F'1'\n"
                      "         END\n";
    Reporter *rep = rep_new(0);
    Module *m;
    DbgOptions o;
    DbgElfSec *dbg = NULL;
    size_t ndbg = 0, n = 0, k, i;
    char err[256];
    uint8_t *obj;
    Elf e;
    long st, ri, rr, ta, tb;
    int ok;
    za_ascii = 1;
    m = asm_assemble_text("t.za", src, strlen(src), rep);
    CHECK_MSG(m != NULL, "assembly failed");
    if (!m) {
        za_ascii = 0;
        return;
    }
    memset(&o, 0, sizeof o);
    o.cwd = "/tmp";
    o.main_file = "t.za";
    o.producer = "zosasm test";
    o.dbg_path = "";
    ok = dbg_build_elf(m, &o, &dbg, &ndbg, err, sizeof err);
    CHECK_MSG(ok, "dwarf: %s", err);
    if (!ok) {
        za_ascii = 0;
        return;
    }
    obj = module_emit_elf_dbg(m, dbg, ndbg, &n, err, sizeof err);
    CHECK_MSG(obj != NULL, "emit: %s", err);
    za_ascii = 0;
    if (!obj) return;
    CHECK(parse(&e, obj, n));
    CHECK(find_sec(&e, ".debug_info") > 0 && find_sec(&e, ".debug_abbrev") > 0);
    CHECK(find_sec(&e, ".debug_line") > 0 && find_sec(&e, ".debug_aranges") > 0);
    CHECK(find_sec(&e, ".rela.debug_info") > 0);
    ri = find_sec(&e, ".debug_ranges");
    rr = find_sec(&e, ".rela.debug_ranges");
    ta = find_sec(&e, ".text.A");
    tb = find_sec(&e, ".text.B");
    CHECK(ri > 0 && rr > 0 && ta > 0 && tb > 0);
    if (ri <= 0 || rr <= 0 || ta <= 0 || tb <= 0) {
        free(obj);
        return;
    }
    /* two entries (begin, end) per CSECT and the zero terminator; the addends are in RELA */
    CHECK(sh(&e, (size_t)ri, F_SIZE) == 48);
    for (i = 0; i < 48; i++) CHECK(sec_data(&e, (size_t)ri)[i] == 0);
    CHECK(sh(&e, (size_t)rr, F_SIZE) == 4 * 24);
    st = find_sec(&e, ".symtab");
    {
        static const uint64_t want[] = {0, 4, 0, 4};
        const uint8_t *r = sec_data(&e, (size_t)rr);
        for (k = 0; k < 4; k++) {
            size_t sidx = (size_t)(be(r + k * 24 + 8, 8) >> 32);
            uint32_t type = (uint32_t)(be(r + k * 24 + 8, 8) & 0xFFFFFFFFu);
            uint64_t shndx = be(sec_data(&e, (size_t)st) + sidx * 24 + 6, 2);
            CHECK(type == 22);
            CHECK(be(r + k * 24 + 16, 8) == want[k]);
            CHECK(shndx == (uint64_t)(k < 2 ? ta : tb));
        }
    }
    free(obj);
    for (i = 0; i < ndbg; i++) free(dbg[i].rels);
    free(dbg);
}

/* CA'..' and CE'..' name the character set, whatever the default is */
static void test_ca_ce(void) {
    const char *src = "T CSECT\n DC CA'ab',CE'ab',CAL4'a',CEL4'a'\n LA %r1,CA'a'\n LA %r2,CE'a'\n"
                      " MVI 0(%r1),CA' '\n MVI 0(%r1),CE' '\n END\n";
    int mode;
    for (mode = 0; mode < 2; mode++) {
        Reporter *rep = rep_new(0);
        Module *m;
        za_ascii = mode;
        m = asm_assemble_text("t.za", src, strlen(src), rep);
        CHECK_MSG(m != NULL, "assembly failed (ascii=%d)", mode);
        if (m) {
            const uint8_t *d = m->sections.v[0]->classes.v[0]->data;
            CHECK(d[0] == 0x61 && d[1] == 0x62 && d[2] == 0x81 && d[3] == 0x82);
            CHECK(d[4] == 0x61 && d[5] == 0x20 && d[6] == 0x20 && d[7] == 0x20);
            CHECK(d[8] == 0x81 && d[9] == 0x40 && d[10] == 0x40 && d[11] == 0x40);
            CHECK(d[12] == 0x41 && d[15] == 0x61 && d[16] == 0x41 && d[19] == 0x81);
            CHECK(d[21] == 0x20 && d[25] == 0x40);
        }
    }
    za_ascii = 0;
}

int main(void) {
    test_ca_ce();
    test_basic();
    test_classes();
    test_rejected();
    test_charset();
    test_dwarf();
    TEST_MAIN_END();
}
