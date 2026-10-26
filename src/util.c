#define _POSIX_C_SOURCE 200809L
#include "util.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

int lbc_id(const char *text) {
    size_t n = text ? strlen(text) : 0;
    if (!n || n > 64) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}
int lbc_copy(char *out, size_t cap, const char *text) {
    size_t n = strlen(text);
    if (n >= cap) { if (cap) *out = 0; return -1; }
    memcpy(out, text, n + 1); return 0;
}
int lbc_format(char *out, size_t cap, const char *format, ...) {
    va_list args; va_start(args, format);
    int n = vsnprintf(out, cap, format, args); va_end(args);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}
int lbc_json_string(const Json *j, int object, const char *key, char *out, size_t cap, const char *fallback) {
    int n = json_string(j, json_get(j, object, key), out, cap);
    if (n < 0 && fallback) return lbc_copy(out, cap, fallback);
    return n < 0 ? -1 : 0;
}
int64_t lbc_json_int(const Json *j, int object, const char *key, int64_t fallback) {
    int64_t value;
    return json_i64(j, json_get(j, object, key), &value) ? fallback : value;
}
int lbc_json_bool(const Json *j, int object, const char *key, int fallback) {
    int value;
    return json_bool(j, json_get(j, object, key), &value) ? fallback : value;
}
int lbc_list_contains(const Setting *list, const char *text) {
    if (setting_type(list) == SETTING_STRING) return !strcasecmp(setting_str(list, ""), text);
    for (size_t i = 0; i < setting_size(list); ++i)
        if (!strcasecmp(setting_str(setting_at(list, i), ""), text)) return 1;
    return 0;
}
/* SplitMix64; per-process, noncryptographic. */
uint64_t lbc_random(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}
uint64_t lbc_random_below(uint64_t *state, uint64_t limit) {
    if (limit < 2) return 0;
    uint64_t x, threshold = (UINT64_MAX - limit + 1) % limit;
    do { x = lbc_random(state); } while (x < threshold);
    return x % limit;
}
int lbc_url_encode(char *out, size_t cap, const char *text) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        int plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (n + (plain ? 1u : 3u) >= cap) return -1;
        if (plain) out[n++] = (char)*p;
        else { out[n++] = '%'; out[n++] = hex[*p >> 4]; out[n++] = hex[*p & 15]; }
    }
    if (n >= cap) return -1;
    out[n] = 0; return 0;
}
int lbc_form_add(char *out, size_t cap, const char *key, const char *value) {
    size_t used = strlen(out);
    if (used + 2 >= cap) return -1;
    if (used) out[used++] = '&';
    if (lbc_url_encode(out + used, cap - used, key)) return -1;
    used += strlen(out + used);
    if (used + 2 >= cap) return -1;
    out[used++] = '=';
    return lbc_url_encode(out + used, cap - used, value);
}
int lbc_mkdirs(const char *path) {
    char *copy = strdup(path);
    if (!copy) return -1;
    for (char *p = copy + (*copy == '/'); ; ++p) {
        if (*p && *p != '/') continue;
        char saved = *p; *p = 0;
        if (*copy && mkdir(copy, 0700) && errno != EEXIST) { free(copy); return -1; }
        struct stat st;
        if (*copy && (stat(copy, &st) || !S_ISDIR(st.st_mode))) { free(copy); return -1; }
        *p = saved; if (!saved) break;
    }
    free(copy); return 0;
}
int lbc_atomic_write(const char *path, const char *data, size_t size) {
    size_t length = strlen(path);
    char *tmp = malloc(length + 16);
    if (!tmp) return -1;
    (void)snprintf(tmp, length + 16, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp), ok = 0;
    if (fd >= 0) {
        size_t n = 0;
        while (n < size) {
            ssize_t w = write(fd, data + n, size - n);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) break;
            n += (size_t)w;
        }
        ok = n == size && !fsync(fd);
        if (close(fd)) ok = 0;
        if (ok && rename(tmp, path)) ok = 0;
        if (!ok) (void)unlink(tmp);
    }
    free(tmp); return ok ? 0 : -1;
}
void lbc_safe_filename(char *out, size_t cap, const char *text) {
    if (!cap) return;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && n + 1 < cap; ++p)
        out[n++] = *p < 32 || *p == 127 || strchr("/\\:*?\"<>|", *p) ? '_' : (char)*p;
    while (n && (out[n-1] == '.' || out[n-1] == ' ')) --n;
    if (!n && cap > 1) out[n++] = '_';
    out[n] = 0;
}
int64_t lbc_seconds_us(double seconds) {
    if (isnan(seconds) || seconds <= 0) return 0;
    if (!isfinite(seconds) || seconds >= (double)INT64_MAX / 1000000.0) return INT64_MAX;
    return (int64_t)(seconds * 1000000.0);
}

static int json_append(char *out, size_t cap, size_t *used, const char *text) {
    size_t n = strlen(text);
    if (*used + n >= cap) return -1;
    memcpy(out + *used, text, n + 1); *used += n; return 0;
}
static int json_quote(char *out, size_t cap, size_t *used, const char *text) {
    if (json_append(out, cap, used, "\"")) return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        char escaped[8];
        if (*p == '"' || *p == '\\') { escaped[0] = '\\'; escaped[1] = (char)*p; escaped[2] = 0; }
        else if (*p < 32) (void)snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)*p);
        else { escaped[0] = (char)*p; escaped[1] = 0; }
        if (json_append(out, cap, used, escaped)) return -1;
    }
    return json_append(out, cap, used, "\"");
}
static int json_setting(const Setting *s, char *out, size_t cap, size_t *used, int depth) {
    if (depth > 64) return -1;
    SettingType type = setting_type(s); char value[64];
    if (type == SETTING_STRING) return json_quote(out, cap, used, setting_str(s, ""));
    if (type == SETTING_NULL) return json_append(out, cap, used, "null");
    if (type == SETTING_BOOL) return json_append(out, cap, used, setting_bool(s, 0) ? "true" : "false");
    if (type == SETTING_INT) {
        (void)snprintf(value, sizeof(value), "%" PRId64, setting_int(s, 0)); return json_append(out, cap, used, value);
    }
    if (type == SETTING_DOUBLE) {
        double number = setting_double(s, 0);
        if (!isfinite(number)) return -1;
        (void)snprintf(value, sizeof(value), "%.17g", number); return json_append(out, cap, used, value);
    }
    int map = type == SETTING_MAP;
    if (json_append(out, cap, used, map ? "{" : "[")) return -1;
    for (size_t i = 0; i < setting_size(s); ++i) {
        if (i && json_append(out, cap, used, ",")) return -1;
        if (map && (json_quote(out, cap, used, setting_key(s, i)) || json_append(out, cap, used, ":"))) return -1;
        if (json_setting(setting_at(s, i), out, cap, used, depth + 1)) return -1;
    }
    return json_append(out, cap, used, map ? "}" : "]");
}
int lbc_setting_json(const Setting *s, char *out, size_t cap) {
    if (!cap) return -1;
    *out = 0; size_t used = 0;
    return json_setting(s, out, cap, &used, 0);
}
