#include <stdlib.h>
#include <string.h>

#include "../src/ebcdic.h"
#include "../src/goff.h"
#include "../src/json.h"
#define GOFFEDIT_NO_MAIN
#include "../src/goffedit.c"
#include "testlib.h"

static void test_roundtrip(void) {
    GoffWriter *w = goff_writer_new(1);
    GoffESD sd, ed, ld;
    GoffEnd end;
    const uint8_t *obj;
    size_t n;
    GoffModule *m, *m2;
    char errbuf[256];
    const char *err = NULL;
    char *js;
    Json *j;
    Buf out = {0};

    memset(&sd, 0, sizeof sd); sd.type = GOFF_SD; sd.name = (uint8_t *)"S"; sd.name_len = 1; sd.name_space = 1;
    goff_write_esd(w, &sd);
    memset(&ed, 0, sizeof ed); ed.type = GOFF_ED; ed.parent = sd.id; ed.name = (uint8_t *)"B_TEXT"; ed.name_len = 6;
    ed.name_space = 1; ed.length = 4; ed.attr.alignment = 3; goff_write_esd(w, &ed);
    memset(&ld, 0, sizeof ld); ld.type = GOFF_LD; ld.parent = ed.id; ld.name = (uint8_t *)"S"; ld.name_len = 1;
    ld.name_space = 1; goff_write_esd(w, &ld);
    { uint8_t data[4] = {1,2,3,4}; goff_write_text(w, ed.id, 0, GOFF_STYLE_BYTE, data, 4); }
    memset(&end, 0, sizeof end); goff_write_end(w, &end, &obj, &n);
    m = goff_parse(obj, n, &err);
    CHECK_MSG(m != NULL, "parse: %s", err ? err : "");
    if (!m) return;
    js = module_json(m);
    j = json_parse(js, strlen(js), &err);
    CHECK_MSG(j != NULL, "json: %s", err ? err : "");
    if (j) {
        const char *re = reverse_module(j, &out, errbuf, sizeof errbuf);
        CHECK_MSG(re == NULL, "reverse: %s", re ? re : "");
        if (!re) {
            m2 = goff_parse((const uint8_t *)out.p, out.len, &err);
            CHECK_MSG(m2 != NULL, "parse back: %s", err ? err : "");
            if (m2) {
                CHECK(m2->esds.n == 3 && m2->texts.n == 1 && m2->texts.v[0].len == 4);
                goff_free(m2);
            }
        }
        json_free(j);
    }
    free(out.p); free(js); goff_free(m); goff_writer_free(w);
}

int main(void) { test_roundtrip(); TEST_MAIN_END(); }
