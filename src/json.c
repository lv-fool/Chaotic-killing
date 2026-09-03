#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* JSON value helpers                                                  */
/* ------------------------------------------------------------------ */

static JsonValue *value_new(JsonType type)
{
    JsonValue *v = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (v) {
        v->type = type;
    }
    return v;
}

void json_free(JsonValue *v)
{
    int i;
    if (!v) return;
    free(v->key);
    free(v->str);
    if (v->items) {
        for (i = 0; i < v->count; i++) {
            json_free(v->items[i]);
        }
        free(v->items);
    }
    free(v);
}

JsonValue *json_get(const JsonValue *obj, const char *key)
{
    int i;
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (i = 0; i < obj->count; i++) {
        if (obj->items[i]->key && strcmp(obj->items[i]->key, key) == 0) {
            return obj->items[i];
        }
    }
    return NULL;
}

const char *json_get_string(const JsonValue *obj, const char *key, const char *dflt)
{
    JsonValue *v = json_get(obj, key);
    if (v && v->type == JSON_STRING && v->str) return v->str;
    return dflt;
}

int json_get_int(const JsonValue *obj, const char *key, int dflt)
{
    JsonValue *v = json_get(obj, key);
    if (v && v->type == JSON_NUMBER) return (int)v->num;
    return dflt;
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    const char *end;
    int error;
} Parser;

static void skip_ws(Parser *ps)
{
    while (ps->p < ps->end && isspace((unsigned char)*ps->p)) ps->p++;
}

static int ensure_capacity(char **buf, size_t *cap, size_t *len, size_t need)
{
    char *nb;
    size_t nc;
    if (*len + need + 1 <= *cap) return 1;
    nc = *cap;
    while (nc < *len + need + 1) nc *= 2;
    nb = (char *)realloc(*buf, nc);
    if (!nb) return 0;
    *buf = nb;
    *cap = nc;
    return 1;
}

static int append_utf8_char(char **buf, size_t *cap, size_t *len, unsigned int c)
{
    if (c < 0x80) {
        if (!ensure_capacity(buf, cap, len, 1)) return 0;
        (*buf)[(*len)++] = (char)c;
    } else if (c < 0x800) {
        if (!ensure_capacity(buf, cap, len, 2)) return 0;
        (*buf)[(*len)++] = (char)(0xC0 | (c >> 6));
        (*buf)[(*len)++] = (char)(0x80 | (c & 0x3F));
    } else {
        if (!ensure_capacity(buf, cap, len, 3)) return 0;
        (*buf)[(*len)++] = (char)(0xE0 | (c >> 12));
        (*buf)[(*len)++] = (char)(0x80 | ((c >> 6) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | (c & 0x3F));
    }
    return 1;
}

static int parse_string(Parser *ps, char **out)
{
    char *buf;
    size_t cap = 32, len = 0;

    skip_ws(ps);
    if (ps->p >= ps->end || *ps->p != '"') {
        ps->error = 1;
        return 0;
    }
    ps->p++;
    buf = (char *)malloc(cap);
    if (!buf) { ps->error = 1; return 0; }

    while (ps->p < ps->end) {
        unsigned int c = (unsigned char)*ps->p++;
        if (c == '"') {
            buf[len] = '\0';
            *out = buf;
            return 1;
        }
        if (c == '\\') {
            if (ps->p >= ps->end) break;
            c = *ps->p++;
            switch (c) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'u': {
                    /* Basic Unicode escape: only handles BMP by encoding UTF-8. */
                    unsigned int u = 0;
                    int k;
                    for (k = 0; k < 4; k++) {
                        if (ps->p >= ps->end) { ps->error = 1; free(buf); return 0; }
                        c = *ps->p++;
                        if (c >= '0' && c <= '9') u = (u << 4) + (c - '0');
                        else if (c >= 'a' && c <= 'f') u = (u << 4) + (c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') u = (u << 4) + (c - 'A' + 10);
                        else { ps->error = 1; free(buf); return 0; }
                    }
                    if (!append_utf8_char(&buf, &cap, &len, u)) { ps->error = 1; free(buf); return 0; }
                    continue;
                }
                default:
                    ps->error = 1;
                    free(buf);
                    return 0;
            }
        }
        if (c >= 0x80) {
            /* Raw UTF-8 bytes are already encoded; copy them through unchanged. */
            if (!ensure_capacity(&buf, &cap, &len, 1)) {
                ps->error = 1;
                free(buf);
                return 0;
            }
            buf[len++] = (char)c;
        } else if (!append_utf8_char(&buf, &cap, &len, c)) {
            ps->error = 1;
            free(buf);
            return 0;
        }
    }
    free(buf);
    ps->error = 1;
    return 0;
}

static JsonValue *parse_value(Parser *ps);

static JsonValue *parse_array(Parser *ps)
{
    JsonValue *arr = value_new(JSON_ARRAY);
    if (!arr) { ps->error = 1; return NULL; }
    if (ps->p >= ps->end || *ps->p != '[') { ps->error = 1; json_free(arr); return NULL; }
    ps->p++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        return arr;
    }
    for (;;) {
        JsonValue *item;
        JsonValue **ni;
        if (ps->p >= ps->end) { ps->error = 1; break; }
        item = parse_value(ps);
        if (!item) break;
        ni = (JsonValue **)realloc(arr->items, sizeof(JsonValue *) * (arr->count + 1));
        if (!ni) { json_free(item); ps->error = 1; break; }
        arr->items = ni;
        arr->items[arr->count++] = item;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            return arr;
        }
        ps->error = 1;
        break;
    }
    json_free(arr);
    return NULL;
}

static JsonValue *parse_object(Parser *ps)
{
    JsonValue *obj = value_new(JSON_OBJECT);
    if (!obj) { ps->error = 1; return NULL; }
    if (ps->p >= ps->end || *ps->p != '{') { ps->error = 1; json_free(obj); return NULL; }
    ps->p++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        return obj;
    }
    for (;;) {
        char *key = NULL;
        JsonValue *val;
        JsonValue **ni;
        if (!parse_string(ps, &key)) { ps->error = 1; break; }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') { free(key); ps->error = 1; break; }
        ps->p++;
        val = parse_value(ps);
        if (!val) { free(key); ps->error = 1; break; }
        val->key = key;
        ni = (JsonValue **)realloc(obj->items, sizeof(JsonValue *) * (obj->count + 1));
        if (!ni) { json_free(val); ps->error = 1; break; }
        obj->items = ni;
        obj->items[obj->count++] = val;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            return obj;
        }
        ps->error = 1;
        break;
    }
    json_free(obj);
    return NULL;
}

static JsonValue *parse_value(Parser *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end) return NULL;
    if (*ps->p == '"') {
        char *s = NULL;
        JsonValue *v;
        if (!parse_string(ps, &s)) return NULL;
        v = value_new(JSON_STRING);
        if (!v) { free(s); return NULL; }
        v->str = s;
        return v;
    }
    if (*ps->p == '{') return parse_object(ps);
    if (*ps->p == '[') return parse_array(ps);
    if (strncmp(ps->p, "true", 4) == 0) {
        JsonValue *v = value_new(JSON_BOOL);
        if (!v) return NULL;
        v->boolean = 1;
        ps->p += 4;
        return v;
    }
    if (strncmp(ps->p, "false", 5) == 0) {
        JsonValue *v = value_new(JSON_BOOL);
        if (!v) return NULL;
        v->boolean = 0;
        ps->p += 5;
        return v;
    }
    if (strncmp(ps->p, "null", 4) == 0) {
        JsonValue *v = value_new(JSON_NULL);
        if (!v) return NULL;
        ps->p += 4;
        return v;
    }
    {
        char *end = NULL;
        double num = strtod(ps->p, &end);
        if (end != ps->p) {
            JsonValue *v = value_new(JSON_NUMBER);
            if (!v) return NULL;
            v->num = num;
            ps->p = end;
            return v;
        }
    }
    return NULL;
}

JsonValue *json_parse(const char *text)
{
    Parser ps;
    JsonValue *v;
    if (!text) return NULL;
    ps.p = text;
    ps.end = text + strlen(text);
    ps.error = 0;
    v = parse_value(&ps);
    if (!v || ps.error || ps.p != ps.end) {
        json_free(v);
        return NULL;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* Output buffer                                                       */
/* ------------------------------------------------------------------ */

void jsonb_init(JsonBuf *b)
{
    b->len = 0;
    b->cap = 256;
    b->data = (char *)malloc(b->cap);
    if (b->data) b->data[0] = '\0';
}

void jsonb_free(JsonBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void jsonb_append(JsonBuf *b, const char *s)
{
    size_t n = strlen(s);
    if (!b->data) return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap;
        char *nd;
        while (nc < b->len + n + 1) nc *= 2;
        nd = (char *)realloc(b->data, nc);
        if (!nd) return;
        b->data = nd;
        b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void jsonb_appendf(JsonBuf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    jsonb_append(b, tmp);
}

void jsonb_string(JsonBuf *b, const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    jsonb_append(b, "\"");
    while (*p) {
        unsigned char c = *p++;
        switch (c) {
            case '"':  jsonb_append(b, "\\\""); break;
            case '\\': jsonb_append(b, "\\\\"); break;
            case '\b': jsonb_append(b, "\\b"); break;
            case '\f': jsonb_append(b, "\\f"); break;
            case '\n': jsonb_append(b, "\\n"); break;
            case '\r': jsonb_append(b, "\\r"); break;
            case '\t': jsonb_append(b, "\\t"); break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    jsonb_append(b, esc);
                } else {
                    char tmp[2] = { (char)c, '\0' };
                    jsonb_append(b, tmp);
                }
        }
    }
    jsonb_append(b, "\"");
}

const char *jsonb_cstr(JsonBuf *b)
{
    return b->data ? b->data : "";
}