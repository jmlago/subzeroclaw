#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <fcntl.h>
#include <cjson/cJSON.h>

// NEW: Für getcwd (Sandbox CWD)
#include <limits.h>   // PATH_MAX
#include <readline/readline.h>
#include <readline/history.h>

#define SZC_VERSION "0.2.0"
#define MAX_PATH   512
#define MAX_VALUE  1024
#define MAX_OUTPUT (128 * 1024)

typedef struct {
    char api_key[MAX_VALUE], model[MAX_VALUE], endpoint[MAX_VALUE];
    char skills_dir[MAX_PATH], log_dir[MAX_PATH];
    int  max_turns, max_messages;
    // NEW: Sandbox-Optionen
    int sandbox_bwrap;      // 1 = Bubblewrap aktiv (default)
    int sandbox_net;        // 1 = Netzwerk isolieren (nur localhost, default)
    int sandbox_timeout;    // Timeout in Sekunden (default 60, 0 = deaktiviert)
} Config;

static void config_parse_line(Config *cfg, const char *key, const char *val) {
    if      (!strcmp(key, "api_key"))      snprintf(cfg->api_key,    MAX_VALUE, "%s", val);
    else if (!strcmp(key, "model"))        snprintf(cfg->model,      MAX_VALUE, "%s", val);
    else if (!strcmp(key, "endpoint"))     snprintf(cfg->endpoint,   MAX_VALUE, "%s", val);
    else if (!strcmp(key, "skills_dir"))   snprintf(cfg->skills_dir, MAX_PATH,  "%s", val);
    else if (!strcmp(key, "log_dir"))      snprintf(cfg->log_dir,    MAX_PATH,  "%s", val);
    else if (!strcmp(key, "max_turns"))    cfg->max_turns    = atoi(val);
    else if (!strcmp(key, "max_messages")) cfg->max_messages = atoi(val);
    // NEW: Sandbox-Keys parsen
    else if (!strcmp(key, "sandbox_bwrap"))   cfg->sandbox_bwrap   = atoi(val);
    else if (!strcmp(key, "sandbox_net"))     cfg->sandbox_net     = atoi(val);
    else if (!strcmp(key, "sandbox_timeout")) cfg->sandbox_timeout = atoi(val);
}

int config_load(Config *cfg) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->endpoint,   MAX_VALUE, "https://openrouter.ai/api/v1/chat/completions");
    snprintf(cfg->model,      MAX_VALUE, "minimax/minimax-m2.5");
    snprintf(cfg->skills_dir, MAX_PATH,  "%s/.subzeroclaw/skills", home);
    snprintf(cfg->log_dir,    MAX_PATH,  "%s/.subzeroclaw/logs", home);
    cfg->max_turns = 200; cfg->max_messages = 40;
    // NEW: Sandbox-Defaults
    cfg->sandbox_bwrap   = 1;  // Aktiv
    cfg->sandbox_net     = 1;  // Netzwerk isolieren (nur localhost)
    cfg->sandbox_timeout = 60; // 60 Sekunden pro Befehl

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
    // NEW: Env-Vars für Sandbox
    if ((v = getenv("SUBZEROCLAW_SANDBOX_BWRAP")))   cfg->sandbox_bwrap   = atoi(v);
    if ((v = getenv("SUBZEROCLAW_SANDBOX_NET")))     cfg->sandbox_net     = atoi(v);
    if ((v = getenv("SUBZEROCLAW_SANDBOX_TIMEOUT"))) cfg->sandbox_timeout = atoi(v);
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

static int write_temp(const char *prefix, const char *data, char *out, size_t out_size) {
    snprintf(out, out_size, "/tmp/.szc_%s_XXXXXX", prefix);
    int fd = mkstemp(out); if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(out); return -1; }
    fputs(data, f); fclose(f);
    return 0;
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

static char *tool_execute(const Config *cfg, const char *name, cJSON *arguments) {
    if (strcmp(name, "shell") != 0) return NULL;
    cJSON *command_json = cJSON_GetObjectItem(arguments, "command");
    if (!command_json || !cJSON_IsString(command_json)) return strdup("error: missing 'command'");
    const char *command = command_json->valuestring;

    // Sandbox mit Bubblewrap + Timeout + optional localhost-only
    char cmd_buf[8192];
    if (cfg->sandbox_bwrap) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            return strdup("Failed to get current working directory for sandbox.");
        }
        // Befehl in Temp-Datei schreiben (vermeidet Quoting-Probleme)
        char script_path[PATH_MAX];
        snprintf(script_path, sizeof(script_path), "%s/.szc_cmd", cwd);
        FILE *sf = fopen(script_path, "w");
        if (!sf) return strdup("Failed to create sandbox script.");
        fprintf(sf, "%s\n", command);
        fclose(sf);

        char timeout_str[32] = "";
        if (cfg->sandbox_timeout > 0) {
            snprintf(timeout_str, sizeof(timeout_str), "timeout %d ", cfg->sandbox_timeout);
        }
        char net_str[32] = "";
        if (cfg->sandbox_net) {
            snprintf(net_str, sizeof(net_str), "--unshare-net ");
        }
        char skills_bind[MAX_PATH + 64] = "";
        if (cfg->skills_dir[0]) {
            mkdirp(cfg->skills_dir);
            struct stat sb;
            if (stat(cfg->skills_dir, &sb) == 0 && S_ISDIR(sb.st_mode))
                snprintf(skills_bind, sizeof(skills_bind),
                    "--ro-bind %s /skills ", cfg->skills_dir);
        }
        snprintf(cmd_buf, sizeof(cmd_buf),
            "%s bwrap "
            "--unshare-user-try "
            "--unshare-pid "
            "--unshare-ipc "
            "--unshare-uts "
            "--unshare-cgroup-try "
            "--die-with-parent "
            "--new-session "              // verhindert TIOCSTI-Escape
            "--tmpfs / "                  // leerer Root
            "--bind %s /work "            // aktueller Ordner → /work
            "%s"                          // system skills read-only (optional)
            "--chdir /work "              // startet im Arbeitsverzeichnis
            "--ro-bind /bin /bin "
            "--ro-bind /usr /usr "
            "--ro-bind /lib /lib "
            "--ro-bind /lib64 /lib64 "
            "--dev /dev "
            "--proc /proc "
            "--tmpfs /tmp "
            "%s "                         // optional: Netzwerk isolieren
            "bash /work/.szc_cmd 2>&1",
            timeout_str,
            cwd,
            skills_bind,
            net_str);
    } else {
        snprintf(cmd_buf, sizeof(cmd_buf), "%s 2>&1", command);
    }

    FILE *fp = popen(cmd_buf, "r");
    if (!fp) return strdup("Failed to start bubblewrap sandbox or command.");

    char *out = malloc(MAX_OUTPUT);
    size_t total = 0, n;
    while ((n = fread(out + total, 1, MAX_OUTPUT - total - 1, fp)) > 0) {
        total += n; if (total >= MAX_OUTPUT - 1) break;
    }
    out[total] = '\0'; pclose(fp);
    if (cfg->sandbox_bwrap) {
        char script_path[PATH_MAX];
        char cwd2[PATH_MAX];
        if (getcwd(cwd2, sizeof(cwd2)))
            { snprintf(script_path, sizeof(script_path), "%s/.szc_cmd", cwd2); unlink(script_path); }
    }
    return out;
}

static size_t load_skills(char **prompt, size_t len, size_t *cap,
                          const char *dir, const char *label) {
    DIR *d = opendir(dir); if (!d) return len;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        size_t nlen = strlen(entry->d_name);
        if (nlen < 4 || strcmp(entry->d_name + nlen - 3, ".md") != 0) continue;
        char fp[MAX_PATH]; snprintf(fp, MAX_PATH, "%s/%s", dir, entry->d_name);
        FILE *sf = fopen(fp, "r"); if (!sf) continue;
        fseek(sf, 0, SEEK_END); long sz = ftell(sf); fseek(sf, 0, SEEK_SET);
        char *content = malloc(sz + 1);
        content[fread(content, 1, sz, sf)] = '\0'; fclose(sf);
        size_t clen = strlen(content);
        while (len + clen + 128 >= *cap) { *cap *= 2; *prompt = realloc(*prompt, *cap); }
        len += snprintf(*prompt + len, *cap - len, "\n--- %s: %s ---\n", label, entry->d_name);
        memcpy(*prompt + len, content, clen); len += clen; (*prompt)[len] = '\0';
        free(content);
    }
    closedir(d);
    return len;
}

char *agent_build_system_prompt(const char *skills_dir, const char *project_skills_dir) {
    size_t cap = 8192;
    char *prompt = malloc(cap);
    size_t len = snprintf(prompt, cap,
        "You are SubZeroClaw, a minimal agentic assistant.\n"
        "You have one tool: shell. Use it to run any command.\n"
        "For files, use cat, tee, sed, etc. Be concise. Just do it.\n"
        "System skills: /skills/ (read-only). "
        "Project skills: /work/skills/ (read-write, create and edit freely).\n\n");
    len = load_skills(&prompt, len, &cap, skills_dir, "SKILL");
    len = load_skills(&prompt, len, &cap, project_skills_dir, "PROJECT SKILL");
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

    size_t cap = 512000, len = 0;
    char *convo = malloc(cap);
    convo[0] = '\0';
    cJSON *m = NULL;
    cJSON_ArrayForEach(m, msgs) {
        if (len + 256 >= cap) break;
        cJSON *role = cJSON_GetObjectItem(m, "role");
        cJSON *ct = cJSON_GetObjectItem(m, "content");
        if (!role || !cJSON_IsString(role) || !ct || !cJSON_IsString(ct)) continue;
        const char *r = role->valuestring, *c = ct->valuestring;
        size_t clen = strlen(c);
        if (strcmp(r, "tool") == 0 && clen > 2000) {
            // Trim long tool outputs
            len += snprintf(convo + len, cap - len, "[%s] ", r);
            if (clen > 1000) {
                memcpy(convo + len, c, 1000);
                len += 1000;
                len += snprintf(convo + len, cap - len, "... [trimmed] ...\n");
            } else {
                len += snprintf(convo + len, cap - len, "%s\n", c);
            }
        } else {
            len += snprintf(convo + len, cap - len, "[%s] %s\n", r, c);
        }
    }

    const char *summary_prompt = "Summarize this conversation. Keep all facts, details, and key points. Be concise but complete.";
    cJSON *sum_msgs = cJSON_CreateArray();
    cJSON_AddItemToArray(sum_msgs, make_msg("system", summary_prompt));
    cJSON_AddItemToArray(sum_msgs, make_msg("user", convo));
    free(convo);

    char *req = build_request(cfg, sum_msgs, NULL);
    cJSON_Delete(sum_msgs);
    char *resp_body = http_post(cfg->endpoint, cfg->api_key, req);
    free(req);
    if (!resp_body) return -1;

    Response resp;
    if (parse_response(resp_body, &resp) < 0) {
        free(resp_body); return -1;
    }
    free(resp_body);
    if (!resp.text) {
        response_free(&resp); return -1;
    }

    // Ersetze alte Nachrichten durch Summary
    while (cJSON_GetArraySize(msgs) > 10) {  // behalte letzte 10
        cJSON_DeleteItemFromArray(msgs, 0);
    }
    cJSON_InsertItemInArray(msgs, 0, make_msg("assistant", resp.text));
    cJSON_InsertItemInArray(msgs, 0, make_msg("user", summary_prompt));
    response_free(&resp);
    return 0;
}

static int process_tool_calls(const Config *cfg, cJSON *tool_calls, cJSON *msgs, FILE *log) {
    cJSON *call = NULL;
    cJSON_ArrayForEach(call, tool_calls) {
        cJSON *func = cJSON_GetObjectItem(call, "function");
        if (!func) continue;
        cJSON *name = cJSON_GetObjectItem(func, "name");
        cJSON *args = cJSON_GetObjectItem(func, "arguments");
        if (!name || !cJSON_IsString(name) || !args) continue;
        // NEW: Handle arguments as string (OpenAI format)
        cJSON *args_parsed = NULL;
        if (cJSON_IsString(args)) {
            const char *args_str = args->valuestring;
            args_parsed = cJSON_Parse(args_str);
            if (args_parsed) {
                args = args_parsed;
            } else {
                fprintf(stderr, "Failed to parse tool arguments: %s\n", args_str);
                continue;
            }
        } else if (!cJSON_IsObject(args)) {
            continue;
        }
        log_write(log, "TOOL_CALL", name->valuestring);
        char *result = tool_execute(cfg, name->valuestring, args);
        if (args_parsed) cJSON_Delete(args_parsed);
        if (result) {
            log_write(log, "TOOL_RESULT", result);
            cJSON *tool_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_msg, "role", "tool");
            cJSON_AddStringToObject(tool_msg, "name", name->valuestring);
            cJSON_AddStringToObject(tool_msg, "content", result);
            cJSON_AddStringToObject(tool_msg, "tool_call_id", call->child->valuestring);  // id
            cJSON_AddItemToArray(msgs, tool_msg);
            free(result);
        }
    }
    return 0;
}

static int agent_run(const Config *cfg, const char *task, FILE *log) {
    char project_skills[MAX_PATH] = "";
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)))
        snprintf(project_skills, MAX_PATH, "%s/skills", cwd);
    if (project_skills[0]) mkdirp(project_skills);
    char *system_prompt = agent_build_system_prompt(cfg->skills_dir, project_skills);
    cJSON *msgs = cJSON_CreateArray();
    cJSON_AddItemToArray(msgs, make_msg("system", system_prompt));
    free(system_prompt);
    if (task) cJSON_AddItemToArray(msgs, make_msg("user", task));

    cJSON *tools = cJSON_Parse(TOOLS_JSON);
    for (int turn = 1; turn <= cfg->max_turns; turn++) {
        if (compact_messages(cfg, msgs, log) < 0) break;

        char *req = build_request(cfg, msgs, tools);
        log_write(log, "REQUEST", req);
        char *resp_body = http_post(cfg->endpoint, cfg->api_key, req);
        free(req);
        if (!resp_body) break;

        Response resp;
        if (parse_response(resp_body, &resp) < 0) { free(resp_body); break; }
        free(resp_body);

        log_write(log, "RESPONSE", resp.text ? resp.text : "(tool calls)");
        cJSON_AddItemToArray(msgs, resp.msg); resp.msg = NULL;

        if (strcmp(resp.finish_reason, "stop") == 0) {
            if (resp.text) printf("%s\n", resp.text);
            response_free(&resp); break;
        } else if (strcmp(resp.finish_reason, "tool_calls") == 0) {
            if (process_tool_calls(cfg, resp.tool_calls, msgs, log) < 0) { response_free(&resp); break; }
        }
        response_free(&resp);
    }
    cJSON_Delete(msgs); cJSON_Delete(tools);
    return 0;
}

#ifndef SZC_TEST
int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("subzeroclaw %s\n"
               "Copyright (c) 2025-2026 SubZeroClaw contributors\n"
               "License MIT: https://opensource.org/licenses/MIT\n"
               "This is free software: you are free to change and redistribute it.\n"
               "There is NO WARRANTY, to the extent permitted by law.\n",
               SZC_VERSION);
        return 0;
    }
    Config cfg;
    if (config_load(&cfg) < 0) return 1;

    mkdirp(cfg.skills_dir); mkdirp(cfg.log_dir);

    char session_id[32];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(session_id, sizeof(session_id), "%Y%m%dT%H%M%S", tm);

    char log_path[MAX_PATH];
    snprintf(log_path, MAX_PATH, "%s/%s.txt", cfg.log_dir, session_id);
    FILE *log = fopen(log_path, "w");
    if (log) fprintf(log, "# Session %s\n", session_id);

    if (argc > 1) {
        char task[8192]; task[0] = '\0';
        for (int i = 1; i < argc; i++) {
            strcat(task, argv[i]); if (i < argc - 1) strcat(task, " ");
        }
        agent_run(&cfg, task, log);
    } else {
        while (1) {
            char *line = readline("> ");
            if (!line) break;
            if (!strcmp(line, "/quit") || !strcmp(line, "/exit")) { free(line); break; }
            if (line[0]) add_history(line);
            agent_run(&cfg, line, log);
            free(line);
        }
    }

    if (log) fclose(log);
    return 0;
}
#endif