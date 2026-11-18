#include <stdlib.h>

#include "../src/diag.h"
#include "testlib.h"

/* render all diagnostics to a malloc'ed string */
static char *render(Reporter *r) {
    FILE *f = tmpfile();
    long n;
    char *s;
    rep_print(r, f, 0);
    n = ftell(f);
    rewind(f);
    s = malloc((size_t)n + 1);
    n = (long)fread(s, 1, (size_t)n, f);
    s[n] = 0;
    fclose(f);
    return s;
}

static void test_excerpt(void) {
    Reporter *r = rep_new(0);
    const char *src = "MAIN     CSECT\n         LGRR  1,2\n         END\n";
    char *out;
    rep_add_source(r, "t.za", src, strlen(src));
    diag_help(diag_label(rep_errorf(r, "ZA0101", span_make("t.za", 2, 10, 4), "unknown instruction \"%s\"", "LGRR"),
                         "not a known mnemonic"),
              "did you mean \"%s\"?", "LGR");
    out = render(r);
    CHECK_STR(out,
              "error[ZA0101]: unknown instruction \"LGRR\"\n"
              "  --> t.za:2:10\n"
              "  |\n"
              "2 |          LGRR  1,2\n"
              "  |          ^^^^ not a known mnemonic\n"
              "  help: did you mean \"LGR\"?\n"
              "1 error, 0 warnings\n");
    free(out);
}

static void test_sorted_and_limit(void) {
    Reporter *r = rep_new(2);
    size_t n;
    const Diag **s;
    rep_errorf(r, "A", span_make("f", 5, 1, 0), "late");
    rep_warnf(r, "B", span_make("f", 1, 1, 0), "early");
    CHECK(!rep_too_many(r));
    rep_errorf(r, "C", span_make("f", 3, 1, 0), "mid");
    CHECK(rep_too_many(r));
    s = rep_sorted(r, &n);
    CHECK(n == 3 && !strcmp(s[0]->code, "B") && !strcmp(s[1]->code, "C") && !strcmp(s[2]->code, "A"));
    CHECK(rep_error_count(r) == 2 && rep_warning_count(r) == 1);
    free(s);
}

static void test_notes_and_tabs(void) {
    Reporter *r = rep_new(0);
    const char *src = "X\tEQU 1\nX\tEQU 2\n";
    char *out;
    rep_add_source(r, "a", src, strlen(src));
    diag_note(rep_errorf(r, "E", span_make("a", 2, 1, 1), "duplicate symbol X"), span_make("a", 1, 1, 1),
              "previously defined here");
    out = render(r);
    CHECK(strstr(out, "note: previously defined here") != NULL);
    CHECK(strstr(out, "--> a:1:1") != NULL);
    free(out);
}

static void test_warn_error(void) {
    Reporter *r = rep_new(0);
    rep_set_warn_error(r, 1);
    rep_warnf(r, "W", span_make("f", 1, 1, 0), "careful");
    CHECK(rep_error_count(r) == 1 && rep_warning_count(r) == 1);
}

static void test_closest(void) {
    const char *c[] = {"LGR", "LR", "LGFR", "STM", "CSECT", "DSECT"};
    const char *in[] = {"LGRR", "CSET", "STN", "DSEC", "XYZZY"};
    const char *want[] = {"LGR", "CSECT", "STM", "DSECT", NULL};
    size_t i;
    for (i = 0; i < 5; i++) {
        const char *got = diag_closest(in[i], c, 6);
        CHECK_MSG((got == NULL) == (want[i] == NULL) && (!got || !strcmp(got, want[i])), "Closest(%s) = %s", in[i],
                  got ? got : "(none)");
    }
    /* a long token (hostile input) is skipped instead of an O(n*len) scan */
    {
        static char big[1000];
        memset(big, 'A', sizeof big - 1);
        CHECK(diag_closest(big, c, 6) == NULL);
    }
}

int main(void) {
    test_excerpt();
    test_sorted_and_limit();
    test_notes_and_tabs();
    test_warn_error();
    test_closest();
    TEST_MAIN_END();
}
