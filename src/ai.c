#include "ai.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
  #endif
  typedef SOCKET ai_socket_t;
  #define AI_INVALID INVALID_SOCKET
  #define AI_CLOSE(s) closesocket(s)
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <netdb.h>
  typedef int ai_socket_t;
  #define AI_INVALID (-1)
  #define AI_CLOSE(s) close(s)
#endif

#define AI_RESPONSE_MAX 16384
#define AI_CONNECT_TIMEOUT_MS 3000
#define AI_RECV_TIMEOUT_S 6
#define AI_CONFIG_FILE "luansha_ai.conf"

static char g_ai_url[512];
static char g_ai_model[128];
static char g_ai_key[256];
static int g_ai_runtime = 0;   /* 1 = 运行时/配置文件配置已加载，优先于环境变量 */

static void ai_trim_crlf(char *str)
{
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\r' || str[len - 1] == '\n')) {
        str[len - 1] = '\0';
        len--;
    }
}

static void ai_set_value(char *dst, size_t dst_size, const char *value)
{
    size_t n;
    if (!value) value = "";
    if (dst_size == 0) return;
    n = strlen(value);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, value, n);
    dst[n] = '\0';
}

const char *ai_get_url(void)
{
    if (g_ai_runtime) return g_ai_url;
    return getenv("LUANSHA_AI_URL");
}

const char *ai_get_model(void)
{
    if (g_ai_runtime) return g_ai_model;
    return getenv("LUANSHA_AI_MODEL");
}

const char *ai_get_key(void)
{
    if (g_ai_runtime) return g_ai_key;
    return getenv("LUANSHA_AI_KEY");
}

static int ai_config_save(const char *url, const char *model, const char *key)
{
    FILE *f = fopen(AI_CONFIG_FILE, "w");
    if (!f) return -1;
    fprintf(f, "LUANSHA_AI_URL=%s\n", url ? url : "");
    fprintf(f, "LUANSHA_AI_MODEL=%s\n", model ? model : "");
    fprintf(f, "LUANSHA_AI_KEY=%s\n", key ? key : "");
    fclose(f);
    return 0;
}

static int ai_config_load(void)
{
    FILE *f = fopen(AI_CONFIG_FILE, "r");
    char line[1024];
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        ai_trim_crlf(line);
        if (strncmp(line, "LUANSHA_AI_URL=", 15) == 0) {
            ai_set_value(g_ai_url, sizeof(g_ai_url), line + 15);
        } else if (strncmp(line, "LUANSHA_AI_MODEL=", 17) == 0) {
            ai_set_value(g_ai_model, sizeof(g_ai_model), line + 17);
        } else if (strncmp(line, "LUANSHA_AI_KEY=", 15) == 0) {
            ai_set_value(g_ai_key, sizeof(g_ai_key), line + 15);
        }
    }
    fclose(f);
    g_ai_runtime = 1;
    return 0;
}

void ai_configure(const char *url, const char *model, const char *key)
{
    ai_set_value(g_ai_url, sizeof(g_ai_url), url);
    ai_set_value(g_ai_model, sizeof(g_ai_model), model);
    ai_set_value(g_ai_key, sizeof(g_ai_key), key);
    g_ai_runtime = 1;
    ai_config_save(url, model, key);
}

void ai_init(void)
{
    ai_config_load();
}


static int net_startup(void)
{
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

static void net_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

static int parse_url(const char *url, char *host, size_t host_size,
                     int *port, char *path, size_t path_size)
{
    const char *p = url;
    const char *host_start, *host_end, *path_start;
    size_t len;

    if (strncmp(p, "http://", 7) != 0) return -1;
    p += 7;
    host_start = p;
    host_end = strchr(p, ':');
    path_start = strchr(p, '/');
    if (!host_end || host_end > (path_start ? path_start : p + strlen(p))) {
        host_end = path_start ? path_start : p + strlen(p);
    }
    len = (size_t)(host_end - host_start);
    if (len == 0 || len >= host_size) return -1;
    memcpy(host, host_start, len);
    host[len] = '\0';

    if (*host_end == ':') {
        const char *port_start = host_end + 1;
        char port_buf[16];
        size_t port_len = path_start ? (size_t)(path_start - port_start) : strlen(port_start);
        if (port_len >= sizeof(port_buf)) return -1;
        memcpy(port_buf, port_start, port_len);
        port_buf[port_len] = '\0';
        *port = atoi(port_buf);
        if (*port <= 0 || *port > 65535) return -1;
    } else {
        *port = 80;
    }

    if (path_start && *path_start) {
        snprintf(path, path_size, "%s", path_start);
    } else {
        snprintf(path, path_size, "/");
    }
    return 0;
}

static int send_all_sock(ai_socket_t s, const char *data, int len)
{
    int sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = send(s, data + sent, len - sent, 0);
#else
        ssize_t n = send(s, data + sent, (size_t)(len - sent), 0);
#endif
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}
static int decode_chunked_body(const char *src, char *dst, size_t dst_size)
{
    size_t o = 0;
    while (*src) {
        char *end = NULL;
        long size = strtol(src, &end, 16);
        if (end == src) return -1;
        src = end;
        while (*src && *src != '\n') src++;
        if (*src == '\n') src++;
        if (size <= 0) break;
        if ((size_t)size >= dst_size - o) return -1;
        memcpy(dst + o, src, (size_t)size);
        o += (size_t)size;
        src += size;
        if (*src == '\r') src++;
        if (*src == '\n') src++;
    }
    dst[o] = '\0';
    return 0;
}
static int write_temp_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "wb");
    size_t len;
    if (!f) return -1;
    len = strlen(data);
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        remove(path);
        return -1;
    }
    fclose(f);
    return 0;
}

static int read_temp_file(const char *path, char *out, size_t out_size)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return -1;
    n = fread(out, 1, out_size - 1, f);
    fclose(f);
    out[n] = '\0';
    return 0;
}

/* HTTPS is delegated to the system curl because this project has no TLS library. */
static int https_post_json_via_curl(const char *url, const char *json_body,
                                    char *response, size_t response_size)
{
    char body_file[L_tmpnam];
    char resp_file[L_tmpnam];
    char cmd[8192];
    const char *api_key = ai_get_key();
    const char *curl = "curl";
    int rc;

#ifdef _WIN32
    curl = "curl.exe";
#endif

    if (!tmpnam(body_file) || !tmpnam(resp_file)) return -1;
    if (write_temp_file(body_file, json_body) != 0) return -1;

    if (api_key && *api_key) {
        snprintf(cmd, sizeof(cmd),
                 "%s -sS --http1.1 --tlsv1.2 --keepalive-time 20 --keepalive-cnt 3 --retry 2 --retry-delay 1 --retry-all-errors --connect-timeout 15 -m 30 -X POST \"%s\" "
                 "-H \"Content-Type: application/json\" "
                 "-H \"Authorization: Bearer %s\" "
                 "--data-binary \"@%s\" -o \"%s\"",
                 curl, url, api_key, body_file, resp_file);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "%s -sS --http1.1 --tlsv1.2 --keepalive-time 20 --keepalive-cnt 3 --retry 2 --retry-delay 1 --retry-all-errors --connect-timeout 15 -m 30 -X POST \"%s\" "
                 "-H \"Content-Type: application/json\" "
                 "--data-binary \"@%s\" -o \"%s\"",
                 curl, url, body_file, resp_file);
    }

    rc = system(cmd);
    remove(body_file);
    if (rc != 0 || read_temp_file(resp_file, response, response_size) != 0) {
        remove(resp_file);
        return -1;
    }
    remove(resp_file);
    return 0;
}

static int http_post_json(const char *url, const char *json_body,
                          char *response, size_t response_size)
{
    char host[256];
    char path[512];
    int port;
    struct addrinfo hints, *res = NULL;
    ai_socket_t sock = AI_INVALID;
    char request[AI_RESPONSE_MAX];
    char recv_buf[AI_RESPONSE_MAX];
    size_t recv_len = 0;
    const char *body_start;
    const char *api_key;
    int req_len;

    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0) return -1;
    if (net_startup() != 0) return -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);
        if (getaddrinfo(host, port_str, &hints, &res) != 0) {
            net_cleanup();
            return -1;
        }
    }

    sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == AI_INVALID) {
        freeaddrinfo(res);
        net_cleanup();
        return -1;
    }

    /* Non-blocking connect with timeout would be nicer; blocking is fine for local AI. */
    if (connect(sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
        AI_CLOSE(sock);
        freeaddrinfo(res);
        net_cleanup();
        return -1;
    }
    freeaddrinfo(res);

    api_key = ai_get_key();
    if (api_key && *api_key) {
        req_len = snprintf(request, sizeof(request),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            path, host, api_key, strlen(json_body), json_body);
    } else {
        req_len = snprintf(request, sizeof(request),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            path, host, strlen(json_body), json_body);
    }
    if (req_len < 0 || (size_t)req_len >= sizeof(request)) {
        AI_CLOSE(sock);
        net_cleanup();
        return -1;
    }
    if (send_all_sock(sock, request, req_len) != 0) {
        AI_CLOSE(sock);
        net_cleanup();
        return -1;
    }

    /* Read until connection closes or buffer is full. */
    while (recv_len + 1 < sizeof(recv_buf)) {
        fd_set fds;
        struct timeval tv;
        int n;

        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        tv.tv_sec = AI_RECV_TIMEOUT_S;
        tv.tv_usec = 0;
        n = select((int)sock + 1, &fds, NULL, NULL, &tv);
        if (n <= 0) break;
        n = recv(sock, recv_buf + recv_len, (int)(sizeof(recv_buf) - recv_len - 1), 0);
        if (n <= 0) break;
        recv_len += (size_t)n;
        recv_buf[recv_len] = '\0';
    }
    AI_CLOSE(sock);

    if (recv_len == 0) {
        net_cleanup();
        return -1;
    }
    recv_buf[recv_len] = '\0';

    body_start = strstr(recv_buf, "\r\n\r\n");
    if (!body_start) {
        net_cleanup();
        return -1;
    }
    body_start += 4;
    if (isxdigit((unsigned char)body_start[0]) && strchr(body_start, '\n')) {
        char raw[AI_RESPONSE_MAX];
        snprintf(raw, sizeof(raw), "%s", body_start);
        if (decode_chunked_body(raw, response, response_size) != 0) {
            snprintf(response, response_size, "%s", raw);
        }
    } else {
        snprintf(response, response_size, "%s", body_start);
    }
    net_cleanup();
    return 0;
}

static const char *find_json_in_text(const char *text, const char **end)
{
    const char *start = strchr(text, '{');
    const char *last;
    if (!start) return NULL;
    last = strrchr(text, '}');
    if (!last || last <= start) return NULL;
    *end = last + 1;
    return start;
}
static void normalize_ai_url(const char *input, char *out, size_t out_size)
{
    size_t len;
    if (!input || !*input) {
        if (out_size > 0) out[0] = '\0';
        return;
    }
    if (strstr(input, "/chat/completions") || strstr(input, "chat-completions")) {
        snprintf(out, out_size, "%s", input);
        return;
    }
    len = strlen(input);
    while (len > 0 && input[len - 1] == '/') len--;
    if (len + strlen("/chat/completions") + 1 > out_size) {
        snprintf(out, out_size, "%s", input);
        return;
    }
    memcpy(out, input, len);
    memcpy(out + len, "/chat/completions", strlen("/chat/completions") + 1);
}
int ai_is_configured(void)
{
    const char *u = ai_get_url();
    return u && *u;
}
static const char *json_find_string_with_key(const JsonValue *v, const char *key)
{
    int i;
    const char *s;
    if (!v) return NULL;
    if (v->type == JSON_OBJECT) {
        for (i = 0; i < v->count; i++) {
            if (v->items[i]->key && strcmp(v->items[i]->key, key) == 0 &&
                v->items[i]->type == JSON_STRING && v->items[i]->str) {
                return v->items[i]->str;
            }
        }
        for (i = 0; i < v->count; i++) {
            s = json_find_string_with_key(v->items[i], key);
            if (s) return s;
        }
    } else if (v->type == JSON_ARRAY) {
        for (i = 0; i < v->count; i++) {
            s = json_find_string_with_key(v->items[i], key);
            if (s) return s;
        }
    }
    return NULL;
}

static const char *json_find_first_string(const JsonValue *v)
{
    int i;
    const char *s;
    if (!v) return NULL;
    if (v->type == JSON_STRING) return v->str;
    if (v->type == JSON_ARRAY) {
        for (i = 0; i < v->count; i++) {
            s = json_find_first_string(v->items[i]);
            if (s) return s;
        }
    } else if (v->type == JSON_OBJECT) {
        for (i = 0; i < v->count; i++) {
            s = json_find_first_string(v->items[i]);
            if (s) return s;
        }
    }
    return NULL;
}

static const char *extract_generation_text(const JsonValue *root)
{
    const JsonValue *choices, *message, *content_val;

    choices = json_get(root, "choices");
    if (choices && choices->type == JSON_ARRAY && choices->count > 0) {
        message = json_get(choices->items[0], "message");
        if (message) {
            content_val = json_get(message, "content");
            if (content_val && content_val->type == JSON_STRING && content_val->str) {
                return content_val->str;
            }
            if (content_val && content_val->type == JSON_ARRAY && content_val->count > 0) {
                if (content_val->items[0]->type == JSON_STRING) return content_val->items[0]->str;
                {
                    const char *s = json_find_string_with_key(content_val->items[0], "text");
                    if (s) return s;
                    s = json_find_first_string(content_val->items[0]);
                    if (s) return s;
                }
            }
        }
        {
            const char *s = json_find_string_with_key(choices->items[0], "text");
            if (s) return s;
        }
    }
    {
        const char *s = json_find_string_with_key(root, "output");
        if (s) return s;
        s = json_find_string_with_key(root, "text");
        if (s) return s;
        s = json_find_string_with_key(root, "content");
        if (s) return s;
    }
    return json_find_first_string(root);
}
static int ai_complete_text(const char *system_prompt, const char *user_prompt,
                            char *out, size_t out_size)
{
    const char *api_url = ai_get_url();
    const char *api_model = ai_get_model();
    const char *debug = getenv("LUANSHA_AI_DEBUG");
    char ai_url[512];
    char request_body[4096];
    char response[AI_RESPONSE_MAX];
    JsonBuf b;
    JsonValue *root = NULL;
    const char *model = api_model && *api_model ? api_model : "qwen2.5:7b";
    const char *text;
    size_t len;
    int attempt;

    if (!api_url || !*api_url || !out || out_size == 0) return 0;
    out[0] = '\0';
    normalize_ai_url(api_url, ai_url, sizeof(ai_url));

    jsonb_init(&b);
    jsonb_append(&b, "{\"model\":");
    jsonb_string(&b, model);
    jsonb_append(&b, ",\"temperature\":0.7,\"stream\":false,\"max_tokens\":200,\"thinking\":{\"type\":\"disabled\"},\"messages\":[");
    jsonb_append(&b, "{\"role\":\"system\",\"content\":");
    jsonb_string(&b, system_prompt ? system_prompt : "");
    jsonb_append(&b, "},{\"role\":\"user\",\"content\":");
    jsonb_string(&b, user_prompt ? user_prompt : "");
    jsonb_append(&b, "}]}");
    if (strlen(jsonb_cstr(&b)) >= sizeof(request_body)) {
        jsonb_free(&b);
        return 0;
    }
    snprintf(request_body, sizeof(request_body), "%s", jsonb_cstr(&b));
    jsonb_free(&b);

    {
        int got_response = 0;
        for (attempt = 0; attempt < 1; attempt++) {
            if (strncmp(ai_url, "https://", 8) == 0) {
                if (https_post_json_via_curl(ai_url, request_body, response, sizeof(response)) == 0) {
                    got_response = 1;
                } else if (debug && *debug) {
                    fprintf(stderr, "[AI] HTTPS generation failed (attempt %d)\n", attempt + 1);
                }
            } else {
                if (http_post_json(ai_url, request_body, response, sizeof(response)) == 0) {
                    got_response = 1;
                } else if (debug && *debug) {
                    fprintf(stderr, "[AI] HTTP generation failed (attempt %d)\n", attempt + 1);
                }
            }
            if (got_response) break;
        }
        if (!got_response) return 0;
    }
    if (!*response) return 0;

    root = json_parse(response);
    if (!root) {
        const char *js, *je;
        char tmp[AI_RESPONSE_MAX];
        js = find_json_in_text(response, &je);
        if (js && (size_t)(je - js) < sizeof(tmp)) {
            memcpy(tmp, js, (size_t)(je - js));
            tmp[je - js] = '\0';
            root = json_parse(tmp);
        }
        if (!root) return 0;
    }

    text = extract_generation_text(root);
    if (!text) {
        json_free(root);
        return 0;
    }
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
    len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                       text[len - 1] == '\n' || text[len - 1] == '\r')) len--;
    if (len >= 2 && text[0] == '"' && text[len - 1] == '"') {
        text++;
        len -= 2;
    }
    if (len > 0 && len < out_size) {
        memcpy(out, text, len);
        out[len] = '\0';
        json_free(root);
        return 1;
    }
    json_free(root);
    return 0;
}
int ai_test_connection(char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    return ai_complete_text(
        "你是一个连通性测试助手。",
        "请只回复一句话：AI连接正常。不要输出解释。",
        out, out_size);
}
int ai_validate_narration(
    const char *room_name,
    const char *players_text,
    const char *narrator,
    const char *content,
    int operation,
    int is_first_sentence,
    int *valid_reason,
    int *valid_predicate,
    int *valid_mode,
    char *reason,
    size_t reason_size)
{
    const char *debug = getenv("LUANSHA_AI_DEBUG");
    char system_prompt[1200];
    char user_prompt[2048];
    char out[1024];
    JsonValue *v = NULL;
    const char *op_name = "普通陈述";
    int judged = 0;

    if (!valid_reason || !valid_predicate || !valid_mode) return 0;
    *valid_reason = 1;
    *valid_predicate = 1;
    *valid_mode = 1;
    if (reason && reason_size > 0) reason[0] = '\0';

    if (operation == 1) op_name = "创造";
    else if (operation == 2) op_name = "扭曲";
    else if (operation == 3) op_name = "解释/补充";
    else if (operation == 4) op_name = "限定";

    snprintf(system_prompt, sizeof(system_prompt),
        "你是《乱杀法则》跑团游戏的严格裁判。"
        "请判断玩家的一句话是否满足三条规则："
        "1. 合理性：叙述要符合作家原则，所有事物都有原因，不能凭空无敌或明显不合理；"
        "2. 单一谓语：每轮每人只能说一个谓语/行为核心，不能使用连词，不能多个动作并列；"
        "3. 模式匹配：获得物品/创造新事物应选“创造”；改变已有事件/攻击应选“扭曲”；"
        "普通叙述/观察应选“陈述”；补全解释应选“解释/补充”；设定关键词转折应选“限定”。"
        "如果这是本局第一句话，还要符合设定：理论上每个玩家出生在家里，"
        "所以第一句话应该类似“我走出了家门”。"
        "只输出JSON，不要输出其他文字，格式："
        "{\"reasonable\":true或false,\"single_predicate\":true或false,\"mode_match\":true或false,"
        "\"reason\":\"简短中文原因\"}");

    snprintf(user_prompt, sizeof(user_prompt),
        "房间名：%s\n在线玩家：%s\n发言玩家：%s\n操作类型：%s\n"
        "是否本局第一句话：%s\n发言内容：%s",
        room_name ? room_name : "未知",
        players_text ? players_text : "无",
        narrator ? narrator : "未知",
        op_name,
        is_first_sentence ? "是" : "否",
        content ? content : "");

    if (ai_complete_text(system_prompt, user_prompt, out, sizeof(out)) != 1) return 0;

    v = json_parse(out);
    if (!v) {
        const char *js, *je;
        char tmp[1024];
        js = find_json_in_text(out, &je);
        if (js && (size_t)(je - js) < sizeof(tmp)) {
            memcpy(tmp, js, (size_t)(je - js));
            tmp[je - js] = '\0';
            v = json_parse(tmp);
        }
    }
    if (v) {
        JsonValue *reasonable = json_get(v, "reasonable");
        JsonValue *single = json_get(v, "single_predicate");
        JsonValue *mode = json_get(v, "mode_match");
        if (reasonable && reasonable->type == JSON_BOOL) {
            *valid_reason = reasonable->boolean ? 1 : 0;
        }
        if (single && single->type == JSON_BOOL) {
            *valid_predicate = single->boolean ? 1 : 0;
        }
        if (mode && mode->type == JSON_BOOL) {
            *valid_mode = mode->boolean ? 1 : 0;
        }
        {
            const char *r = json_get_string(v, "reason", "");
            if (r && *r && reason && reason_size > 0) {
                snprintf(reason, reason_size, "%s", r);
            }
        }
        json_free(v);
        judged = 1;
    }
    if (judged && debug && *debug) fprintf(stderr, "[AI] validation ok: %s\n", out);
    return judged;
}
int ai_judge_random_check(
    const char *room_name,
    const char *players_text,
    const char *narrator,
    const char *content,
    int operation,
    const char *recent_text,
    int *need_roll,
    int *difficulty,
    int *plausibility,
    int *preparation,
    char *reason,
    size_t reason_size)
{
    const char *debug = getenv("LUANSHA_AI_DEBUG");
    char system_prompt[1400];
    char user_prompt[2048];
    char out[1024];
    JsonValue *v = NULL;
    const char *op_name = "普通陈述";
    int judged = 0;

    if (!need_roll || !difficulty || !plausibility || !preparation) return 0;
    *need_roll = 0;
    *difficulty = 3;
    *plausibility = 3;
    *preparation = 3;
    if (reason && reason_size > 0) reason[0] = '\0';

    if (operation == 1) op_name = "创造";
    else if (operation == 2) op_name = "扭曲";
    else if (operation == 3) op_name = "解释/补充";
    else if (operation == 4) op_name = "限定";

    snprintf(system_prompt, sizeof(system_prompt),
        "你是《乱杀法则》跑团游戏的裁判，负责决定一句叙述是否需要随机判定。"
        "规则："
        "1) 必然成功的日常动作（走路、观察、说话、拿起身边物品）不需要判定；"
        "2) 结果不确定、依赖运气或对抗其他玩家的动作（偷袭、命中、逃脱、说服、"
        "临时制造复杂物品、破坏结构）需要判定；"
        "3) 给出难度 difficulty：1=非常容易 5=近乎不可能；"
        "4) 给出合理性 plausibility：1=非常牵强 5=非常合理；"
        "5) 给出准备度 preparation：1=完全没有前置铺垫 5=前文已充分铺垫。"
        "只输出JSON，不要输出其他文字，格式："
        "{\"need_roll\":true或false,\"difficulty\":1-5,\"plausibility\":1-5,"
        "\"preparation\":1-5,\"reason\":\"简短中文原因\"}");

    snprintf(user_prompt, sizeof(user_prompt),
        "房间名：%s\n在线玩家：%s\n发言玩家：%s\n操作类型：%s\n最近发言：%s\n发言内容：%s",
        room_name ? room_name : "未知",
        players_text ? players_text : "无",
        narrator ? narrator : "未知",
        op_name,
        recent_text && *recent_text ? recent_text : "暂无",
        content ? content : "");

    if (ai_complete_text(system_prompt, user_prompt, out, sizeof(out)) != 1) return 0;

    v = json_parse(out);
    if (!v) {
        const char *js, *je;
        char tmp[1024];
        js = find_json_in_text(out, &je);
        if (js && (size_t)(je - js) < sizeof(tmp)) {
            memcpy(tmp, js, (size_t)(je - js));
            tmp[je - js] = '\0';
            v = json_parse(tmp);
        }
    }
    if (v) {
        JsonValue *nr = json_get(v, "need_roll");
        if (nr && nr->type == JSON_BOOL) *need_roll = nr->boolean ? 1 : 0;
        *difficulty = json_get_int(v, "difficulty", 3);
        *plausibility = json_get_int(v, "plausibility", 3);
        *preparation = json_get_int(v, "preparation", 3);
        if (*difficulty < 1) *difficulty = 1;
        if (*difficulty > 5) *difficulty = 5;
        if (*plausibility < 1) *plausibility = 1;
        if (*plausibility > 5) *plausibility = 5;
        if (*preparation < 1) *preparation = 1;
        if (*preparation > 5) *preparation = 5;
        {
            const char *rs = json_get_string(v, "reason", "");
            if (rs && *rs && reason && reason_size > 0) {
                snprintf(reason, reason_size, "%s", rs);
            }
        }
        json_free(v);
        judged = 1;
    }
    if (judged && debug && *debug) fprintf(stderr, "[AI] random-check ok: %s\n", out);
    return judged;
}


int ai_try_judge_rescue(
    const char *room_name,
    const char *players_text,
    const char *narrator,
    const char *content,
    const char *warning_text,
    int *rescued,
    char *reason,
    size_t reason_size)
{
    const char *debug = getenv("LUANSHA_AI_DEBUG");
    char system_prompt[1024];
    char user_prompt[2048];
    char out[1024];
    JsonValue *v = NULL;
    int judged = 0;

    if (!rescued) return 0;
    *rescued = 0;
    if (reason && reason_size > 0) reason[0] = '\0';

    snprintf(system_prompt, sizeof(system_prompt),
        "你是《乱杀法则》跑团游戏的严格裁判。"
        "一名玩家正遭受濒死警告，警告内容为：%s。"
        "该玩家说了一句话试图化解/摆脱濒死警告。"
        "请判断这句话是否合理有效地化解或摆脱了该濒死威胁。"
        "只输出JSON，不要输出其他文字，格式："
        "{\"rescued\":true或false,\"reason\":\"简短中文原因\"}",
        warning_text ? warning_text : "无");

    snprintf(user_prompt, sizeof(user_prompt),
        "房间名：%s\n在线玩家：%s\n发言玩家：%s\n发言内容：%s",
        room_name ? room_name : "未知",
        players_text ? players_text : "无",
        narrator ? narrator : "未知",
        content ? content : "");

    if (ai_complete_text(system_prompt, user_prompt, out, sizeof(out)) != 1) return 0;

    v = json_parse(out);
    if (!v) {
        const char *js, *je;
        char tmp[1024];
        js = find_json_in_text(out, &je);
        if (js && (size_t)(je - js) < sizeof(tmp)) {
            memcpy(tmp, js, (size_t)(je - js));
            tmp[je - js] = '\0';
            v = json_parse(tmp);
        }
    }
    if (v) {
        JsonValue *r = json_get(v, "rescued");
        if (r && r->type == JSON_BOOL) {
            *rescued = r->boolean ? 1 : 0;
        }
        {
            const char *rs = json_get_string(v, "reason", "");
            if (rs && *rs && reason && reason_size > 0) {
                snprintf(reason, reason_size, "%s", rs);
            }
        }
        json_free(v);
        judged = 1;
    }
    if (judged && debug && *debug) fprintf(stderr, "[AI] rescue ok: %s\n", out);
    return judged;
}

int ai_try_judge_narration(
    const char *room_name,
    const char *players_text,
    const char *narrator,
    const char *content,
    int operation,
    const char *target_name,
    int *creates_warning,
    char *reason,
    size_t reason_size)
{
    const char *api_url = ai_get_url();
    const char *api_model = ai_get_model();
    const char *debug = getenv("LUANSHA_AI_DEBUG");
    char ai_url[512];
    char system_content[1024];
    char user_content[2048];
    char request_body[4096];
    char response[AI_RESPONSE_MAX];
    JsonBuf b;
    JsonValue *root = NULL;
    JsonValue *choices, *message, *content_val;
    const char *model = api_model && *api_model ? api_model : "qwen2.5:7b";
    const char *op_name = "普通陈述";

    if (!creates_warning || !reason || !content) return 0;
    *creates_warning = 0;
    if (reason_size > 0) reason[0] = '\0';

    if (!api_url || !*api_url) return 0;
    normalize_ai_url(api_url, ai_url, sizeof(ai_url));
    if (debug && *debug) fprintf(stderr, "[AI] using url=%s model=%s\n", ai_url, model);

    if (operation == 1) op_name = "创造";
    else if (operation == 2) op_name = "扭曲";
    else if (operation == 3) op_name = "解释/补充";
    else if (operation == 4) op_name = "限定";

    snprintf(system_content, sizeof(system_content),
        "你是《乱杀法则》跑团游戏的裁判。"
        "请判断玩家的一句话是否对某个玩家造成了致命威胁/濒死状态。"
        "只输出JSON，不要输出任何其他文字，格式："
        "{\"danger\":true或false,\"reason\":\"简短中文原因\"}");

    snprintf(user_content, sizeof(user_content),
        "房间名：%s\n"
        "在线玩家：%s\n"
        "发言玩家：%s\n"
        "目标玩家：%s\n"
        "操作类型：%s\n"
        "发言内容：%s",
        room_name ? room_name : "未知",
        players_text ? players_text : "无",
        narrator ? narrator : "未知",
        target_name && *target_name ? target_name : "无",
        op_name,
        content);

    jsonb_init(&b);
    jsonb_append(&b, "{\"model\":");
    jsonb_string(&b, model);
    jsonb_append(&b, ",\"temperature\":0,\"stream\":false,\"messages\":[");
    jsonb_append(&b, "{\"role\":\"system\",\"content\":");
    jsonb_string(&b, system_content);
    jsonb_append(&b, "},{\"role\":\"user\",\"content\":");
    jsonb_string(&b, user_content);
    jsonb_append(&b, "}]}");
    if (strlen(jsonb_cstr(&b)) >= sizeof(request_body)) {
        jsonb_free(&b);
        return 0;
    }
    snprintf(request_body, sizeof(request_body), "%s", jsonb_cstr(&b));
    jsonb_free(&b);

    if (strncmp(ai_url, "https://", 8) == 0) {
        if (https_post_json_via_curl(ai_url, request_body, response, sizeof(response)) != 0) {
            if (debug && *debug) fprintf(stderr, "[AI] HTTPS request failed via curl\n");
            return 0;
        }
    } else {
        if (http_post_json(ai_url, request_body, response, sizeof(response)) != 0) {
            if (debug && *debug) fprintf(stderr, "[AI] HTTP request failed\n");
            return 0;
        }
    }
    if (!*response) {
        if (debug && *debug) fprintf(stderr, "[AI] empty response\n");
        return 0;
    }
    if (debug && *debug) fprintf(stderr, "[AI] response=%s\n", response);

    root = json_parse(response);
    if (!root) {
        /* Try to extract embedded JSON from markdown code fences. */
        const char *js, *je;
        char tmp[AI_RESPONSE_MAX];
        js = find_json_in_text(response, &je);
        if (js && (size_t)(je - js) < sizeof(tmp)) {
            memcpy(tmp, js, (size_t)(je - js));
            tmp[je - js] = '\0';
            root = json_parse(tmp);
        }
        if (!root) {
            if (debug && *debug) fprintf(stderr, "[AI] JSON parse failed\n");
            return 0;
        }
    }

    choices = json_get(root, "choices");
    if (choices && choices->type == JSON_ARRAY && choices->count > 0) {
        message = json_get(choices->items[0], "message");
        content_val = message ? json_get(message, "content") : NULL;
        if (content_val && content_val->type == JSON_STRING && content_val->str) {
            JsonValue *inner = json_parse(content_val->str);
            if (!inner) {
                const char *js, *je;
                char tmp[AI_RESPONSE_MAX];
                js = find_json_in_text(content_val->str, &je);
                if (js && (size_t)(je - js) < sizeof(tmp)) {
                    memcpy(tmp, js, (size_t)(je - js));
                    tmp[je - js] = '\0';
                    inner = json_parse(tmp);
                }
            }
            if (inner) {
                JsonValue *danger = json_get(inner, "danger");
                if (danger && danger->type == JSON_BOOL) {
                    *creates_warning = danger->boolean ? 1 : 0;
                }
                {
                    const char *r = json_get_string(inner, "reason", "");
                    if (r && *r && reason_size > 0) {
                        snprintf(reason, reason_size, "%s", r);
                    }
                }
                json_free(inner);
            }
        }
    }
    json_free(root);
    return 1;
}
int ai_try_generate_narration(
    const char *room_name,
    const char *players_text,
    const char *ai_name,
    const char *recent_text,
    const char *constraint,
    char *out,
    size_t out_size)
{
    const char *debug = getenv("LUANSHA_AI_DEBUG");
    char system_prompt[1024];
    char user_prompt[2048];
    int ok;

    if (!out || out_size == 0) return 0;
    out[0] = '\0';

    snprintf(system_prompt, sizeof(system_prompt),
        "你是《乱杀法则》跑团游戏中的AI玩家%s。"
        "请根据当前情况说出一句话的行动/发言。"
        "要求：只能说一句话；不能使用连词；不要直接操控其他玩家的主意识；"
        "不要使用固定的模板句，不要重复前面已经说过的话；"
        "尽量自由创造独特、合理的行动，可以创造物品、设下陷阱、观察、移动、"
        "试探、谈判、准备攻击等；"
        "不要输出解释、不要加引号、不要输出JSON。",
        ai_name ? ai_name : "");

    snprintf(user_prompt, sizeof(user_prompt),
        "房间名：%s\n"
        "在线玩家：%s\n"
        "你：%s\n"
        "最近发言：%s\n"
        "当前约束：%s",
        room_name ? room_name : "未知",
        players_text ? players_text : "无",
        ai_name ? ai_name : "AI",
        recent_text && *recent_text ? recent_text : "暂无",
        constraint && *constraint ? constraint : "无");

    ok = ai_complete_text(system_prompt, user_prompt, out, out_size);
    if (ok && debug && *debug) fprintf(stderr, "[AI] generation ok: %s\n", out);
    return ok;
}
int ai_try_generate_lethal(
    const char *room_name,
    const char *players_text,
    const char *narrator,
    const char *target_name,
    const char *recent_text,
    char *out,
    size_t out_size)
{
    const char *debug = getenv("LUANSHA_AI_DEBUG");
    char system_prompt[1200];
    char user_prompt[2048];
    int ok;

    if (!out || out_size == 0) return 0;
    out[0] = '\0';

    snprintf(system_prompt, sizeof(system_prompt),
        "你是《乱杀法则》跑团游戏的AI创作者，负责为玩家生成一句致命攻击/致命威胁描述。"
        "要求："
        "1. 必须根据当前场景、玩家关系和最近发言，推断合理的致命方式；"
        "2. 只说一句话，不要用固定模板，不要直接说“我给了X致命一击”这种套话；"
        "3. 可以暗示陷阱、投毒、伏击、武器、环境利用等；"
        "4. 不要输出解释、不要加引号、不要输出JSON。");

    snprintf(user_prompt, sizeof(user_prompt),
        "房间名：%s\n在线玩家：%s\n攻击者：%s\n目标：%s\n最近发言：%s",
        room_name ? room_name : "未知",
        players_text ? players_text : "无",
        narrator ? narrator : "未知",
        target_name && *target_name ? target_name : "目标",
        recent_text && *recent_text ? recent_text : "暂无");

    ok = ai_complete_text(system_prompt, user_prompt, out, out_size);
    if (ok && debug && *debug) fprintf(stderr, "[AI] lethal ok: %s\n", out);
    return ok;
}