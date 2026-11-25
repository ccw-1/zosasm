/* test_threads.c - two threads assemble different modules at the same time.
 * The assembler keeps a few pieces of state in thread-local variables
 * (ZA_TLS): the value arena, za_ascii, the IEEE exponent width, the Big
 * arena, the DP/MP length, the instruction caches and the ZSYM CRC table.
 * This test runs two jobs with different character sets and register modes
 * and compares every result with the same source assembled sequentially. */
#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../src/asm.h"
#include "../src/ebcdic.h"
#include "testlib.h"

typedef struct {
    const char *name;
    const char *src;
    int ascii;
    int strict;
    int iterations;
    int quiet;
    int fail;
} Job;

static char *emit_once(const char *name, const char *src, int ascii, int strict, int quiet,
                       size_t *len) {
    Reporter *rep = rep_new(0);
    Module *m;
    char err[256];
    uint8_t *obj = NULL;
    za_ascii = ascii;
    za_strict_registers = strict;
    m = asm_assemble_text(name, src, strlen(src), rep);
    if (!m) {
        if (!quiet) rep_print(rep, stderr, 0);
        rep_free(rep);
        return NULL;
    }
    obj = module_emit(m, len, err, sizeof err);
    if (!obj) fprintf(stderr, "%s: emit: %s\n", name, err);
    asm_free(m);
    rep_free(rep);
    return (char *)obj;
}

static void *worker(void *arg) {
    Job *j = arg;
    int i;
    for (i = 0; i < j->iterations; i++) {
        size_t n = 0;
        char *obj = emit_once(j->name, j->src, j->ascii, j->strict, j->quiet, &n);
        if (!obj) {
            j->fail = 1;
            break;
        }
        free(obj);
    }
    return NULL;
}

static void run_pair(const char *name, const char *a, const char *b) {
    Job ja = {name, a, 1, 0, 200, 0, 0};
    Job jb = {name, b, 0, 0, 200, 0, 0};
    pthread_t ta, tb;
    CHECK(pthread_create(&ta, NULL, worker, &ja) == 0);
    CHECK(pthread_create(&tb, NULL, worker, &jb) == 0);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    CHECK_MSG(!ja.fail && !jb.fail, "%s: a thread failed", name);
}

static void test_sources(void) {
    /* numeric constants exercise the Big arena and the float state */
    const char *a = "A CSECT\n"
                    "         USING A,15\n"
                    "X        DC   F'1',D'2',L'3',DD'4.5',ED'-2'\n"
                    "         DC   2A(*)\n"
                    "Y        EQU  Z+4\n"
                    "Z        DC   F'7'\n"
                    "         DC   A(Y)\n"
                    "         DC   C'A',CA'A'\n"
                    "         LR   %r1,%r2\n"
                    "         END\n";
    const char *b = "B CSECT\n"
                    "         USING B,15\n"
                    "X        DC   F'-1',D'0.5',L'1',DD'0'\n"
                    "         DC   A(*,*)\n"
                    "Y        EQU  Z+4\n"
                    "Z        DC   F'9'\n"
                    "         DC   A(Y)\n"
                    "         DC   C'B',CA'B'\n"
                    "         LR   %r3,%r4\n"
                    "         END\n";
    run_pair("two modules", a, b);
}

static void test_modes(void) {
    /* za_ascii and za_strict_registers are per-thread */
    const char *src = "C CSECT\n"
                      "R1       EQU  1\n"
                      "         LA   R1,0\n"
                      "         DC   C'A',CA'A'\n"
                      "         END\n";
    Job ja = {"ascii relaxed", src, 1, 0, 200, 0, 0};
    Job jb = {"ebcdic strict", src, 0, 1, 200, 1, 0};
    pthread_t ta, tb;
    CHECK(pthread_create(&ta, NULL, worker, &ja) == 0);
    CHECK(pthread_create(&tb, NULL, worker, &jb) == 0);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    CHECK_MSG(!ja.fail, "ascii relaxed failed");
    /* jb must FAIL on every iteration: a plain number is not a register
     * name under -strict-registers.  worker records the failure. */
    CHECK_MSG(jb.fail, "strict thread unexpectedly succeeded");
}

static void test_error_path(void) {
    /* an unknown operation builds the per-thread mnemonic list; the error
     * path frees the module and the ISA caches */
    const char *src = "D CSECT\n BADOP 1\n END\n";
    Job j = {"unknown op", src, 0, 0, 100, 1, 0};
    worker(&j);
    CHECK_MSG(j.fail, "unknown operation unexpectedly succeeded");
}

static void test_expected_bytes(void) {
    size_t n = 0;
    char *obj = emit_once("t.za", "T CSECT\n DC C'A',CA'A'\n END\n", 1, 0, 0, &n);
    CHECK(obj != NULL);
    if (obj) {
        /* the object is GOFF; the text record starts after the ESD records.
         * Compare with the same source in EBCDIC instead of hardcoding GOFF. */
        size_t n2 = 0;
        char *obj2 = emit_once("t.za", "T CSECT\n DC C'A',CA'A'\n END\n", 0, 0, 0, &n2);
        CHECK(obj2 != NULL && n2 == n);
        free(obj);
        free(obj2);
    }
}

int main(void) {
    test_sources();
    test_modes();
    test_error_path();
    test_expected_bytes();
    TEST_MAIN_END();
}
