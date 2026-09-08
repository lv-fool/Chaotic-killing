#include "server.h"
#include "game.h"
#include "json.h"
#include "ai.h"
#include "web_assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <shellapi.h>
  #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "shell32.lib")
  #endif
  typedef SOCKET socket_t;
  #define CLOSE_SOCKET(s) closesocket(s)
  #define INVALID_SOCK INVALID_SOCKET
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  typedef int socket_t;
  #define INVALID_SOCK (-1)
  #define CLOSE_SOCKET(s) close(s)
#endif

#define REQ_BUF_SIZE 16384
#define MAX_BODY_SIZE 8192
#define WEB_PATH_MAX 512

/* Phase 0：简单限流 —— 每个 IP 在固定时间窗内允许的请求次数上限（防止被打爆）。 */
#define RATE_TABLE_SIZE 256
#define RATE_LIMIT_WINDOW_SEC 10
#define RATE_LIMIT_MAX_REQS 120

static int rate_limit_window_sec(void)
{
    const char *s = getenv("LUANSHA_RATE_LIMIT_WINDOW");
    if (s && *s) {
        int v = atoi(s);
        if (v > 0) return v;
    }
    return RATE_LIMIT_WINDOW_SEC;
}

static int rate_limit_max_reqs(void)
{
    const char *s = getenv("LUANSHA_RATE_LIMIT_MAX");
    if (s && *s) {
        int v = atoi(s);
        if (v > 0) return v;
    }
    return RATE_LIMIT_MAX_REQS;
}

static const char *WEB_ROOT = "web";

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t i = 0, o = 0;
    if (!src || !dst || dst_size == 0) return -1;
    while (src[i] && o + 1 < dst_size) {
        if (src[i] == '%' && hex_val(src[i+1]) >= 0 && hex_val(src[i+2]) >= 0) {
            dst[o++] = (char)((hex_val(src[i+1]) << 4) | hex_val(src[i+2]));
            i += 3;
        } else if (src[i] == '+') {
            dst[o++] = ' ';
            i++;
        } else {
            dst[o++] = src[i++];
        }
    }
    dst[o] = '\0';
    return 0;
}

static const char *query_param(const char *query, const char *name, char *out, size_t out_size)
{
    const char *p = query;
    size_t n = strlen(name);
    if (!query) return NULL;
    while (*p) {
        const char *eq;
        const char *amp = strchr(p, '&');
        size_t seg_len = amp ? (size_t)(amp - p) : strlen(p);
        char *copy = (char *)malloc(seg_len + 1);
        char dec[256];
        if (!copy) return NULL;
        memcpy(copy, p, seg_len);
        copy[seg_len] = '\0';
        eq = strchr(copy, '=');
        if (eq && strncmp(copy, name, n) == 0 && copy[n] == '=') {
            size_t dec_len;
            url_decode(eq + 1, dec, sizeof(dec));
            dec_len = strlen(dec);
            if (dec_len >= out_size) dec_len = out_size - 1;
            memcpy(out, dec, dec_len);
            out[dec_len] = '\0';
            free(copy);
            return out;
        }
        free(copy);
        if (!amp) break;
        p = amp + 1;
    }
    return NULL;
}

static int parse_operation(const char *s)
{
    if (!s) return OP_NORMAL;
    if (strcmp(s, "create") == 0) return OP_CREATE;
    if (strcmp(s, "twist") == 0) return OP_TWIST;
    if (strcmp(s, "explain") == 0) return OP_EXPLAIN;
    if (strcmp(s, "limit") == 0) return OP_LIMIT;
    return OP_NORMAL;
}

static const char *mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    return "application/octet-stream";
}

/* ------------------------------------------------------------------ */
/* Socket helpers                                                      */
/* ------------------------------------------------------------------ */

static int send_all(socket_t client, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = send(client, data + sent, (int)(len - sent), 0);
#else
        ssize_t n = send(client, data + sent, len - sent, 0);
#endif
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void http_response(socket_t client, int status, const char *ctype, const char *body)
{
    char head[512];
    int n;
    if (!body) body = "";
    n = snprintf(head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        status,
        status == 200 ? "OK"
        : status == 404 ? "Not Found"
        : status == 400 ? "Bad Request"
        : status == 413 ? "Payload Too Large"
        : status == 429 ? "Too Many Requests"
        : "Error",
        ctype, strlen(body));
    if (n > 0) send_all(client, head, (size_t)n);
    send_all(client, body, strlen(body));
}

static void send_json(socket_t client, JsonBuf *b)
{
    http_response(client, 200, "application/json; charset=utf-8", jsonb_cstr(b));
}

static void send_error_json(socket_t client, const char *msg)
{
    JsonBuf b;
    jsonb_init(&b);
    jsonb_append(&b, "{\"ok\":false,\"error\":");
    jsonb_string(&b, msg);
    jsonb_append(&b, "}");
    send_json(client, &b);
    jsonb_free(&b);
}

/* Build {"ok":true,...,"state": STATE_JSON} without running AI work. */
static void send_api_success_with_state(socket_t client, int room_id, const char *token)
{
    JsonBuf state, res;
    jsonb_init(&state);
    jsonb_init(&res);
    if (game_room_to_json_ex(room_id, token, &state, 0) != 0) {
        send_error_json(client, "Room not found");
        jsonb_free(&state);
        jsonb_free(&res);
        return;
    }
    jsonb_append(&res, "{\"ok\":true,\"state\":");
    jsonb_append(&res, jsonb_cstr(&state));
    jsonb_append(&res, "}");
    send_json(client, &res);
    jsonb_free(&state);
    jsonb_free(&res);
}

/* State GET: may process one pending AI turn / AI judgment before returning. */
static void send_state_processed(socket_t client, int room_id, const char *token)
{
    JsonBuf state, res;
    jsonb_init(&state);
    jsonb_init(&res);
    if (game_room_to_json_ex(room_id, token, &state, 1) != 0) {
        send_error_json(client, "Room not found");
        jsonb_free(&state);
        jsonb_free(&res);
        return;
    }
    jsonb_append(&res, "{\"ok\":true,\"state\":");
    jsonb_append(&res, jsonb_cstr(&state));
    jsonb_append(&res, "}");
    send_json(client, &res);
    jsonb_free(&state);
    jsonb_free(&res);
}

static void open_browser(int port)
{
    char url[128];
    snprintf(url, sizeof(url), "http://localhost:%d/", port);
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "xdg-open %s >/dev/null 2>&1 &", url);
        system(cmd);
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Static file serving                                                 */
/* ------------------------------------------------------------------ */

static int serve_static(socket_t client, const char *url_path)
{
    const WebAsset *asset;
    char full[WEB_PATH_MAX + 32];
    FILE *f;
    long size;
    char *buf;
    const char *p = url_path;
    size_t root_len, p_len;

    asset = strcmp(url_path, "/") == 0 ? web_asset_find("/index.html") : web_asset_find(url_path);
    if (asset) {
        http_response(client, 200, asset->mime, asset->data);
        return 0;
    }

    if (*p == '/') p++;
    if (!*p) p = "index.html";

    if (strstr(p, "..") || strchr(p, '\\')) {
        http_response(client, 403, "text/plain; charset=utf-8", "Forbidden");
        return -1;
    }

    root_len = strlen(WEB_ROOT);
    p_len = strlen(p);
    if (root_len + 1 + p_len >= sizeof(full)) {
        http_response(client, 404, "text/plain; charset=utf-8", "Not Found");
        return -1;
    }
    memcpy(full, WEB_ROOT, root_len);
    full[root_len] = '/';
    memcpy(full + root_len + 1, p, p_len + 1);
    f = fopen(full, "rb");
    if (!f) {
        http_response(client, 404, "text/plain; charset=utf-8", "Not Found");
        return -1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > 10 * 1024 * 1024) {
        fclose(f);
        http_response(client, 500, "text/plain; charset=utf-8", "File too large");
        return -1;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        http_response(client, 500, "text/plain; charset=utf-8", "Out of memory");
        return -1;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        http_response(client, 500, "text/plain; charset=utf-8", "Read error");
        return -1;
    }
    fclose(f);
    buf[size] = '\0';
    http_response(client, 200, mime_type(full), buf);
    free(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Request parsing                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char method[8];
    char path[512];
    char query[512];
    char body[REQ_BUF_SIZE];
} HttpRequest;

static int read_request(socket_t client, HttpRequest *req)
{
    char buf[REQ_BUF_SIZE];
    size_t used = 0;
    char *header_end = NULL;
    long content_length = 0;

    memset(req, 0, sizeof(*req));
    memset(buf, 0, sizeof(buf));

    while (used + 1 < sizeof(buf)) {
        int n = recv(client, buf + used, (int)(sizeof(buf) - used - 1), 0);
        if (n <= 0) return -1;
        used += (size_t)n;
        buf[used] = '\0';
        header_end = strstr(buf, "\r\n\r\n");
        if (header_end) {
            const char *cl = strstr(buf, "Content-Length:");
            const char *tmp;
            if (!cl) cl = strstr(buf, "content-length:");
            if (cl) {
                cl += 15;
                while (*cl == ' ' || *cl == '\t') cl++;
                content_length = strtol(cl, NULL, 10);
            }
            /* Phase 0：请求体大小限制，防止超大请求耗尽内存。 */
            if (content_length < 0) return -1;
            if (content_length > MAX_BODY_SIZE) return -2;
            tmp = header_end + 4;
            if (content_length > 0 && (size_t)(buf + used - tmp) < (size_t)content_length) {
                continue;
            }
            break;
        }
        if (used == sizeof(buf) - 1) return -1;
    }

    if (!header_end) return -1;
    *header_end = '\0';

    {
        char *line_end = strstr(buf, "\r\n");
        char *sp1, *sp2;
        if (!line_end) return -1;
        *line_end = '\0';
        sp1 = strchr(buf, ' ');
        if (!sp1) return -1;
        sp2 = strchr(sp1 + 1, ' ');
        if (!sp2) return -1;
        *sp1 = '\0';
        *sp2 = '\0';

        snprintf(req->method, sizeof(req->method), "%s", buf);
        {
            char *q = strchr(sp1 + 1, '?');
            if (q) {
                size_t plen = (size_t)(q - (sp1 + 1));
                if (plen >= sizeof(req->path)) plen = sizeof(req->path) - 1;
                memcpy(req->path, sp1 + 1, plen);
                req->path[plen] = '\0';
                snprintf(req->query, sizeof(req->query), "%s", q + 1);
            } else {
                snprintf(req->path, sizeof(req->path), "%s", sp1 + 1);
                req->query[0] = '\0';
            }
        }

        if (content_length > 0) {
            const char *body_start = header_end + 4;
            size_t have = (size_t)(buf + used - body_start);
            if (have > (size_t)content_length) have = (size_t)content_length;
            memcpy(req->body, body_start, have);
            req->body[have] = '\0';
        }
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* API handlers                                                        */
/* ------------------------------------------------------------------ */

static void handle_api(socket_t client, HttpRequest *req)
{
    const char *path = req->path;

    if (strcmp(path, "/api/room/create") == 0 && strcmp(req->method, "POST") == 0) {
        JsonValue *v = json_parse(req->body);
        const char *name, *player_name, *mode_str;
        int mode = MODE_AUTO;
        int difficulty = 0;
        char token[TOKEN_LEN + 1] = {0};
        int room_id;
        JsonBuf res;
        if (!v) { send_error_json(client, "Invalid JSON"); return; }
        name = json_get_string(v, "name", "");
        player_name = json_get_string(v, "player_name", "");
        mode_str = json_get_string(v, "mode", "auto");
        if (strcmp(mode_str, "gm") == 0) mode = MODE_GM;
        difficulty = json_get_int(v, "difficulty", 0);
        room_id = game_create_room(name, mode, difficulty, player_name, token);
        json_free(v);
        if (room_id < 0) { send_error_json(client, "Create failed"); return; }
        jsonb_init(&res);
        jsonb_appendf(&res, "{\"ok\":true,\"room_id\":%d,\"token\":", room_id);
        jsonb_string(&res, token);
        jsonb_append(&res, "}");
        send_json(client, &res);
        jsonb_free(&res);
        return;
    }

    if (strcmp(path, "/api/room/join") == 0 && strcmp(req->method, "POST") == 0) {
        JsonValue *v = json_parse(req->body);
        int room_id;
        const char *player_name;
        char token[TOKEN_LEN + 1] = {0};
        JsonBuf res;
        if (!v) { send_error_json(client, "Invalid JSON"); return; }
        room_id = json_get_int(v, "room_id", 0);
        player_name = json_get_string(v, "player_name", "");
        room_id = game_join_room(room_id, player_name, token);
        json_free(v);
        if (room_id < 0) { send_error_json(client, "Join failed"); return; }
        jsonb_init(&res);
        jsonb_appendf(&res, "{\"ok\":true,\"room_id\":%d,\"token\":", room_id);
        jsonb_string(&res, token);
        jsonb_append(&res, "}");
        send_json(client, &res);
        jsonb_free(&res);
        return;
    }

    if (strcmp(path, "/api/room/add_ai") == 0 && strcmp(req->method, "POST") == 0) {
        JsonValue *v = json_parse(req->body);
        int room_id, r;
        const char *token;
        if (!v) { send_error_json(client, "Invalid JSON"); return; }
        room_id = json_get_int(v, "room_id", 0);
        token = json_get_string(v, "token", "");
        r = game_add_ai(room_id);
        json_free(v);
        if (r < 0) { send_error_json(client, "Add AI failed"); return; }
        send_api_success_with_state(client, room_id, token);
        return;
    }

    if (strcmp(path, "/api/game/set_ready") == 0 && strcmp(req->method, "POST") == 0) {
        JsonValue *v = json_parse(req->body);
        int room_id, ready, r;
        const char *token;
        if (!v) { send_error_json(client, "Invalid JSON"); return; }
        room_id = json_get_int(v, "room_id", 0);
        ready = json_get_int(v, "ready", 0);
        token = json_get_string(v, "token", "");
        r = game_set_ready(room_id, token, ready);
        json_free(v);
        if (r < 0) { send_error_json(client, "Set ready failed"); return; }
        send_api_success_with_state(client, room_id, token);
        return;
    }

    if (strcmp(path, "/api/game/start") == 0 && strcmp(req->method, "POST") == 0) {
        JsonValue *v = json_parse(req->body);
        int room_id, fill_ai, r;
        const char *token;
        const char *err = "Start failed";
        if (!v) { send_error_json(client, "Invalid JSON"); return; }
        room_id = json_get_int(v, "room_id", 0);
        token = json_get_string(v, "token", "");
        fill_ai = json_get_int(v, "fill_ai", 1);
        r = game_start(room_id, token, fill_ai);
        json_free(v);
        if (r < 0) {
            if (r == -1) err = "房间或玩家不存在";
            else if (r == -2) err = "房间不在等待状态";
            else if (r == -3) err = "只有房主可以开始游戏";
            else if (r == -4) err = "至少需要4名玩家";
            else if (r == -5) err = "玩家数量超过上限";
            else if (r == -6) err = "还有真人玩家未准备";
            send_error_json(client, err);
            return;
        }
        send_api_success_with_state(client, room_id, token);
        return;
    }

    if (strcmp(path, "/api/game/speak") == 0 && strcmp(req->method, "POST") == 0) {
        JsonValue *v = json_parse(req->body);
        int room_id, target_id, r;
        const char *token, *operation, *content, *keyword;
        char err[256] = "";
        if (!v) { send_error_json(client, "Invalid JSON"); return; }
        room_id = json_get_int(v, "room_id", 0);
        target_id = json_get_int(v, "target_id", 0);
        token = json_get_string(v, "token", "");
        operation = json_get_string(v, "operation", "normal");
        content = json_get_string(v, "content", "");
        keyword = json_get_string(v, "limit_keyword", "");
        r = game_speak(room_id, token, parse_operation(operation), content, keyword, target_id,
                       err, sizeof(err));
        json_free(v);
        if (r < 0) {
            if (err[0]) send_error_json(client, err);
            else send_error_json(client, "Speak rejected");
            return;
        }
        send_api_success_with_state(client, room_id, token);
        return;
    }

    if (strcmp(path, "/api/game/declare_death") == 0 && strcmp(req->method, "POST") == 0) {
        JsonValue *v = json_parse(req->body);
        int room_id, target_id, r;
        const char *token;
        if (!v) { send_error_json(client, "Invalid JSON"); return; }
        room_id = json_get_int(v, "room_id", 0);
        target_id = json_get_int(v, "target_id", 0);
        token = json_get_string(v, "token", "");
        r = game_declare_death(room_id, token, target_id);
        json_free(v);
        if (r < 0) { send_error_json(client, "Declare death failed"); return; }
        send_api_success_with_state(client, room_id, token);
        return;
    }

    if (strcmp(path, "/api/game/gm") == 0 && strcmp(req->method, "POST") == 0) {
        JsonValue *v = json_parse(req->body);
        int room_id, target_id, r;
        const char *token, *action;
        if (!v) { send_error_json(client, "Invalid JSON"); return; }
        room_id = json_get_int(v, "room_id", 0);
        target_id = json_get_int(v, "target_id", 0);
        token = json_get_string(v, "token", "");
        action = json_get_string(v, "action", "");
        r = game_gm_command(room_id, token, action, target_id);
        json_free(v);
        if (r < 0) { send_error_json(client, "GM command failed"); return; }
        send_api_success_with_state(client, room_id, token);
        return;
    }

    if (strcmp(path, "/api/ai/config") == 0 && strcmp(req->method, "GET") == 0) {
        const char *u = ai_get_url();
        const char *m = ai_get_model();
        const char *k = ai_get_key();
        JsonBuf res;
        jsonb_init(&res);
        jsonb_append(&res, "{\"ok\":true,\"url\":");
        jsonb_string(&res, u ? u : "");
        jsonb_append(&res, ",\"model\":");
        jsonb_string(&res, m ? m : "");
        jsonb_append(&res, ",\"key_set\":");
        jsonb_append(&res, (k && *k) ? "true}" : "false}");
        send_json(client, &res);
        jsonb_free(&res);
        return;
    }

    if (strcmp(path, "/api/ai/config") == 0 && strcmp(req->method, "POST") == 0) {
        JsonValue *v = json_parse(req->body);
        const char *url, *model, *key;
        JsonBuf res;
        if (!v) { send_error_json(client, "Invalid JSON"); return; }
        url = json_get_string(v, "url", "");
        model = json_get_string(v, "model", "");
        key = json_get_string(v, "key", "");
        /* Key 留空表示保留已有 Key，不清空（避免用户误操作导致 AI 失效）。 */
        if ((!key || !*key) && ai_get_key() && *ai_get_key()) {
            key = ai_get_key();
        }
        ai_configure(url, model, key);
        json_free(v);
        jsonb_init(&res);
        jsonb_append(&res, "{\"ok\":true}");
        send_json(client, &res);
        jsonb_free(&res);
        return;
    }

    if (strcmp(path, "/api/ai/test") == 0 && strcmp(req->method, "GET") == 0) {
        char reply[512] = "";
        const char *k = ai_get_key();
        JsonBuf res;
        jsonb_init(&res);
        if (!k || !*k) {
            /* Phase 0.5：不内置 Key，引导用户在前端弹窗里填写。 */
            jsonb_append(&res,
                "{\"ok\":false,\"error\":\"未配置 LUANSHA_AI_KEY（AI 已回退关键词判断）。"
                "请点击页面上的 AI 配置按钮填写 Key。\"}");
        } else if (ai_test_connection(reply, sizeof(reply)) == 1) {
            jsonb_append(&res, "{\"ok\":true,\"reply\":");
            jsonb_string(&res, reply);
            jsonb_append(&res, "}");
        } else {
            jsonb_append(&res, "{\"ok\":false,\"error\":\"AI connection failed（请检查 Key / 接口地址）\"}");
        }
        send_json(client, &res);
        jsonb_free(&res);
        return;
    }

    if (strcmp(path, "/api/ai/lethal") == 0 && strcmp(req->method, "POST") == 0) {
        JsonValue *v = json_parse(req->body);
        int room_id, target_id;
        const char *token;
        Room *room;
        Player *p;
        char players_text[512] = "";
        char recent_text[1024] = "";
        char reply[512] = "";
        int i, start, ti = -1, pi, k;
        JsonBuf res;

        if (!v) { send_error_json(client, "Invalid JSON"); return; }
        room_id = json_get_int(v, "room_id", 0);
        target_id = json_get_int(v, "target_id", 0);
        token = json_get_string(v, "token", "");
        json_free(v);

        room = game_find_room(room_id);
        p = room ? game_find_player(room, token) : NULL;
        if (!room || !p) { send_error_json(client, "Room or player not found"); return; }
        for (i = 0; i < room->player_count; i++) {
            if (room->players[i].id == target_id) { ti = i; break; }
        }
        if (ti < 0) { send_error_json(client, "Target not found"); return; }

        for (i = 0; i < room->player_count; i++) {
            if (i) strncat(players_text, ", ", sizeof(players_text) - strlen(players_text) - 1);
            strncat(players_text, room->players[i].name,
                    sizeof(players_text) - strlen(players_text) - 1);
        }
        start = room->narrative_count > 3 ? room->narrative_count - 3 : 0;
        for (i = start; i < room->narrative_count; i++) {
            char line[600];
            pi = -1;
            for (k = 0; k < room->player_count; k++) {
                if (room->players[k].id == room->narratives[i].player_id) { pi = k; break; }
            }
            snprintf(line, sizeof(line), "%s: %s\n",
                     pi >= 0 ? room->players[pi].name : "System",
                     room->narratives[i].content);
            strncat(recent_text, line, sizeof(recent_text) - strlen(recent_text) - 1);
        }

        if (ai_try_generate_lethal(room->name, players_text, p->name,
                                   room->players[ti].name,
                                   recent_text, reply, sizeof(reply)) != 1 || !reply[0]) {
            send_error_json(client, "AI lethal generation failed");
            return;
        }

        jsonb_init(&res);
        jsonb_append(&res, "{\"ok\":true,\"reply\":");
        jsonb_string(&res, reply);
        jsonb_append(&res, "}");
        send_json(client, &res);
        jsonb_free(&res);
        return;
    }

    if (strcmp(path, "/api/game/state") == 0 && strcmp(req->method, "GET") == 0) {
        int room_id = 0;
        char token[64] = "";
        const char *q = req->query;
        char value[64];
        if (query_param(q, "room_id", value, sizeof(value))) room_id = atoi(value);
        query_param(q, "token", token, sizeof(token));
        send_state_processed(client, room_id, token);
        return;
    }

    send_error_json(client, "Unknown API");
}

/* ------------------------------------------------------------------ */
/* Phase 0：限流（per-IP 固定窗口计数）                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char ip[INET_ADDRSTRLEN];
    time_t window_start;
    int count;
} RateEntry;

static RateEntry g_rate[RATE_TABLE_SIZE];

/* 返回 1 = 放行，0 = 触发限流。表满且无过期槽位时 fail-open（放行）。 */
static int rate_allow(const char *ip, time_t now)
{
    int i, free_slot = -1, expired_slot = -1;

    for (i = 0; i < RATE_TABLE_SIZE; i++) {
        if (!g_rate[i].ip[0]) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (strcmp(g_rate[i].ip, ip) == 0) {
            if (now - g_rate[i].window_start >= rate_limit_window_sec()) {
                g_rate[i].window_start = now;
                g_rate[i].count = 1;
                return 1;
            }
            g_rate[i].count++;
            return g_rate[i].count <= rate_limit_max_reqs();
        }
        if (expired_slot < 0 && now - g_rate[i].window_start >= rate_limit_window_sec()) {
            expired_slot = i;
        }
    }
    if (free_slot >= 0) {
        snprintf(g_rate[free_slot].ip, sizeof(g_rate[free_slot].ip), "%s", ip);
        g_rate[free_slot].window_start = now;
        g_rate[free_slot].count = 1;
        return 1;
    }
    if (expired_slot >= 0) {
        snprintf(g_rate[expired_slot].ip, sizeof(g_rate[expired_slot].ip), "%s", ip);
        g_rate[expired_slot].window_start = now;
        g_rate[expired_slot].count = 1;
        return 1;
    }
    return 1;
}

static void client_ip_str(const struct sockaddr_in *a, char *out, size_t n)
{
    if (inet_ntop(AF_INET, &a->sin_addr, out, (socklen_t)n) == NULL) {
        snprintf(out, n, "0.0.0.0");
    }
}


/* ------------------------------------------------------------------ */
/* Main server loop                                                    */
/* ------------------------------------------------------------------ */

static void handle_client(socket_t client, const char *ip)
{
    HttpRequest req;
    int rr;

    /* Phase 0：限流 —— 每个 IP 在窗口内请求过多直接拒绝。 */
    if (!rate_allow(ip, time(NULL))) {
        http_response(client, 429, "text/plain; charset=utf-8",
                      "Too Many Requests");
        return;
    }

    rr = read_request(client, &req);
    if (rr == -2) {
        http_response(client, 413, "text/plain; charset=utf-8",
                      "Payload Too Large");
        return;
    }
    if (rr != 0) {
        http_response(client, 400, "text/plain; charset=utf-8",
                      "Bad Request");
        return;
    }
    if (strncmp(req.path, "/api/", 5) == 0) {
        handle_api(client, &req);
    } else {
        serve_static(client, req.path);
    }
}

int server_run_ex(int port, int open_browser_flag)
{
    socket_t listener = INVALID_SOCK;
    struct sockaddr_in addr;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCK) {
        fprintf(stderr, "socket failed\n");
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "bind failed on port %d\n", port);
#ifdef _WIN32
        CLOSE_SOCKET(listener);
        WSACleanup();
#endif
        return 1;
    }
    if (listen(listener, 16) != 0) {
        fprintf(stderr, "listen failed\n");
#ifdef _WIN32
        CLOSE_SOCKET(listener);
        WSACleanup();
#endif
        return 1;
    }

    printf("Server started: http://localhost:%d\n", port);
    printf("Press Ctrl+C to stop.\n");
    fflush(stdout);
    if (open_browser_flag) open_browser(port);

    for (;;) {
        socket_t client;
        struct sockaddr_in client_addr;
#ifdef _WIN32
        int addr_len = sizeof(client_addr);
#else
        socklen_t addr_len = sizeof(client_addr);
#endif

        client = accept(listener, (struct sockaddr *)&client_addr, &addr_len);
        if (client == INVALID_SOCK) {
            continue;
        }
        {
            char ip[INET_ADDRSTRLEN];
            client_ip_str(&client_addr, ip, sizeof(ip));
            handle_client(client, ip);
        }
        CLOSE_SOCKET(client);
    }

#ifdef _WIN32
    CLOSE_SOCKET(listener);
    WSACleanup();
#endif
    return 0;
}

int server_run(int port)
{
    return server_run_ex(port, 0);
}