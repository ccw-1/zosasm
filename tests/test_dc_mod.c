/* test_dc_mod.c - scale (Sn) and exponent (En) modifiers, fractions and U in
 * F/H, hexadecimal floating point scaling (DESIGN.md 4.5).
 * Expected values follow the HLASM Language Reference; they are not checked
 * against IBM's assembler yet. */
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "testlib.h"

typedef struct {
    Module *m;
    char codes[256];
    char hex[256];
    int warnings;
} Res;

static Res run(const char *body) {
    Res r;
    Reporter *rep = rep_new(0);
    char *src = xasprintf("M CSECT\n%s END\n", body);
    size_t i;
    memset(&r, 0, sizeof r);
    r.m = asm_assemble_text("t.za", src, strlen(src), rep);
    for (i = 0; i < rep_count(rep); i++) {
        const Diag *d = rep_get(rep, i);
        if (d->sev == SEV_WARNING) r.warnings++;
        if (d->sev != SEV_ERROR) continue;
        if (r.codes[0]) strcat(r.codes, " ");
        strncat(r.codes, d->code, 20);
    }
    if (r.m && r.m->sections.n) {
        const Class *c = r.m->sections.v[0]->classes.v[0];
        for (i = 0; i < (size_t)c->len && i < 100; i++) sprintf(r.hex + 2 * i, "%02X", c->data[i]);
    }
    return r;
}

static void hex(const char *src, const char *want) {
    Res r = run(src);
    CHECK_MSG(r.m != NULL && !r.codes[0], "%s: does not assemble (%s)", src, r.codes);
    if (r.m) CHECK_MSG(!strcmp(r.hex, want), "%s: %s, want %s", src, r.hex, want);
}

static void err(const char *src, const char *code) {
    Res r = run(src);
    CHECK_MSG(!strcmp(r.codes, code), "%s: codes \"%s\", want \"%s\"", src, r.codes, code);
}

static void test_fixed(void) {
    /* the examples of the manual */
    hex(" DC FS24'6'\n", "06000000");
    hex(" DC FS24'6.5'\n", "06800000");
    hex(" DC HS6'-25.46'\n", "F9A3");
    hex(" DC HS12'3.50E-2'\n", "008F");
    hex(" DC FS4'-10,25.3,U268435455'\n", "FFFFFF60" "00000195" "FFFFFFF0");
    /* fractions and rounding without scale: half away from zero */
    hex(" DC F'1.5,-1.5,2.4,-2.4'\n", "00000002" "FFFFFFFE" "00000002" "FFFFFFFE");
    hex(" DC H'0.5'\n", "0001");
    /* negative scale */
    hex(" DC FS-2'12,14,-14'\n", "00000003" "00000004" "FFFFFFFC");
    /* exponent modifier and exponent in the nominal value add up */
    hex(" DC FE2'3'\n", "0000012C");
    hex(" DC F'3E2'\n", "0000012C");
    hex(" DC FE1'3E1'\n", "0000012C");
    hex(" DC FE-1'30'\n", "00000003");
    hex(" DC FE(1+1)'3'\n", "0000012C");
    hex(" DC FS(2+1)'1'\n", "00000008");
    hex(" DC FS+2E+1'1'\n", "00000028");
    /* the order is length, scale, exponent */
    hex(" DC FL2S4E1'1'\n", "00A0");
    /* U: unsigned */
    hex(" DC F'U4000000000'\n", "EE6B2800");
    hex(" DC FD'U18446744073709551615'\n", "FFFFFFFFFFFFFFFF");
    hex(" DC FD'-9223372036854775808'\n", "8000000000000000");
    /* previous behaviour */
    hex(" DC FL1'255,-128'\n", "FF80");
    hex(" DC F'+7,-7'\n", "00000007" "FFFFFFF9");
    err(" DC FL1'256'\n", "ZA2023");
    err(" DC FD'9223372036854775808'\n", "ZA2023");
    err(" DC FS4'U-1'\n", "ZA2022");
    err(" DC F'abc'\n", "ZA2022");
    err(" DC F'1.5.5'\n", "ZA2022");
    hex(" DC FS28'1'\n", "10000000");
    err(" DC FS32'1'\n", "ZA2023");
}

static void test_float(void) {
    hex(" DC E'1'\n", "41100000");
    hex(" DC ES1'1'\n", "42010000");
    hex(" DC ES2'1'\n", "43001000");
    hex(" DC EE2'1'\n", "42640000");
    hex(" DC EE1'1.5E2'\n", "435DC000");
    hex(" DC DL4S2E1'1'\n", "4300A000");
    hex(" DC EH'1'\n", "41100000");
    hex(" DC EHS1'1'\n", "42010000");
    hex(" DC EE-1'10'\n", "41100000");
    /* binary floating point: no scale, a large exponent range */
    hex(" DC EB'1'\n", "3F800000");
    hex(" DC EBE2'1'\n", "42C80000");
    err(" DC ES15'1'\n", "ZA2057");
    err(" DC ES-1'1'\n", "ZA2057");
    hex(" DC LS27'1'\n", "5C000000000000004E00000000000001");
    err(" DC LS29'1'\n", "ZA2057");
    hex(" DC DS13'1'\n", "4E00000000000001");
    err(" DC EE76'1'\n", "ZA2058");
    err(" DC EE-86'1'\n", "ZA2058");
    err(" DC EBE2147483648'1'\n", "ZA2058");
}

static void test_errors(void) {
    err(" DC FS347'1'\n", "ZA2057");
    hex(" DC FS346'0'\n", "00000000");
    err(" DC FS-188'1'\n", "ZA2057");
    err(" DC FE76'1'\n", "ZA2058");
    err(" DC FE-86'1'\n", "ZA2058");
    err(" DC PS2'1'\n", "ZA2056");
    err(" DC ZE2'1'\n", "ZA2056");
    err(" DC FS'1'\n", "ZA2014");
    err(" DC FS(NOPE)'1'\n", "ZA1010");
    {
        Res r = run(" DC EBS3'1'\n");
        CHECK_MSG(r.m != NULL && !r.codes[0] && r.warnings == 1, "EB scale: %s %d", r.codes,
                  r.warnings);
        CHECK_MSG(!strcmp(r.hex, "3F800000"), "EB scale value %s", r.hex);
    }
}

static void test_attrs(void) {
    Res r = run("X DC FS8'1'\n"
                "Y DC HS-3'8'\n"
                "Z DC FS8'1',F'2'\n"
                " DC A(S'X,S'Y,L'X,I'X,I'Y)\n");
    CHECK_MSG(r.m != NULL && !r.codes[0], "attrs: %s", r.codes);
    if (!r.m) return;
    /* X 0..3, Y 4..5, pad, Z 8..15, A 16.. */
    CHECK_STR(r.hex + 2 * 16 + 0, "00000008" "FFFFFFFD" "00000004" "00000017" "0000000F");
}

static void test_cu(void) {
    hex(" DC CU'AA'\n", "00410041");
    hex(" DC CU'ABC'\n", "004100420043");
    hex(" DC CU'a''b'\n", "00610027" "0062");
    hex(" DC CUL8'AB'\n", "00410042" "00200020");
    hex(" DC CUL2'AB'\n", "0041");
    hex(" DC CU'\xC3\xA9'\n", "00E9");              /* e acute, UTF-8 in the source */
    hex(" DC CU'\xE2\x82\xAC'\n", "20AC");           /* euro */
    hex(" DC CU'\xF0\x9F\x98\x80'\n", "D83DDE00");  /* U+1F600, a surrogate pair */
    err(" DC CUL2'\xF0\x9F\x98\x80'\n", "ZA2049");
    hex(" DC CUL4'\xF0\x9F\x98\x80'\n", "D83DDE00");
    hex(" DC 2CU'A'\n", "00410041");
    hex(" DS CU\n DC AL1(*-M)\n", "000002");
    hex("X DC CU'ABC'\n DC AL1(L'X)\n", "004100420043" "06");
    hex(" DC CUL4'A',CU'B'\n", "00410020" "0042");
    err(" DC CUL3'A'\n", "ZA2042");
    err(" DC CU''\n", "ZA2019");
    hex(" DC CUL2''\n", "0020");
}

static void test_ccw(void) {
    hex(" CCW0 X'06',DAT,X'40',80\nDAT DC X'AA'\n", "0600000840000050" "AA");
    hex(" CCW X'06',DAT,X'40',80\nDAT DC X'AA'\n", "0600000840000050" "AA");
    hex(" CCW1 X'0C',DAT,X'10',L'DAT\nDAT DC XL4'0'\n", "0C1000040000000" "8" "00000000");
    hex(" DC X'AA'\nX CCW 1,0,0,0\n DC AL1(L'X,T'X-C'W')\n", "AA00000000000000" "0100000000000000" "0800");
    hex(" CCW0 X'47',DAT+4,X'48',X'0100'\nDAT DS CL8\n", "4700000C48000100" "0000000000000000");
    hex(" CCW0 1,X'123456',0,0\n", "0112345600000000");
    hex(" CCW1 1,X'7FFFFFFF',0,0\n", "01000000" "7FFFFFFF");
    err(" CCW 256,0,0,0\n", "ZA2064");
    err(" CCW 1,0,256,0\n", "ZA2064");
    err(" CCW 1,0,0,65536\n", "ZA2064");
    err(" CCW 1,0,0\n", "ZA0010");
    err(" CCW1 1,NOPE,0,0\n", "ZA1011");
    err(" CCW 1,0,0,DAT\nDAT DC X'1'\n", "ZA1013");
    {
        Res r = run(" CCW0 1,0,3,0\n");
        CHECK_MSG(r.m != NULL && !r.codes[0] && r.warnings == 1, "CCW flags: %s %d", r.codes,
                  r.warnings);
    }
}

static void same(const char *a, const char *b) {
    Res x = run(a), y = run(b);
    CHECK_MSG(x.m && y.m && !x.codes[0] && !y.codes[0], "%s / %s do not assemble (%s %s)", a, b,
              x.codes, y.codes);
    CHECK_MSG(!strcmp(x.hex, y.hex), "%s: %s, but %s: %s", a, x.hex, b, y.hex);
}

static void warns(const char *src, const char *want, int nw) {
    Res r = run(src);
    CHECK_MSG(r.m != NULL && !r.codes[0], "%s: %s", src, r.codes);
    CHECK_MSG(r.warnings == nw, "%s: %d warnings, want %d", src, r.warnings, nw);
    CHECK_MSG(!strcmp(r.hex, want), "%s: %s, want %s", src, r.hex, want);
}

/* ten bit declet to three digits (IEEE 754-2008 / the decoder of the LLDB port) */
static void dpd_decode(unsigned d, int out[3]) {
    int p = (d >> 9) & 1, q = (d >> 8) & 1, r = (d >> 7) & 1, s = (d >> 6) & 1, t = (d >> 5) & 1,
        u = (d >> 4) & 1, v = (d >> 3) & 1, w = (d >> 2) & 1, x = (d >> 1) & 1, y = d & 1;
    if (!v) {
        out[0] = 4 * p + 2 * q + r, out[1] = 4 * s + 2 * t + u, out[2] = 4 * w + 2 * x + y;
    } else if (!w && !x) {
        out[0] = 4 * p + 2 * q + r, out[1] = 4 * s + 2 * t + u, out[2] = 8 + y;
    } else if (!w && x) {
        out[0] = 4 * p + 2 * q + r, out[1] = 8 + u, out[2] = 4 * s + 2 * t + y;
    } else if (w && !x) {
        out[0] = 8 + r, out[1] = 4 * s + 2 * t + u, out[2] = 4 * p + 2 * q + y;
    } else if (!s && !t) {
        out[0] = 8 + r, out[1] = 8 + u, out[2] = 4 * p + 2 * q + y;
    } else if (!s && t) {
        out[0] = 8 + r, out[1] = 4 * p + 2 * q + u, out[2] = 8 + y;
    } else if (s && !t) {
        out[0] = 4 * p + 2 * q + r, out[1] = 8 + u, out[2] = 8 + y;
    } else {
        out[0] = 8 + r, out[1] = 8 + u, out[2] = 8 + y;
    }
}

static void test_dpd(void) {
    int v;
    for (v = 0; v < 1000; v++) {
        char src[64];
        Res r;
        unsigned w;
        int dg[3];
        sprintf(src, " DC ED'%d'\n", v);
        r = run(src);
        w = (unsigned)strtoul(r.hex, NULL, 16); /* the 4 bytes */
        dpd_decode(w & 0x3FF, dg);
        CHECK_MSG(dg[0] * 100 + dg[1] * 10 + dg[2] == v, "DPD of %d decodes to %d%d%d", v, dg[0],
                  dg[1], dg[2]);
        sprintf(src, " DC ED'%d000'\n", v);
        r = run(src);
        w = (unsigned)strtoul(r.hex, NULL, 16);
        dpd_decode((w >> 10) & 0x3FF, dg);
        if (v < 10) continue; /* ED'000' strips to the digit 0, the same declet */
        CHECK_MSG(dg[0] * 100 + dg[1] * 10 + dg[2] == v, "high DPD of %d decodes to %d%d%d", v,
                  dg[0], dg[1], dg[2]);
    }
}

static void test_dfp(void) {
    hex(" DC ED'1'\n", "22500001");
    hex(" DC ED'-1'\n", "A2500001");
    hex(" DC ED'1.5'\n", "22400015");
    hex(" DC ED'0'\n", "22500000");
    hex(" DC ED'0.00'\n", "22300000");
    hex(" DC DD'1'\n", "2238000000000001");
    hex(" DC LD'1'\n", "22080000000000000000000000000001");
    hex(" DC ED'(INF)'\n", "78000000");
    hex(" DC ED'-(INF)'\n", "F8000000");
    hex(" DC ED'(QNAN)'\n", "7C000000");
    hex(" DC ED'(NAN)'\n", "7C000000");
    hex(" DC ED'(SNAN)'\n", "7E000000");
    hex(" DC DD'(INF)'\n", "7800000000000000");
    hex(" DC LD'(SNAN)'\n", "7E000000000000000000000000000000");
    same(" DC ED'(MAX)'\n", " DC ED'9999999E90'\n");
    same(" DC ED'(MIN)'\n", " DC ED'1000000E-101'\n");
    same(" DC ED'(DMIN)'\n", " DC ED'1E-101'\n");
    same(" DC DD'(MAX)'\n", " DC DD'9999999999999999E369'\n");
    same(" DC LD'(MAX)'\n", " DC LD'9999999999999999999999999999999999E6111'\n");
    same(" DC DDE2'1.5'\n", " DC DD'15E1'\n");
    same(" DC DD'12.50'\n", " DC DD'1250E-2'\n");
    same(" DC ED'1E96'\n", " DC ED'1000000E90'\n");
    /* rounding to 7 digits */
    same(" DC ED'12345678'\n", " DC ED'1234568E1'\n");
    same(" DC ED'12345675'\n", " DC ED'1234568E1'\n");
    same(" DC ED'12345665'\n", " DC ED'1234566E1'\n");
    same(" DC ED'12345678R9'\n", " DC ED'1234567E1'\n");
    same(" DC ED'12345671R14'\n", " DC ED'1234568E1'\n");
    same(" DC ED'12345651R15'\n", " DC ED'1234566E1'\n");
    same(" DC ED'12345650R15'\n", " DC ED'1234565E1'\n");
    same(" DC ED'-12345671R10'\n", " DC ED'-1234567E1'\n");
    same(" DC ED'-12345671R11'\n", " DC ED'-1234568E1'\n");
    same(" DC ED'12345675R13'\n", " DC ED'1234567E1'\n");
    same(" DC ED'12345675R12'\n", " DC ED'1234568E1'\n");
    same(" DC ED'9999999.6'\n", " DC ED'1000000E1'\n");
    {
        Res r = run(" DC ED'12345678'\n");
        CHECK_MSG(r.warnings == 1, "rounding warns: %d", r.warnings);
        r = run(" DC ED'1234567'\n");
        CHECK_MSG(r.warnings == 0, "exact does not warn: %d", r.warnings);
    }
}

static void test_dfp_range(void) {
    Res r = run(" DC ED'(MAX)'\n");
    char max[64];
    strcpy(max, r.hex);
    warns(" DC ED'1E97'\n", "78000000", 1);
    warns(" DC ED'-1E97R9'\n", "F7F3FCFF", 1);
    CHECK_STR(max, "77F3FCFF");
    warns(" DC ED'1E-110'\n", "00000000", 1);
    warns(" DC ED'12E-102'\n", "00000001", 1);
    same(" DC ED'12E-102'\n", " DC ED'1E-101'\n");
    err(" DC EDS1'1'\n", "ZA2057");
    err(" DC EDL5'1'\n", "ZA2053");
    err(" DC ED'1R4'\n", "ZA2053");
    err(" DC ED'1R7'\n", "ZA2053");
    err(" DC EB'1R8'\n", "ZA2053");
    err(" DC E'1R4'\n", "ZA2053");
    err(" DC ED'(MAX'\n", "ZA2050");
    err(" DC ED'abc'\n", "ZA2050");
    err(" DC E'(INF)'\n", "ZA2050");
}

static void test_bfp(void) {
    hex(" DC EB'0.1'\n", "3DCCCCCD");
    hex(" DC EB'0.1R5'\n", "3DCCCCCC");
    hex(" DC EB'0.1R6'\n", "3DCCCCCD");
    hex(" DC EB'-0.1R6'\n", "BDCCCCCC");
    hex(" DC EB'-0.1R7'\n", "BDCCCCCD");
    hex(" DC EB'0.1R1'\n", "3DCCCCCD");
    hex(" DC EH'0.1'\n", "4019999A");
    hex(" DC EH'0.1R5'\n", "40199999");
    hex(" DC EH'0.1R4'\n", "4019999A");
    hex(" DC EB'(INF)'\n", "7F800000");
    hex(" DC EB'-(INF)'\n", "FF800000");
    hex(" DC EB'(QNAN)'\n", "7FE00000");
    hex(" DC EB'(SNAN)'\n", "7FA00000");
    hex(" DC EB'(NAN)'\n", "7FC00000");
    hex(" DC EB'(max)'\n", "7F7FFFFF");
    hex(" DC EB'(MIN)'\n", "00800000");
    hex(" DC EB'(DMIN)'\n", "00000001");
    hex(" DC DB'(INF)'\n", "7FF0000000000000");
    hex(" DC LB'(INF)'\n", "7FFF0000000000000000000000000000");
    hex(" DC EH'(MAX)'\n", "7FFFFFFF");
    hex(" DC EH'-(MIN)'\n", "80100000");
    hex(" DC EH'(DMIN)'\n", "00000001");
    hex(" DC DH'(MAX)'\n", "7FFFFFFFFFFFFFFF");
    err(" DC EH'(INF)'\n", "ZA2050");
    warns(" DC EB'1E39'\n", "7F800000", 1);
    warns(" DC EB'1E39R5'\n", "7F7FFFFF", 1);
    warns(" DC EB'-1E39R6'\n", "FF7FFFFF", 1);
    warns(" DC EB'1E-50'\n", "00000000", 1);
    warns(" DC EB'0.1'\n", "3DCCCCCD", 0);
    hex(" DC EB'-0'\n", "80000000");
    hex(" DC X'AA'\n DC LQ'1'\n", "AA000000000000000000000000000000" "41100000000000003300000000000000");
}

int main(void) {
    test_bfp();
    test_dfp_range();
    test_dpd();
    test_dfp();
    test_ccw();
    test_cu();
    test_fixed();
    test_float();
    test_errors();
    test_attrs();
    TEST_MAIN_END();
}
