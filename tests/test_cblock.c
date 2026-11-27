/* test_cblock.c - capture and M1 compilation of #pragma cblock. */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "../src/pp.h"
#include "testlib.h"

typedef struct {
    char *text;
    char *codes;
    Reporter *rep;
} Out;

static Out run_pp(const char *src) {
    PpOptions o;
    SrcVec lines = {0};
    Reporter *rep = rep_new(0);
    Buf t = {0}, c = {0};
    size_t i;
    Out out;
    memset(&o, 0, sizeof o);
    rep_add_source(rep, "t.za", src, strlen(src));
    pp_run("t.za", src, strlen(src), &o, rep, &lines);
    for (i = 0; i < lines.n; i++) {
        if (i) buf_putc(&t, '|');
        buf_puts(&t, lines.v[i].text);
    }
    for (i = 0; i < rep_count(rep); i++) {
        if (i) buf_putc(&c, ' ');
        buf_puts(&c, rep_get(rep, i)->code);
    }
    out.text = xstrdup(buf_cstr(&t));
    out.codes = xstrdup(buf_cstr(&c));
    out.rep = rep;
    return out;
}

static void expect(const char *name, const char *src, const char *want_codes, const char *needle) {
    Out o = run_pp(src);
    CHECK_MSG(!strcmp(o.codes, want_codes), "%s: codes \"%s\", want \"%s\"", name, o.codes,
              want_codes);
    if (needle) CHECK_MSG(strstr(o.text, needle) != NULL, "%s: output lacks \"%s\": %s", name,
                         needle, o.text);
    free(o.text);
    free(o.codes);
    rep_free(o.rep);
}


static void test_arch_builtins(void) {
    const char *src =
        "#pragma cblock arch\n"
        "#pragma cin uint32_t a=%r2\n"
        "#pragma cin uint32_t *p=%r3\n"
        "#pragma cout uint32_t r=%r4\n"
        "#pragma cscratch %r0, %r1, %r5\n"
        "#pragma ctemp %r12, 128, down\n"
        "#pragma cbegin\n"
        "r = __popcnt(a);\n"
        "r = r + (int32_t)__lrv(p);\n"
        "__strv(r, p);\n"
        "r = r + (int32_t)__stck(p);\n"
        "#pragma cend\n"
        " END\n";
    Out o = run_pp(src);
    CHECK_STR(o.codes, "");
    CHECK_MSG(strstr(o.text, "POPCNT %r1,%r0") != NULL, "missing POPCNT: %s", o.text);
    CHECK_MSG(strstr(o.text, "LRV %r0,0(") != NULL, "missing LRV: %s", o.text);
    CHECK_MSG(strstr(o.text, "STRV %r0,0(") != NULL, "missing STRV: %s", o.text);
    CHECK_MSG(strstr(o.text, "STCK 0(") != NULL, "missing STCK: %s", o.text);
    CHECK_MSG(strstr(o.text, "IPM %r0") != NULL, "missing IPM: %s", o.text);
    free(o.text);
    free(o.codes);
    rep_free(o.rep);
}

static void test_atomic_builtins(void) {
    expect("lad builtin",
           "#pragma cblock a\n"
           "#pragma cin uint32_t *out=%r2\n"
           "#pragma cin uint32_t v=%r3\n"
           "#pragma cin uint32_t *mem=%r4\n"
           "#pragma cout int32_t cc=%r5\n"
           "#pragma cscratch %r0, %r1, %r6, %r7\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "cc = __lad(out, v, mem);\n"
           "#pragma cend\n"
           " END\n",
           "", "LAA %r1,%r0,0(");
    expect("lpd builtin",
           "#pragma cblock a\n"
           "#pragma cin uint32_t *a=%r2, uint32_t *b=%r3\n"
           "#pragma cin uint32_t *c=%r4, uint32_t *d=%r5\n"
           "#pragma cout int32_t cc=%r6\n"
           "#pragma cscratch %r0, %r1, %r7, %r8, %r9\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "cc = __lpd(a, b, c, d);\n"
           "#pragma cend\n"
           " END\n",
           "", "LPD %r8,0(");
    expect("cdsg builtin",
           "#pragma cblock a\n"
           "#pragma cin void *a=%r2, void *b=%r3, void *c=%r4\n"
           "#pragma cout int32_t cc=%r5\n"
           "#pragma cscratch %r0, %r1, %r6, %r7, %r8, %r9\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "cc = __cdsg(a, b, c);\n"
           "#pragma cend\n"
           " END\n",
           "", "CDSG %r6,%r8,0(");
    expect("csg builtin",
           "#pragma cblock a\n"
           "#pragma cin void *a=%r2, void *b=%r3, void *c=%r4\n"
           "#pragma cout int32_t cc=%r5\n"
           "#pragma cscratch %r0, %r1, %r6, %r7, %r8\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "cc = __csg(a, b, c);\n"
           "#pragma cend\n"
           " END\n",
           "", "CSG %r0,%r1,0(");
}

static void test_decimal_builtins(void) {
    expect("decimal and character builtins",
           "#pragma cblock a\n"
           "#pragma cin unsigned char *a=%r2, unsigned char *b=%r3\n"
           "#pragma cout int32_t cc=%r4\n"
           "#pragma cscratch %r0, %r1, %r5, %r6\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "cc = __ap(a, 1, b, 1);\n"
           "cc = __nc(a, b, 3);\n"
           "__tr(a, b, 3);\n"
           "cc = __cvb(a);\n"
           "__cvd(cc, a);\n"
           "cc = __ed(a, b, 5);\n"
           "cc = __tp(a, 5);\n"
           "cc = __srp(a, 5, 2, 15);\n"
           "__pack(a, 2, b, 3);\n"
           "#pragma cend\n"
           " END\n",
           "", "AP 0(1,%r1),0(1,%r5)");
    expect("IEEE float and double builtins",
           "#pragma cblock a\n"
           "#pragma cin double a=%f0, float b=%f2\n"
           "#pragma cout double c=%f4, float d=%f6\n"
           "#pragma cscratch %r2, %r3, %r4, %r5, %f8, %f10, %f12\n"
           "#pragma ctemp %r12, 256, down\n"
           "#pragma cbegin\n"
           "c = __sqdbr(a);\n"
           "d = __sqebr(b);\n"
           "c = __madbr(a, a, a);\n"
           "d = __fmadds(b, b, b);\n"
           "c = __msdbr(a, a, a);\n"
           "d = __fmsub(a, a, a);\n"
           "c = __fidbr(0, a);\n"
           "d = __fiebr(0, b);\n"
           "double x;\n"
           "float y;\n"
           "int32_t cc;\n"
           "cc = __lpdbr(&x, a);\n"
           "cc = __lndbr(&x, a);\n"
           "cc = __lpebr(&y, b);\n"
           "cc = __lnebr(&y, b);\n"
           "#pragma cend\n"
           " END\n",
           "", "SQDBR %f8,%f10");
    expect("IEEE conversions and FPC",
           "#pragma cblock a\n"
           "#pragma cin double a=%f0, float b=%f2\n"
           "#pragma cout int32_t cc=%r3\n"
           "#pragma cscratch %r2, %r4, %r5, %r6, %f8, %f10\n"
           "#pragma ctemp %r12, 256, down\n"
           "#pragma cbegin\n"
           "int32_t x;\n"
           "int64_t y;\n"
           "cc = __cfdbr(&x, 0, a);\n"
           "cc = __cfebr(&x, 0, b);\n"
           "cc = __cgdbr(&y, 0, a);\n"
           "cc = __cgebr(&y, 0, b);\n"
           "cc = __efpc();\n"
           "__sfpc(cc);\n"
           "#pragma cend\n"
           " END\n",
           "", "CFDBR %r0,0,%f8");
    expect("float literals",
           "#pragma cblock a\n"
           "#pragma cout double c=%f4, float d=%f6\n"
           "#pragma cscratch %r2, %r3, %f8\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "c = 1.5;\n"
           "d = 2.25f;\n"
           "#pragma cend\n"
           " END\n",
           "", "IIHF %r0,1073217536");
    expect("float casts",
           "#pragma cblock a\n"
           "#pragma cout float f=%f4, int32_t i=%r2\n"
           "#pragma cscratch %r3, %f8\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "f = 1.5;\n"
           "i = 1.5;\n"
           "#pragma cend\n"
           " END\n",
           "", "LEDBR %f8,%f8");
    expect("floating arithmetic and comparison",
           "#pragma cblock a\n"
           "#pragma cout double c=%f4, int32_t r=%r3\n"
           "#pragma cscratch %r2, %r5, %r6, %f8, %f10\n"
           "#pragma ctemp %r12, 256, down\n"
           "#pragma cbegin\n"
           "c = 1.5 + 2.25;\n"
           "c = c * 2.0;\n"
           "r = (c < 7.5) + (c >= 7.5) * 2;\n"
           "if (c != 0.0) r = r + 4;\n"
           "#pragma cend\n"
           " END\n",
           "", "ADBR %f8,%f10");
    expect("long double arithmetic and binding",
           "#pragma cblock a\n"
           "#pragma cin long double a=%f0\n"
           "#pragma cout long double c=%f8, int32_t r=%r2\n"
           "#pragma cscratch %r3, %f4, %f6, %f12, %f14\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "long double x;\n"
           "x = a + 2.0;\n"
           "c = x * 2.0;\n"
           "r = (a < 2.0) + (a >= 2.0) * 2;\n"
           "#pragma cend\n"
           " END\n",
           "", "AXBR %f4,%f12");
    expect("long double builtins",
           "#pragma cblock a\n"
           "#pragma cin long double a=%f0\n"
           "#pragma cout long double c=%f8, int32_t r=%r2\n"
           "#pragma cscratch %r3, %f4, %f6, %f12, %f14\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "long double x;\n"
           "int32_t i;\n"
           "c = __sqxbr(a);\n"
           "c = __fixbr(0, a);\n"
           "r = __lpxbr(&x, a);\n"
           "r = __lnxbr(&x, a);\n"
           "r = __cfxbr(&i, 0, a);\n"
           "r = __cgxbr(&i, 0, a);\n"
           "#pragma cend\n"
           " END\n",
           "", "SQXBR %f4,%f4");
    expect("HFP conversions and divide-to-integer",
           "#pragma cblock a\n"
           "#pragma cin float a=%f0, double b=%f2\n"
           "#pragma cout int32_t c=%r2\n"
           "#pragma cscratch %r3, %r4, %r5, %f4, %f6, %f8, %f10\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "double d;\n"
           "float e;\n"
           "double q;\n"
           "float qf;\n"
           "int32_t m;\n"
           "c = __thder(&d, a);\n"
           "c = __thdr(&d, b);\n"
           "c = __tbedr(&e, 0, b);\n"
           "c = __tbdr(&d, 0, b);\n"
           "c = __diebr(&qf, &e, a, a, 0);\n"
           "c = __didbr(&q, &d, b, b, 0);\n"
           "c = __tceb(a, m);\n"
           "c = __tcdb(b, m);\n"
           "c = __tcxb((long double)b, m);\n"
           "__srnm(m);\n"
           "#pragma cend\n"
           " END\n",
           "", "THDER %f4,%f4");
    expect("hexadecimal floating-point builtins",
           "#pragma cblock a\n"
           "#pragma cin float f=%f0, double d=%f2, long double x=%f4\n"
           "#pragma cout int32_t c=%r2, long double q=%f8\n"
           "#pragma cscratch %r3, %r4, %r5, %f12, %f14\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "float of; double od; long double ox;\n"
           "int32_t ci; int64_t cl;\n"
           "c = __lper(&of, f);\n"
           "c = __lpdr(&od, d);\n"
           "c = __lner(&of, f);\n"
           "c = __lndr(&od, d);\n"
           "of = __sqer(f);\n"
           "od = __sqdr(d);\n"
           "ox = __sqxr(x);\n"
           "of = __fier(f);\n"
           "od = __fidr(d);\n"
           "ox = __fixr(x);\n"
           "c = __cfer(&ci, 0, f);\n"
           "c = __cfdr(&ci, 0, d);\n"
           "c = __cfxr(&ci, 0, x);\n"
           "c = __lpxr(&ox, x);\n"
           "c = __lnxr(&ox, x);\n"
           "q = __sqxr(x);\n"
           "c = __cger(&cl, 0, f);\n"
           "c = __cgdr(&cl, 0, d);\n"
           "c = __cgxr(&cl, 0, x);\n"
           "#pragma cend\n"
           " END\n",
           "", "FIXR %f12,%f12");
    expect("vector select and permute",
           "#pragma cblock a\n"
           "#pragma cin __vector unsigned int a=%v0, __vector unsigned int b=%v2, __vector unsigned char m=%v4\n"
           "#pragma cout __vector unsigned int c=%v6\n"
           "#pragma cscratch %r2, %r3, %v8, %v10, %v12, %v14\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_sel(a, b, m);\n"
           "c = __vec_perm(a, b, m);\n"
           "c = __vec_permi(a, b, 1);\n"
           "#pragma cend\n"
           " END\n",
           "", "VSEL %v8,%v10,%v12,%v14");
    expect("vector GF multiply sum",
           "#pragma cblock a\n"
           "#pragma cin __vector unsigned char a=%v0, __vector unsigned char b=%v2\n"
           "#pragma cout __vector unsigned short c=%v4\n"
           "#pragma cscratch %r2, %r3, %v6, %v8, %v10\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_gfmsum(a, b);\n"
           "#pragma cend\n"
           " END\n",
           "", "VGFM %v6,%v8,%v10,0");
    expect("vector load pair",
           "#pragma cblock a\n"
           "#pragma cin int64_t x=%r2, int64_t y=%r3\n"
           "#pragma cout __vector signed long long c=%v0\n"
           "#pragma cscratch %r4, %r5, %v2, %v4\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_load_pair_s8(x, y);\n"
           "#pragma cend\n"
           " END\n",
           "", "VL %v2,");
    expect("vector bit permute",
           "#pragma cblock a\n"
           "#pragma cin __vector unsigned char a=%v0, __vector unsigned char b=%v2\n"
           "#pragma cout __vector unsigned long long c=%v4\n"
           "#pragma cscratch %r2, %r3, %v6, %v8, %v10\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_bperm(a, b);\n"
           "#pragma cend\n"
           " END\n",
           "", "VBPERM %v6,%v8,%v10");
    expect("vector floating-point rounding",
           "#pragma cblock a\n"
           "#pragma cin __vector double a=%v0\n"
           "#pragma cout __vector double c=%v2\n"
           "#pragma cscratch %r2, %r3, %v4, %v6\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_round(a);\n"
           "c = __vec_roundc(a);\n"
           "c = __vec_roundm(a);\n"
           "c = __vec_roundp(a);\n"
           "c = __vec_roundz(a);\n"
           "c = __vec_trunc(a);\n"
           "c = __vec_ceil(a);\n"
           "c = __vec_floor(a);\n"
           "#pragma cend\n"
           " END\n",
           "", "VFI %v4,%v6,3,0,1");
    expect("vector float comparisons",
           "#pragma cblock a\n"
           "#pragma cin __vector float a=%v0, __vector float b=%v2\n"
           "#pragma cout __vector bool int c=%v4\n"
           "#pragma cscratch %r2, %r3, %v6, %v8, %v10\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_cmpeq(a, b);\n"
           "c = __vec_cmpge(a, b);\n"
           "c = __vec_cmpgt(a, b);\n"
           "c = __vec_cmple(a, b);\n"
           "c = __vec_cmplt(a, b);\n"
           "#pragma cend\n"
           " END\n",
           "", "VFCE %v6,%v8,%v10,2,0,0");
    expect("vector square root",
           "#pragma cblock a\n"
           "#pragma cin __vector double a=%v0\n"
           "#pragma cout __vector double c=%v2\n"
           "#pragma cscratch %r2, %r3, %v4, %v6\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_sqrt(a);\n"
           "#pragma cend\n"
           " END\n",
           "", "VFSQDB %v4,%v6");
    expect("vector boolean logic",
           "#pragma cblock a\n"
           "#pragma cin __vector unsigned int a=%v0, __vector unsigned int b=%v2\n"
           "#pragma cout __vector unsigned int c=%v4\n"
           "#pragma cscratch %r2, %r3, %v6, %v8, %v10\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_andc(a, b);\n"
           "c = __vec_nand(a, b);\n"
           "c = __vec_nor(a, b);\n"
           "c = __vec_orc(a, b);\n"
           "c = __vec_eqv(a, b);\n"
           "#pragma cend\n"
           " END\n",
           "", "VNX %v6,%v8,%v10");
    expect("vector rotations and double shifts",
           "#pragma cblock a\n"
           "#pragma cin __vector unsigned int a=%v0, __vector unsigned int n=%v2\n"
           "#pragma cout __vector unsigned int c=%v4\n"
           "#pragma cscratch %r2, %r3, %v6, %v8, %v10\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_rl(a, n);\n"
           "c = __vec_rli(a, 3);\n"
           "c = __vec_rl_mask(a, n, 15);\n"
           "c = __vec_sld(a, n, 4);\n"
           "c = __vec_sldb(a, n, 4);\n"
           "#pragma cend\n"
           " END\n",
           "", "VERIM %v6,%v8,%v10,15,2");
    expect("vector shifts",
           "#pragma cblock a\n"
           "#pragma cin __vector unsigned int a=%v0, __vector unsigned int b=%v2\n"
           "#pragma cin __vector unsigned char n=%v4\n"
           "#pragma cout __vector unsigned int c=%v6\n"
           "#pragma cscratch %r2, %r3, %v8, %v10, %v12\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_sll(a, n);\n"
           "c = __vec_srl(a, n);\n"
           "c = __vec_sral(a, n);\n"
           "c = __vec_slb(a, n);\n"
           "c = __vec_srab(a, n);\n"
           "#pragma cend\n"
           " END\n",
           "", "VSRAB %v8,%v10,%v12");
    expect("vector merge, checksum and bit counts",
           "#pragma cblock a\n"
           "#pragma cin __vector unsigned int a=%v0, __vector unsigned int b=%v2\n"
           "#pragma cout __vector unsigned int c=%v4\n"
           "#pragma cscratch %r2, %r3, %v6, %v8, %v10\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_mergeh(a, b);\n"
           "c = __vec_mergel(a, b);\n"
           "c = __vec_checksum(a, b);\n"
           "c = __vec_cntlz(a);\n"
           "c = __vec_cnttz(a);\n"
           "#pragma cend\n"
           " END\n",
           "", "VCKSM %v6,%v8,%v10");
    expect("vector pack and unpack",
           "#pragma cblock a\n"
           "#pragma cin __vector signed short a=%v0, __vector signed short b=%v2\n"
           "#pragma cin __vector signed char c0=%v4\n"
           "#pragma cout __vector signed char c=%v6\n"
           "#pragma cout __vector signed short d=%v8\n"
           "#pragma cscratch %r2, %r3, %v10, %v12, %v14\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_pack(a, b);\n"
           "c = __vec_packs(a, b);\n"
           "c = __vec_packsu(a, b);\n"
           "d = __vec_unpackh_s2(c0);\n"
           "d = __vec_unpackl_s2(c0);\n"
           "#pragma cend\n"
           " END\n",
           "", "VPK %v10,%v12,%v14,1");
    expect("vector element extract, insert and promote",
           "#pragma cblock a\n"
           "#pragma cin __vector signed int a=%v0, int32_t x=%r2\n"
           "#pragma cout __vector signed int c=%v4, int32_t y=%r3\n"
           "#pragma cscratch %r4, %r5, %v6, %v8, %v10\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_insert_s4(x, a, 1);\n"
           "c = __vec_promote_s4(x, 2);\n"
           "y = __vec_extract_s4(a, 3);\n"
           "c = __vec_insert_and_zero_s4((int32_t *)0);\n"
           "#pragma cend\n"
           " END\n",
           "", "VLEF %v6,0(%r4),1");
    expect("vector max, min and average",
           "#pragma cblock a\n"
           "#pragma cin __vector signed int a=%v0, __vector signed int b=%v2\n"
           "#pragma cout __vector signed int c=%v4\n"
           "#pragma cscratch %r2, %r3, %v6, %v8, %v10\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_max(a, b);\n"
           "c = __vec_min(a, b);\n"
           "c = __vec_avg(a, b);\n"
           "#pragma cend\n"
           " END\n",
           "", "VMX %v6,%v8,%v10,2");
    expect("vector ordered compares",
           "#pragma cblock a\n"
           "#pragma cin __vector signed int a=%v0, __vector signed int b=%v2\n"
           "#pragma cout __vector bool int c=%v4\n"
           "#pragma cscratch %r2, %r3, %v6, %v8, %v10, %v12\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_cmpge(a, b);\n"
           "c = __vec_cmpgt(a, b);\n"
           "c = __vec_cmple(a, b);\n"
           "c = __vec_cmplt(a, b);\n"
           "#pragma cend\n"
           " END\n",
           "", "VNO %v6,%v6,%v12");
    expect("vector compare equal",
           "#pragma cblock a\n"
           "#pragma cin __vector signed int a=%v0, __vector signed int b=%v2\n"
           "#pragma cout __vector bool int c=%v4\n"
           "#pragma cscratch %r2, %r3, %v6, %v8, %v10\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "c = __vec_cmpeq(a, b);\n"
           "#pragma cend\n"
           " END\n",
           "", "VCEQ %v6,%v8,%v10,2,0");
    expect("runtime facility checks",
           "#pragma cblock a\n"
           "#pragma cout int32_t c=%r2\n"
           "#pragma cscratch %r3, %r4, %r5, %r6, %r7\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "uint64_t area;\n"
           "c = __stfle(&area);\n"
           "c = __facility(0);\n"
           "c = __facility(129);\n"
           "c = __facility_test(&area, 0);\n"
           "#pragma cend\n"
           " END\n",
           "", "STFLE 0(");
    expect("vector load and splat",
           "#pragma cblock a\n"
           "#pragma cin __vector unsigned int v=%v0\n"
           "#pragma cout __vector unsigned int w=%v2\n"
           "#pragma cscratch %r2, %r3, %r4, %r5, %v4, %v6\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "__vector unsigned int a;\n"
           "a = __vec_splats_u4(7);\n"
           "w = __vec_load_len_u4((uint32_t *)0, 16);\n"
           "__vec_store_len(a, (uint32_t *)0, 16);\n"
           "w = a;\n"
           "#pragma cend\n"
           " END\n",
           "", "VLREP %v4,0(%r2),2");
    expect("vector operators are rejected",
           "#pragma cblock a\n"
           "#pragma cin __vector unsigned int a=%v0\n"
           "#pragma cout __vector unsigned int c=%v2\n"
           "#pragma cscratch %r2, %v4\n"
           "#pragma ctemp %r12, 256, down\n"
           "#pragma cbegin\n"
           "c = a + a;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5002", "");
    expect("DFP conversions",
           "#pragma cblock a\n"
           "#pragma cin char *p=%r2\n"
           "#pragma cout int32_t c=%r3\n"
           "#pragma cscratch %r4, %r5, %f0, %f2, %f4, %f6\n"
           "#pragma ctemp %r12, 512, down\n"
           "#pragma cbegin\n"
           "_Decimal64 d;\n"
           "_Decimal128 x;\n"
           "d = __cdzt(p, 5, 0);\n"
           "x = __cxzt(p, 5, 0);\n"
           "c = __czdt(d, p, 5, 0);\n"
           "c = __czxt(x, p, 5, 0);\n"
           "#pragma cend\n"
           " END\n",
           "", "CDZT %f0,0(5,%r4),0");
    expect("DFP arithmetic is rejected",
           "#pragma cblock a\n"
           "#pragma cin _Decimal64 a=%f0\n"
           "#pragma cout _Decimal64 c=%f2\n"
           "#pragma cscratch %r2, %f4, %f6\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "c = a + a;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5002", "");
    expect("long double binding needs an FPR pair",
           "#pragma cblock a\n"
           "#pragma cin long double a=%f2\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "a = a;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5005", "");
    expect("float binding needs an FPR",
           "#pragma cblock a\n"
           "#pragma cin double a=%r2\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "a = a;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5005", "");
    expect("packing and 64-bit decimal builtins",
           "#pragma cblock a\n"
           "#pragma cin char *a=%r2, char *b=%r3, uint16_t *u=%r4\n"
           "#pragma cout int64_t r=%r5\n"
           "#pragma cscratch %r0, %r1, %r6, %r7\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "r = __cvbg(a);\n"
           "__cvdg(r, b);\n"
           "__pka(a, b, 5);\n"
           "__pku(a, u, 5);\n"
           "__unpka(a, 5, b);\n"
           "__unpku(u, 5, b);\n"
           "__mvcrl(a, b, 5);\n"
           "#pragma cend\n"
           " END\n",
           "", "PKA 0(%r1),0(5,%r6)");
    expect("decimal length must be constant",
           "#pragma cblock a\n"
           "#pragma cin unsigned char *a=%r2, unsigned char *b=%r3\n"
           "#pragma cin int32_t n=%r4\n"
           "#pragma cscratch %r0, %r1, %r5\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "__nc(a, b, n);\n"
           "#pragma cend\n"
           " END\n",
           "ZA5005", NULL);
}

static void test_16bit_types(void) {
    expect("16-bit scalars and pointers",
           "#pragma cblock a\n"
           "#pragma cin uint16_t *p=%r2\n"
           "#pragma cout int32_t r=%r3\n"
           "#pragma cscratch %r0, %r1, %r4\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "int16_t x;\n"
           "uint16_t y;\n"
           "x = -1; y = 65535; x = x + 1; y = y + 1;\n"
           "r = x + y;\n"
           "r = r + (int32_t)__lrvh(p);\n"
           "__strvh(y, p);\n"
           "#pragma cend\n"
           " END\n",
           "", "LRVH %r0,0(");
}

static void test_debug_metadata(void) {
    const char *src =
        "#pragma cblock dbg\n"
        "#pragma cout int32_t c=%r4\n"
        "#pragma cscratch %r0, %r1, %r5\n"
        "#pragma ctemp %r12, 64, down\n"
        "#pragma cbegin\n"
        "int32_t x = 5; c = x;\n"
        "#pragma cend\n"
        " END\n";
    PpOptions o;
    SrcVec lines = {0};
    Reporter *rep = rep_new(0);
    CBlockDebugVec dbg = {0};
    memset(&o, 0, sizeof o);
    o.cblock_debug = &dbg;
    rep_add_source(rep, "t.za", src, strlen(src));
    pp_run("t.za", src, strlen(src), &o, rep, &lines);
    CHECK_MSG(dbg.n == 1, "debug: %zu cblocks", dbg.n);
    if (dbg.n == 1) {
        CHECK_MSG(dbg.v[0].nvars == 1, "debug: %zu vars", dbg.v[0].nvars);
        if (dbg.v[0].nvars == 1) {
            CHECK_STR(dbg.v[0].vars[0].name, "x");
            CHECK_MSG(dbg.v[0].vars[0].bits == 32, "debug: bits %d", dbg.v[0].vars[0].bits);
        }
        CHECK_MSG(dbg.v[0].temp_reg == 12, "debug: temp reg %d", dbg.v[0].temp_reg);
        CHECK_MSG(dbg.v[0].start_label != NULL && dbg.v[0].end_label != NULL,
                  "debug: missing range labels");
    }
    cblock_debug_free(&dbg);
    rep_free(rep);
    free(lines.v);
}

static void test_compiled_assembly(void) {
    const char *src =
        "P        DSECT\n"
        "P_A      DS    F\n"
        "P_B      DS    CL4\n"
        "MAIN CSECT\n"
        "         USING MAIN,15\n"
        "         LA    %r12,AREA\n"
        "         LHI   %r2,3\n"
        "         LHI   %r3,4\n"
        "#pragma cblock squares\n"
        "#pragma cin int32_t a=%r2, int32_t b=%r3\n"
        "#pragma cout int32_t c=%r4\n"
        "#pragma cscratch %r0, %r1, %r5\n"
        "#pragma ctemp %r12, 96, down\n"
        "#pragma cbegin\n"
        "c = a*a + b*b;\n"
        "#pragma cend\n"
        "         ST    %r4,RES\n"
        "#pragma cblock second\n"
        "#pragma cout int32_t d=%r5\n"
        "#pragma cscratch %r0, %r1, %r6\n"
        "#pragma ctemp %r12, 96, down\n"
        "#pragma cbegin\n"
        "int32_t i = 0; while (i < 2) { i++; } d = i;\n"
        "#pragma cend\n"
        "         ST    %r5,RES2\n"
        "#pragma cblock typed\n"
        "#pragma cout int32_t n=%r4, P *o=%r5\n"
        "#pragma cscratch %r0, %r1, %r6\n"
        "#pragma ctemp %r12, 96, down\n"
        "#pragma cbegin\n"
        "P *p; p = (P *)0; o = p; n = (int32_t)__sizeof(P) + (int32_t)__offsetof(P, P_B);\n"
        "#pragma cend\n"
        "         ST    %r4,RES3\n"
        "RES      DS    F\n"
        "RES2     DS    F\n"
        "RES3     DS    F\n"
        "AREA     DS    12F\n"
        "         END\n";
    Out o = run_pp(src);
    Buf b = {0};
    size_t i;
    Reporter *rep;
    Module *m;
    for (i = 0; o.text[i]; i++) buf_putc(&b, o.text[i] == '|' ? '\n' : o.text[i]);
    rep = rep_new(0);
    m = asm_assemble_text("t.za", buf_cstr(&b), b.len, rep);
    CHECK_MSG(!strcmp(o.codes, ""), "compiled block codes: %s", o.codes);
    CHECK_MSG(m != NULL, "compiled block does not assemble");
    if (m) asm_free(m);
    rep_free(rep);
    free(b.p);
    free(o.text);
    free(o.codes);
    rep_free(o.rep);
}

int main(void) {
    expect("compiled block",
           "#pragma cblock demo\n"
           "#pragma cin int32_t a=%r2\n"
           "#pragma cout int32_t b=%r3\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "b = a + 1;\n"
           "#pragma cend\n"
           " END\n",
           "", "AR %r0,%r1");
    expect("no cbegin",
           "#pragma cblock demo\n"
           " END\n",
           "ZA5001", NULL);
    expect("unterminated",
           "#pragma cblock demo\n"
           "#pragma cbegin\n"
           "return;\n"
           " END\n",
           "ZA5001", NULL);
    expect("return unsupported",
           "#pragma cblock demo\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma cbegin\n"
           "return;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5008", NULL);
    expect("temp too small",
           "#pragma cblock demo\n"
           "#pragma cin int32_t a=%r2\n"
           "#pragma cout int32_t b=%r3\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 8, down\n"
           "#pragma cbegin\n"
           "b = a;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5006", NULL);
    expect("string literal table",
           "#pragma cblock lit\n"
           "#pragma cout uint32_t n=%r2\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "const char *p = __align(8) \"A\\x00B\";\n"
           "n = __strlen(p);\n"
           "#pragma cend\n"
           " END\n",
           "", "DC X'41004200'");
    expect("string literal strlen and byte",
           "#pragma cblock lit\n"
           "#pragma cin char *p=%r2\n"
           "#pragma cout uint32_t n=%r3\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "n = __strlen(\"hello\");\n"
           "n = p[1];\n"
           "#pragma cend\n"
           " END\n",
           "", "LGFI %r0,5");
    expect("memcmp and strcmp compile",
           "#pragma cblock cmp\n"
           "#pragma cin char *a=%r2, char *b=%r3\n"
           "#pragma cout int32_t n=%r4\n"
           "#pragma cscratch %r0, %r1, %r5\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "n = __memcmp(a, b, 4);\n"
           "n = n + __strcmp(a, b);\n"
           "#pragma cend\n"
           " END\n",
           "", "CLC 0(1,%r1),0(%r5)");
    expect("strcpy compile",
           "#pragma cblock cp\n"
           "#pragma cin char *d=%r2, char *s=%r3\n"
           "#pragma cout char *r=%r4\n"
           "#pragma cscratch %r0, %r1, %r5\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "r = __strcpy(d, s);\n"
           "#pragma cend\n"
           " END\n",
           "", "MVC 0(1,");
    expect("bad string alignment",
           "#pragma cblock lit\n"
           "#pragma ctemp %r12, 128, down\n"
           "#pragma cbegin\n"
           "char *p = __align(3) \"x\";\n"
           "#pragma cend\n"
           " END\n",
           "ZA5001 ZA5001", NULL);
    expect("ctemp r1 is internal scratch",
           "#pragma cblock demo\n"
           "#pragma ctemp %r1, 64, down\n"
           "#pragma cbegin\n"
           "int32_t x;\n"
           "x = 1;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5005", NULL);
    expect("read before assign",
           "#pragma cblock demo\n"
           "#pragma cout int32_t b=%r3\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "b = b + 1;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5003", NULL);
    expect("cend outside",
           "#pragma cend\n"
           " END\n",
           "ZA5001", NULL);
    expect("if and while compile",
           "#pragma cblock demo\n"
           "#pragma cout int32_t c=%r4\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "int32_t i = 0; while (i < 2) { i++; } c = i;\n"
           "#pragma cend\n"
           " END\n",
           "", "BRCL");
    expect("switch and goto compile",
           "#pragma cblock demo\n"
           "#pragma cout int32_t c=%r4\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "int32_t t = 0; switch (t) { case 0: t = 1; break; default: t = 2; } goto done; done: c = t;\n"
           "#pragma cend\n"
           " END\n",
           "", "BRCL");
    expect("memcpy compiles",
           "#pragma cblock demo\n"
           "#pragma cin uint8_t *d=%r2, uint8_t *s=%r3\n"
           "#pragma cout uint8_t *o=%r4\n"
           "#pragma cscratch %r0, %r1, %r5\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "__memcpy(d, s, 4); o = d;\n"
           "#pragma cend\n"
           " END\n",
           "", "MVC");
    expect("memcpy needs pointers",
           "#pragma cblock demo\n"
           "#pragma cscratch %r0, %r1, %r5\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "__memcpy(1, 2, 4);\n"
           "#pragma cend\n"
           " END\n",
           "ZA5003", NULL);
    expect("undefined goto",
           "#pragma cblock demo\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "goto nowhere;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5004", NULL);
    expect("break outside loop",
           "#pragma cblock demo\n"
           "#pragma cscratch %r0, %r1\n"
           "#pragma ctemp %r12, 64, down\n"
           "#pragma cbegin\n"
           "break;\n"
           "#pragma cend\n"
           " END\n",
           "ZA5008", NULL);
    expect("cbegin outside",
           "#pragma cbegin\n"
           " END\n",
           "ZA5001", NULL);
    test_arch_builtins();
    test_atomic_builtins();
    test_decimal_builtins();
    test_16bit_types();
    test_debug_metadata();
    test_compiled_assembly();
    TEST_MAIN_END();
}
