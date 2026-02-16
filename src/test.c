#define SZC_TEST
#include "subzeroclaw.c"
#include <assert.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("  %-40s ", name);
#define PASS() do { printf("✓\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("✗ %s\n", msg); tests_failed++; } while(0)

/* ======== TOOL TESTS ======== */

static void test_shell_ls(void) {
    TEST("shell: ls /tmp");
    char *r = tool_execute("shell", "{\"command\": \"echo hello_subzeroclaw\"}");
    assert(r);
    if (strstr(r, "hello_subzeroclaw")) PASS();
    else FAIL(r);
    free(r);
}

static void test_shell_pipe(void) {
    TEST("shell: pipe + grep");
    char *r = tool_execute("shell", "{\"command\": \"echo abc123def | grep -o '[0-9]\\\\+'\"}");
    assert(r);
    if (strstr(r, "123")) PASS();
    else FAIL(r);
    free(r);
}

static void test_shell_stderr(void) {
    TEST("shell: captures stderr");
    char *r = tool_execute("shell", "{\"command\": \"LC_ALL=C ls /nonexistent_path_xyz\"}");
    assert(r);
    if (strstr(r, "No such file") || strstr(r, "cannot access") || strstr(r, "error")) PASS();
    else FAIL(r);
    free(r);
}

static void test_write_read_file(void) {
    TEST("write_file + read_file roundtrip");
    char *w = tool_execute("write_file",
        "{\"path\": \"/tmp/subzeroclaw_test.txt\", \"content\": \"hola desde el bosque\\n\"}");
    assert(w);
    free(w);

    char *r = tool_execute("read_file", "{\"path\": \"/tmp/subzeroclaw_test.txt\"}");
    assert(r);
    if (strstr(r, "hola desde el bosque")) PASS();
    else FAIL(r);
    free(r);
    remove("/tmp/subzeroclaw_test.txt");
}

static void test_write_mkdir(void) {
    TEST("write_file: mkdir -p nested dirs");
    char *w = tool_execute("write_file",
        "{\"path\": \"/tmp/szc_test/nested/dir/file.txt\", \"content\": \"deep\"}");
    assert(w);
    if (strstr(w, "wrote")) PASS();
    else FAIL(w);
    free(w);
    system("rm -rf /tmp/szc_test");
}

static void test_unknown_tool(void) {
    TEST("unknown tool returns error");
    char *r = tool_execute("teleport", "{\"destination\": \"mars\"}");
    assert(r);
    if (strstr(r, "unknown tool")) PASS();
    else FAIL(r);
    free(r);
}

static void test_shell_bad_args(void) {
    TEST("shell: bad args JSON");
    char *r = tool_execute("shell", "not json at all");
    assert(r);
    if (strstr(r, "error")) PASS();
    else FAIL(r);
    free(r);
}

/* ======== JSON / TOOLS DEFINITION TESTS ======== */

static void test_tools_definitions(void) {
    TEST("tools_build_definitions structure");
    cJSON *tools = tools_build_definitions();
    assert(tools);
    int n = cJSON_GetArraySize(tools);
    if (n == 3) PASS();
    else { char m[64]; snprintf(m, 64, "expected 3 tools, got %d", n); FAIL(m); }

    /* verify shell tool structure */
    cJSON *t0 = cJSON_GetArrayItem(tools, 0);
    cJSON *fn = cJSON_GetObjectItem(t0, "function");
    cJSON *name = cJSON_GetObjectItem(fn, "name");
    assert(name && strcmp(name->valuestring, "shell") == 0);

    cJSON_Delete(tools);
}

/* ======== RESPONSE PARSING TESTS ======== */

static void test_parse_stop_response(void) {
    TEST("parse: stop response");
    const char *mock = "{"
        "\"choices\": [{"
        "  \"finish_reason\": \"stop\","
        "  \"message\": {"
        "    \"role\": \"assistant\","
        "    \"content\": \"Hello from the forest!\""
        "  }"
        "}]"
        "}";

    cJSON *root = cJSON_Parse(mock);
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *fr = cJSON_GetObjectItem(choice, "finish_reason");
    cJSON *msg = cJSON_GetObjectItem(choice, "message");
    cJSON *content = cJSON_GetObjectItem(msg, "content");

    if (strcmp(fr->valuestring, "stop") == 0 &&
        strstr(content->valuestring, "forest"))
        PASS();
    else
        FAIL("parse mismatch");

    cJSON_Delete(root);
}

static void test_parse_tool_calls_response(void) {
    TEST("parse: tool_calls response");
    const char *mock = "{"
        "\"choices\": [{"
        "  \"finish_reason\": \"tool_calls\","
        "  \"message\": {"
        "    \"role\": \"assistant\","
        "    \"content\": null,"
        "    \"tool_calls\": [{"
        "      \"id\": \"call_abc123\","
        "      \"type\": \"function\","
        "      \"function\": {"
        "        \"name\": \"shell\","
        "        \"arguments\": \"{\\\"command\\\": \\\"uname -a\\\"}\""
        "      }"
        "    }]"
        "  }"
        "}]"
        "}";

    cJSON *root = cJSON_Parse(mock);
    assert(root);
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *fr = cJSON_GetObjectItem(choice, "finish_reason");
    cJSON *msg = cJSON_GetObjectItem(choice, "message");
    cJSON *tc = cJSON_GetObjectItem(msg, "tool_calls");

    int ok = 1;
    if (strcmp(fr->valuestring, "tool_calls") != 0) ok = 0;
    if (cJSON_GetArraySize(tc) != 1) ok = 0;

    cJSON *call = cJSON_GetArrayItem(tc, 0);
    cJSON *fn = cJSON_GetObjectItem(call, "function");
    cJSON *fn_name = cJSON_GetObjectItem(fn, "name");
    cJSON *fn_args = cJSON_GetObjectItem(fn, "arguments");

    if (strcmp(fn_name->valuestring, "shell") != 0) ok = 0;

    /* parse the double-encoded args */
    cJSON *args = cJSON_Parse(fn_args->valuestring);
    if (!args) ok = 0;
    else {
        cJSON *cmd = cJSON_GetObjectItem(args, "command");
        if (!cmd || strcmp(cmd->valuestring, "uname -a") != 0) ok = 0;
        cJSON_Delete(args);
    }

    if (ok) PASS();
    else FAIL("tool_calls parse mismatch");

    cJSON_Delete(root);
}

/* ======== END-TO-END MOCK TEST ======== */

static void test_full_tool_dispatch(void) {
    TEST("e2e: parse tool_call -> execute -> result");
    const char *mock_args = "{\"command\": \"echo subzeroclaw_e2e_ok\"}";
    char *result = tool_execute("shell", mock_args);
    assert(result);

    cJSON *tool_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_msg, "role", "tool");
    cJSON_AddStringToObject(tool_msg, "tool_call_id", "call_test");
    cJSON_AddStringToObject(tool_msg, "content", result);

    cJSON *role = cJSON_GetObjectItem(tool_msg, "role");
    cJSON *content = cJSON_GetObjectItem(tool_msg, "content");

    if (strcmp(role->valuestring, "tool") == 0 &&
        strstr(content->valuestring, "subzeroclaw_e2e_ok"))
        PASS();
    else
        FAIL("e2e mismatch");

    cJSON_Delete(tool_msg);
    free(result);
}

/* ======== SYSTEM PROMPT / SKILLS TEST ======== */

static void test_system_prompt(void) {
    TEST("system prompt builds without crash");
    char *p = agent_build_system_prompt("/nonexistent_dir");
    assert(p);
    if (strstr(p, "SubZeroClaw")) PASS();
    else FAIL("missing base prompt");
    free(p);
}

static void test_skills_loading(void) {
    TEST("skills: loads .md files into prompt");
    system("mkdir -p /tmp/szc_skills");
    FILE *f = fopen("/tmp/szc_skills/email.md", "w");
    fprintf(f, "You can use himalaya for email.\n");
    fclose(f);

    char *p = agent_build_system_prompt("/tmp/szc_skills");
    assert(p);
    if (strstr(p, "himalaya")) PASS();
    else FAIL("skill not loaded");
    free(p);
    system("rm -rf /tmp/szc_skills");
}

/* ======== CONFIG TEST ======== */

static void test_config_defaults(void) {
    TEST("config: loads with env var");
    setenv("SUBZEROCLAW_API_KEY", "sk-test-fake-key", 1);
    Cfg cfg;
    int rc = config_load(&cfg);
    if (rc == 0 &&
        strcmp(cfg.key, "sk-test-fake-key") == 0 &&
        strstr(cfg.ep, "openrouter.ai"))
        PASS();
    else
        FAIL("config mismatch");
    unsetenv("SUBZEROCLAW_API_KEY");
}

/* ======== MAIN ======== */

int main(void) {
    printf("\n  SubZeroClaw test suite\n");
    printf("  ═══════════════════════════════════════════\n\n");

    test_shell_ls();
    test_shell_pipe();
    test_shell_stderr();
    test_write_read_file();
    test_write_mkdir();
    test_unknown_tool();
    test_shell_bad_args();
    test_tools_definitions();
    test_parse_stop_response();
    test_parse_tool_calls_response();
    test_full_tool_dispatch();
    test_system_prompt();
    test_skills_loading();
    test_config_defaults();

    printf("\n  ═══════════════════════════════════════════\n");
    printf("  %d passed, %d failed\n\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
