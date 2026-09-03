#ifndef JSON_H
#define JSON_H

#include <stddef.h>

typedef enum JsonType {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue {
    JsonType type;
    char *key;          /* key when part of an object */
    char *str;          /* string value */
    double num;         /* number value */
    int boolean;        /* bool value */
    struct JsonValue **items;
    int count;
} JsonValue;

/* Parse a JSON document. Returns NULL on error. */
JsonValue *json_parse(const char *text);

/* Free a JSON value tree. */
void json_free(JsonValue *v);

/* Get a member from an object, or NULL. */
JsonValue *json_get(const JsonValue *obj, const char *key);

/* Convenience getters. Defaults are returned when missing/mismatched. */
const char *json_get_string(const JsonValue *obj, const char *key, const char *dflt);
int json_get_int(const JsonValue *obj, const char *key, int dflt);

/* Minimal JSON output buffer. */
typedef struct JsonBuf {
    char *data;
    size_t len;
    size_t cap;
} JsonBuf;

void jsonb_init(JsonBuf *b);
void jsonb_free(JsonBuf *b);
void jsonb_append(JsonBuf *b, const char *s);
void jsonb_appendf(JsonBuf *b, const char *fmt, ...);
void jsonb_string(JsonBuf *b, const char *s);
const char *jsonb_cstr(JsonBuf *b);

#endif /* JSON_H */