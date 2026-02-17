#define SZC_TEST
#include "subzeroclaw.c"
#include <assert.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("  %-40s ", name);
#define PASS() do { printf("✓\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("✗ %s\n", msg); tests_failed++; } while(0)

/* ======== TOOL TESTS ======== */

static void test_shell_echo(void) {
    TEST("shell: echo");
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
    TEST("TOOLS_JSON structure");
    cJSON *tools = cJSON_Parse(TOOLS_JSON);
    assert(tools);
    int n = cJSON_GetArraySize(tools);
    if (n == 1) PASS();
    else { char m[64]; snprintf(m, 64, "expected 1 tool, got %d", n); FAIL(m); }

    /* verify shell tool structure */
    cJSON *t0 = cJSON_GetArrayItem(tools, 0);
    cJSON *fn = cJSON_GetObjectItem(t0, "function");
    cJSON *name = cJSON_GetObjectItem(fn, "name");
    assert(name && strcmp(name->valuestring, "shell") == 0);

    cJSON_Delete(tools);
}

/* ======== RESPONSE PARSING TESTS ======== */

static void test_parse_stop_response(void) {
    TEST("parse_response: stop");
    const char *mock = "{"
        "\"choices\": [{"
        "  \"finish_reason\": \"stop\","
        "  \"message\": {"
        "    \"role\": \"assistant\","
        "    \"content\": \"Hello from the forest!\""
        "  }"
        "}]"
        "}";
    Response resp;
    int rc = parse_response(mock, &resp);
    if (rc == 0 &&
        strcmp(resp.finish_reason, "stop") == 0 &&
        resp.text && strstr(resp.text, "forest") &&
        resp.tool_calls == NULL)
        PASS();
    else
        FAIL("parse mismatch");
    response_free(&resp);
}

static void test_parse_tool_calls_response(void) {
    TEST("parse_response: tool_calls");
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
    Response resp;
    int rc = parse_response(mock, &resp);
    int ok = (rc == 0 &&
        strcmp(resp.finish_reason, "tool_calls") == 0 &&
        resp.text == NULL &&
        resp.tool_calls != NULL &&
        cJSON_GetArraySize(resp.tool_calls) == 1);
    if (ok) PASS();
    else FAIL("parse mismatch");
    response_free(&resp);
}

static void test_parse_error_response(void) {
    TEST("parse_response: API error");
    const char *mock = "{\"error\": {\"message\": \"rate limited\"}}";
    Response resp;
    int rc = parse_response(mock, &resp);
    if (rc == -1) PASS();
    else { FAIL("expected -1"); response_free(&resp); }
}

static void test_parse_garbage(void) {
    TEST("parse_response: garbage input");
    Response resp;
    int rc = parse_response("not json {{{", &resp);
    if (rc == -1) PASS();
    else { FAIL("expected -1"); response_free(&resp); }
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

/* ======== REQUEST BUILDING TESTS ======== */

static void test_build_request(void) {
    TEST("build_request: JSON structure");
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.model, MAX_VALUE, "test-model");
    cJSON *msgs = cJSON_CreateArray();
    cJSON_AddItemToArray(msgs, make_msg("system", "hello"));
    cJSON *tools = cJSON_Parse(TOOLS_JSON);
    char *json = build_request(&cfg, msgs, tools);
    assert(json);
    cJSON *req = cJSON_Parse(json);
    assert(req);
    cJSON *model = cJSON_GetObjectItem(req, "model");
    cJSON *m = cJSON_GetObjectItem(req, "messages");
    cJSON *t = cJSON_GetObjectItem(req, "tools");
    if (model && strcmp(model->valuestring, "test-model") == 0 &&
        m && cJSON_GetArraySize(m) == 1 &&
        t && cJSON_GetArraySize(t) == 1)
        PASS();
    else
        FAIL("request structure mismatch");
    cJSON_Delete(req); free(json);
    cJSON_Delete(msgs); cJSON_Delete(tools);
}

/* ======== CONFIG TESTS ======== */

static void test_config_no_key(void) {
    TEST("config: fails without API key");
    unsetenv("SUBZEROCLAW_API_KEY");
    unsetenv("SUBZEROCLAW_MODEL");
    unsetenv("SUBZEROCLAW_ENDPOINT");
    Config cfg;
    /* point config at nonexistent file so file parsing is skipped */
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", "/tmp/szc_no_config", 1);
    int rc = config_load(&cfg);
    if (old_home) { setenv("HOME", old_home, 1); free(old_home); }
    else unsetenv("HOME");
    if (rc == -1) PASS();
    else FAIL("expected failure");
}

static void test_config_defaults(void) {
    TEST("config: loads with env var");
    setenv("SUBZEROCLAW_API_KEY", "sk-test-fake-key", 1);
    Config cfg;
    int rc = config_load(&cfg);
    if (rc == 0 &&
        strcmp(cfg.api_key, "sk-test-fake-key") == 0 &&
        strstr(cfg.endpoint, "openrouter.ai"))
        PASS();
    else
        FAIL("config mismatch");
    unsetenv("SUBZEROCLAW_API_KEY");
}

/* ======== MAIN ======== */

int main(void) {
    printf("\n  SubZeroClaw test suite\n");
    printf("  ═══════════════════════════════════════════\n\n");

    test_shell_echo();
    test_shell_pipe();
    test_shell_stderr();
    test_unknown_tool();
    test_shell_bad_args();
    test_tools_definitions();
    test_parse_stop_response();
    test_parse_tool_calls_response();
    test_parse_error_response();
    test_parse_garbage();
    test_build_request();
    test_full_tool_dispatch();
    test_system_prompt();
    test_skills_loading();
    test_config_no_key();
    test_config_defaults();

    printf("\n  ═══════════════════════════════════════════\n");
    printf("  %d passed, %d failed\n\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
