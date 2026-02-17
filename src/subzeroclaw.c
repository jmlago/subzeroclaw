/* subzeroclaw.c — skill-driven agentic runtime */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cjson/cJSON.h>

#define MAX_PATH   512
#define MAX_VALUE  1024
#define MAX_OUTPUT (128 * 1024)

typedef struct {
    char api_key[MAX_VALUE], model[MAX_VALUE], endpoint[MAX_VALUE];
    char skills_dir[MAX_PATH], log_dir[MAX_PATH];
    int  max_turns, max_messages;
} Config;

static void config_parse_line(Config *cfg, const char *key, const char *val) {
    if      (!strcmp(key, "api_key"))      snprintf(cfg->api_key,    MAX_VALUE, "%s", val);
    else if (!strcmp(key, "model"))        snprintf(cfg->model,      MAX_VALUE, "%s", val);
    else if (!strcmp(key, "endpoint"))     snprintf(cfg->endpoint,   MAX_VALUE, "%s", val);
    else if (!strcmp(key, "skills_dir"))   snprintf(cfg->skills_dir, MAX_PATH,  "%s", val);
    else if (!strcmp(key, "log_dir"))      snprintf(cfg->log_dir,    MAX_PATH,  "%s", val);
    else if (!strcmp(key, "max_turns"))    cfg->max_turns    = atoi(val);
    else if (!strcmp(key, "max_messages")) cfg->max_messages = atoi(val);
}

int config_load(Config *cfg) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->endpoint,   MAX_VALUE, "https://openrouter.ai/api/v1/chat/completions");
    snprintf(cfg->model,      MAX_VALUE, "anthropic/claude-sonnet-4-20250514");
    snprintf(cfg->skills_dir, MAX_PATH,  "%s/.subzeroclaw/skills", home);
    snprintf(cfg->log_dir,    MAX_PATH,  "%s/.subzeroclaw/logs", home);
    cfg->max_turns = 200; cfg->max_messages = 40;

    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s/.subzeroclaw/config", home);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[2048];
        while (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            while (len && strchr("\n\r ", line[len - 1])) line[--len] = '\0';
            char *s = line; while (*s == ' ') s++;
            if (*s == '#' || *s == '\0') continue;
            char *eq = strchr(s, '='); if (!eq) continue; *eq = '\0';
            char *key = s, *val = eq + 1;
            len = strlen(key); while (len && key[len-1] == ' ') key[--len] = '\0';
            while (*val == ' ') val++;
            len = strlen(val);
            if (len >= 2 && val[0] == '"' && val[len-1] == '"') { val++; val[len-2] = '\0'; }
            config_parse_line(cfg, key, val);
        }
        fclose(f);
    }
    char *v;
    if ((v = getenv("SUBZEROCLAW_API_KEY")))  snprintf(cfg->api_key,  MAX_VALUE, "%s", v);
    if ((v = getenv("SUBZEROCLAW_MODEL")))    snprintf(cfg->model,    MAX_VALUE, "%s", v);
    if ((v = getenv("SUBZEROCLAW_ENDPOINT"))) snprintf(cfg->endpoint, MAX_VALUE, "%s", v);
    if (!cfg->api_key[0]) { fprintf(stderr, "error: no api_key\n"); return -1; }
    return 0;
}

static void mkdirp(const char *path) {
    char tmp[MAX_PATH];
    snprintf(tmp, MAX_PATH, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static void log_write(FILE *log, const char *role, const char *content) {
    if (!log) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
    fprintf(log, "[%s] %s: %s\n", ts, role, content); fflush(log);
}

static char *read_file(const char *path, long max_size) {
    FILE *f = fopen(path, "r"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    if (max_size > 0 && size > max_size) size = max_size;
    char *buf = malloc(size + 1);
    buf[fread(buf, 1, size, f)] = '\0';
    fclose(f);
    return buf;
}

static int write_temp(const char *prefix, const char *data, char *out, size_t out_size) {
    snprintf(out, out_size, "/tmp/.szc_%s_XXXXXX", prefix);
    int fd = mkstemp(out); if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(out); return -1; }
    fputs(data, f); fclose(f);
    return 0;
}

static char *generate_session_id(void) {
    static char sid[32];
    FILE *r = fopen("/dev/urandom", "r");
    if (r) {
        unsigned char b[8];
        if (fread(b, 1, 8, r) == 8)
            snprintf(sid, sizeof(sid), "%02x%02x%02x%02x%02x%02x%02x%02x",
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        fclose(r);
    }
    if (!sid[0]) snprintf(sid, sizeof(sid), "%lx%x", (long)time(NULL), getpid());
    return sid;
}

static char *http_post(const char *url, const char *api_key, const char *body) {
    char body_path[64], hdr_path[64];
    if (write_temp("body", body, body_path, sizeof(body_path)) < 0) return NULL;
    char hdr[MAX_VALUE + 64];
    snprintf(hdr, sizeof(hdr), "-H \"Authorization: Bearer %s\"", api_key);
    if (write_temp("hdr", hdr, hdr_path, sizeof(hdr_path)) < 0) { unlink(body_path); return NULL; }

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "curl -s -m 120 -K '%s' -H 'Content-Type: application/json' -d @'%s' '%s' 2>&1",
        hdr_path, body_path, url);
    FILE *fp = popen(cmd, "r");
    if (!fp) { unlink(body_path); unlink(hdr_path); return NULL; }

    size_t cap = 65536, len = 0, n;
    char *buf = malloc(cap);
    while (buf && (n = fread(buf + len, 1, cap - len - 1, fp)) > 0) {
        len += n;
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
    }
    if (buf) buf[len] = '\0';
    pclose(fp); unlink(body_path); unlink(hdr_path);
    return buf;
}

static const char TOOLS_JSON[] =
    "[{\"type\":\"function\",\"function\":{\"name\":\"shell\","
    "\"description\":\"Run a shell command\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}}}]";

static const char *arg_str(cJSON *args, const char *field) {
    cJSON *item = cJSON_GetObjectItem(args, field);
    return (item && cJSON_IsString(item)) ? item->valuestring : NULL;
}

char *tool_execute(const char *name, const char *args_json) {
    if (strcmp(name, "shell")) return strdup("error: unknown tool");
    cJSON *args = cJSON_Parse(args_json);
    const char *cmd = arg_str(args, "command");
    if (!cmd) { if (args) cJSON_Delete(args); return strdup("error: missing 'command'"); }
    size_t len = strlen(cmd);
    char *full = malloc(len + 8);
    memcpy(full, cmd, len); memcpy(full + len, " 2>&1", 6);
    if (args) cJSON_Delete(args);
    FILE *fp = popen(full, "r"); free(full);
    if (!fp) return strdup("error: popen failed");
    char *out = malloc(MAX_OUTPUT);
    size_t total = 0, n;
    while ((n = fread(out + total, 1, MAX_OUTPUT - total - 1, fp)) > 0) {
        total += n; if (total >= MAX_OUTPUT - 1) break;
    }
    out[total] = '\0'; pclose(fp);
    if (total + 1 < MAX_OUTPUT / 2) { char *s = realloc(out, total + 1); if (s) out = s; }
    return out;
}

cJSON *tools_build_definitions(void) { return cJSON_Parse(TOOLS_JSON); }

char *agent_build_system_prompt(const char *skills_dir) {
    size_t cap = 8192;
    char *prompt = malloc(cap);
    size_t len = snprintf(prompt, cap,
        "You are SubZeroClaw, a minimal agentic assistant.\n"
        "You have one tool: shell. Use it to run any command.\n"
        "For files, use cat, tee, sed, etc. Be concise. Just do it.\n\n");
    DIR *d = opendir(skills_dir); if (!d) return prompt;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        size_t nlen = strlen(entry->d_name);
        if (nlen < 4 || strcmp(entry->d_name + nlen - 3, ".md") != 0) continue;
        char fp[MAX_PATH]; snprintf(fp, MAX_PATH, "%s/%s", skills_dir, entry->d_name);
        char *content = read_file(fp, 0); if (!content) continue;
        size_t clen = strlen(content);
        while (len + clen + 128 >= cap) { cap *= 2; prompt = realloc(prompt, cap); }
        len += snprintf(prompt + len, cap - len, "\n--- SKILL: %s ---\n", entry->d_name);
        memcpy(prompt + len, content, clen); len += clen; prompt[len] = '\0';
        free(content);
    }
    closedir(d);
    return prompt;
}

typedef struct {
    char *finish_reason, *text;
    cJSON *tool_calls, *msg;
} Response;

static void response_free(Response *r) {
    if (r->finish_reason) free(r->finish_reason);
    if (r->msg) cJSON_Delete(r->msg);
}

/* references avoid copying the full message array */
static char *build_request(const Config *cfg, cJSON *msgs, cJSON *tools) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", cfg->model);
    cJSON_AddItemReferenceToObject(req, "messages", msgs);
    if (tools) cJSON_AddItemReferenceToObject(req, "tools", tools);
    char *json = cJSON_PrintUnformatted(req);
    cJSON_DetachItemFromObject(req, "messages");
    if (tools) cJSON_DetachItemFromObject(req, "tools");
    cJSON_Delete(req);
    return json;
}

static int parse_response(const char *body, Response *out) {
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(body); if (!root) return -1;
    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (err) {
        cJSON *m = cJSON_GetObjectItem(err, "message");
        fprintf(stderr, "API error: %s\n", m ? m->valuestring : "unknown");
        cJSON_Delete(root); return -1;
    }
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!choices || !choices->child) { cJSON_Delete(root); return -1; }
    cJSON *choice  = choices->child;
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    if (!message) { cJSON_Delete(root); return -1; }
    cJSON *fr = cJSON_GetObjectItem(choice, "finish_reason");
    out->finish_reason = strdup(fr && cJSON_IsString(fr) ? fr->valuestring : "stop");
    out->msg = cJSON_DetachItemFromObject(choice, "message");
    cJSON *ct = cJSON_GetObjectItem(out->msg, "content");
    out->text = (ct && cJSON_IsString(ct)) ? ct->valuestring : NULL;
    out->tool_calls = cJSON_GetObjectItem(out->msg, "tool_calls");
    cJSON_Delete(root);
    return 0;
}

static cJSON *make_msg(const char *role, const char *content) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content);
    return m;
}

static int compact_messages(const Config *cfg, cJSON *msgs, FILE *log) {
    int total = cJSON_GetArraySize(msgs);
    if (total <= cfg->max_messages) return 0;
    fprintf(stderr, "[compact] %d msgs, summarizing\n", total);
    log_write(log, "SYS", "compacting context");

    /* send the full conversation to the model for summarization */
    char *convo = cJSON_PrintUnformatted(msgs);
    size_t plen = strlen(convo) + 256;
    char *prompt = malloc(plen);
    snprintf(prompt, plen,
        "Summarize this conversation. Keep all facts, file paths, commands, "
        "and decisions. Be concise.\n\n%s", convo);
    free(convo);

    cJSON *sm = cJSON_CreateArray();
    cJSON_AddItemToArray(sm, make_msg("user", prompt)); free(prompt);
    char *rj = build_request(cfg, sm, NULL);
    char *rb = http_post(cfg->endpoint, cfg->api_key, rj);
    free(rj); cJSON_Delete(sm);
    if (!rb) return -1;

    Response resp;
    if (parse_response(rb, &resp) != 0 || !resp.text) { free(rb); response_free(&resp); return -1; }
    char *summary = strdup(resp.text); free(rb); response_free(&resp);
    log_write(log, "COMPACT", summary);

    /* rebuild: system + summary pair + last 3 raw messages */
    int keep = 10;
    int start = total - keep;
    if (start < 1) start = 1;

    /* delete everything except system prompt and last 3 */
    for (int i = start - 1; i >= 1; i--)
        cJSON_DeleteItemFromArray(msgs, i);
    /* insert summary pair after system */
    cJSON_InsertItemInArray(msgs, 1, make_msg("user", "[Summary of previous context]"));
    cJSON_InsertItemInArray(msgs, 2, make_msg("assistant", summary));
    free(summary);
    return 0;
}

static void process_tool_calls(cJSON *tool_calls, cJSON *msgs, FILE *log) {
    cJSON *tc = NULL;
    cJSON_ArrayForEach(tc, tool_calls) {
        cJSON *id = cJSON_GetObjectItem(tc, "id");
        cJSON *fn = cJSON_GetObjectItem(tc, "function");
        if (!id || !fn) continue;
        cJSON *name = cJSON_GetObjectItem(fn, "name");
        cJSON *args = cJSON_GetObjectItem(fn, "arguments");
        if (!name || !args) continue;
        log_write(log, "TOOL", name->valuestring);
        char *result = tool_execute(name->valuestring, args->valuestring);
        log_write(log, "RES", result ? result : "null");
        cJSON *tm = cJSON_CreateObject();
        cJSON_AddStringToObject(tm, "role", "tool");
        cJSON_AddStringToObject(tm, "tool_call_id", id->valuestring);
        cJSON_AddStringToObject(tm, "content", result ? result : "error");
        cJSON_AddItemToArray(msgs, tm);
        free(result);
    }
}

static int agent_run(const Config *cfg, cJSON *msgs, cJSON *tools,
                     const char *input, FILE *log)
{
    compact_messages(cfg, msgs, log);
    cJSON_AddItemToArray(msgs, make_msg("user", input));
    log_write(log, "USER", input);

    for (int turn = 1; turn <= cfg->max_turns; turn++) {
        char *rj = build_request(cfg, msgs, tools); if (!rj) return -1;
        fprintf(stderr, "[%d] %s...\n", turn, cfg->model);
        char *rb = http_post(cfg->endpoint, cfg->api_key, rj); free(rj);
        if (!rb) return -1;
        Response resp;
        if (parse_response(rb, &resp) != 0) { free(rb); return -1; }
        free(rb);
        cJSON_AddItemToArray(msgs, resp.msg); resp.msg = NULL;

        if (!strcmp(resp.finish_reason, "stop")) {
            if (resp.text) { printf("%s\n", resp.text); log_write(log, "ASST", resp.text); }
            free(resp.finish_reason); return 0;
        }
        if (!strcmp(resp.finish_reason, "tool_calls") && resp.tool_calls) {
            process_tool_calls(resp.tool_calls, msgs, log);
            free(resp.finish_reason); continue;
        }
        if (resp.text) { printf("%s\n", resp.text); log_write(log, "ASST", resp.text); }
        free(resp.finish_reason); return 0;
    }
    fprintf(stderr, "error: max turns (%d) reached\n", cfg->max_turns);
    return -1;
}

#ifndef SZC_TEST
int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        fprintf(stderr, "SubZeroClaw — skill-driven agentic runtime\n"
            "Usage: subzeroclaw [\"prompt\"]\nConfig: ~/.subzeroclaw/config\n");
        return 0;
    }
    Config cfg;
    if (config_load(&cfg)) return 1;
    char *sysprompt = agent_build_system_prompt(cfg.skills_dir);
    if (!sysprompt) return 1;

    char *sid = generate_session_id();
    mkdirp(cfg.log_dir);
    char lp[600]; snprintf(lp, sizeof(lp), "%s/%s.txt", cfg.log_dir, sid);
    FILE *log = fopen(lp, "a");
    if (log) { time_t now = time(NULL); fprintf(log, "=== %s %s", sid, ctime(&now)); fflush(log); }

    cJSON *msgs = cJSON_CreateArray();
    cJSON_AddItemToArray(msgs, make_msg("system", sysprompt));
    cJSON *tools = cJSON_Parse(TOOLS_JSON);
    int rc = 0;
    fprintf(stderr, "subzeroclaw · %s · %s\n", cfg.model, sid);

    if (argc > 1) {
        char input[4096], *p = input, *end = input + sizeof(input) - 1;
        for (int i = 1; i < argc && p < end; i++) {
            if (i > 1) *p++ = ' ';
            size_t l = strlen(argv[i]), a = end - p; if (l > a) l = a;
            memcpy(p, argv[i], l); p += l;
        }
        *p = '\0';
        rc = agent_run(&cfg, msgs, tools, input, log);
    } else {
        char input[4096];
        for (;;) {
            printf("> "); fflush(stdout);
            if (!fgets(input, sizeof(input), stdin)) break;
            size_t len = strlen(input);
            while (len && strchr("\n\r", input[len - 1])) input[--len] = '\0';
            if (!len) continue;
            if (!strcmp(input, "/quit") || !strcmp(input, "/exit")) break;
            agent_run(&cfg, msgs, tools, input, log); printf("\n");
        }
    }
    cJSON_Delete(msgs); cJSON_Delete(tools); free(sysprompt);
    if (log) fclose(log);
    return rc;
}
#endif
