#ifndef LBC_UTIL_H
#define LBC_UTIL_H
#include "json.h"
#include "settings.h"
#include <stddef.h>
#include <stdint.h>

int lbc_id(const char *text);
int lbc_copy(char *out, size_t cap, const char *text);
int lbc_format(char *out, size_t cap, const char *format, ...);
int lbc_json_string(const Json *j, int object, const char *key, char *out, size_t cap, const char *fallback);
int64_t lbc_json_int(const Json *j, int object, const char *key, int64_t fallback);
int lbc_json_bool(const Json *j, int object, const char *key, int fallback);
int lbc_list_contains(const Setting *list, const char *text);
uint64_t lbc_random(uint64_t *state);
uint64_t lbc_random_below(uint64_t *state, uint64_t limit);
int lbc_url_encode(char *out, size_t cap, const char *text);
int lbc_form_add(char *out, size_t cap, const char *key, const char *value);
int lbc_mkdirs(const char *path);
int lbc_atomic_write(const char *path, const char *data, size_t size);
void lbc_safe_filename(char *out, size_t cap, const char *text);
int64_t lbc_seconds_us(double seconds);
int lbc_setting_json(const Setting *setting, char *out, size_t cap);
#endif
