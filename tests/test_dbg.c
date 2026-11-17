/* test_dbg.c - debug information of -g: MD5, host paths, the .dbg ELF/DWARF
 * and the GOFF link module appended to the object (DESIGN.md 21). */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "../src/dbg.h"
#include "../src/ebcdic.h"
#include "../src/goff.h"
#include "../src/md5.h"
#include "testlib.h"

static void md5_hex(const void *p, size_t n, char out[33]) {
    Md5 c;
    uint8_t d[16];
    int i;
    md5_init(&c);
    md5_update(&c, p, n);
    md5_final(&c, d);
    for (i = 0; i < 16; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void test_md5(void) {
    char h[33];
    char *big;
    Md5 c;
    uint8_t d[16];
    int i;
    md5_hex("", 0, h);
    CHECK_STR(h, "d41d8cd98f00b204e9800998ecf8427e");
    md5_hex("abc", 3, h);
    CHECK_STR(h, "900150983cd24fb0d6963f7d28e17f72");
    md5_hex("message digest", 14, h);
    CHECK_STR(h, "f96b697d7cb7938d525a2f31aaf161d0");
    md5_hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890", 80, h);
    CHECK_STR(h, "57edf4a22be3c955ac49da2e2107b67a");
    /* a million bytes fed in odd pieces */
    big = malloc(1000000);
    memset(big, 'a', 1000000);
    md5_init(&c);
    for (i = 0; i < 1000000; i += 7777) md5_update(&c, big + i, (size_t)(1000000 - i < 7777 ? 1000000 - i : 7777));
    md5_final(&c, d);
    for (i = 0; i < 16; i++) sprintf(h + 2 * i, "%02x", d[i]);
    CHECK_STR(h, "7707d6ae4e027c70eea2a935c2296f21");
    free(big);
}

static void test_paths(void) {
    DbgOptions o;
    char *p;
    memset(&o, 0, sizeof o);
    o.cwd = "/home/me/proj";
    p = dbg_host_path(&o, "a.asm");
    CHECK_STR(p, "/home/me/proj/a.asm");
    free(p);
    p = dbg_host_path(&o, "./sub/../b.asm");
    CHECK_STR(p, "/home/me/proj/b.asm");
    free(p);
    p = dbg_host_path(&o, "/abs//x/./y.asm");
    CHECK_STR(p, "/abs/x/y.asm");
    free(p);
    o.gdir = "/u/ccw/tmp";
    p = dbg_host_path(&o, "sub/a.asm");
    CHECK_STR(p, "/u/ccw/tmp/sub/a.asm");
    free(p);
    p = dbg_host_path(&o, "../other/a.asm"); /* outside the directory */
    CHECK_STR(p, "/home/me/other/a.asm");
    free(p);
    vec_push(&o.map_from, "/home/me/other");
    vec_push(&o.map_to, "/u/ccw/other");
    p = dbg_host_path(&o, "../other/a.asm");
    CHECK_STR(p, "/u/ccw/other/a.asm");
    free(p);
}

/* ---- the .dbg ELF ---- */

static uint64_t be(const uint8_t *p, int n) {
    uint64_t v = 0;
    int i;
    for (i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

typedef struct {
    const uint8_t *b;
    size_t n, shoff, shnum, shstr;
} Elf;

static int elf_parse(Elf *e, const uint8_t *b, size_t n) {
    memset(e, 0, sizeof *e);
    e->b = b;
    e->n = n;
    if (n < 52 || memcmp(b, "\x7f" "ELF", 4) != 0) return 0;
    e->shoff = (size_t)be(b + 0x20, 4);
    e->shnum = (size_t)be(b + 0x30, 2);
    e->shstr = (size_t)be(b + 0x32, 2);
    return e->shoff + e->shnum * 40 <= n;
}
static uint32_t shf(const Elf *e, size_t i, int off) { return (uint32_t)be(e->b + e->shoff + i * 40 + (size_t)off, 4); }
static const char *shname(const Elf *e, size_t i) {
    return (const char *)e->b + shf(e, e->shstr, 16) + shf(e, i, 0);
}
static long find_sec(const Elf *e, const char *name) {
    size_t i;
    for (i = 1; i < e->shnum; i++)
        if (!strcmp(shname(e, i), name)) return (long)i;
    return -1;
}

/* value of the symbol with this name or prefix, -1 when absent; *nm gets its full name */
static long sym_find(const Elf *e, const char *prefix, const char **nm) {
    long st = find_sec(e, ".symtab");
    size_t i, n;
    const char *strs;
    if (st < 0) return -1;
    strs = (const char *)e->b + shf(e, shf(e, (size_t)st, 24), 16);
    n = shf(e, (size_t)st, 20) / 16;
    for (i = 0; i < n; i++) {
        const uint8_t *s = e->b + shf(e, (size_t)st, 16) + i * 16;
        const char *name = strs + be(s, 4);
        if (!strncmp(name, prefix, strlen(prefix))) {
            if (nm) *nm = name;
            return (long)be(s + 4, 4);
        }
    }
    return -1;
}

static int contains(const uint8_t *h, size_t n, const char *x, size_t xn) {
    size_t i;
    for (i = 0; i + xn <= n; i++)
        if (!memcmp(h + i, x, xn)) return 1;
    return 0;
}

static Module *assemble(const char *src, uint8_t **obj, size_t *olen) {
    Reporter *rep = rep_new(0);
    char err[256];
    Module *m = asm_assemble_text("t.asm", src, strlen(src), rep);
    if (!m) {
        rep_print(rep, stdout, 0);
        return NULL;
    }
    *obj = module_emit(m, olen, err, sizeof err);
    if (!*obj) printf("emit: %s\n", err);
    return m;
}

static const char *const PROG = "PB       CSECT\n"
                                "         USING *,15\n"
                                "START    LR    1,1\n"
                                "         LA    2,FLD\n"
                                "         BR    14\n"
                                "FLD      DC    F'1'\n"
                                "TXT      DC    CL10'abc'\n"
                                "         END\n";

static void test_build(void) {
    uint8_t *obj = NULL;
    size_t olen = 0;
    Module *m = assemble(PROG, &obj, &olen);
    DbgOptions o;
    DbgResult r;
    char err[256];
    Elf e;
    const char *nm = NULL;
    long v, s;
    const char *perr = NULL;
    GoffModule *g1, *g2;
    size_t k;
    int found_dbgname = 0, found_map = 0, found_lit = 0;
    CHECK(m && obj);
    if (!m || !obj) return;
    memset(&o, 0, sizeof o);
    o.cwd = "/home/me/proj";
    o.gdir = "/u/ccw/dbg";
    o.main_file = "t.asm";
    o.dbg_path = "/u/ccw/dbg/t.dbg";
    o.producer = "test 1";
    CHECK_MSG(dbg_build(m, obj, olen, &o, &r, err, sizeof err), "dbg_build: %s", err);
    if (!r.elf) return;

    /* ELF32, big endian, ET_REL, EM_S390 */
    CHECK(elf_parse(&e, r.elf, r.elf_len));
    CHECK(r.elf[4] == 1 && r.elf[5] == 2);
    CHECK(be(r.elf + 16, 2) == 1 && be(r.elf + 18, 2) == 22);
    CHECK(e.shoff % 8 == 0); /* z/OS libelf rejects an unaligned section header table */
    CHECK(find_sec(&e, ".text") == 1);
    CHECK(find_sec(&e, ".debug_info") > 0);
    CHECK(find_sec(&e, ".debug_line") > 0);
    CHECK(find_sec(&e, ".debug_abbrev") > 0);
    CHECK(find_sec(&e, ".debug_aranges") > 0);
    CHECK(find_sec(&e, ".debug_pubnames") > 0);
    CHECK(find_sec(&e, ".debug_srcfiles") > 0);
    CHECK(find_sec(&e, ".rel.debug_info") > 0);
    CHECK(find_sec(&e, ".strtab") > 0 && find_sec(&e, ".symtab") > 0);
    CHECK(shf(&e, 1, 4) == 8); /* .text is NOBITS */
    /* DWARF 4 unit, big endian, address size 8 */
    s = find_sec(&e, ".debug_info");
    if (s > 0) {
        const uint8_t *d = r.elf + shf(&e, (size_t)s, 16);
        CHECK(be(d + 4, 2) == 4);
        CHECK(d[10] == 8);
        CHECK(shf(&e, (size_t)s, 20) == be(d, 4) + 4);
        CHECK(contains(d, shf(&e, (size_t)s, 20), "test 1", 7));
        CHECK(contains(d, shf(&e, (size_t)s, 20), "/u/ccw/dbg", 10));
        CHECK(contains(d, shf(&e, (size_t)s, 20), "FLD", 4));
        CHECK(contains(d, shf(&e, (size_t)s, 20), "START", 6));
    }
    v = sym_find(&e, ".text_cu1_id", &nm);
    CHECK(v == 0 && nm);
    v = sym_find(&e, ".ADATA_", &nm);
    CHECK(v == 0 && nm && strlen(nm) == 7 + 32);

    /* the link module */
    CHECK(r.mod2_len > 0 && r.mod2_len % 80 == 0);
    g1 = goff_parse(obj, olen, &perr);
    CHECK(g1 != NULL);
    g2 = goff_parse(r.mod2, r.mod2_len, &perr);
    CHECK_MSG(g2 != NULL, "module 2: %s", perr ? perr : "");
    if (!g2) return;
    CHECK(g2->bytes == r.mod2_len);
    for (k = 0; k < g2->esds.n; k++) {
        const GoffESD *d = g2->esds.v[k];
        char nb[64];
        size_t i;
        for (i = 0; i < d->name_len && i < 63; i++) nb[i] = (char)za_to_latin1[d->name[i]];
        nb[i] = 0;
        if (!strcmp(nb, "D_@@ADBG_DBG")) found_dbgname = 1;
        if (!strcmp(nb, "D_@@ADBG_MAP")) found_map = 1;
        if (!strcmp(nb, "D_@@ADBG_LIT")) found_lit = 1;
    }
    CHECK(found_dbgname && found_map && found_lit);
    /* DBGNAME: 8 byte length, 16 byte signature = the ADATA symbol, the EBCDIC path of the .dbg, NUL */
    {
        uint32_t id = 0;
        int ok = 0;
        for (k = 0; k < g2->esds.n; k++) {
            const GoffESD *d = g2->esds.v[k];
            if (d->type == GOFF_PR && d->name_len == 7 && !memcmp(d->name, "\xC4\xC2\xC7\xD5\xC1\xD4\xC5", 7)) id = d->id; /* DBGNAME */
        }
        CHECK(id != 0);
        for (k = 0; k < g2->texts.n; k++) {
            const GoffText *t = &g2->texts.v[k];
            if (t->esdid != id) continue;
            ok = 1;
            CHECK(be(t->data, 8) == ((t->len + 7) & ~(size_t)7)); /* the padded length */
            {
                char hex[33], path[64];
                size_t i;
                for (i = 0; i < 16; i++) sprintf(hex + 2 * i, "%02X", t->data[8 + i]);
                CHECK(nm && !strcmp(nm + 7, hex));
                for (i = 0; 24 + i < t->len && i < 63; i++) path[i] = (char)za_to_latin1[t->data[24 + i]];
                path[i] = 0;
                CHECK_STR(path, "/u/ccw/dbg/t.dbg");
                CHECK(t->data[t->len - 1] == 0);
            }
        }
        CHECK(ok);
    }
    /* one ER per CSECT in the module: the section PB */
    {
        int er = 0;
        for (k = 0; k < g2->esds.n; k++)
            if (g2->esds.v[k]->type == GOFF_ER && g2->esds.v[k]->name_len == 2 && !memcmp(g2->esds.v[k]->name, "\xD7\xC2", 2)) er++;
        CHECK(er == 1);
    }
}

/* several CSECTs: the symbol of each gets its offset in the B_TEXT class of the module (the binder
 * concatenates them, doublewords), the addresses in the DWARF carry it too */
static void test_multi(void) {
    const char *src = "AA       CSECT\n"
                      "         LR    1,1\n"
                      "         LR    2,2\n"
                      "         LR    3,3\n"
                      "BB       CSECT\n"
                      "         LR    3,3\n"
                      "         DC    F'1'\n"
                      "CC       RSECT\n"
                      "         LR    5,5\n"
                      "         END\n";
    uint8_t *obj = NULL;
    size_t olen = 0;
    Module *m = assemble(src, &obj, &olen);
    DbgOptions o;
    DbgResult r;
    char err[256];
    Elf e;
    long st;
    size_t i, n, nsym = 0;
    long vals[8];
    CHECK(m && obj);
    if (!m || !obj) return;
    memset(&o, 0, sizeof o);
    o.cwd = "/w";
    o.main_file = "t.asm";
    o.dbg_path = "/w/t.dbg";
    o.producer = "test";
    CHECK(dbg_build(m, obj, olen, &o, &r, err, sizeof err));
    CHECK(elf_parse(&e, r.elf, r.elf_len));
    st = find_sec(&e, ".symtab");
    n = shf(&e, (size_t)st, 20) / 16;
    for (i = 0; i < n; i++) {
        const uint8_t *s = r.elf + shf(&e, (size_t)st, 16) + i * 16;
        const char *strs = (const char *)r.elf + shf(&e, shf(&e, (size_t)st, 24), 16);
        if (!strncmp(strs + be(s, 4), ".text_cu1_id", 12) && nsym < 8) vals[nsym++] = (long)be(s + 4, 4);
    }
    CHECK(nsym == 3);
    if (nsym == 3) {
        CHECK(vals[0] == 0);  /* AA: 6 bytes */
        CHECK(vals[1] == 8);  /* BB */
        CHECK(vals[2] == 16); /* CC: BB is 8 bytes (LR, pad?, F) */
    }
    /* the line program: a set_address (DW_LNE_set_address, opcode 2, length 9) per sequence whose operand
     * is the offset of the section */
    {
        long s = find_sec(&e, ".debug_line");
        const uint8_t *d = r.elf + shf(&e, (size_t)s, 16);
        size_t len = shf(&e, (size_t)s, 20), k;
        int seq = 0;
        long want[3] = {0, 8, 16};
        for (k = 0; k + 11 <= len; k++)
            if (d[k] == 0 && d[k + 1] == 9 && d[k + 2] == 2 && seq < 3) {
                CHECK_MSG((long)be(d + k + 3, 8) == want[seq], "sequence %d starts at %ld", seq, (long)be(d + k + 3, 8));
                seq++;
            }
        CHECK(seq == 3);
    }
}

int main(void) {
    test_md5();
    test_paths();
    test_build();
    test_multi();
    TEST_MAIN_END();
}
