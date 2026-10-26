#ifndef LICHESS_BOT_JSON_H
#define LICHESS_BOT_JSON_H

#include <stddef.h>
#include <stdint.h>

enum {
    JSON_OBJECT = 1,
    JSON_ARRAY,
    JSON_STRING,
    JSON_NUMBER,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NULL
};

#define JSON_MAX_DEPTH 64

/* String spans exclude quotes; other spans include the whole value.
 * next follows the token's subtree. The caller keeps input and tokens alive. */
typedef struct {
    int start, end, next, type;
} JsonTok;

typedef struct {
    const char *text;
    size_t length;
    JsonTok *tokens;
    int count;
} Json;

/* Parse one JSON value without allocations; input need not be NUL-terminated.
 * Reject invalid UTF-8/escapes, trailing data, depth over 64, or token overflow.
 * Failure resets count to zero. */
int json_parse(Json *json, const char *text, size_t length,
               JsonTok *tokens, int capacity);

/* Find the first direct member with a matching decoded key. */
int json_get(const Json *json, int object, const char *key);

/* Compare decoded text with ASCII: 1 equal, 0 otherwise. */
int json_eq(const Json *json, int token, const char *plain_ascii);

/* Decode UTF-8; cap includes NUL. Return byte length, or -1 on error.
 * Reject decoded NUL. Failure clears out when out != NULL and cap > 0. */
int json_string(const Json *json, int token, char *out, size_t cap);

/* Return 0 on success, -1 on failure; leave *out unchanged on failure.
 * Integers reject decimal/exponent notation and overflow. */
int json_i64(const Json *json, int token, int64_t *out);
int json_bool(const Json *json, int token, int *out);

#endif
