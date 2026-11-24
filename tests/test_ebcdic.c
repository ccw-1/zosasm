#include <stdlib.h>

#include "../src/ebcdic.h"
#include "testlib.h"

int main(void) {
    static const struct { const char *in; unsigned char out; } known[] = {
        {"A", 0xC1}, {"a", 0x81}, {"0", 0xF0}, {" ", 0x40}, {"[", 0xAD}, {"]", 0xBD},
        {"^", 0x5F}, {"!", 0x5A}, {"|", 0x4F}, {"~", 0xA1}, {"_", 0x6D}, {"\n", 0x25},
        {"\\", 0xE0}, {"{", 0xC0}, {"}", 0xD0}, {"\"", 0x7F}, {"#", 0x7B}, {"@", 0x7C}};
    size_t i, n;
    int c;
    for (i = 0; i < sizeof known / sizeof known[0]; i++) {
        unsigned char *e = ebcdic_from_source(known[i].in, 1, &n);
        CHECK(n == 1 && e[0] == known[i].out);
        free(e);
    }
    /* every Latin-1 byte survives a round trip */
    for (c = 0; c < 256; c++) CHECK(za_to_latin1[za_to_ebcdic[c]] == c);
    {
        unsigned char *e = ebcdic_from_source("\xC3\xA9\xE2\x82\xAC", 5, &n); /* e-acute, euro */
        CHECK(n == 2 && e[0] == za_to_ebcdic[0xE9] && e[1] == za_to_ebcdic['?']);
        free(e);
    }
    TEST_MAIN_END();
}
