#include "../src/isa.h"
#include "testlib.h"

static void enc(const char *mn, const int64_t *vals, size_t n, const char *want, size_t wl) {
    size_t cnt;
    const IsaInst *in = isa_lookup(mn, &cnt);
    uint8_t out[6];
    char err[200];
    if (!in) {
        CHECK_MSG(0, "no instruction %s", mn);
        return;
    }
    if (isa_encode(in, vals, n, out, err, sizeof err)) {
        CHECK_MSG(0, "%s: %s", mn, err);
        return;
    }
    if (!t_hexeq(out, in->length, (const unsigned char *)want, wl)) {
        printf("%s = ", mn);
        t_hexprint(out, in->length);
        printf("\n");
        CHECK_MSG(0, "encoding of %s differs", mn);
    } else
        CHECK(1);
}

#define E(mn, want, ...)                                                    \
    do {                                                                    \
        int64_t v_[] = {__VA_ARGS__};                                       \
        enc(mn, v_, sizeof v_ / sizeof v_[0], want, sizeof want - 1);       \
    } while (0)

int main(void) {
    size_t n;
    char err[100];
    int64_t one[] = {1};
    int64_t bad[] = {16, 0};
    CHECK(isa_inst_count > 1500);
    E("lr", "\x18\x12", 1, 2);
    E("lgr", "\xB9\x04\x00\x12", 1, 2);
    E("br", "\x07\xFE", 14);
    E("nopr", "\x07\x00", 0);
    E("bras", "\xA7\xE5\x00\x05", 14, 5);
    E("larl", "\xC0\xC0\xFF\xFF\xFF\xFE", 12, -2);
    /* RX operands follow the table: R, D, X, B */
    E("be", "\x47\x80\xC0\x04", 4, 0, 12);
    E("l", "\x58\x12\x30\x04", 1, 4, 2, 3);
    E("mvc", "\xD2\x07\x10\x00\x20\x00", 0, 7, 1, 0, 2);
    E("ly", "\xE3\x12\x33\x45\x12\x58", 1, 0x12345, 2, 3);
    E("ly", "\xE3\x10\x3F\xFF\xFF\x58", 1, -1, 0, 3);
    E("vl", "\xE7\x10\x20\x00\x00\x06", 1, 0, 0, 2, 0);
    E("vl", "\xE7\x10\x20\x00\x08\x06", 17, 0, 0, 2, 0);
    CHECK(isa_lookup("LR", &n) && n >= 1);
    CHECK(!isa_lookup("nosuchinsn", &n));
    {
        const IsaInst *lr = isa_lookup("lr", &n);
        CHECK(isa_encode(lr, bad, 2, (uint8_t[6]){0}, err, sizeof err) != 0);
        CHECK(isa_encode(lr, one, 1, (uint8_t[6]){0}, err, sizeof err) != 0);
    }
    {
        size_t nm;
        const char *const *m = isa_mnemonics(&nm);
        CHECK(nm > 800 && m[0] != NULL);
    }
    TEST_MAIN_END();
}
