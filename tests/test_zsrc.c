/* test_zsrc.c - the LZ coder and the original sources in the object (zsrc.h, lz.h). */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/asm.h"
#include "../src/ebcdic.h"
#include "../src/goff.h"
#include "../src/lz.h"
#include "../src/zsrc.h"
#include "../src/zsym.h"
#include "testlib.h"

/* ---- LZ ---------------------------------------------------------------------- */

static uint32_t rng = 12345;
static uint32_t rnd(void) {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

static int round_trip(const uint8_t *src, size_t n, size_t *packed) {
    Buf b = {0};
    uint8_t *out = xmalloc(n + 1);
    int ok;
    lz_compress(src, n, &b);
    if (packed) *packed = b.len;
    ok = lz_decompress((const uint8_t *)b.p, b.len, out, n) && (n == 0 || memcmp(out, src, n) == 0);
    free(out);
    return ok;
}

static void test_lz(void) {
    size_t n, i, packed;
    uint8_t *d;
    /* hand made blocks: "a" + match(offset 1, 9) + 5 literals; stored by the reference rules */
    static const uint8_t blk[] = {0x15, 'a', 0x01, 0x00, 0x50, 'a', 'a', 'a', 'a', 'a'};
    uint8_t out[32];
    static const uint8_t empty[] = {0x00};

    CHECK(lz_decompress(blk, sizeof blk, out, 15));
    for (i = 0; i < 15; i++) CHECK(out[i] == 'a');
    CHECK(!lz_decompress(blk, sizeof blk, out, 14)); /* the block gives 15 bytes */
    CHECK(!lz_decompress(blk, sizeof blk, out, 16));
    CHECK(lz_decompress(empty, 1, out, 0));
    CHECK(!lz_decompress(empty, 0, out, 0));
    CHECK(!lz_decompress(empty, 1, out, 1));
    {
        static const uint8_t bad_off0[] = {0x15, 'a', 0x00, 0x00, 0x50, 'a', 'a', 'a', 'a', 'a'};
        static const uint8_t bad_off[] = {0x15, 'a', 0x02, 0x00, 0x50, 'a', 'a', 'a', 'a', 'a'};
        static const uint8_t trunc_lit[] = {0xF0, 0xFF};
        static const uint8_t trunc_off[] = {0x15, 'a', 0x01};
        CHECK(!lz_decompress(bad_off0, sizeof bad_off0, out, 15)); /* offset 0 */
        CHECK(!lz_decompress(bad_off, sizeof bad_off, out, 15));   /* before the start */
        CHECK(!lz_decompress(trunc_lit, sizeof trunc_lit, out, 32));
        CHECK(!lz_decompress(trunc_off, sizeof trunc_off, out, 32));
    }

    /* sizes around the limits of the format */
    for (n = 0; n < 80; n++) {
        d = xmalloc(n + 1);
        for (i = 0; i < n; i++) d[i] = (uint8_t)(i % 7 == 0 ? 'x' : 'a' + i % 3);
        CHECK_MSG(round_trip(d, n, NULL), "pattern, n=%zu", n);
        for (i = 0; i < n; i++) d[i] = (uint8_t)rnd();
        CHECK_MSG(round_trip(d, n, NULL), "random, n=%zu", n);
        memset(d, 'q', n);
        CHECK_MSG(round_trip(d, n, NULL), "run, n=%zu", n);
        free(d);
    }
    /* long runs and literals (length extension bytes), distances beyond 64K */
    n = 300000;
    d = xmalloc(n);
    memset(d, 0, n);
    CHECK(round_trip(d, n, &packed));
    CHECK(packed < n / 200);
    for (i = 0; i < n; i++) d[i] = (uint8_t)rnd();
    CHECK(round_trip(d, n, &packed)); /* incompressible */
    memcpy(d + 200000, d, 70000);     /* a copy 200000 bytes away: out of reach, still correct */
    CHECK(round_trip(d, n, NULL));
    memcpy(d + 1000, d, 500);
    memcpy(d + 5000, d + 1000, 4000);
    CHECK(round_trip(d, n, &packed));
    free(d);
    /* text compresses */
    {
        Buf t = {0};
        for (i = 0; i < 200; i++) buf_printf(&t, "         LR    R%zu,R%zu            copy a register\n", i % 16, (i + 1) % 16);
        CHECK(round_trip((const uint8_t *)t.p, t.len, &packed));
        CHECK(packed * 3 < t.len);
    }
    /* the decoder survives garbage and damaged blocks */
    for (i = 0; i < 4000; i++) {
        uint8_t g[64], res[256];
        size_t k, gl = rnd() % 64, want = rnd() % 256;
        for (k = 0; k < gl; k++) g[k] = (uint8_t)rnd();
        (void)lz_decompress(g, gl, res, want);
    }
    {
        Buf b = {0};
        uint8_t *src = xmalloc(5000), *res = xmalloc(5001);
        for (i = 0; i < 5000; i++) src[i] = (uint8_t)("abcabcabd"[i % 9] + (i / 700));
        lz_compress(src, 5000, &b);
        for (i = 0; i < 3000; i++) {
            Buf c = {0};
            buf_write(&c, b.p, b.len);
            c.p[rnd() % c.len] = (char)rnd();
            if (lz_decompress((const uint8_t *)c.p, c.len, res, 5000)) CHECK(1);
            if (rnd() % 2) (void)lz_decompress((const uint8_t *)c.p, rnd() % (c.len + 1), res, rnd() % 5001);
        }
    }
}

/* ---- the object ---------------------------------------------------------------- */

static void add_src(Module *m, const char *name, const char *text) {
    ZSrcIn f;
    f.name = xstrdup(name);
    f.text = xstrdup(text);
    f.len = strlen(text);
    vec_push(&m->zsrc, f);
}

static const char *SRC = "T1 CSECT\n LR 1,2\n END\n";

static GoffModule *build(int zlevel, uint8_t **objp, size_t *objlen, const char *extra) {
    Reporter *rep = rep_new(0);
    Module *m = asm_assemble_text("m.za", SRC, strlen(SRC), rep);
    char err[256];
    const char *perr = NULL;
    uint8_t *obj;
    GoffModule *g;
    if (!m) {
        CHECK_MSG(0, "assembly failed");
        return NULL;
    }
    m->zlevel = zlevel;
    add_src(m, "m.za", SRC);
    if (extra) add_src(m, "inc/big.inc", extra);
    vec_push(&m->zdefs, xstrdup("FOO=1"));
    obj = module_emit(m, objlen, err, sizeof err);
    CHECK_MSG(obj != NULL, "emit: %s", err);
    if (!obj) return NULL;
    g = goff_parse(obj, *objlen, &perr);
    CHECK_MSG(g != NULL, "parse: %s", perr ? perr : "?");
    if (objp) *objp = obj;
    return g;
}

static void test_object(void) {
    size_t zlen, objlen, i;
    uint8_t *z, *obj;
    GoffModule *g;
    ZSrc *s;
    Buf big = {0};
    for (i = 0; i < 300; i++) buf_printf(&big, "* line %zu of an included file, not at all random\n", i % 10);

    /* at -gz1 there is no class for the sources */
    g = build(1, NULL, &objlen, big.p);
    CHECK(g && zsrc_extract(g, &zlen) == NULL);
    CHECK(g && zsym_extract(g, &zlen) != NULL);
    g = build(0, NULL, &objlen, NULL);
    CHECK(g && zsrc_extract(g, &zlen) == NULL);

    g = build(2, &obj, &objlen, big.p);
    if (!g) return;
    z = zsrc_extract(g, &zlen);
    CHECK(z != NULL);
    CHECK(zsym_extract(g, &zlen) != NULL); /* ZSYM is there too */
    z = zsrc_extract(g, &zlen);
    if (!z) return;
    s = zsrc_parse(z, zlen);
    CHECK(s->bad_chunks == 0);
    CHECK(s->blocks.n == 1);
    if (s->blocks.n == 1) {
        const ZSrcBlock *b = s->blocks.v[0];
        CHECK(b->complete && !b->damaged);
        CHECK(b->level == 2);
        CHECK(b->defs.n == 1 && !strcmp(b->defs.v[0], "FOO=1"));
        CHECK(b->files.n == 2);
        if (b->files.n == 2) {
            CHECK(!strcmp(b->files.v[0].name, "m.za"));
            CHECK(b->files.v[0].len == strlen(SRC) && !memcmp(b->files.v[0].data, SRC, strlen(SRC)));
            CHECK(b->files.v[0].method == ZR_STORED); /* too short to pay */
            CHECK(!strcmp(b->files.v[1].name, "inc/big.inc"));
            CHECK(b->files.v[1].len == big.len && !memcmp(b->files.v[1].data, big.p, big.len));
            CHECK(b->files.v[1].method == ZR_LZ);
            CHECK(b->files.v[1].packed * 4 < big.len);
        }
    }

    /* the class is not part of the code: -dump style listing works */
    {
        FILE *f = tmpfile();
        char line[200];
        int seen = 0;
        if (f) {
            zsrc_dump(s, f);
            rewind(f);
            while (fgets(line, sizeof line, f))
                if (strstr(line, "inc/big.inc") && strstr(line, "lz")) seen = 1;
            fclose(f);
            CHECK(seen);
        }
    }

    /* a damaged byte in the data is found by the chunk checksum */
    {
        uint8_t *bad = xmalloc(zlen + 1);
        ZSrc *s2;
        memcpy(bad, z, zlen);
        bad[zlen / 2] ^= 0x40;
        s2 = zsrc_parse(bad, zlen);
        CHECK(s2->bad_chunks > 0 || (s2->blocks.n && !s2->blocks.v[0]->complete));
        CHECK(!(s2->blocks.n == 1 && s2->blocks.v[0]->complete));
        /* a truncated stream is incomplete, not an error */
        s2 = zsrc_parse(z, zlen - 8);
        CHECK(s2->blocks.n == 1 && !s2->blocks.v[0]->complete);
        /* two blocks (several modules bound together) */
        {
            uint8_t *two = xmalloc(2 * zlen);
            memcpy(two, z, zlen);
            memcpy(two + zlen, z, zlen);
            s2 = zsrc_parse(two, 2 * zlen);
            CHECK(s2->blocks.n == 2 && s2->blocks.v[0]->complete && s2->blocks.v[1]->complete);
        }
        /* every single damaged byte is survived */
        for (i = 0; i < zlen; i++) {
            memcpy(bad, z, zlen);
            bad[i] ^= (uint8_t)(1 + i % 255);
            s2 = zsrc_parse(bad, zlen);
            CHECK(!(s2->blocks.n == 1 && s2->blocks.v[0]->complete && s2->bad_chunks == 0));
        }
        /* a wrong version or kind is counted, not trusted */
        s2 = zsrc_parse((const uint8_t *)"", 0);
        CHECK(s2->blocks.n == 0 && s2->bad_chunks == 0);
    }
}

static void test_unpack(void) {
    char dir[] = "/tmp/zosasm_zsrc_XXXXXX";
    Module m;
    Buf big = {0};
    size_t zlen, i;
    uint8_t *z;
    ZSrc *s;
    int k;
    if (!mkdtemp(dir)) {
        CHECK_MSG(0, "mkdtemp");
        return;
    }
    memset(&m, 0, sizeof m);
    m.zlevel = 2;
    for (i = 0; i < 100; i++) buf_puts(&big, "binary-ish \xC3\xA9 \x00 data\n");
    add_src(&m, "main.za", "A CSECT\n");
    add_src(&m, "/abs/../../escape.inc", "x\n");
    add_src(&m, "./a/./b//c.inc", "y\n");
    {
        ZSrcIn f;
        f.name = xstrdup("bin.dat");
        f.text = xmalloc(big.len + 1);
        memcpy(f.text, big.p, big.len);
        f.len = big.len;
        vec_push(&m.zsrc, f);
    }
    z = zsrc_build(&m, &zlen);
    CHECK(z != NULL);
    if (!z) return;
    s = zsrc_parse(z, zlen);
    CHECK(s->blocks.n == 1 && s->blocks.v[0]->complete && s->blocks.v[0]->files.n == 4);
    k = zsrc_unpack(s, dir, NULL);
    CHECK(k == 4);
    {
        const char *rel[] = {"main.za", "abs/__/__/escape.inc", "a/b/c.inc", "bin.dat"};
        for (i = 0; i < 4; i++) {
            char *path = xasprintf("%s/%s", dir, rel[i]);
            FILE *f = fopen(path, "rb");
            CHECK_MSG(f != NULL, "missing %s", path);
            if (f) {
                char buf[4096];
                size_t n = fread(buf, 1, sizeof buf, f);
                fclose(f);
                if (i == 3)
                    CHECK(n == big.len && !memcmp(buf, big.p, n));
                else
                    CHECK(n > 0);
                remove(path);
            }
        }
    }
    /* the directories are left empty; remove what the test made */
    {
        char *p;
        p = xasprintf("%s/abs/__/__", dir); rmdir(p);
        p = xasprintf("%s/abs/__", dir); rmdir(p);
        p = xasprintf("%s/abs", dir); rmdir(p);
        p = xasprintf("%s/a/b", dir); rmdir(p);
        p = xasprintf("%s/a", dir); rmdir(p);
        rmdir(dir);
        CHECK(access(dir, F_OK) != 0);
    }
}

int main(void) {
    test_lz();
    test_object();
    test_unpack();
    printf("test_zsrc: %d checks, %d failed\n", t_count, t_fail);
    return t_fail != 0;
}
