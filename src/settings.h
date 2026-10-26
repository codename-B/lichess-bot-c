#ifndef LBC_SETTINGS_H
#define LBC_SETTINGS_H

#include <stddef.h>
#include <stdint.h>

typedef struct Settings Settings;
typedef struct Setting Setting;
typedef enum {
    SETTING_NULL, SETTING_BOOL, SETTING_INT, SETTING_DOUBLE,
    SETTING_STRING, SETTING_LIST, SETTING_MAP
} SettingType;

/* One YAML 1.1 document; empty input gives an empty map.
 * Limit size, nesting, and alias expansion. Reject cycles and unsupported tags.
 * Errors never include configuration values. */
Settings *settings_load_file(const char *path, char *error, size_t error_cap);
Settings *settings_load_string(const char *yaml, char *error, size_t error_cap);
void settings_free(Settings *settings);

/* Dotted paths; .0 or [0] list indices; empty path selects root.
 * Use setting_get for literal keys containing dots. */
const Setting *settings_get(const Settings *settings, const char *path);
const Setting *setting_get(const Setting *map, const char *key);
SettingType setting_type(const Setting *setting);
size_t setting_size(const Setting *setting);
const Setting *setting_at(const Setting *setting, size_t index);
const char *setting_key(const Setting *setting, size_t index);

/* Require matching scalar types; int also accepts integral finite doubles.
 * setting_text returns scalar text, or NULL for null/maps/lists. */
const char *setting_str(const Setting *setting, const char *fallback);
const char *setting_text(const Setting *setting);
int setting_bool(const Setting *setting, int fallback);
int64_t setting_int(const Setting *setting, int64_t fallback);
double setting_double(const Setting *setting, double fallback);
const char *settings_str(const Settings *settings, const char *path, const char *fallback);
int settings_bool(const Settings *settings, const char *path, int fallback);
int64_t settings_int(const Settings *settings, const char *path, int64_t fallback);
double settings_double(const Settings *settings, const char *path, double fallback);

/* Mutate during startup; return 0 on success, -1 on invalid input or OOM.
 * Values are copied; replacement invalidates old references.
 * Defaults fill missing keys recursively. */
int settings_set(Settings *settings, const char *path, const Setting *value);
int settings_set_str(Settings *settings, const char *path, const char *value);
int settings_set_bool(Settings *settings, const char *path, int value);
int settings_set_int(Settings *settings, const char *path, int64_t value);
int settings_set_double(Settings *settings, const char *path, double value);
int settings_set_null(Settings *settings, const char *path);
int settings_apply_defaults(Settings *settings, const Settings *defaults);
int settings_listify(Settings *settings, const char *path, int null_as_empty);
int settings_list_append(Settings *settings, const Setting *list, const Setting *value);
int settings_map_set(Settings *settings, const Setting *map, const char *key, const Setting *value);
/* NULL items become YAML null. */
int settings_map_set_list(Settings *settings, const Setting *map, const char *key,
                          const Setting *const *items, size_t count);

#endif
