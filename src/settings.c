#define _POSIX_C_SOURCE 200809L
#include "settings.h"

#include <yaml.h>
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

#define INPUT_LIMIT (4U * 1024U * 1024U)
#define VALUE_LIMIT (16U * 1024U * 1024U)
#define NODE_LIMIT 65536U
#define DEPTH_LIMIT 64U
#define ANCHOR_LIMIT 4096U

typedef struct { char *key; struct Setting *value; } Entry;
struct Setting {
    SettingType type;
    char *text;
    union {
        int boolean;
        int64_t integer;
        double real;
        struct { struct Setting **values; size_t size, capacity; } list;
        struct { Entry *entries; size_t size, capacity; } map;
    } as;
};
struct Settings { Setting *root; size_t nodes, bytes; };
typedef struct { char *name; Setting *value; int complete; } Anchor;
typedef struct {
    Settings *settings;
    yaml_parser_t parser;
    Anchor *anchors;
    size_t anchors_size;
    char *error;
    size_t error_cap;
    int failed;
} Loader;

static void value_free(Setting *v) {
    if (!v) return;
    if (v->type == SETTING_LIST) {
        for (size_t i = 0; i < v->as.list.size; ++i) value_free(v->as.list.values[i]);
        free(v->as.list.values);
    } else if (v->type == SETTING_MAP) {
        for (size_t i = 0; i < v->as.map.size; ++i) {
            free(v->as.map.entries[i].key);
            value_free(v->as.map.entries[i].value);
        }
        free(v->as.map.entries);
    }
    free(v->text);
    free(v);
}

void settings_free(Settings *s) {
    if (!s) return;
    value_free(s->root);
    free(s);
}

static Setting *value_new(Settings *s, SettingType type) {
    if (!s || s->nodes >= NODE_LIMIT) return NULL;
    Setting *v = calloc(1, sizeof(*v));
    if (v) { ++s->nodes; v->type = type; }
    return v;
}

static char *value_string(Settings *s, const char *text, size_t length) {
    if (!s || length >= VALUE_LIMIT || s->bytes > VALUE_LIMIT - length - 1) return NULL;
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length);
    copy[length] = 0;
    s->bytes += length + 1;
    return copy;
}

static int list_add(Setting *list, Setting *value) {
    if (!list || list->type != SETTING_LIST) return -1;
    if (list->as.list.size == list->as.list.capacity) {
        size_t capacity = list->as.list.capacity ? list->as.list.capacity * 2 : 8;
        Setting **values = realloc(list->as.list.values, capacity * sizeof(*values));
        if (!values) return -1;
        list->as.list.values = values;
        list->as.list.capacity = capacity;
    }
    list->as.list.values[list->as.list.size++] = value;
    return 0;
}

static int map_index(const Setting *map, const char *key) {
    if (!map || map->type != SETTING_MAP || !key) return -1;
    for (size_t i = 0; i < map->as.map.size; ++i)
        if (!strcmp(map->as.map.entries[i].key, key)) return (int)i;
    return -1;
}

static int map_put(Settings *s, Setting *map, const char *key, Setting *value) {
    if (!map || map->type != SETTING_MAP) return -1;
    int index = map_index(map, key);
    if (index >= 0) {
        value_free(map->as.map.entries[index].value);
        map->as.map.entries[index].value = value;
        return 0;
    }
    if (map->as.map.size == map->as.map.capacity) {
        size_t capacity = map->as.map.capacity ? map->as.map.capacity * 2 : 8;
        Entry *entries = realloc(map->as.map.entries, capacity * sizeof(*entries));
        if (!entries) return -1;
        map->as.map.entries = entries;
        map->as.map.capacity = capacity;
    }
    char *copy = value_string(s, key, strlen(key));
    if (!copy) return -1;
    map->as.map.entries[map->as.map.size++] = (Entry){copy, value};
    return 0;
}

static Setting *value_copy(Settings *s, const Setting *src, unsigned depth) {
    if (!src || depth > DEPTH_LIMIT) return NULL;
    Setting *v = value_new(s, src->type);
    if (!v) return NULL;
    if (src->text && !(v->text = value_string(s, src->text, strlen(src->text)))) goto fail;
    switch (src->type) {
    case SETTING_BOOL: v->as.boolean = src->as.boolean; break;
    case SETTING_INT: v->as.integer = src->as.integer; break;
    case SETTING_DOUBLE: v->as.real = src->as.real; break;
    case SETTING_LIST:
        for (size_t i = 0; i < src->as.list.size; ++i) {
            Setting *child = value_copy(s, src->as.list.values[i], depth + 1);
            if (!child || list_add(v, child)) { value_free(child); goto fail; }
        }
        break;
    case SETTING_MAP:
        for (size_t i = 0; i < src->as.map.size; ++i) {
            const Entry *entry = &src->as.map.entries[i];
            Setting *child = value_copy(s, entry->value, depth + 1);
            if (!child || map_put(s, v, entry->key, child)) { value_free(child); goto fail; }
        }
        break;
    default: break;
    }
    return v;
fail:
    value_free(v);
    return NULL;
}

static void load_error(Loader *l, const char *fmt, ...) {
    if (l->failed) return;
    l->failed = 1;
    if (!l->error || !l->error_cap) return;
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(l->error, l->error_cap, fmt, args);
    va_end(args);
}

static int next_event(Loader *l, yaml_event_t *event) {
    memset(event, 0, sizeof(*event));
    if (yaml_parser_parse(&l->parser, event)) return 0;
    load_error(l, "YAML syntax error at line %zu, column %zu: %s",
               l->parser.problem_mark.line + 1, l->parser.problem_mark.column + 1,
               l->parser.problem ? l->parser.problem : "invalid input");
    return -1;
}

static int anchor_add(Loader *l, const yaml_char_t *name, Setting *v) {
    (void)v;
    if (!name) return -1;
    if (l->anchors_size >= ANCHOR_LIMIT) { load_error(l, "Too many YAML anchors"); return -1; }
    Anchor *anchors = realloc(l->anchors, (l->anchors_size + 1) * sizeof(*anchors));
    if (!anchors) { load_error(l, "Out of memory loading YAML anchors"); return -1; }
    l->anchors = anchors;
    char *copy = value_string(l->settings, (const char *)name, strlen((const char *)name));
    if (!copy) { load_error(l, "YAML memory limit exceeded"); return -1; }
    size_t index = l->anchors_size++;
    l->anchors[index] = (Anchor){copy, NULL, 0};
    return (int)index;
}

static int bool_scalar(const char *text, int *out) {
    if (!strcasecmp(text, "true") || !strcasecmp(text, "yes") || !strcasecmp(text, "on")) {
        *out = 1; return 1;
    }
    if (!strcasecmp(text, "false") || !strcasecmp(text, "no") || !strcasecmp(text, "off")) {
        *out = 0; return 1;
    }
    return 0;
}

/* Parse YAML 1.1 integers as unsigned magnitudes to handle INT64_MIN. */
static int int_scalar(const char *text, int64_t *out) {
    if (!text || !*text || isspace((unsigned char)*text)) return 0;
    int negative = *text == '-';
    if (*text == '-' || *text == '+') ++text;
    if (!*text) return 0;
    unsigned base = 10;
    if (text[0] == '0' && text[1]) {
        if (text[1] == 'x' || text[1] == 'X') { base = 16; text += 2; }
        else if (text[1] == 'b' || text[1] == 'B') { base = 2; text += 2; }
        else if (text[1] == 'o' || text[1] == 'O') { base = 8; text += 2; }
        else base = 8;
    }
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1U : (uint64_t)INT64_MAX;
    uint64_t value = 0, group = 0;
    int digits = 0, sexagesimal = 0;
    for (; *text; ++text) {
        if (*text == '_') continue;
        if (*text == ':' && base == 10 && digits) {
            if (sexagesimal && group >= 60) return 0;
            if (value > (limit - group) / 60) return 0;
            value = value * 60 + group;
            group = 0; digits = 0; sexagesimal = 1;
            continue;
        }
        unsigned digit;
        if (*text >= '0' && *text <= '9') digit = (unsigned)(*text - '0');
        else if (*text >= 'a' && *text <= 'f') digit = (unsigned)(*text - 'a') + 10U;
        else if (*text >= 'A' && *text <= 'F') digit = (unsigned)(*text - 'A') + 10U;
        else return 0;
        if (digit >= base || group > (limit - digit) / base) return 0;
        group = group * base + digit;
        digits = 1;
    }
    if (!digits || (sexagesimal && (group >= 60 || value > (limit - group) / 60))) return 0;
    value = sexagesimal ? value * 60 + group : group;
    *out = negative ? (value == (uint64_t)INT64_MAX + 1U ? INT64_MIN : -(int64_t)value) : (int64_t)value;
    return 1;
}

static int real_scalar(const char *text, double *out) {
    if (!text || !*text || isspace((unsigned char)*text)) return 0;
    if (!strcasecmp(text, ".inf") || !strcasecmp(text, "+.inf")) { *out = INFINITY; return 1; }
    if (!strcasecmp(text, "-.inf")) { *out = -INFINITY; return 1; }
    if (!strcasecmp(text, ".nan")) { *out = NAN; return 1; }
    /* Require a decimal point or exponent before trying floating-point parsing. */
    if (!strchr(text, '.') && !strchr(text, 'e') && !strchr(text, 'E')) return 0;
    size_t length = strlen(text);
    if (length > 1024) return 0;
    char compact[1025]; size_t count = 0;
    for (size_t i = 0; i < length; ++i) if (text[i] != '_') compact[count++] = text[i];
    compact[count] = 0;
    char *end = NULL;
    errno = 0;
    double value = strtod(compact, &end);
    if (end == compact || *end || errno == ERANGE) return 0;
    *out = value;
    return 1;
}

static Setting *parse_scalar(Loader *l, const yaml_event_t *event) {
    const char *text = (const char *)event->data.scalar.value;
    size_t length = event->data.scalar.length;
    if (memchr(text, 0, length)) { load_error(l, "YAML scalars cannot contain embedded NUL bytes"); return NULL; }
    const char *tag = (const char *)event->data.scalar.tag;
    int infer = !tag && event->data.scalar.style == YAML_PLAIN_SCALAR_STYLE;
    SettingType type = SETTING_STRING;
    int boolean = 0; int64_t integer = 0; double real = 0;
    if (tag && strcmp(tag, "tag:yaml.org,2002:str")) {
        if (!strcmp(tag, "tag:yaml.org,2002:null")) type = SETTING_NULL;
        else if (!strcmp(tag, "tag:yaml.org,2002:bool") && bool_scalar(text, &boolean)) type = SETTING_BOOL;
        else if (!strcmp(tag, "tag:yaml.org,2002:int") && int_scalar(text, &integer)) type = SETTING_INT;
        else if (!strcmp(tag, "tag:yaml.org,2002:float") && (real_scalar(text, &real) ||
                 (int_scalar(text, &integer) && (real = (double)integer, 1)))) type = SETTING_DOUBLE;
        else if (!strcmp(tag, "tag:yaml.org,2002:timestamp")) type = SETTING_STRING;
        else { load_error(l, "Unsupported YAML scalar tag or invalid tagged value"); return NULL; }
    } else if (infer) {
        if (!length || !strcmp(text, "~") || !strcasecmp(text, "null")) type = SETTING_NULL;
        else if (bool_scalar(text, &boolean)) type = SETTING_BOOL;
        else if (int_scalar(text, &integer)) type = SETTING_INT;
        else if (real_scalar(text, &real)) type = SETTING_DOUBLE;
    }
    Setting *v = value_new(l->settings, type);
    if (!v) return NULL;
    v->as.integer = integer;
    if (type == SETTING_BOOL) v->as.boolean = boolean;
    if (type == SETTING_DOUBLE) v->as.real = real;
    if (type != SETTING_NULL) {
        char number[64];
        const char *stored = type == SETTING_BOOL ? (boolean ? "true" : "false") : text;
        if (type == SETTING_INT) { (void)snprintf(number, sizeof(number), "%" PRId64, integer); stored = number; }
        if (type == SETTING_DOUBLE) { (void)snprintf(number, sizeof(number), "%.17g", real); stored = number; }
        if (!(v->text = value_string(l->settings, stored, strlen(stored)))) { value_free(v); return NULL; }
    }
    return v;
}

static int merge_map(Loader *l, Setting *map, const Setting *merge) {
    if (merge->type == SETTING_LIST) {
        for (size_t i = 0; i < merge->as.list.size; ++i) {
            const Setting *item = merge->as.list.values[i];
            if (item->type != SETTING_MAP || merge_map(l, map, item)) return -1;
        }
        return 0;
    }
    if (merge->type != SETTING_MAP) return -1;
    for (size_t i = 0; i < merge->as.map.size; ++i) {
        const Entry *entry = &merge->as.map.entries[i];
        if (map_index(map, entry->key) >= 0) continue;
        Setting *copy = value_copy(l->settings, entry->value, 0);
        if (!copy || map_put(l->settings, map, entry->key, copy)) { value_free(copy); return -1; }
    }
    return 0;
}

/* Takes ownership of event contents, including on failure. */
static Setting *parse_value(Loader *l, yaml_event_t *event, unsigned depth) {
    Setting *v = NULL;
    int anchor = -1;
    if (depth > DEPTH_LIMIT) { load_error(l, "YAML nesting exceeds 64 levels"); goto done; }
    if (event->type == YAML_ALIAS_EVENT) {
        for (size_t i = l->anchors_size; i > 0; --i) {
            Anchor *a = &l->anchors[i - 1];
            if (strcmp(a->name, (const char *)event->data.alias.anchor)) continue;
            if (!a->complete) load_error(l, "Recursive YAML aliases are not allowed");
            else v = value_copy(l->settings, a->value, depth);
            goto done;
        }
        load_error(l, "YAML alias refers to an undefined anchor");
        goto done;
    }
    if (event->type == YAML_SCALAR_EVENT) {
        v = parse_scalar(l, event);
        if (v && event->data.scalar.anchor) anchor = anchor_add(l, event->data.scalar.anchor, v);
        goto done;
    }
    if (event->type != YAML_SEQUENCE_START_EVENT && event->type != YAML_MAPPING_START_EVENT) {
        load_error(l, "Expected a YAML scalar, sequence or mapping");
        goto done;
    }
    int mapping = event->type == YAML_MAPPING_START_EVENT;
    const yaml_char_t *name = mapping ? event->data.mapping_start.anchor : event->data.sequence_start.anchor;
    const yaml_char_t *tag = mapping ? event->data.mapping_start.tag : event->data.sequence_start.tag;
    if (tag && strcmp((const char *)tag, mapping ? "tag:yaml.org,2002:map" : "tag:yaml.org,2002:seq")) {
        load_error(l, "Unsupported YAML collection tag"); goto done;
    }
    v = value_new(l->settings, mapping ? SETTING_MAP : SETTING_LIST);
    if (!v) goto done;
    if (name) anchor = anchor_add(l, name, v);
    yaml_event_delete(event);
    memset(event, 0, sizeof(*event));
    while (!l->failed) {
        yaml_event_t child_event;
        if (next_event(l, &child_event)) break;
        if (child_event.type == (mapping ? YAML_MAPPING_END_EVENT : YAML_SEQUENCE_END_EVENT)) {
            yaml_event_delete(&child_event);
            goto done;
        }
        Setting *child = parse_value(l, &child_event, depth + 1);
        if (!child) break;
        if (!mapping) {
            if (list_add(v, child)) { value_free(child); break; }
            continue;
        }
        if (child->type != SETTING_STRING) {
            load_error(l, "YAML configuration mapping keys must be strings (quote boolean or numeric keys)");
            value_free(child); break;
        }
        if (next_event(l, &child_event)) { value_free(child); break; }
        Setting *value = parse_value(l, &child_event, depth + 1);
        if (!value) { value_free(child); break; }
        int result;
        if (!strcmp(child->text, "<<")) {
            result = merge_map(l, v, value);
            value_free(value);
            if (result) load_error(l, "YAML merge requires a mapping or a list of mappings within the memory limit");
        } else {
            result = map_put(l->settings, v, child->text, value);
            if (result) value_free(value);
        }
        value_free(child);
        if (result) break;
    }
    if (!l->failed) load_error(l, "YAML memory or expansion limit exceeded");
done:
    yaml_event_delete(event);
    if (!v && !l->failed) load_error(l, "YAML memory or expansion limit exceeded");
    if (l->failed) { value_free(v); return NULL; }
    if (anchor >= 0) {
        /* Copy anchors before duplicate map keys can replace their values. */
        l->anchors[anchor].value = value_copy(l->settings, v, depth);
        if (!l->anchors[anchor].value) {
            load_error(l, "YAML memory or expansion limit exceeded");
            value_free(v); return NULL;
        }
        l->anchors[anchor].complete = 1;
    }
    return v;
}

static Settings *load_bytes(const unsigned char *bytes, size_t size, char *error, size_t error_cap) {
    if (error && error_cap) *error = 0;
    if (size > INPUT_LIMIT) {
        if (error && error_cap) (void)snprintf(error, error_cap, "YAML input exceeds 4 MiB");
        return NULL;
    }
    Settings *s = calloc(1, sizeof(*s));
    Loader l = {.settings = s, .error = error, .error_cap = error_cap};
    if (!s || !yaml_parser_initialize(&l.parser)) {
        load_error(&l, "Out of memory initializing YAML parser");
        free(s); return NULL;
    }
    yaml_parser_set_input_string(&l.parser, bytes, size);
    yaml_event_t event;
    if (next_event(&l, &event)) goto done;
    if (event.type != YAML_STREAM_START_EVENT) load_error(&l, "Invalid YAML stream start");
    yaml_event_delete(&event);
    if (l.failed || next_event(&l, &event)) goto done;
    if (event.type == YAML_STREAM_END_EVENT) {
        yaml_event_delete(&event);
        s->root = value_new(s, SETTING_MAP);
        goto done;
    }
    if (event.type != YAML_DOCUMENT_START_EVENT) load_error(&l, "Invalid YAML document start");
    yaml_event_delete(&event);
    if (l.failed || next_event(&l, &event)) goto done;
    s->root = parse_value(&l, &event, 0);
    if (!s->root) goto done;
    if (s->root->type == SETTING_NULL) {
        value_free(s->root); s->root = value_new(s, SETTING_MAP);
    }
    if (next_event(&l, &event)) goto done;
    if (event.type != YAML_DOCUMENT_END_EVENT) load_error(&l, "Expected end of YAML document");
    yaml_event_delete(&event);
    if (l.failed || next_event(&l, &event)) goto done;
    if (event.type != YAML_STREAM_END_EVENT) load_error(&l, "Configuration must contain exactly one YAML document");
    yaml_event_delete(&event);
done:
    yaml_parser_delete(&l.parser);
    for (size_t i = 0; i < l.anchors_size; ++i) {
        free(l.anchors[i].name);
        value_free(l.anchors[i].value);
    }
    free(l.anchors);
    if (!s->root && !l.failed) load_error(&l, "YAML memory limit exceeded");
    if (l.failed) { settings_free(s); return NULL; }
    return s;
}

Settings *settings_load_string(const char *yaml, char *error, size_t error_cap) {
    if (!yaml) yaml = "";
    size_t length = strnlen(yaml, (size_t)INPUT_LIMIT + 1);
    return load_bytes((const unsigned char *)yaml, length, error, error_cap);
}

Settings *settings_load_file(const char *path, char *error, size_t error_cap) {
    FILE *fp = path ? fopen(path, "rb") : NULL;
    if (!fp) {
        if (error && error_cap) (void)snprintf(error, error_cap, "Cannot open configuration file: %s", strerror(errno));
        return NULL;
    }
    unsigned char *bytes = malloc((size_t)INPUT_LIMIT + 1);
    if (!bytes) {
        if (error && error_cap) (void)snprintf(error, error_cap, "Out of memory reading YAML");
        fclose(fp); return NULL;
    }
    size_t length = fread(bytes, 1, (size_t)INPUT_LIMIT + 1, fp);
    int failed = ferror(fp);
    fclose(fp);
    Settings *s = NULL;
    if (failed) {
        if (error && error_cap) (void)snprintf(error, error_cap, "Error reading configuration file");
    } else s = load_bytes(bytes, length, error, error_cap);
    free(bytes);
    return s;
}

const Setting *setting_get(const Setting *map, const char *key) {
    int index = map_index(map, key);
    return index >= 0 ? map->as.map.entries[index].value : NULL;
}
SettingType setting_type(const Setting *v) { return v ? v->type : SETTING_NULL; }
size_t setting_size(const Setting *v) {
    if (!v) return 0;
    return v->type == SETTING_MAP ? v->as.map.size : v->type == SETTING_LIST ? v->as.list.size : 0;
}
const Setting *setting_at(const Setting *v, size_t index) {
    if (index >= setting_size(v)) return NULL;
    return v->type == SETTING_MAP ? v->as.map.entries[index].value : v->as.list.values[index];
}
const char *setting_key(const Setting *v, size_t index) {
    return v && v->type == SETTING_MAP && index < v->as.map.size ? v->as.map.entries[index].key : NULL;
}

const Setting *settings_get(const Settings *s, const char *path) {
    const Setting *v = s ? s->root : NULL;
    if (!path) return v;
    while (v && *path) {
        char key[512]; size_t length = 0;
        if (*path == '[') {
            ++path;
            while (*path && *path != ']' && length + 1 < sizeof(key)) key[length++] = *path++;
            if (*path != ']') return NULL;
            ++path;
        } else {
            while (*path && *path != '.' && *path != '[' && length + 1 < sizeof(key)) key[length++] = *path++;
            if (*path && *path != '.' && *path != '[') return NULL;
        }
        key[length] = 0;
        if (!length) return NULL;
        if (v->type == SETTING_MAP) v = setting_get(v, key);
        else if (v->type == SETTING_LIST) {
            size_t index = 0;
            for (size_t i = 0; i < length; ++i) {
                if (key[i] < '0' || key[i] > '9' || index > NODE_LIMIT) return NULL;
                index = index * 10 + (size_t)(key[i] - '0');
            }
            v = setting_at(v, index);
        } else return NULL;
        if (*path == '.') { ++path; if (!*path) return NULL; }
    }
    return v;
}

const char *setting_str(const Setting *v, const char *fallback) {
    return v && v->type == SETTING_STRING ? v->text : fallback;
}
const char *setting_text(const Setting *v) { return v ? v->text : NULL; }
int setting_bool(const Setting *v, int fallback) {
    return v && v->type == SETTING_BOOL ? v->as.boolean : fallback;
}
int64_t setting_int(const Setting *v, int64_t fallback) {
    if (!v) return fallback;
    if (v->type == SETTING_INT) return v->as.integer;
    if (v->type == SETTING_DOUBLE && isfinite(v->as.real) &&
        v->as.real >= -9223372036854775808.0 && v->as.real < 9223372036854775808.0) {
        int64_t n = (int64_t)v->as.real;
        if ((double)n == v->as.real) return n;
    }
    return fallback;
}
double setting_double(const Setting *v, double fallback) {
    if (!v) return fallback;
    if (v->type == SETTING_DOUBLE) return v->as.real;
    return v->type == SETTING_INT ? (double)v->as.integer : fallback;
}
const char *settings_str(const Settings *s, const char *p, const char *fallback) { return setting_str(settings_get(s, p), fallback); }
int settings_bool(const Settings *s, const char *p, int fallback) { return setting_bool(settings_get(s, p), fallback); }
int64_t settings_int(const Settings *s, const char *p, int64_t fallback) { return setting_int(settings_get(s, p), fallback); }
double settings_double(const Settings *s, const char *p, double fallback) { return setting_double(settings_get(s, p), fallback); }

static int set_owned(Settings *s, const char *path, Setting *value) {
    if (!s || !path || !*path || strlen(path) >= 1024) return -1;
    char copy[1024]; strcpy(copy, path);
    Setting *map = s->root;
    char *save = NULL, *key = strtok_r(copy, ".", &save);
    if (!key) return -1;
    for (;;) {
        char *next = strtok_r(NULL, ".", &save);
        if (!map || map->type != SETTING_MAP) return -1;
        if (!next) return map_put(s, map, key, value);
        Setting *child = (Setting *)setting_get(map, key);
        if (!child) {
            child = value_new(s, SETTING_MAP);
            if (!child || map_put(s, map, key, child)) { value_free(child); return -1; }
        }
        map = child; key = next;
    }
}

int settings_set(Settings *s, const char *path, const Setting *value) {
    Setting *copy = value_copy(s, value, 0);
    if (!copy) return -1;
    if (set_owned(s, path, copy)) { value_free(copy); return -1; }
    return 0;
}

static int set_scalar(Settings *s, const char *path, SettingType type,
                      const char *text, int64_t integer, double real) {
    Setting *v = value_new(s, type);
    if (!v) return -1;
    if (text && !(v->text = value_string(s, text, strlen(text)))) { value_free(v); return -1; }
    if (type == SETTING_INT) v->as.integer = integer;
    if (type == SETTING_BOOL) v->as.boolean = integer != 0;
    if (type == SETTING_DOUBLE) v->as.real = real;
    if (set_owned(s, path, v)) { value_free(v); return -1; }
    return 0;
}
int settings_set_str(Settings *s, const char *path, const char *value) {
    return value ? set_scalar(s, path, SETTING_STRING, value, 0, 0) : settings_set_null(s, path);
}
int settings_set_bool(Settings *s, const char *path, int value) {
    return set_scalar(s, path, SETTING_BOOL, value ? "true" : "false", value != 0, 0);
}
int settings_set_int(Settings *s, const char *path, int64_t value) {
    char text[32]; (void)snprintf(text, sizeof(text), "%" PRId64, value);
    return set_scalar(s, path, SETTING_INT, text, value, 0);
}
int settings_set_double(Settings *s, const char *path, double value) {
    char text[64]; (void)snprintf(text, sizeof(text), "%.17g", value);
    return set_scalar(s, path, SETTING_DOUBLE, text, 0, value);
}
int settings_set_null(Settings *s, const char *path) { return set_scalar(s, path, SETTING_NULL, NULL, 0, 0); }

static int default_map(Settings *s, Setting *dst, const Setting *src) {
    if (!dst || !src || dst->type != SETTING_MAP || src->type != SETTING_MAP) return -1;
    for (size_t i = 0; i < src->as.map.size; ++i) {
        const Entry *entry = &src->as.map.entries[i];
        Setting *existing = (Setting *)setting_get(dst, entry->key);
        if (!existing) {
            Setting *copy = value_copy(s, entry->value, 0);
            if (!copy || map_put(s, dst, entry->key, copy)) { value_free(copy); return -1; }
        } else if (entry->value->type == SETTING_MAP && default_map(s, existing, entry->value)) return -1;
    }
    return 0;
}
int settings_apply_defaults(Settings *s, const Settings *defaults) {
    return s && defaults ? default_map(s, s->root, defaults->root) : -1;
}

int settings_listify(Settings *s, const char *path, int null_as_empty) {
    const Setting *old = settings_get(s, path);
    if (old && old->type == SETTING_LIST) return 0;
    Setting *list = value_new(s, SETTING_LIST);
    if (!list) return -1;
    if (old && !(null_as_empty && old->type == SETTING_NULL)) {
        Setting *copy = value_copy(s, old, 0);
        if (!copy || list_add(list, copy)) { value_free(copy); value_free(list); return -1; }
    }
    if (set_owned(s, path, list)) { value_free(list); return -1; }
    return 0;
}

int settings_list_append(Settings *s, const Setting *list, const Setting *value) {
    if (!list || list->type != SETTING_LIST) return -1;
    Setting *copy = value_copy(s, value, 0);
    if (!copy) return -1;
    if (list_add((Setting *)list, copy)) { value_free(copy); return -1; }
    return 0;
}

int settings_map_set(Settings *s, const Setting *map, const char *key, const Setting *value) {
    if (!map || map->type != SETTING_MAP || !key) return -1;
    Setting *copy = value_copy(s, value, 0);
    if (!copy) return -1;
    if (map_put(s, (Setting *)map, key, copy)) { value_free(copy); return -1; }
    return 0;
}

int settings_map_set_list(Settings *s, const Setting *map, const char *key,
                          const Setting *const *items, size_t count) {
    Setting *list = value_new(s, SETTING_LIST);
    if (!list) return -1;
    for (size_t i = 0; i < count; ++i) {
        Setting *item = items[i] ? value_copy(s, items[i], 0) : value_new(s, SETTING_NULL);
        if (!item || list_add(list, item)) { value_free(item); value_free(list); return -1; }
    }
    if (!key || map_put(s, (Setting *)map, key, list)) { value_free(list); return -1; }
    return 0;
}
