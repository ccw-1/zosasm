/* test_mcp.c - the JSON-RPC layer of the MCP server (src/mcp.c included
 * directly: the parser, base64, the workspace resolver and whole requests).
 * The server itself is also driven end to end by tests/run_cli_tests.sh. */
#define _XOPEN_SOURCE 700
#ifndef ZA_MCP_NO_MAIN
#define ZA_MCP_NO_MAIN
#endif
#include <sys/stat.h>
#include <unistd.h>

#include "../src/mcp.c"
#include "testlib.h"

/* ---- json_parse / jfree -------------------------------------------------- */

static void test_json_ok(void) {
    const char *err = NULL;
    J *j = json_parse("{\"a\":1,\"b\":[true,false,null],\"c\":\"x\\ny\\u00e9\"}", 46, &err);
    CHECK(j != NULL);
    if (!j) return;
    CHECK(j->t == J_OBJ && j->n == 3);
    CHECK(jget(j, "a") && jget(j, "a")->t == J_NUM && jget(j, "a")->num == 1);
    CHECK(jget(j, "b") && jget(j, "b")->t == J_ARR && jget(j, "b")->n == 3);
    CHECK(jget(j, "b")->v[0]->b == 1 && jget(j, "b")->v[1]->b == 0);
    CHECK(jget(j, "b")->v[2]->t == J_NULL);
    CHECK(jget(j, "c") && jget(j, "c")->t == J_STR);
    CHECK_STR(jstr(j, "c"), "x\ny\xc3\xa9");
    CHECK(jget(j, "nope") == NULL);
    jfree(j);
    /* numbers, exponents, nesting */
    j = json_parse("[0,-1,3.5,1e2,1E-2,1.5e+3]", strlen("[0,-1,3.5,1e2,1E-2,1.5e+3]"), &err);
    CHECK(j != NULL && j->n == 6);
    if (j) {
        CHECK(j->v[0]->num == 0 && j->v[1]->num == -1 && j->v[2]->num == 3.5);
        CHECK(j->v[3]->num == 100 && j->v[4]->num == 0.01 && j->v[5]->num == 1500);
        jfree(j);
    }
    /* surrogate pairs become one UTF-8 character */
    j = json_parse("\"\\ud83d\\ude00\"", 14, &err);
    CHECK(j != NULL && j->t == J_STR && j->n == 4);
    if (j) {
        CHECK_STR(j->s, "\xf0\x9f\x98\x80");
        jfree(j);
    }
    /* a lone surrogate becomes U+FFFD, not an error */
    j = json_parse("\"\\ud800\"", 8, &err);
    CHECK(j != NULL);
    jfree(j);
}

static void test_json_bad(void) {
    const char *err = NULL;
    static const char *bad[] = {
        "",            "empty",
        "{",           "unterminated object",
        "{\"a\"",      "missing value",
        "{\"a\":}",    "missing value 2",
        "{\"a\":1,}",  "trailing comma",
        "[1,]",        "trailing comma 2",
        "[1 2]",       "missing comma",
        "nul",         "broken null",
        "tru",         "broken true",
        "\"abc",       "unterminated string",
        "\"a\\q\"",    "bad escape",
        "\"a\\u00\"",  "short \\u",
        "01x",         "bad number",
        "{} {}",       "two documents",
        "hello",       "not json",
        "{'a':1}",     "single quotes",
        "[\"a\nb\"]",  "raw newline in a string",
    };
    size_t i;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i += 2) {
        J *j = json_parse(bad[i], strlen(bad[i]), &err);
        CHECK_MSG(j == NULL, "%s: parsed anyway", bad[i + 1]);
        if (j) jfree(j);
    }
    /* nesting is limited */
    {
        Buf b = {0};
        int k;
        J *j;
        for (k = 0; k < 100; k++) buf_putc(&b, '[');
        for (k = 0; k < 100; k++) buf_putc(&b, ']');
        j = json_parse(b.p, b.len, &err);
        CHECK_MSG(j == NULL, "depth limit");
        CHECK_MSG(err && !strcmp(err, "nesting too deep"), "depth message: %s", err ? err : "(none)");
        if (j) jfree(j);
        free(b.p);
    }
}

/* ---- base64 -------------------------------------------------------------- */

static void test_b64(void) {
    size_t n = 0;
    unsigned char *d;
    d = b64_decode("", 0, &n);
    CHECK(d != NULL && n == 0);
    free(d);
    d = b64_decode("Zg==", 4, &n);
    CHECK(d != NULL && n == 1 && d[0] == 'f');
    free(d);
    d = b64_decode("Zm9vYmFy", 8, &n);
    CHECK(d != NULL && n == 6 && !memcmp(d, "foobar", 6));
    free(d);
    /* padding and blanks are skipped, the length need not be a multiple of 4 */
    d = b64_decode("Zm9vYmF", 7, &n);
    CHECK(d != NULL && n == 5 && !memcmp(d, "fooba", 5));
    free(d);
    d = b64_decode("Zm9v\nYmFy", 9, &n);
    CHECK(d != NULL && n == 6 && !memcmp(d, "foobar", 6));
    free(d);
    d = b64_decode("====", 4, &n);
    CHECK(d != NULL && n == 0);
    free(d);
    d = b64_decode("Zm9*", 4, &n);
    CHECK(d == NULL);
}

/* ---- ws_resolve ---------------------------------------------------------- */

static void test_ws(void) {
    const char *msg = NULL;
    char *p;
    CHECK(system("rm -rf build/mcp_ws && mkdir -p build/mcp_ws/sub") == 0);
    CHECK(write_whole("build/mcp_ws/f.za", "T CSECT\n", 8));
    g_unrestricted = 0;
    free(g_workspace);
    g_workspace = realpath("build/mcp_ws", NULL);
    CHECK(g_workspace != NULL);

    /* an existing file inside: canonical and unchanged in meaning */
    p = ws_resolve("f.za", "'path'", &msg);
    CHECK_MSG(p != NULL, "inside: %s", msg ? msg : "");
    if (p) {
        CHECK_MSG(!strcmp(p, g_workspace) || !strncmp(p, g_workspace, strlen(g_workspace)), "prefix");
        CHECK_MSG(strstr(p, "/f.za"), "file name kept: %s", p);
        free(p);
    }
    /* an absolute name that is inside */
    {
        char *abs = xasprintf("%s/f.za", g_workspace);
        p = ws_resolve(abs, "'path'", &msg);
        CHECK_MSG(p != NULL, "abs inside: %s", msg ? msg : "");
        free(p);
        free(abs);
    }
    /* an output that does not exist yet, also nested, with . and .. inside */
    p = ws_resolve("out/o1.o", "'output'", &msg);
    CHECK_MSG(p != NULL && strstr(p, "/out/o1.o"), "new output: %s (%s)", p ? p : "", msg ? msg : "");
    free(p);
    p = ws_resolve("a/./b/../c.o", "'output'", &msg);
    CHECK_MSG(p != NULL && strstr(p, "/a/c.o"), "dot and dotdot: %s (%s)", p ? p : "", msg ? msg : "");
    free(p);
    /* ".." that stays inside is fine */
    p = ws_resolve("sub/../g.o", "'output'", &msg);
    CHECK_MSG(p != NULL && strstr(p, "/g.o"), "dotdot inside: %s", p ? p : "");
    free(p);

    /* outside: absolute, .., a symlink, a missing directory */
    p = ws_resolve("/etc/passwd", "'path'", &msg);
    CHECK(p == NULL && msg && strstr(msg, "outside the workspace"));
    p = ws_resolve("../f.za", "'path'", &msg);
    CHECK(p == NULL);
    p = ws_resolve("sub/../../f.za", "'path'", &msg);
    CHECK(p == NULL);
    CHECK(symlink("/etc", "build/mcp_ws/sub/esc") == 0);
    p = ws_resolve("sub/esc/passwd", "'path'", &msg);
    CHECK(p == NULL);
    p = ws_resolve("/no_such_dir_zzz/x.za", "'path'", &msg);
    CHECK(p == NULL && msg && strstr(msg, "does not exist"));
    p = ws_resolve("", "'path'", &msg);
    CHECK(p == NULL);
    remove("build/mcp_ws/sub/esc");

    /* ZOSASM_UNRESTRICTED lifts the limit */
    g_unrestricted = 1;
    p = ws_resolve("/etc/passwd", "'path'", &msg);
    CHECK_MSG(p != NULL && !strcmp(p, "/etc/passwd"), "unrestricted");
    free(p);
    g_unrestricted = 0;
}

/* ---- whole requests (stdout is captured) ---------------------------------- */

/* runs handle() on one JSON-RPC line and returns the reply */
static char *rpc(const char *line) {
    int save = dup(1);
    FILE *cap = tmpfile();
    char *out;
    long n;
    const char *err = NULL;
    J *msg;
    fflush(stdout);
    dup2(fileno(cap), 1);
    msg = json_parse(line, strlen(line), &err);
    CHECK_MSG(msg != NULL, "rpc parse: %s", line);
    if (msg) {
        handle(msg);
        jfree(msg);
    }
    fflush(stdout);
    dup2(save, 1);
    close(save);
    n = ftell(cap);
    rewind(cap);
    out = xmalloc((size_t)n + 1);
    n = (long)fread(out, 1, (size_t)n, cap);
    out[n] = 0;
    fclose(cap);
    return out;
}

static void test_requests(void) {
    char *r;
    locate("build/zosasm-mcp"); /* finds the guide and the programs */
    g_unrestricted = 0;
    free(g_workspace);
    g_workspace = realpath(".", NULL); /* the tests run in the project root */

    r = rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
    CHECK_MSG(strstr(r, "\"serverInfo\":{\"name\":\"zosasm-mcp\"") != NULL, "initialize");
    CHECK_MSG(strstr(r, "GUIDE.md") != NULL, "initialize names the guide");
    free(r);

    r = rpc("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
    CHECK_MSG(strstr(r, "zosasm_assemble") != NULL, "tools/list");
    CHECK_MSG(strstr(r, "ZOSASM_UNRESTRICTED") != NULL, "tools/list documents the workspace");
    free(r);

    r = rpc("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"no_such_method\"}");
    CHECK_MSG(strstr(r, "-32601") != NULL, "unknown method");
    free(r);

    r = rpc("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"nosuch_tool\"}}");
    CHECK_MSG(strstr(r, "-32602") != NULL, "unknown tool");
    free(r);

    /* wrong types for tool fields are errors, not crashes */
    r = rpc("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"zosasm_assemble\","
            "\"arguments\":{\"source\":\"T CSECT\\n\",\"macro_steps\":\"many\"}}}");
    CHECK_MSG(strstr(r, "\"isError\":true") != NULL && strstr(r, "macro_steps") != NULL, "bad macro_steps");
    free(r);
    r = rpc("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"zosasm_assemble\","
            "\"arguments\":{\"source\":42}}}");
    CHECK_MSG(strstr(r, "\"isError\":true") != NULL, "source of the wrong type");
    free(r);

    /* the workspace is in force for handle() too */
    r = rpc("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"zosasm_assemble\","
            "\"arguments\":{\"path\":\"/etc/passwd\"}}}");
    CHECK_MSG(strstr(r, "outside the workspace") != NULL, "path outside");
    free(r);

    /* notifications get no reply */
    {
        int save = dup(1);
        FILE *cap = tmpfile();
        const char *err = NULL;
        J *msg;
        long n;
        char *out;
        fflush(stdout);
        dup2(fileno(cap), 1);
        msg = json_parse("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
                     strlen("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"), &err);
        CHECK(msg != NULL);
        if (msg) {
            handle(msg);
            jfree(msg);
        }
        fflush(stdout);
        dup2(save, 1);
        close(save);
        n = ftell(cap);
        rewind(cap);
        out = xmalloc((size_t)n + 1);
        n = (long)fread(out, 1, (size_t)n, cap);
        out[n] = 0;
        fclose(cap);
        CHECK_MSG(n == 0, "notification answered: %s", out);
        free(out);
    }
}

int main(void) {
    test_json_ok();
    test_json_bad();
    test_b64();
    test_ws();
    test_requests();
    TEST_MAIN_END();
}
