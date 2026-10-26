#define _POSIX_C_SOURCE 200809L
#include "policy.h"
#include "util.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void lower(char *s) {
    for (; *s; ++s) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 'a' - 'A');
}
static int json_duration_us(const Json *j, int token, int64_t *duration) {
    if (!j || token < 0 || token >= j->count || j->tokens[token].type != JSON_NUMBER) return 0;
    const JsonTok *value = &j->tokens[token];
    int length = value->end - value->start;
    char text[128], *tail;
    if (length <= 0 || (size_t)length >= sizeof(text)) return 0;
    memcpy(text, j->text + value->start, (size_t)length); text[length] = 0;
    errno = 0;
    long double seconds = strtold(text, &tail);
    if (*tail || isnan(seconds) || seconds < 0 || (errno && errno != ERANGE)) return 0;
    if (errno == ERANGE && seconds == 0) {
        if (signbit(seconds)) return 0;
        *duration = 1; /* Keep positive durations nonzero. */
        return 1;
    }
    long double microseconds = seconds * 1000000.0L;
    if (!isfinite(microseconds) || microseconds >= (long double)INT64_MAX) *duration = INT64_MAX;
    else {
        *duration = (int64_t)microseconds;
        /* Round up to avoid early expiry. */
        if ((long double)*duration < microseconds) ++*duration;
    }
    return 1;
}
PolicyRateLimit policy_challenge_rate_limit(const Json *j, long status,
                                             int64_t retry_after_us, unsigned *backoff_step) {
    PolicyRateLimit limit = {POLICY_RATE_LIMIT_NONE, 0, 0};
    if (status != 429 && backoff_step) *backoff_step = 0;
    if (j && j->count && j->tokens[0].type == JSON_OBJECT && (status == 400 || status == 429)) {
        int daily = json_get(j, 0, "ratelimit");
        if (json_get(j, 0, "error") >= 0 && json_eq(j, json_get(j, daily, "key"), "bot.vsBot.day") &&
            json_duration_us(j, json_get(j, daily, "seconds"), &limit.delay_us)) {
            limit.kind = status == 429 ? POLICY_RATE_LIMIT_BOT : POLICY_RATE_LIMIT_OPPONENT;
            limit.daily = 1;
            return limit;
        }
    }
    if (status == 429) {
        limit.kind = POLICY_RATE_LIMIT_BOT;
        if (retry_after_us >= 0) limit.delay_us = retry_after_us;
        else {
            static const int64_t seconds[] = {60, 120, 240, 480, 600};
            unsigned step = backoff_step ? *backoff_step : 0;
            if (step > 4) step = 4;
            limit.delay_us = seconds[step] * INT64_C(1000000);
            if (backoff_step) *backoff_step = step < 4 ? step + 1 : step;
        }
    }
    return limit;
}
static int compare_names(const void *a, const void *b) { return strcasecmp(a, b); }
static int online_has(const Policy *p, const char *name, int owner) {
    for (size_t i = 0; i < p->list_count; ++i) {
        const OnlineBlockList *l = &p->lists[i];
        if ((l->owners & owner) && l->count && bsearch(name, l->names, l->count, sizeof(*l->names), compare_names)) return 1;
    }
    return 0;
}
static int add_lists(Policy *p, const Setting *urls, int owner) {
    for (size_t i = 0; i < setting_size(urls); ++i) {
        const char *url = setting_str(setting_at(urls, i), "");
        if (!*url) continue;
        size_t at = 0;
        for (; at < p->list_count; ++at) if (!strcmp(p->lists[at].url, url)) break;
        if (at == p->list_count) {
            if (at == POLICY_LIST_CAP || (strncmp(url, "https://", 8) && strncmp(url, "http://", 7))) return -1;
            p->lists[at].url = url; ++p->list_count;
        }
        p->lists[at].owners |= owner;
    }
    return 0;
}
int policy_init(Policy *p, const Config *cfg, uint64_t seed, char *error, size_t cap) {
    memset(p, 0, sizeof(*p)); p->random = seed;
    if (add_lists(p, settings_get(cfg->settings, "challenge.online_block_list"), 1) ||
        add_lists(p, settings_get(cfg->settings, "matchmaking.online_block_list"), 2)) {
        (void)lbc_copy(error, cap, "At most 32 distinct HTTP(S) online block lists are supported."); return -1;
    }
    const char *path = settings_str(cfg->settings, "matchmaking.challenge_decliner_file_name", "");
    if (!*path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return 0;
        (void)lbc_copy(error, cap, "Could not open the challenge decliner file."); return -1;
    }
    char line[256]; int bad = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!*line) continue;
        char *comma = strchr(line, ',');
        if (!comma) { bad = 1; break; }
        *comma++ = 0;
        if (policy_filter(p, cfg, line, comma, INT64_MAX, 0)) { bad = 1; break; }
    }
    if (ferror(f)) bad = 1;
    (void)fclose(f);
    if (bad) { (void)lbc_copy(error, cap, "Malformed or oversized challenge decliner file."); return -1; }
    return 0;
}
void policy_free(Policy *p) {
    for (size_t i = 0; i < p->list_count; ++i) free(p->lists[i].names);
    memset(p, 0, sizeof(*p));
}
int policy_profile(Policy *p, const Json *j, int object) {
    BotProfile next = {0};
    if (lbc_json_string(j, object, "id", next.id, sizeof(next.id), NULL) || !lbc_id(next.id)) return -1;
    (void)lbc_json_string(j, object, "username", next.name, sizeof(next.name), next.id);
    int perfs = json_get(j, object, "perfs");
    if (perfs >= 0 && j->tokens[perfs].type == JSON_OBJECT) {
        for (int t = perfs + 1; t < j->tokens[perfs].next && next.count < 24; ) {
            int value = t + 1;
            int64_t rating = lbc_json_int(j, value, "rating", 0);
            if (rating > 0 && rating < 100000 &&
                json_string(j, t, next.perfs[next.count].name, sizeof(next.perfs[0].name)) >= 0) {
                next.perfs[next.count].rating = (int)rating; ++next.count;
            }
            t = j->tokens[value].next;
        }
    }
    p->profile = next; return 0;
}
int policy_rating(const Policy *p, const char *perf) {
    for (size_t i = 0; i < p->profile.count; ++i)
        if (!strcasecmp(p->profile.perfs[i].name, perf)) return p->profile.perfs[i].rating;
    return 0;
}
int policy_challenge_parse(Challenge *c, const Json *j, int object, int64_t now) {
    memset(c, 0, sizeof(*c)); c->initial = c->increment = c->days = -1;
    char variant[32], title[32], type[32];
    int player = json_get(j, object, "challenger"), tc = json_get(j, object, "timeControl");
    if (lbc_json_string(j, object, "id", c->id, sizeof(c->id), NULL) || !lbc_id(c->id) ||
        lbc_json_string(j, player, "id", c->opponent, sizeof(c->opponent), NULL) || !lbc_id(c->opponent) ||
        lbc_json_string(j, json_get(j, object, "variant"), "key", variant, sizeof(variant), NULL) ||
        lbc_json_string(j, object, "speed", c->speed, sizeof(c->speed), NULL) ||
        lbc_json_string(j, tc, "type", type, sizeof(type), NULL)) return -1;
    (void)lbc_json_string(j, player, "name", c->name, sizeof(c->name), c->opponent);
    (void)lbc_json_string(j, player, "title", title, sizeof(title), "");
    (void)lbc_json_string(j, json_get(j, object, "perf"), "name", c->perf, sizeof(c->perf), c->speed);
    (void)lbc_json_string(j, object, "initialFen", c->fen, sizeof(c->fen), "startpos");
    c->variant = config_variant(variant); c->bot = !strcmp(title, "BOT"); c->titled = *title && !c->bot;
    c->rating = (int)lbc_json_int(j, player, "rating", 0); c->rated = lbc_json_bool(j, object, "rated", 0);
    if (!strcmp(type, "clock")) {
        c->initial = lbc_json_int(j, tc, "limit", -1); c->increment = lbc_json_int(j, tc, "increment", -1);
        if (c->initial < 0 || c->increment < 0) return -1;
    } else if (!strcmp(type, "correspondence")) {
        c->days = lbc_json_int(j, tc, "daysPerTurn", -1);
        if (c->days < 1) return -1;
    } else if (strcmp(type, "unlimited")) return -1;
    lower(c->opponent); c->used = 1; c->created_us = now; return 0;
}
int policy_is_blocked(const Policy *p, const Config *cfg, const char *name, int matchmaking, int64_t now) {
    const Settings *s = cfg->settings;
    if (!matchmaking) return lbc_list_contains(settings_get(s, "challenge.block_list"), name) || online_has(p, name, 1);
    if (lbc_list_contains(settings_get(s, "matchmaking.block_list"), name) || online_has(p, name, 2)) return 1;
    if (settings_bool(s, "matchmaking.include_challenge_block_list", 0) &&
        (lbc_list_contains(settings_get(s, "challenge.block_list"), name) || online_has(p, name, 1))) return 1;
    for (size_t i = 0; i < POLICY_FILTER_CAP; ++i)
        if (p->filters[i].until_us > now && !*p->filters[i].aspect && !strcasecmp(p->filters[i].name, name)) return 1;
    return 0;
}
const char *policy_challenge_reason(Policy *p, const Config *cfg, const Challenge *c, int engagements, int64_t now) {
    const Settings *s = cfg->settings;
    if (!cfg->accept) return "later";
    if (c->bot && !settings_bool(s, "challenge.accept_bot", 1)) return "noBot";
    if (!c->bot && cfg->only_bots) return "onlyBot";
    const Setting *speeds = settings_get(s, "challenge.time_controls");
    if (!lbc_list_contains(speeds, c->speed)) return "timeControl";
    if (!strcmp(c->speed, "ultraBullet")) return "tooFast";
    if (c->initial >= 0) {
        int minimum_inc = cfg->min_increment;
        if (c->bot && !strcmp(c->speed, "bullet") && settings_bool(s, "challenge.bullet_requires_increment", 0) && !minimum_inc) minimum_inc = 1;
        if (c->initial < cfg->min_initial || c->increment < minimum_inc) return "tooFast";
        if (c->initial > cfg->max_initial || c->increment > cfg->max_increment) return "tooSlow";
    } else {
        double min_days = settings_double(s, "challenge.min_days", 1), max_days = settings_double(s, "challenge.max_days", INFINITY);
        if (c->days < 0 ? !isinf(max_days) : ((double)c->days < min_days || (double)c->days > max_days)) return "timeControl";
    }
    if (!(cfg->variants & c->variant)) return "variant";
    const Setting *modes = settings_get(s, "challenge.modes");
    if ((c->rated && cfg->casual_only) || !lbc_list_contains(modes, c->rated ? "rated" : "casual"))
        return c->rated ? "casual" : "rated";
    int64_t minimum = settings_int(s, "challenge.min_rating", 0), maximum = settings_int(s, "challenge.max_rating", 4000);
    int64_t difference = settings_int(s, "challenge.rating_difference", -1);
    int rating = policy_rating(p, c->perf);
    if (difference >= 0 && rating) {
        if (minimum < rating - difference) minimum = rating - difference;
        if (maximum > rating + difference) maximum = rating + difference;
    }
    if (c->rating && (c->rating < minimum || c->rating > maximum)) return "generic";
    if (policy_is_blocked(p, cfg, c->opponent, 0, now)) return "generic";
    const Setting *allow = settings_get(s, "challenge.allow_list");
    int nonempty = 0;
    for (size_t i = 0; i < setting_size(allow); ++i) if (*setting_str(setting_at(allow, i), "")) nonempty = 1;
    if (nonempty && !lbc_list_contains(allow, c->opponent)) return "generic";
    if (c->bot && !c->admitted) {
        int recent = 0;
        for (size_t i = 0; i < POLICY_RECENT_CAP; ++i)
            if (p->recent[i].until_us > now && !strcasecmp(p->recent[i].name, c->opponent)) ++recent;
        int64_t max_recent = settings_int(s, "challenge.max_recent_bot_challenges", -1);
        if (max_recent >= 0 && recent >= max_recent) return "later";
    }
    if (engagements >= settings_int(s, "challenge.max_simultaneous_games_per_user", 5)) return "later";
    return NULL;
}
int policy_queue_add(Policy *p, const Challenge *c) {
    for (size_t i = 0; i < POLICY_QUEUE_CAP; ++i) if (p->queue[i].used && !strcmp(p->queue[i].id, c->id)) return 0;
    for (size_t i = 0; i < POLICY_QUEUE_CAP; ++i) if (!p->queue[i].used) { p->queue[i] = *c; return 0; }
    return -1;
}
void policy_queue_remove(Policy *p, const char *id) {
    for (size_t i = 0; i < POLICY_QUEUE_CAP; ++i) if (p->queue[i].used && !strcmp(p->queue[i].id, id)) p->queue[i].used = 0;
}
Challenge *policy_queue_best(Policy *p, const Config *cfg, int bot_slots_available, int64_t now) {
    Challenge *best = NULL;
    const char *preference = settings_str(cfg->settings, "challenge.preference", "none");
    int prefer = !strcmp(preference, "bot") ? 1 : !strcmp(preference, "human") ? -1 : 0;
    int first = !strcmp(settings_str(cfg->settings, "challenge.sort_by", "best"), "first");
    for (size_t i = 0; i < POLICY_QUEUE_CAP; ++i) {
        Challenge *c = &p->queue[i];
        if (c->used && now - c->created_us > INT64_C(120000000)) c->used = 0;
        if (!c->used || (c->bot && bot_slots_available <= 0)) continue;
        if (!best) { best = c; continue; }
        int priority = prefer * (c->bot - best->bot);
        int score = c->rating + c->rated * 200 + c->titled * 200;
        int previous = best->rating + best->rated * 200 + best->titled * 200;
        if (priority > 0 || (!priority && ((!first && score > previous) ||
            ((first || score == previous) && c->created_us < best->created_us)))) best = c;
    }
    return best;
}
void policy_accepted(Policy *p, const Config *cfg, const Challenge *c, int64_t now) {
    if (!c->bot) return;
    const Setting *configured_age = settings_get(cfg->settings, "challenge.recent_bot_challenge_age");
    if (!configured_age || setting_type(configured_age) == SETTING_NULL) return;
    size_t oldest = 0;
    for (size_t i = 0; i < POLICY_RECENT_CAP; ++i) {
        if (p->recent[i].until_us <= now) { oldest = i; break; }
        if (p->recent[i].until_us < p->recent[oldest].until_us) oldest = i;
    }
    (void)lbc_copy(p->recent[oldest].name, sizeof(p->recent[oldest].name), c->opponent);
    int64_t age = lbc_seconds_us(setting_double(configured_age, 0));
    p->recent[oldest].until_us = age > INT64_MAX - now ? INT64_MAX : now + age;
}
int policy_online_due(const Policy *p, int64_t now) {
    for (size_t i = 0; i < p->list_count; ++i) if (p->lists[i].due_us <= now) return (int)i;
    return -1;
}
int policy_online_update(Policy *p, size_t index, const char *text, size_t length, int64_t now) {
    if (index >= p->list_count || length > 4u * 1024u * 1024u) return -1;
    size_t capacity = 1;
    for (size_t i = 0; i < length; ++i) if (text[i] == '\n') ++capacity;
    if (capacity > 65536) return -1;
    char (*names)[65] = calloc(capacity, sizeof(*names));
    if (!names) return -1;
    size_t count = 0, at = 0;
    while (at < length) {
        size_t end = at;
        while (end < length && text[end] != '\n') ++end;
        size_t a = at, b = end;
        while (a < b && isspace((unsigned char)text[a])) ++a;
        while (b > a && isspace((unsigned char)text[b-1])) --b;
        if (a < b && text[a] == '@') ++a;
        if (a < b && text[a] != '#') {
            if (b - a > 64) { free(names); return -1; }
            memcpy(names[count], text + a, b - a); names[count][b-a] = 0;
            if (!lbc_id(names[count])) { free(names); return -1; }
            lower(names[count]); ++count;
        }
        at = end + (end < length ? 1u : 0u);
    }
    qsort(names, count, sizeof(*names), compare_names);
    free(p->lists[index].names); p->lists[index].names = names; p->lists[index].count = count;
    p->lists[index].due_us = now + INT64_C(1800000000); p->lists[index].attempts = 0;
    p->lists[index].initialized = 1;
    return 0;
}
const char *policy_game_category(int variant, int64_t initial, int64_t increment, int days) {
    if (variant != VAR_STANDARD && variant != VAR_FROM_POSITION) return config_variant_name(variant);
    if (days) return "correspondence";
    int64_t estimate = initial + increment * 40;
    return estimate < 179 ? "bullet" : estimate < 479 ? "blitz" : estimate < 1499 ? "rapid" : "classical";
}
static const Setting *merged(const Setting *base, const Setting *override, const char *key) {
    const Setting *value = setting_get(override, key); return value ? value : setting_get(base, key);
}
static int random_int(Policy *p, const Setting *value) {
    if (setting_type(value) == SETTING_LIST) {
        size_t n = setting_size(value);
        value = n ? setting_at(value, (size_t)lbc_random_below(&p->random, n)) : NULL;
    }
    int64_t result = setting_int(value, 0);
    return result < 0 || result > INT_MAX ? 0 : (int)result;
}
int policy_match_begin(Policy *p, const Config *cfg) {
    const Setting *base = settings_get(cfg->settings, "matchmaking");
    const Setting *overrides = setting_get(base, "overrides");
    size_t selection = (size_t)lbc_random_below(&p->random, setting_size(overrides) + 1);
    const Setting *override = selection ? setting_at(overrides, selection - 1) : NULL;
    MatchSpec *m = &p->match; memset(m, 0, sizeof(*m));
    (void)lbc_copy(m->override_name, sizeof(m->override_name), selection ? setting_key(overrides, selection - 1) : "default");
    const char *variant = setting_str(merged(base, override, "challenge_variant"), "random");
    int variant_bit = config_variant(variant);
    if (!strcmp(variant, "random")) {
        int choices[10], n = 0;
        for (int bit = 1; bit <= VAR_THREE_CHECK; bit *= 2)
            if ((cfg->variants & bit) && bit != VAR_FROM_POSITION) choices[n++] = bit;
        if (!n) return -1;
        variant_bit = choices[lbc_random_below(&p->random, (uint64_t)n)];
        variant = config_variant_name(variant_bit);
    }
    if (!variant_bit || variant_bit == VAR_FROM_POSITION) return -1;
    (void)lbc_copy(m->variant, sizeof(m->variant), variant);
    const char *mode = setting_str(merged(base, override, "challenge_mode"), "random");
    if (!strcmp(mode, "random")) mode = lbc_random_below(&p->random, 2) ? "rated" : "casual";
    if (strcmp(mode, "rated") && strcmp(mode, "casual")) return -1;
    (void)lbc_copy(m->mode, sizeof(m->mode), mode);
    (void)lbc_copy(m->preference, sizeof(m->preference), setting_str(merged(base, override, "rating_preference"), "none"));
    m->initial = random_int(p, merged(base, override, "challenge_initial_time"));
    m->increment = random_int(p, merged(base, override, "challenge_increment"));
    m->days = random_int(p, merged(base, override, "challenge_days"));
    if (m->days && ((!m->initial && !m->increment) || lbc_random_below(&p->random, 2))) m->initial = m->increment = 0;
    else m->days = 0;
    if (!m->days && !m->initial && !m->increment) return -1;
    (void)lbc_copy(m->speed, sizeof(m->speed), policy_game_category(variant_bit, m->initial, m->increment, m->days));
    m->min_rating = (int)setting_int(merged(base, override, "opponent_min_rating"), 600);
    m->max_rating = (int)setting_int(merged(base, override, "opponent_max_rating"), 4000);
    int64_t diff = setting_int(merged(base, override, "opponent_rating_difference"), -1);
    int rating = policy_rating(p, m->speed);
    if (diff >= 0 && diff < 100000 && rating) { m->min_rating = rating - (int)diff; m->max_rating = rating + (int)diff; }
    return m->max_rating < m->min_rating ? -1 : 0;
}
static int aspect_blocked(const Policy *p, const char *name, const char *aspect, int64_t now) {
    for (size_t i = 0; i < POLICY_FILTER_CAP; ++i)
        if (p->filters[i].until_us > now && !strcasecmp(p->filters[i].name, name) && !strcmp(p->filters[i].aspect, aspect)) return 1;
    return 0;
}
void policy_match_consider(Policy *p, const Config *cfg, const Json *j, int object, int engaged, int64_t now) {
    MatchSpec *m = &p->match; char name[65];
    if (engaged || lbc_json_string(j, object, "id", name, sizeof(name), NULL)) {
        if (engaged || lbc_json_string(j, object, "username", name, sizeof(name), NULL)) return;
    }
    if (!lbc_id(name) || !strcasecmp(name, p->profile.id) || policy_is_blocked(p, cfg, name, 1, now)) return;
    int perf = json_get(j, json_get(j, object, "perfs"), m->speed);
    int64_t rating = lbc_json_int(j, perf, "rating", 0);
    if (lbc_json_int(j, perf, "games", 0) < 1 || rating < m->min_rating || rating > m->max_rating) return;
    uint64_t weight = 1;
    int64_t span = (int64_t)m->max_rating - m->min_rating;
    if (!strcmp(m->preference, "high")) {
        int64_t offset = (int64_t)m->min_rating - (span > 1 ? span : 1);
        weight = (uint64_t)(rating - offset);
    } else if (!strcmp(m->preference, "low")) {
        int64_t offset = (int64_t)m->max_rating + (span > 1 ? span : 1);
        weight = (uint64_t)(offset - rating);
    }
    m->all_weight += weight;
    if (lbc_random_below(&p->random, m->all_weight) < weight) (void)lbc_copy(m->fallback, sizeof(m->fallback), name);
    int fine = !strcmp(settings_str(cfg->settings, "matchmaking.challenge_filter", "none"), "fine");
    if (fine && (aspect_blocked(p, name, m->variant, now) || aspect_blocked(p, name, m->speed, now) || aspect_blocked(p, name, m->mode, now))) return;
    m->ready_weight += weight;
    if (lbc_random_below(&p->random, m->ready_weight) < weight) (void)lbc_copy(m->opponent, sizeof(m->opponent), name);
}
const char *policy_match_finish(Policy *p) {
    if (!*p->match.opponent) (void)lbc_copy(p->match.opponent, sizeof(p->match.opponent), p->match.fallback);
    return p->match.opponent;
}
int policy_match_body(const Policy *p, char *body, size_t cap) {
    const MatchSpec *m = &p->match; char number[32]; *body = 0;
    if (lbc_form_add(body, cap, "rated", !strcmp(m->mode, "rated") ? "true" : "false") ||
        lbc_form_add(body, cap, "variant", m->variant)) return -1;
    if (m->days) { (void)snprintf(number, sizeof(number), "%d", m->days); return lbc_form_add(body, cap, "days", number); }
    (void)snprintf(number, sizeof(number), "%d", m->initial);
    if (lbc_form_add(body, cap, "clock.limit", number)) return -1;
    (void)snprintf(number, sizeof(number), "%d", m->increment); return lbc_form_add(body, cap, "clock.increment", number);
}
int policy_filter(Policy *p, const Config *cfg, const char *name, const char *aspect, int64_t until, int persistent) {
    if (!lbc_id(name) || strlen(aspect) >= sizeof(p->filters[0].aspect)) return -1;
    for (const unsigned char *q = (const unsigned char *)aspect; *q; ++q)
        if (!isalnum(*q)) return -1;
    size_t index = POLICY_FILTER_CAP, oldest = 0;
    for (size_t i = 0; i < POLICY_FILTER_CAP; ++i) {
        if (!strcasecmp(p->filters[i].name, name) && !strcmp(p->filters[i].aspect, aspect)) { index = i; break; }
        if (p->filters[i].until_us < p->filters[oldest].until_us) oldest = i;
    }
    if (index == POLICY_FILTER_CAP) index = oldest;
    if (p->filters[index].until_us == INT64_MAX && strcasecmp(p->filters[index].name, name)) return -1;
    (void)lbc_copy(p->filters[index].name, sizeof(p->filters[index].name), name);
    (void)lbc_copy(p->filters[index].aspect, sizeof(p->filters[index].aspect), aspect);
    lower(p->filters[index].name); p->filters[index].until_us = until;
    if (persistent) {
        const char *path = settings_str(cfg->settings, "matchmaking.challenge_decliner_file_name", "");
        if (*path) {
            FILE *f = fopen(path, "a"); if (!f) return -1;
            int bad = fprintf(f, "%s,%s\n", name, aspect) < 0;
            if (fclose(f)) bad = 1;
            return bad ? -1 : 0;
        }
    }
    return 0;
}
void policy_match_declined(Policy *p, const Config *cfg, const Json *j, int object, int64_t now) {
    const char *filter = settings_str(cfg->settings, "matchmaking.challenge_filter", "none");
    if (!strcmp(filter, "none")) return;
    char source[65], name[65], reason[32], aspect[32] = "";
    if (lbc_json_string(j, json_get(j, object, "challenger"), "id", source, sizeof(source), NULL) ||
        strcasecmp(source, p->profile.id) ||
        lbc_json_string(j, json_get(j, object, "destUser"), "id", name, sizeof(name), NULL)) return;
    (void)lbc_json_string(j, object, "declineReasonKey", reason, sizeof(reason), "generic"); lower(reason);
    if (!strcmp(filter, "fine")) {
        if (!strcmp(reason, "toofast") || !strcmp(reason, "tooslow") || !strcmp(reason, "timecontrol"))
            (void)lbc_json_string(j, object, "speed", aspect, sizeof(aspect), "");
        else if (!strcmp(reason, "rated") || !strcmp(reason, "casual"))
            (void)lbc_copy(aspect, sizeof(aspect), lbc_json_bool(j, object, "rated", 0) ? "rated" : "casual");
        else if (!strcmp(reason, "standard") || !strcmp(reason, "variant"))
            (void)lbc_json_string(j, json_get(j, object, "variant"), "key", aspect, sizeof(aspect), "");
    }
    int permanent = *settings_str(cfg->settings, "matchmaking.challenge_decliner_file_name", "") != 0;
    (void)policy_filter(p, cfg, name, aspect, permanent ? INT64_MAX : now + INT64_C(86400000000), permanent);
}
