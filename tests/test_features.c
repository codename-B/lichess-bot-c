#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "evaluation.h"
#include "json.h"
#include "policy.h"
#include "position.h"
#include "providers.h"
#include "settings.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Policy and move-source fixtures from lichess-bot f2b38b4. No network I/O. */
static unsigned checks;
#define CHECK(test) do { ++checks; if (!(test)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); } } while (0)
#define STR(a, b) CHECK(!strcmp((a), (b)))
#define INTEGER(c, p, v) CHECK(settings_set_int((c)->settings, (p), (v)) == 0)
#define STRING(c, p, v) CHECK(settings_set_str((c)->settings, (p), (v)) == 0)
#define BOOLEAN(c, p, v) CHECK(settings_set_bool((c)->settings, (p), (v)) == 0)
#define NULL_SETTING(c, p) CHECK(settings_set_null((c)->settings, (p)) == 0)

static Json parsed(const char *text, JsonTok *tokens, int capacity) {
    Json j;
    CHECK(json_parse(&j, text, strlen(text), tokens, capacity) == 0);
    return j;
}
static Config configuration(void) {
    Config cfg;
    char *argv[] = {"test_features", "--engine", "/bin/cat", NULL};
    CHECK(setenv("LICHESS_BOT_TOKEN", "test_features_not_a_real_token", 1) == 0);
    CHECK(config_parse(&cfg, 3, argv) == 0);
    cfg.variants = VAR_ALL;
    return cfg;
}
static void yaml(Config *cfg, const char *path, const char *value) {
    char text[4096], error[512];
    CHECK(snprintf(text, sizeof(text), "value: %s\n", value) > 0);
    Settings *s = settings_load_string(text, error, sizeof(error));
    if (!s) fprintf(stderr, "%s\n", error);
    CHECK(s != NULL);
    CHECK(settings_set(cfg->settings, path, settings_get(s, "value")) == 0);
    settings_free(s);
}
static void profile(Policy *p) {
    JsonTok tokens[128];
    Json j = parsed("{\"id\":\"own-bot\",\"username\":\"Own-Bot\",\"perfs\":{"
                    "\"bullet\":{\"rating\":1800},\"blitz\":{\"rating\":2000},"
                    "\"rapid\":{\"rating\":2100},\"atomic\":{\"rating\":2200},"
                    "\"chess960\":{\"rating\":2300},\"correspondence\":{\"rating\":2400},"
                    "\"bad\":{\"rating\":-1},\"huge\":{\"rating\":100000}}}", tokens, 128);
    CHECK(policy_profile(p, &j, 0) == 0);
}
static Policy *policy(const Config *cfg, uint64_t seed) {
    Policy *p = malloc(sizeof(*p)); char error[512] = "";
    CHECK(p != NULL);
    CHECK(policy_init(p, cfg, seed, error, sizeof(error)) == 0);
    profile(p);
    return p;
}
static void free_policy(Policy *p) { policy_free(p); free(p); }
static Challenge clock_challenge(void) {
    JsonTok tokens[160]; Challenge c;
    Json j = parsed("{\"id\":\"challenge1\",\"rated\":true,\"challenger\":{"
                    "\"id\":\"Other_Bot\",\"name\":\"Other_Bot\",\"title\":\"BOT\",\"rating\":2100},"
                    "\"variant\":{\"key\":\"standard\"},\"speed\":\"blitz\",\"perf\":{\"name\":\"Blitz\"},"
                    "\"timeControl\":{\"type\":\"clock\",\"limit\":180,\"increment\":2}}", tokens, 160);
    CHECK(policy_challenge_parse(&c, &j, 0, INT64_C(1000000)) == 0);
    return c;
}
static void reason(Policy *p, Config *cfg, const Challenge *c, int engagements, int64_t now, const char *expected) {
    const char *actual = policy_challenge_reason(p, cfg, c, engagements, now);
    if (expected) { CHECK(actual != NULL); STR(actual, expected); }
    else CHECK(actual == NULL);
}
static void test_challenge_parse(void) {
    Config cfg = configuration(); Policy *p = policy(&cfg, 123);
    STR(p->profile.name, "Own-Bot"); CHECK(p->profile.count == 6);
    CHECK(policy_rating(p, "BLITZ") == 2000); CHECK(policy_rating(p, "unknown") == 0);
    Challenge c = clock_challenge();
    STR(c.opponent, "other_bot"); STR(c.name, "Other_Bot"); STR(c.fen, "startpos");
    CHECK(c.bot && !c.titled && c.used && c.rated);
    CHECK(c.initial == 180 && c.increment == 2 && c.days == -1);
    CHECK(c.rating == 2100 && c.variant == VAR_STANDARD && c.created_us == INT64_C(1000000));
    JsonTok tokens[160];
    Json j = parsed("{\"id\":\"correspondence1\",\"challenger\":{\"id\":\"human\",\"title\":\"GM\"},"
                    "\"variant\":{\"key\":\"threeCheck\"},\"speed\":\"correspondence\","
                    "\"initialFen\":\"custom fen\",\"timeControl\":{\"type\":\"correspondence\",\"daysPerTurn\":3}}", tokens, 160);
    CHECK(policy_challenge_parse(&c, &j, 0, 12) == 0);
    CHECK(!c.bot && c.titled && !c.rated && c.rating == 0 && c.days == 3);
    CHECK(c.variant == VAR_THREE_CHECK && c.initial == -1 && c.increment == -1);
    STR(c.perf, "correspondence"); STR(c.fen, "custom fen"); STR(c.name, "human");
    j = parsed("{\"id\":\"unlimited\",\"challenger\":{\"id\":\"human\"},"
               "\"variant\":{\"key\":\"standard\"},\"speed\":\"correspondence\","
               "\"timeControl\":{\"type\":\"unlimited\"}}", tokens, 160);
    CHECK(policy_challenge_parse(&c, &j, 0, 13) == 0);
    CHECK(c.initial == -1 && c.increment == -1 && c.days == -1);
    static const char *const invalid[] = {
        "{}", "[]", "null",
        "{\"id\":\"../../bad\",\"challenger\":{\"id\":\"human\"},\"variant\":{\"key\":\"standard\"},\"speed\":\"blitz\",\"timeControl\":{\"type\":\"unlimited\"}}",
        "{\"id\":\"ok\",\"challenger\":{\"id\":\"bad user\"},\"variant\":{\"key\":\"standard\"},\"speed\":\"blitz\",\"timeControl\":{\"type\":\"unlimited\"}}",
        "{\"id\":\"ok\",\"challenger\":{\"id\":\"human\"},\"variant\":{\"key\":\"standard\"},\"speed\":\"blitz\",\"timeControl\":{\"type\":\"clock\",\"limit\":-1,\"increment\":0}}",
        "{\"id\":\"ok\",\"challenger\":{\"id\":\"human\"},\"variant\":{\"key\":\"standard\"},\"speed\":\"blitz\",\"timeControl\":{\"type\":\"clock\",\"limit\":180}}",
        "{\"id\":\"ok\",\"challenger\":{\"id\":\"human\"},\"variant\":{\"key\":\"standard\"},\"speed\":\"correspondence\",\"timeControl\":{\"type\":\"correspondence\",\"daysPerTurn\":0}}",
        "{\"id\":\"ok\",\"challenger\":{\"id\":\"human\"},\"variant\":{\"key\":\"standard\"},\"speed\":\"blitz\",\"timeControl\":{\"type\":\"other\"}}"
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i) {
        j = parsed(invalid[i], tokens, 160);
        CHECK(policy_challenge_parse(&c, &j, 0, 14) == -1);
    }
    free_policy(p); config_free(&cfg);
}
static void test_challenge_policy(void) {
    Config cfg = configuration(); Policy *p = policy(&cfg, 123); Challenge c = clock_challenge();
    reason(p, &cfg, &c, 0, 1, NULL);
    cfg.accept = 0; reason(p, &cfg, &c, 0, 1, "later"); cfg.accept = 1;
    BOOLEAN(&cfg, "challenge.accept_bot", 0); reason(p, &cfg, &c, 0, 1, "noBot");
    c.bot = 0; reason(p, &cfg, &c, 0, 1, NULL);
    cfg.only_bots = 1; reason(p, &cfg, &c, 0, 1, "onlyBot"); cfg.only_bots = 0;
    BOOLEAN(&cfg, "challenge.accept_bot", 1); c.bot = 1;
    yaml(&cfg, "challenge.time_controls", "[]"); reason(p, &cfg, &c, 0, 1, "timeControl");
    yaml(&cfg, "challenge.time_controls", "[bullet, blitz, rapid, classical, correspondence, ultraBullet]");
    strcpy(c.speed, "ultraBullet"); reason(p, &cfg, &c, 0, 1, "tooFast"); strcpy(c.speed, "blitz");
    cfg.min_initial = 181; reason(p, &cfg, &c, 0, 1, "tooFast"); cfg.min_initial = 0;
    cfg.max_initial = 179; reason(p, &cfg, &c, 0, 1, "tooSlow"); cfg.max_initial = 1800;
    cfg.min_increment = 3; reason(p, &cfg, &c, 0, 1, "tooFast"); cfg.min_increment = 0;
    cfg.max_increment = 1; reason(p, &cfg, &c, 0, 1, "tooSlow"); cfg.max_increment = 60;
    BOOLEAN(&cfg, "challenge.bullet_requires_increment", 1);
    strcpy(c.speed, "bullet"); c.increment = 0; reason(p, &cfg, &c, 0, 1, "tooFast");
    c.bot = 0; reason(p, &cfg, &c, 0, 1, NULL);
    c = clock_challenge(); cfg.variants = VAR_ATOMIC; reason(p, &cfg, &c, 0, 1, "variant");
    cfg.variants = VAR_ALL;
    for (int bit = VAR_STANDARD; bit <= VAR_THREE_CHECK; bit *= 2) {
        c.variant = bit; reason(p, &cfg, &c, 0, 1, NULL);
    }
    c.variant = 0; reason(p, &cfg, &c, 0, 1, "variant"); c.variant = VAR_STANDARD;
    cfg.casual_only = 1; reason(p, &cfg, &c, 0, 1, "casual"); cfg.casual_only = 0;
    yaml(&cfg, "challenge.modes", "[]"); reason(p, &cfg, &c, 0, 1, "casual");
    c.rated = 0; reason(p, &cfg, &c, 0, 1, "rated");
    yaml(&cfg, "challenge.modes", "[rated]"); reason(p, &cfg, &c, 0, 1, "rated");
    yaml(&cfg, "challenge.modes", "[casual, rated]"); c.rated = 1;
    INTEGER(&cfg, "challenge.min_rating", 2101); reason(p, &cfg, &c, 0, 1, "generic");
    INTEGER(&cfg, "challenge.min_rating", 1000); INTEGER(&cfg, "challenge.max_rating", 2099);
    reason(p, &cfg, &c, 0, 1, "generic");
    INTEGER(&cfg, "challenge.max_rating", 3000); INTEGER(&cfg, "challenge.rating_difference", 99);
    reason(p, &cfg, &c, 0, 1, "generic");
    INTEGER(&cfg, "challenge.rating_difference", 100); reason(p, &cfg, &c, 0, 1, NULL);
    c.rating = 1899; reason(p, &cfg, &c, 0, 1, "generic");
    c.rating = 1900; reason(p, &cfg, &c, 0, 1, NULL);
    c.rating = 0; reason(p, &cfg, &c, 0, 1, NULL);
    c.rating = 2900; strcpy(c.perf, "Unknown"); reason(p, &cfg, &c, 0, 1, NULL);
    c = clock_challenge(); NULL_SETTING(&cfg, "challenge.rating_difference");
    yaml(&cfg, "challenge.allow_list", "['', 'Different']"); reason(p, &cfg, &c, 0, 1, "generic");
    yaml(&cfg, "challenge.allow_list", "['OTHER_BOT']"); reason(p, &cfg, &c, 0, 1, NULL);
    yaml(&cfg, "challenge.block_list", "['OtHeR_bOt']"); reason(p, &cfg, &c, 0, 1, "generic");
    yaml(&cfg, "challenge.block_list", "[]"); yaml(&cfg, "challenge.allow_list", "['']");
    reason(p, &cfg, &c, 4, 1, NULL); reason(p, &cfg, &c, 5, 1, "later");
    c.bot = 0; c.initial = c.increment = -1; c.days = 3; strcpy(c.speed, "correspondence");
    reason(p, &cfg, &c, 0, 1, NULL);
    INTEGER(&cfg, "challenge.min_days", 4); reason(p, &cfg, &c, 0, 1, "timeControl");
    INTEGER(&cfg, "challenge.min_days", 1); INTEGER(&cfg, "challenge.max_days", 2);
    reason(p, &cfg, &c, 0, 1, "timeControl"); c.days = -1;
    reason(p, &cfg, &c, 0, 1, "timeControl");
    CHECK(settings_set_double(cfg.settings, "challenge.max_days", INFINITY) == 0);
    reason(p, &cfg, &c, 0, 1, NULL);
    c = clock_challenge(); INTEGER(&cfg, "challenge.max_recent_bot_challenges", 2);
    INTEGER(&cfg, "challenge.recent_bot_challenge_age", 60);
    policy_accepted(p, &cfg, &c, 100); reason(p, &cfg, &c, 0, 101, NULL);
    policy_accepted(p, &cfg, &c, 102); reason(p, &cfg, &c, 0, 103, "later");
    c.admitted = 1; reason(p, &cfg, &c, 0, 103, NULL); c.admitted = 0;
    reason(p, &cfg, &c, 0, INT64_C(60000102), NULL);
    c.bot = 0; reason(p, &cfg, &c, 0, 103, NULL);
    c.bot = 1; memset(p->recent, 0, sizeof(p->recent));
    NULL_SETTING(&cfg, "challenge.recent_bot_challenge_age");
    policy_accepted(p, &cfg, &c, 100); policy_accepted(p, &cfg, &c, 102);
    reason(p, &cfg, &c, 0, 103, NULL);
    INTEGER(&cfg, "challenge.max_recent_bot_challenges", 0); reason(p, &cfg, &c, 0, 103, "later");
    NULL_SETTING(&cfg, "challenge.max_recent_bot_challenges"); reason(p, &cfg, &c, 0, 103, NULL);
    free_policy(p); config_free(&cfg);
}
static void test_queue(void) {
    Config cfg = configuration(); Policy *p = policy(&cfg, 123); Challenge a = clock_challenge(), b = a, c = a;
    strcpy(a.id, "oldbot"); a.created_us = 100; a.rating = 2100;
    strcpy(b.id, "newhuman"); b.created_us = 200; b.bot = 0; b.rating = 2200; b.rated = b.titled = 0;
    strcpy(c.id, "titled"); c.created_us = 300; c.bot = 0; c.rating = 2000; c.titled = c.rated = 1;
    CHECK(policy_queue_add(p, &a) == 0); CHECK(policy_queue_add(p, &b) == 0); CHECK(policy_queue_add(p, &c) == 0);
    STR(policy_queue_best(p, &cfg, 1, 301)->id, "titled");
    STRING(&cfg, "challenge.sort_by", "first"); STR(policy_queue_best(p, &cfg, 1, 301)->id, "oldbot");
    STRING(&cfg, "challenge.preference", "human"); STR(policy_queue_best(p, &cfg, 1, 301)->id, "newhuman");
    STRING(&cfg, "challenge.sort_by", "best"); STR(policy_queue_best(p, &cfg, 1, 301)->id, "titled");
    STRING(&cfg, "challenge.preference", "bot"); STR(policy_queue_best(p, &cfg, 1, 301)->id, "oldbot");
    STR(policy_queue_best(p, &cfg, 0, 301)->id, "titled");
    a.rating = 5000; CHECK(policy_queue_add(p, &a) == 0); CHECK(p->queue[0].rating == 2100);
    policy_queue_remove(p, "titled"); STR(policy_queue_best(p, &cfg, 0, 301)->id, "newhuman");
    policy_queue_remove(p, "newhuman"); CHECK(policy_queue_best(p, &cfg, 0, 301) == NULL);
    CHECK(policy_queue_best(p, &cfg, 1, INT64_C(120000101)) == NULL);
    for (int i = 0; i < POLICY_QUEUE_CAP; ++i) {
        CHECK(snprintf(a.id, sizeof(a.id), "game%d", i) > 0); CHECK(policy_queue_add(p, &a) == 0);
    }
    strcpy(a.id, "one_too_many"); CHECK(policy_queue_add(p, &a) == -1);
    policy_queue_remove(p, "game0"); CHECK(policy_queue_add(p, &a) == 0);
    free_policy(p); config_free(&cfg);
}
static void test_block_lists(void) {
    Config cfg = configuration();
    yaml(&cfg, "challenge.online_block_list", "['https://lists.example/shared', 'https://lists.example/challenges']");
    yaml(&cfg, "matchmaking.online_block_list", "['https://lists.example/shared', 'https://lists.example/matches']");
    Policy *p = policy(&cfg, 123);
    CHECK(p->list_count == 3 && p->lists[0].owners == 3 && p->lists[1].owners == 1 && p->lists[2].owners == 2);
    CHECK(!p->lists[0].initialized && !p->lists[1].initialized && !p->lists[2].initialized);
    CHECK(policy_online_due(p, 100) == 0);
    const char *text = "# Comment\r\n  @MiXeD_Bot  \r\n\nAnother-Bot\n";
    CHECK(policy_online_update(p, 0, text, strlen(text), 100) == 0);
    CHECK(p->lists[0].count == 2 && p->lists[0].due_us == INT64_C(1800000100));
    CHECK(p->lists[0].initialized && !p->lists[1].initialized && !p->lists[2].initialized);
    CHECK(policy_is_blocked(p, &cfg, "mixed_bot", 0, 100));
    CHECK(policy_is_blocked(p, &cfg, "MIXED_BOT", 1, 100));
    CHECK(policy_online_update(p, 1, "challenge-only\n", 15, 100) == 0);
    CHECK(policy_is_blocked(p, &cfg, "Challenge-Only", 0, 100));
    CHECK(!policy_is_blocked(p, &cfg, "challenge-only", 1, 100));
    BOOLEAN(&cfg, "matchmaking.include_challenge_block_list", 1);
    CHECK(policy_is_blocked(p, &cfg, "challenge-only", 1, 100));
    CHECK(policy_online_update(p, 2, "match-only\n", 11, 100) == 0);
    CHECK(!policy_is_blocked(p, &cfg, "match-only", 0, 100));
    CHECK(policy_is_blocked(p, &cfg, "match-only", 1, 100));
    CHECK(policy_online_due(p, 101) == -1); CHECK(policy_online_due(p, INT64_C(1800000100)) == 0);
    CHECK(policy_online_update(p, 0, "bad user\n", 9, 101) == -1);
    CHECK(policy_is_blocked(p, &cfg, "mixed_bot", 0, 102));
    CHECK(policy_online_update(p, 99, "", 0, 101) == -1);
    CHECK(policy_online_update(p, 0, "", 4u*1024u*1024u+1u, 101) == -1);
    yaml(&cfg, "challenge.block_list", "['Local']"); CHECK(policy_is_blocked(p, &cfg, "LOCAL", 1, 100));
    BOOLEAN(&cfg, "matchmaking.include_challenge_block_list", 0);
    CHECK(!policy_is_blocked(p, &cfg, "LOCAL", 1, 100));
    yaml(&cfg, "matchmaking.block_list", "['Other']"); CHECK(policy_is_blocked(p, &cfg, "other", 1, 100));
    CHECK(!policy_is_blocked(p, &cfg, "other", 0, 100));
    CHECK(policy_filter(p, &cfg, "temporary", "", 200, 0) == 0);
    CHECK(policy_is_blocked(p, &cfg, "temporary", 1, 199));
    CHECK(!policy_is_blocked(p, &cfg, "temporary", 1, 200));
    CHECK(!policy_is_blocked(p, &cfg, "temporary", 0, 199));
    CHECK(policy_filter(p, &cfg, "bad name", "", 200, 0) == -1);
    CHECK(policy_filter(p, &cfg, "name", "bad,aspect", 200, 0) == -1);
    free_policy(p);
    yaml(&cfg, "challenge.online_block_list", "['file:///etc/passwd']");
    p = malloc(sizeof(*p)); CHECK(p != NULL); char error[512] = "";
    CHECK(policy_init(p, &cfg, 1, error, sizeof(error)) == -1 && *error);
    free_policy(p); config_free(&cfg);
}
static void base_match(Config *cfg) {
    STRING(cfg, "matchmaking.challenge_variant", "standard"); STRING(cfg, "matchmaking.challenge_mode", "rated");
    yaml(cfg, "matchmaking.challenge_initial_time", "[180]"); yaml(cfg, "matchmaking.challenge_increment", "[2]");
    yaml(cfg, "matchmaking.challenge_days", "[null]");
    INTEGER(cfg, "matchmaking.opponent_min_rating", 1000); INTEGER(cfg, "matchmaking.opponent_max_rating", 3000);
}
static void test_match_settings(void) {
    Config cfg = configuration(); Policy *p = policy(&cfg, 7654321); char body[256];
    CHECK(policy_match_begin(p, &cfg) == -1);
    base_match(&cfg); CHECK(policy_match_begin(p, &cfg) == 0);
    STR(p->match.variant, "standard"); STR(p->match.mode, "rated"); STR(p->match.speed, "blitz");
    CHECK(p->match.initial == 180 && p->match.increment == 2 && !p->match.days);
    CHECK(policy_match_body(p, body, sizeof(body)) == 0); STR(body, "rated=true&variant=standard&clock.limit=180&clock.increment=2");
    CHECK(policy_match_body(p, body, 5) == -1);
    INTEGER(&cfg, "matchmaking.opponent_rating_difference", 125);
    CHECK(policy_match_begin(p, &cfg) == 0); CHECK(p->match.min_rating == 1875 && p->match.max_rating == 2125);
    NULL_SETTING(&cfg, "matchmaking.opponent_rating_difference");
    yaml(&cfg, "matchmaking.challenge_initial_time", "[0]"); yaml(&cfg, "matchmaking.challenge_increment", "[0]");
    yaml(&cfg, "matchmaking.challenge_days", "[3]");
    CHECK(policy_match_begin(p, &cfg) == 0); CHECK(p->match.days == 3 && !p->match.initial && !p->match.increment);
    STR(p->match.speed, "correspondence"); CHECK(policy_match_body(p, body, sizeof(body)) == 0);
    STR(body, "rated=true&variant=standard&days=3");
    base_match(&cfg);
    yaml(&cfg, "matchmaking.challenge_initial_time", "[60, 180]");
    yaml(&cfg, "matchmaking.challenge_increment", "[1, 2]"); yaml(&cfg, "matchmaking.challenge_days", "[2, 3]");
    STRING(&cfg, "matchmaking.challenge_variant", "random"); STRING(&cfg, "matchmaking.challenge_mode", "random");
    cfg.variants = VAR_STANDARD | VAR_FROM_POSITION | VAR_ATOMIC | VAR_CHESS960;
    int variants = 0, modes = 0, clocks = 0, days = 0;
    for (int i = 0; i < 256; ++i) {
        CHECK(policy_match_begin(p, &cfg) == 0);
        variants |= config_variant(p->match.variant); modes |= !strcmp(p->match.mode, "rated") ? 1 : 2;
        if (p->match.days) { CHECK(p->match.days == 2 || p->match.days == 3); CHECK(!p->match.initial && !p->match.increment); ++days; }
        else { CHECK(p->match.initial == 60 || p->match.initial == 180); CHECK(p->match.increment == 1 || p->match.increment == 2); ++clocks; }
    }
    CHECK(variants == (VAR_STANDARD | VAR_ATOMIC | VAR_CHESS960)); CHECK(modes == 3 && clocks > 50 && days > 50);
    cfg.variants = VAR_FROM_POSITION; CHECK(policy_match_begin(p, &cfg) == -1);
    cfg.variants = VAR_ALL; base_match(&cfg);
    yaml(&cfg, "matchmaking.overrides", "{'atomic.fast': {challenge_variant: atomic, challenge_initial_time: [30], challenge_increment: [1]}, 'postal': {challenge_days: [5], challenge_initial_time: [0], challenge_increment: [0], challenge_mode: casual}}");
    int seen = 0;
    for (int i = 0; i < 120; ++i) {
        CHECK(policy_match_begin(p, &cfg) == 0);
        if (!strcmp(p->match.override_name, "default")) { STR(p->match.variant, "standard"); CHECK(p->match.initial == 180); seen |= 1; }
        else if (!strcmp(p->match.override_name, "atomic.fast")) { STR(p->match.variant, "atomic"); CHECK(p->match.initial == 30 && p->match.increment == 1); STR(p->match.mode, "rated"); seen |= 2; }
        else { STR(p->match.override_name, "postal"); CHECK(p->match.days == 5); STR(p->match.mode, "casual"); seen |= 4; }
        CHECK(p->match.min_rating == 1000 && p->match.max_rating == 3000);
    }
    CHECK(seen == 7);
    STR(policy_game_category(VAR_STANDARD, 178, 0, 0), "bullet");
    STR(policy_game_category(VAR_STANDARD, 179, 0, 0), "blitz");
    STR(policy_game_category(VAR_STANDARD, 478, 0, 0), "blitz");
    STR(policy_game_category(VAR_STANDARD, 479, 0, 0), "rapid");
    STR(policy_game_category(VAR_STANDARD, 1498, 0, 0), "rapid");
    STR(policy_game_category(VAR_STANDARD, 1499, 0, 0), "classical");
    STR(policy_game_category(VAR_STANDARD, 0, 5, 0), "blitz");
    STR(policy_game_category(VAR_STANDARD, 0, 0, 2), "correspondence");
    STR(policy_game_category(VAR_ATOMIC, 0, 0, 2), "atomic");
    STR(policy_game_category(VAR_CHESS960, 60, 0, 0), "chess960");
    free_policy(p); config_free(&cfg);
}
static void candidate(Policy *p, Config *cfg, const char *text, int engaged, int64_t now) {
    JsonTok tokens[128]; Json j = parsed(text, tokens, 128);
    policy_match_consider(p, cfg, &j, 0, engaged, now);
}
static void test_match_candidates(void) {
    Config cfg = configuration(); base_match(&cfg); Policy *p = policy(&cfg, 12345);
    CHECK(policy_match_begin(p, &cfg) == 0);
    candidate(p, &cfg, "{\"id\":\"own-bot\",\"perfs\":{\"blitz\":{\"games\":3,\"rating\":2000}}}", 0, 1);
    candidate(p, &cfg, "{\"id\":\"engaged\",\"perfs\":{\"blitz\":{\"games\":3,\"rating\":2000}}}", 1, 1);
    candidate(p, &cfg, "{\"id\":\"unplayed\",\"perfs\":{\"blitz\":{\"games\":0,\"rating\":2000}}}", 0, 1);
    candidate(p, &cfg, "{\"id\":\"weak\",\"perfs\":{\"blitz\":{\"games\":3,\"rating\":999}}}", 0, 1);
    candidate(p, &cfg, "{\"id\":\"strong\",\"perfs\":{\"blitz\":{\"games\":3,\"rating\":3001}}}", 0, 1);
    candidate(p, &cfg, "{\"id\":\"missingperf\"}", 0, 1);
    yaml(&cfg, "matchmaking.block_list", "[blocked]");
    candidate(p, &cfg, "{\"id\":\"Blocked\",\"perfs\":{\"blitz\":{\"games\":3,\"rating\":2000}}}", 0, 1);
    CHECK(!*policy_match_finish(p) && !p->match.all_weight);
    candidate(p, &cfg, "{\"username\":\"Eligible\",\"perfs\":{\"blitz\":{\"games\":1,\"rating\":1000}}}", 0, 1);
    STR(policy_match_finish(p), "Eligible"); CHECK(p->match.all_weight == 1 && p->match.ready_weight == 1);
    STRING(&cfg, "matchmaking.challenge_filter", "fine");
    CHECK(policy_filter(p, &cfg, "fineblocked", "blitz", 1000, 0) == 0);
    CHECK(policy_match_begin(p, &cfg) == 0);
    candidate(p, &cfg, "{\"id\":\"fineblocked\",\"perfs\":{\"blitz\":{\"games\":1,\"rating\":2000}}}", 0, 1);
    CHECK(p->match.all_weight == 1 && p->match.ready_weight == 0);
    STR(policy_match_finish(p), "fineblocked");
    CHECK(policy_match_begin(p, &cfg) == 0);
    candidate(p, &cfg, "{\"id\":\"fineblocked\",\"perfs\":{\"blitz\":{\"games\":1,\"rating\":2000}}}", 0, 1);
    candidate(p, &cfg, "{\"id\":\"ready\",\"perfs\":{\"blitz\":{\"games\":1,\"rating\":2000}}}", 0, 1);
    STR(policy_match_finish(p), "ready"); CHECK(p->match.all_weight == 2 && p->match.ready_weight == 1);
    CHECK(policy_match_begin(p, &cfg) == 0);
    candidate(p, &cfg, "{\"id\":\"fineblocked\",\"perfs\":{\"blitz\":{\"games\":1,\"rating\":2000}}}", 0, 1000);
    CHECK(p->match.ready_weight == 1);
    STRING(&cfg, "matchmaking.challenge_filter", "none");
    JsonTok low_tokens[64], high_tokens[64];
    Json low = parsed("{\"id\":\"low\",\"perfs\":{\"blitz\":{\"games\":1,\"rating\":1000}}}", low_tokens, 64);
    Json high = parsed("{\"id\":\"high\",\"perfs\":{\"blitz\":{\"games\":1,\"rating\":3000}}}", high_tokens, 64);
    static const char *const preferences[] = {"none", "high", "low"};
    for (size_t preference = 0; preference < 3; ++preference) {
        STRING(&cfg, "matchmaking.rating_preference", preferences[preference]);
        int selected_high = 0;
        for (int i = 0; i < 2048; ++i) {
            CHECK(policy_match_begin(p, &cfg) == 0);
            policy_match_consider(p, &cfg, &low, 0, 0, 1); policy_match_consider(p, &cfg, &high, 0, 0, 1);
            CHECK(p->match.all_weight == (preference ? 6000u : 2u));
            CHECK(p->match.all_weight == p->match.ready_weight);
            if (!strcmp(policy_match_finish(p), "high")) ++selected_high;
        }
        if (!preference) CHECK(selected_high > 880 && selected_high < 1170);
        else if (preference == 1) CHECK(selected_high > 1230 && selected_high < 1510);
        else CHECK(selected_high > 540 && selected_high < 820);
    }
    free_policy(p); config_free(&cfg);
}
static void declined(Policy *p, Config *cfg, const char *source, const char *target, const char *why, int64_t now) {
    char text[512]; JsonTok tokens[128];
    CHECK(snprintf(text, sizeof(text), "{\"challenger\":{\"id\":\"%s\"},\"destUser\":{\"id\":\"%s\"},\"declineReasonKey\":\"%s\",\"speed\":\"blitz\",\"rated\":true,\"variant\":{\"key\":\"atomic\"}}", source, target, why) > 0);
    Json j = parsed(text, tokens, 128); policy_match_declined(p, cfg, &j, 0, now);
}
static const PolicyFilter *find_filter(const Policy *p, const char *name) {
    for (size_t i = 0; i < POLICY_FILTER_CAP; ++i) if (!strcmp(p->filters[i].name, name)) return &p->filters[i];
    return NULL;
}
static void test_decline_file(void) {
    Config cfg = configuration(); Policy *p = policy(&cfg, 1);
    declined(p, &cfg, "own-bot", "ignored", "generic", 100); CHECK(find_filter(p, "ignored") == NULL);
    STRING(&cfg, "matchmaking.challenge_filter", "fine");
    declined(p, &cfg, "someone-else", "ignored", "generic", 100); CHECK(find_filter(p, "ignored") == NULL);
    declined(p, &cfg, "Own-Bot", "Speed", "timeControl", 100);
    CHECK(find_filter(p, "speed") != NULL); STR(find_filter(p, "speed")->aspect, "blitz");
    CHECK(find_filter(p, "speed")->until_us == INT64_C(86400000100));
    declined(p, &cfg, "own-bot", "mode", "Casual", 100); STR(find_filter(p, "mode")->aspect, "rated");
    declined(p, &cfg, "own-bot", "variant", "variant", 100); STR(find_filter(p, "variant")->aspect, "atomic");
    declined(p, &cfg, "own-bot", "unknown", "unexpected", 100); STR(find_filter(p, "unknown")->aspect, "");
    CHECK(policy_is_blocked(p, &cfg, "unknown", 1, 101)); CHECK(!policy_is_blocked(p, &cfg, "unknown", 1, INT64_C(86400000100)));
    STRING(&cfg, "matchmaking.challenge_filter", "coarse");
    declined(p, &cfg, "own-bot", "coarse", "variant", 100); STR(find_filter(p, "coarse")->aspect, "");
    char path[] = "/tmp/lbc-decliners-XXXXXX"; int fd = mkstemp(path); CHECK(fd >= 0); CHECK(close(fd) == 0);
    STRING(&cfg, "matchmaking.challenge_decliner_file_name", path); STRING(&cfg, "matchmaking.challenge_filter", "fine");
    declined(p, &cfg, "own-bot", "Persistent", "toofast", 100);
    CHECK(find_filter(p, "persistent")->until_us == INT64_MAX);
    free_policy(p); p = policy(&cfg, 2);
    CHECK(find_filter(p, "persistent") != NULL); STR(find_filter(p, "persistent")->aspect, "blitz");
    CHECK(find_filter(p, "persistent")->until_us == INT64_MAX);
    CHECK(policy_filter(p, &cfg, "Whole", "", INT64_MAX, 1) == 0);
    free_policy(p); p = policy(&cfg, 3); CHECK(policy_is_blocked(p, &cfg, "whole", 1, 999));
    free_policy(p);
    FILE *f = fopen(path, "w"); CHECK(f != NULL); CHECK(fputs("not a valid decliner line\n", f) >= 0); CHECK(fclose(f) == 0);
    p = malloc(sizeof(*p)); CHECK(p != NULL); char error[512] = "";
    CHECK(policy_init(p, &cfg, 4, error, sizeof(error)) == -1 && *error); free_policy(p);
    CHECK(unlink(path) == 0); config_free(&cfg);
}
static Position position(unsigned variant, const char *fen, const char *moves) {
    Position p; char error[512];
    CHECK(position_from_history(&p, variant, fen, moves, error, sizeof(error)) == 1);
    return p;
}
static const char *const provider_paths[] = {"", "engine.online_moves.online_egtb", "engine.online_moves.chessdb_book", "engine.online_moves.lichess_cloud_analysis", "engine.online_moves.lichess_opening_explorer"};
static void provider_bool(Config *cfg, enum ProviderKind kind, const char *key, int value) {
    char path[256]; CHECK(snprintf(path, sizeof(path), "%s.%s", provider_paths[kind], key) > 0); BOOLEAN(cfg, path, value);
}
static void provider_string(Config *cfg, enum ProviderKind kind, const char *key, const char *value) {
    char path[256]; CHECK(snprintf(path, sizeof(path), "%s.%s", provider_paths[kind], key) > 0); STRING(cfg, path, value);
}
static int response(Config *cfg, enum ProviderKind kind, const Position *p, const char *text, uint64_t *random, ProviderResult *result) {
    JsonTok tokens[2048]; Json j = parsed(text, tokens, 2048); return provider_parse(cfg, kind, p, &j, random, result);
}
static void test_provider_queries(void) {
    Config cfg = configuration(); Position p = position(VAR_STANDARD, "startpos", ""), tb = position(VAR_STANDARD, "7k/8/8/8/8/8/8/KR6 w - - 0 1", "");
    ProviderQuery q;
    for (int value = PROVIDER_EGTB; value < PROVIDER_DONE; ++value) {
        enum ProviderKind kind = (enum ProviderKind)value; const Position *board = kind == PROVIDER_EGTB ? &tb : &p;
        CHECK(provider_query(&cfg, kind, board, "Own-Bot", 20000, 180000, 0, 0, &q) == 0);
        provider_bool(&cfg, kind, "enabled", 1);
        CHECK(provider_query(&cfg, kind, board, "Own-Bot", 20000, 180000, 0, 0, &q) == 1);
        CHECK(q.kind == kind && q.timeout_ms > 0 && strstr(q.url, kind == PROVIDER_CHESSDB ? "board=" : "fen=") != NULL);
        CHECK(provider_query(&cfg, kind, board, "Own-Bot", 19999, 180000, 0, 0, &q) == 0);
        CHECK(provider_query(&cfg, kind, board, "Own-Bot", 20000, 10800001, 0, 0, &q) == 0);
        CHECK(provider_query(&cfg, kind, board, "Own-Bot", 20000, 10800000, 0, 0, &q) == 1);
    }
    INTEGER(&cfg, "engine.online_moves.max_depth", 2); INTEGER(&cfg, "engine.online_moves.max_out_of_book_moves", 3);
    CHECK(provider_query(&cfg, PROVIDER_CLOUD, &p, "me", 20000, 180000, 3, 2, &q) == 1);
    CHECK(provider_query(&cfg, PROVIDER_CLOUD, &p, "me", 20000, 180000, 4, 2, &q) == 0);
    CHECK(provider_query(&cfg, PROVIDER_CLOUD, &p, "me", 20000, 180000, 3, 3, &q) == 0);
    CHECK(provider_query(&cfg, PROVIDER_EGTB, &tb, "me", 20000, 180000, 99, 99, &q) == 1);
    CHECK(provider_query(&cfg, PROVIDER_EGTB, &p, "me", 20000, 180000, 0, 0, &q) == 0);
    CHECK(provider_query(&cfg, PROVIDER_CHESSDB, &p, "me", 20000, 180000, 0, 0, &q) == 1);
    CHECK(strstr(q.url, "action=querybest") && !q.authenticated);
    provider_string(&cfg, PROVIDER_CHESSDB, "move_quality", "best");
    CHECK(provider_query(&cfg, PROVIDER_CHESSDB, &p, "me", 20000, 180000, 0, 0, &q) == 1 && strstr(q.url, "action=querypv"));
    provider_string(&cfg, PROVIDER_CHESSDB, "move_quality", "all");
    CHECK(provider_query(&cfg, PROVIDER_CHESSDB, &p, "me", 20000, 180000, 0, 0, &q) == 1 && strstr(q.url, "action=query&"));
    CHECK(provider_query(&cfg, PROVIDER_CLOUD, &p, "me", 20000, 180000, 0, 0, &q) == 1);
    CHECK(strstr(q.url, "https://lichess.org/api/cloud-eval?") == q.url && strstr(q.url, "multiPv=1") && q.authenticated);
    provider_string(&cfg, PROVIDER_CLOUD, "move_quality", "good");
    CHECK(provider_query(&cfg, PROVIDER_CLOUD, &p, "me", 20000, 180000, 0, 0, &q) == 1 && strstr(q.url, "multiPv=5"));
    provider_string(&cfg, PROVIDER_CLOUD, "url", "http://127.0.0.1:8080/cloud?fixture=1");
    CHECK(provider_query(&cfg, PROVIDER_CLOUD, &p, "me", 20000, 180000, 0, 0, &q) == 1);
    CHECK(strstr(q.url, "fixture=1&fen=") && !q.authenticated);
    provider_string(&cfg, PROVIDER_CLOUD, "url", "file:///etc/passwd");
    CHECK(provider_query(&cfg, PROVIDER_CLOUD, &p, "me", 20000, 180000, 0, 0, &q) == -1);
    provider_string(&cfg, PROVIDER_CLOUD, "url", "");
    provider_string(&cfg, PROVIDER_EXPLORER, "source", "player");
    CHECK(provider_query(&cfg, PROVIDER_EXPLORER, &p, "Own-Bot", 20000, 180000, 0, 0, &q) == 1);
    CHECK(q.ndjson && q.authenticated && strstr(q.url, "/player?") && strstr(q.url, "player=Own-Bot") && strstr(q.url, "color=white"));
    CHECK(position_apply_uci(&p, "e2e4", NULL, 0));
    provider_string(&cfg, PROVIDER_EXPLORER, "player_name", "Name & Other");
    CHECK(provider_query(&cfg, PROVIDER_EXPLORER, &p, "Own-Bot", 20000, 180000, 0, 0, &q) == 1);
    CHECK(strstr(q.url, "player=Name%20%26%20Other") && strstr(q.url, "color=black") && strstr(q.url, "recentGames=0"));
    Position variant = position(VAR_ATOMIC, "startpos", "");
    provider_string(&cfg, PROVIDER_EXPLORER, "source", "masters");
    CHECK(provider_query(&cfg, PROVIDER_EXPLORER, &variant, "me", 20000, 180000, 0, 0, &q) == 0);
    provider_string(&cfg, PROVIDER_EXPLORER, "source", "lichess");
    CHECK(provider_query(&cfg, PROVIDER_EXPLORER, &variant, "me", 20000, 180000, 0, 0, &q) == 1);
    CHECK(!q.ndjson && strstr(q.url, "variant=atomic") && strstr(q.url, "topGames=0"));
    CHECK(provider_query(&cfg, PROVIDER_CHESSDB, &variant, "me", 20000, 180000, 0, 0, &q) == 0);
    CHECK(provider_query(&cfg, PROVIDER_CLOUD, &variant, "me", 20000, 180000, 0, 0, &q) == 1 && strstr(q.url, "variant=atomic"));
    variant = position(VAR_ATOMIC, "7k/8/8/8/8/8/8/KR6 w - - 0 1", "");
    CHECK(provider_query(&cfg, PROVIDER_EGTB, &variant, "me", 20000, 180000, 0, 0, &q) == 1 && strstr(q.url, "/atomic?"));
    variant = position(VAR_ANTICHESS, "7k/8/8/8/8/8/8/KR6 w - - 0 1", "");
    CHECK(provider_query(&cfg, PROVIDER_EGTB, &variant, "me", 20000, 180000, 0, 0, &q) == 1 && strstr(q.url, "/antichess?"));
    variant = position(VAR_CRAZYHOUSE, "7k/8/8/8/8/8/8/KR6[] w - - 0 1", "");
    CHECK(provider_query(&cfg, PROVIDER_EGTB, &variant, "me", 20000, 180000, 0, 0, &q) == 0);
    INTEGER(&cfg, "engine.online_moves.online_egtb.max_pieces", 8);
    Position eight = position(VAR_STANDARD, "4k2r/3p4/8/8/3P4/8/4P3/RN2K3 w - - 0 1", "");
    CHECK(position_piece_count(&eight) == 8);
    CHECK(provider_query(&cfg, PROVIDER_EGTB, &eight, "me", 20000, 180000, 0, 0, &q) == 1);
    Position no_opposition = position(VAR_STANDARD, "4k2r/2p5/8/8/3P4/8/4P3/RN2K3 w - - 0 1", "");
    CHECK(provider_query(&cfg, PROVIDER_EGTB, &no_opposition, "me", 20000, 180000, 0, 0, &q) == 0);
    provider_string(&cfg, PROVIDER_EGTB, "source", "chessdb");
    CHECK(provider_query(&cfg, PROVIDER_EGTB, &eight, "me", 20000, 180000, 0, 0, &q) == 0);
    CHECK(provider_query(&cfg, PROVIDER_EGTB, &tb, "me", 20000, 180000, 0, 0, &q) == 1 && strstr(q.url, "action=querypv"));
    provider_string(&cfg, PROVIDER_EGTB, "move_quality", "suggest");
    CHECK(provider_query(&cfg, PROVIDER_EGTB, &tb, "me", 20000, 180000, 0, 0, &q) == 1 && strstr(q.url, "action=queryall"));
    config_free(&cfg);
}
static void test_chessdb_responses(void) {
    Config cfg = configuration(); Position p = position(VAR_STANDARD, "startpos", ""); ProviderResult r; uint64_t random = 3;
    CHECK(response(&cfg, PROVIDER_CHESSDB, &p, "{\"status\":\"ok\",\"move\":\"e2e4\"}", &random, &r) == 1);
    STR(r.move, "e2e4"); STR(r.info.source, "ChessDB"); CHECK(!r.info.has_score);
    CHECK(response(&cfg, PROVIDER_CHESSDB, &p, "{\"status\":\"ok\",\"move\":\"e2e5\"}", &random, &r) == 0);
    CHECK(response(&cfg, PROVIDER_CHESSDB, &p, "{\"status\":\"unknown\",\"move\":\"e2e4\"}", &random, &r) == 0);
    provider_string(&cfg, PROVIDER_CHESSDB, "move_quality", "best");
    CHECK(response(&cfg, PROVIDER_CHESSDB, &p, "{\"status\":\"ok\",\"depth\":20,\"score\":31,\"pv\":[\"d2d4\",\"d7d5\"]}", &random, &r) == 1);
    STR(r.move, "d2d4"); STR(r.info.pv, "d2d4 d7d5"); CHECK(r.info.has_score && r.info.score == 31 && r.info.depth == 20);
    CHECK(response(&cfg, PROVIDER_CHESSDB, &p, "{\"status\":\"ok\",\"depth\":19,\"pv\":[\"d2d4\"]}", &random, &r) == 0);
    CHECK(response(&cfg, PROVIDER_CHESSDB, &p, "{\"status\":\"ok\",\"depth\":9223372036854775807,\"pv\":[\"d2d4\"]}", &random, &r) == 0);
    CHECK(response(&cfg, PROVIDER_CHESSDB, &p, "{\"status\":\"ok\",\"depth\":30,\"pv\":[\"e7e5\"]}", &random, &r) == 0);
    CHECK(response(&cfg, PROVIDER_CHESSDB, &p, "{\"status\":\"ok\",\"depth\":30,\"pv\":[]}", &random, &r) == 0);
    CHECK(response(&cfg, PROVIDER_CHESSDB, &p, "{\"status\":\"ok\",\"depth\":30,\"pv\":\"d2d4\"}", &random, &r) == 0);
    p = position(VAR_STANDARD, "startpos", "e2e4");
    CHECK(response(&cfg, PROVIDER_CHESSDB, &p, "{\"status\":\"ok\",\"depth\":30,\"score\":-51,\"pv\":[\"e7e5\"]}", &random, &r) == 1);
    CHECK(r.info.score == -51); /* ChessDB already returns side-to-move POV. */
    config_free(&cfg);
}
static void test_cloud_responses(void) {
    Config cfg = configuration(); Position white = position(VAR_STANDARD, "startpos", ""), black = position(VAR_STANDARD, "startpos", "e2e4");
    ProviderResult r; uint64_t random = 912345;
    const char *data = "{\"depth\":22,\"knodes\":321,\"pvs\":[{\"cp\":100,\"moves\":\"e2e4 e7e5\"},{\"cp\":50,\"moves\":\"d2d4 d7d5\"},{\"cp\":49,\"moves\":\"g1f3 g8f6\"}]}";
    CHECK(response(&cfg, PROVIDER_CLOUD, &white, data, &random, &r) == 1);
    STR(r.move, "e2e4"); CHECK(r.info.has_score && r.info.score == 100 && r.info.depth == 22 && r.info.nodes == 321000);
    STR(r.info.pv, "e2e4 e7e5"); STR(r.info.source, "Lichess Cloud Analysis");
    provider_string(&cfg, PROVIDER_CLOUD, "move_quality", "good"); int seen = 0;
    for (int i = 0; i < 64; ++i) {
        CHECK(response(&cfg, PROVIDER_CLOUD, &white, data, &random, &r) == 1);
        CHECK(!strcmp(r.move, "e2e4") || !strcmp(r.move, "d2d4")); seen |= !strcmp(r.move, "e2e4") ? 1 : 2;
    }
    CHECK(seen == 3);
    data = "{\"depth\":22,\"knodes\":1,\"pvs\":[{\"cp\":-40,\"moves\":\"e7e5 g1f3\"},{\"cp\":10,\"moves\":\"c7c5 g1f3\"},{\"cp\":11,\"moves\":\"g8f6 e4e5\"}]}"; seen = 0;
    for (int i = 0; i < 64; ++i) {
        CHECK(response(&cfg, PROVIDER_CLOUD, &black, data, &random, &r) == 1);
        CHECK(!strcmp(r.move, "e7e5") || !strcmp(r.move, "c7c5"));
        CHECK(r.info.score == (!strcmp(r.move, "e7e5") ? 40 : -10)); seen |= !strcmp(r.move, "e7e5") ? 1 : 2;
    }
    CHECK(seen == 3); provider_string(&cfg, PROVIDER_CLOUD, "move_quality", "best");
    CHECK(response(&cfg, PROVIDER_CLOUD, &black, "{\"depth\":30,\"knodes\":2,\"pvs\":[{\"mate\":-3,\"moves\":\"e7e5\"}]}", &random, &r) == 1);
    CHECK(r.info.is_mate && r.info.mate == 3 && r.info.score == 40000);
    CHECK(response(&cfg, PROVIDER_CLOUD, &white, "{\"depth\":30,\"knodes\":2,\"pvs\":[{\"mate\":-1,\"moves\":\"e2e4\"}]}", &random, &r) == 1);
    CHECK(r.info.is_mate && r.info.mate == -1 && r.info.score == -40000);
    INTEGER(&cfg, "engine.online_moves.lichess_cloud_analysis.min_knodes", 2);
    CHECK(response(&cfg, PROVIDER_CLOUD, &black, data, &random, &r) == 0);
    INTEGER(&cfg, "engine.online_moves.lichess_cloud_analysis.min_knodes", 0);
    static const char *const invalid[] = {
        "{}", "[]", "null", "{\"error\":\"No cloud evaluation available\"}",
        "{\"depth\":19,\"knodes\":1,\"pvs\":[{\"cp\":0,\"moves\":\"e2e4\"}]}",
        "{\"depth\":20,\"knodes\":9223372036854775807,\"pvs\":[{\"cp\":0,\"moves\":\"e2e4\"}]}",
        "{\"depth\":20,\"knodes\":-1,\"pvs\":[{\"cp\":0,\"moves\":\"e2e4\"}]}",
        "{\"depth\":20,\"knodes\":1,\"pvs\":[{\"cp\":9223372036854775807,\"moves\":\"e2e4\"}]}",
        "{\"depth\":20,\"knodes\":1,\"pvs\":[{\"cp\":0,\"moves\":\"e7e5\"}]}",
        "{\"depth\":20,\"knodes\":1,\"pvs\":[{\"cp\":0,\"moves\":\"e2e5\"}]}",
        "{\"depth\":20,\"knodes\":1,\"pvs\":[{\"cp\":0,\"moves\":\"0000\"}]}",
        "{\"depth\":20,\"knodes\":1,\"pvs\":[{\"cp\":0,\"moves\":\"\"}]}",
        "{\"depth\":20,\"knodes\":1,\"pvs\":[]}",
        "{\"depth\":20,\"knodes\":1,\"pvs\":{\"moves\":\"e2e4\"}}"
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i) CHECK(response(&cfg, PROVIDER_CLOUD, &white, invalid[i], &random, &r) == 0);
    config_free(&cfg);
}
static void test_explorer_responses(void) {
    Config cfg = configuration(); Position p = position(VAR_STANDARD, "startpos", ""); ProviderResult r; uint64_t random = 123;
    const char *data = "{\"moves\":[{\"uci\":\"e2e4\",\"white\":50,\"black\":40,\"draws\":10},{\"uci\":\"d2d4\",\"white\":15,\"black\":0,\"draws\":5},{\"uci\":\"g1f3\",\"white\":9,\"black\":0,\"draws\":0},{\"uci\":\"e2e5\",\"white\":1000,\"black\":0,\"draws\":0}]}";
    CHECK(response(&cfg, PROVIDER_EXPLORER, &p, data, &random, &r) == 1); STR(r.move, "d2d4");
    CHECK(strstr(r.info.source, "Lichess Opening Explorer") && !r.info.has_score);
    provider_string(&cfg, PROVIDER_EXPLORER, "sort", "games_played");
    CHECK(response(&cfg, PROVIDER_EXPLORER, &p, data, &random, &r) == 1); STR(r.move, "e2e4");
    provider_string(&cfg, PROVIDER_EXPLORER, "sort", "winrate"); INTEGER(&cfg, "engine.online_moves.lichess_opening_explorer.min_games", 1);
    CHECK(response(&cfg, PROVIDER_EXPLORER, &p, data, &random, &r) == 1); STR(r.move, "g1f3");
    INTEGER(&cfg, "engine.online_moves.lichess_opening_explorer.min_games", 10);
    data = "{\"moves\":[{\"uci\":\"e2e4\",\"white\":5,\"black\":5,\"draws\":0},{\"uci\":\"d2d4\",\"white\":10,\"black\":10,\"draws\":0},{\"uci\":\"g1f3\",\"white\":5,\"black\":5,\"draws\":10}]}";
    CHECK(response(&cfg, PROVIDER_EXPLORER, &p, data, &random, &r) == 1); STR(r.move, "g1f3");
    CHECK(position_apply_uci(&p, "e2e4", NULL, 0));
    data = "{\"moves\":[{\"uci\":\"e7e5\",\"white\":90,\"black\":10,\"draws\":0},{\"uci\":\"c7c5\",\"white\":10,\"black\":80,\"draws\":10}]}";
    CHECK(response(&cfg, PROVIDER_EXPLORER, &p, data, &random, &r) == 1); STR(r.move, "c7c5");
    static const char *const invalid[] = {
        "{}", "{\"moves\":[]}", "{\"moves\":[{\"uci\":\"e7e5\",\"white\":0,\"black\":0,\"draws\":0}]}",
        "{\"moves\":[{\"uci\":\"e7e5\",\"white\":-1,\"black\":20,\"draws\":0}]}",
        "{\"moves\":[{\"uci\":\"e7e5\",\"white\":9223372036854775807,\"black\":20,\"draws\":0}]}",
        "{\"moves\":[{\"uci\":\"e7e4\",\"white\":0,\"black\":20,\"draws\":0}]}",
        "{\"moves\":[{\"uci\":\"e7e5\",\"white\":0,\"black\":20}]}",
        "{\"moves\":[{\"uci\":\"e7e5\",\"white\":0,\"black\":9,\"draws\":0}]}"
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i) CHECK(response(&cfg, PROVIDER_EXPLORER, &p, invalid[i], &random, &r) == 0);
    config_free(&cfg);
}
static void test_egtb_responses(void) {
    Config cfg = configuration(); Position p = position(VAR_STANDARD, "7k/8/8/8/8/8/8/KR6 w - - 0 1", ""); ProviderResult r; uint64_t random = 123;
    const char *data = "{\"moves\":[{\"uci\":\"b1b8\",\"category\":\"loss\",\"dtz\":-11,\"dtm\":-17},{\"uci\":\"b1b2\",\"category\":\"loss\",\"dtz\":-13},{\"uci\":\"a1a2\",\"category\":\"draw\"}]}";
    CHECK(response(&cfg, PROVIDER_EGTB, &p, data, &random, &r) == 1);
    STR(r.move, "b1b8"); CHECK(!r.root_count && r.info.has_tb && r.info.tb_wdl == 2 && r.info.score == 9900);
    CHECK(r.info.dtz == 11 && r.info.dtm == 17); STR(r.info.source, "Lichess EGTB");
    provider_string(&cfg, PROVIDER_EGTB, "move_quality", "suggest");
    CHECK(response(&cfg, PROVIDER_EGTB, &p, data, &random, &r) == 1);
    CHECK(!*r.move && r.root_count == 2); STR(r.roots[0], "b1b8"); STR(r.roots[1], "b1b2");
    CHECK(response(&cfg, PROVIDER_EGTB, &p, "{\"moves\":[{\"uci\":\"b1b9\",\"category\":\"loss\"},{\"uci\":\"a1a2\",\"category\":\"draw\"},{\"uci\":\"a1a3\",\"category\":\"draw\"}]}", &random, &r) == 1);
    STR(r.move, "a1a2"); CHECK(!r.root_count && r.info.tb_wdl == 0);
    provider_string(&cfg, PROVIDER_EGTB, "move_quality", "best");
    static const struct { const char *category; int wdl, score; } categories[] = {
        {"loss",2,9900},{"blessed-loss",1,500},{"maybe-loss",1,500},{"draw",0,0},
        {"cursed-win",-1,-500},{"maybe-win",-1,-500},{"win",-2,-9900}
    };
    char text[512];
    for (size_t i = 0; i < sizeof(categories)/sizeof(*categories); ++i) {
        CHECK(snprintf(text, sizeof(text), "{\"moves\":[{\"uci\":\"b1b2\",\"category\":\"%s\"}]}", categories[i].category) > 0);
        CHECK(response(&cfg, PROVIDER_EGTB, &p, text, &random, &r) == 1);
        CHECK(r.info.tb_wdl == categories[i].wdl && r.info.score == categories[i].score);
    }
    CHECK(response(&cfg, PROVIDER_EGTB, &p, "{\"moves\":[{\"uci\":\"b1b2\",\"category\":\"loss\",\"dtz\":-9223372036854775808,\"dtm\":9223372036854775807}]}", &random, &r) == 1);
    CHECK(r.info.dtz == 0 && r.info.dtm == 0);
    CHECK(response(&cfg, PROVIDER_EGTB, &p, "{\"moves\":[{\"uci\":\"b1b2\",\"category\":\"unknown\"}]}", &random, &r) == 0);
    CHECK(response(&cfg, PROVIDER_EGTB, &p, "{\"moves\":[{\"uci\":\"b1c2\",\"category\":\"loss\"}]}", &random, &r) == 0);
    CHECK(response(&cfg, PROVIDER_EGTB, &p, "{\"moves\":[]}", &random, &r) == 0);
    Position eight = position(VAR_STANDARD, "4k2r/3p4/8/8/3P4/8/4P3/RN2K3 w - - 0 1", "");
    CHECK(response(&cfg, PROVIDER_EGTB, &eight, "{\"moves\":[{\"uci\":\"b1c3\",\"category\":\"draw\",\"dtz\":-1,\"dtc\":-7}]}", &random, &r) == 1);
    CHECK(r.info.dtz == 7);
    provider_string(&cfg, PROVIDER_EGTB, "source", "chessdb");
    static const struct { int64_t score; int wdl; } scores[] = {
        {-30001,-2},{-20001,-2},{-20000,-1},{-1,-1},{0,0},{1,1},{20000,1},{20001,2},{30001,2}
    };
    for (size_t i = 0; i < sizeof(scores)/sizeof(*scores); ++i) {
        CHECK(snprintf(text, sizeof(text), "{\"status\":\"ok\",\"score\":%" PRId64 ",\"pv\":[\"b1b2\"]}", scores[i].score) > 0);
        CHECK(response(&cfg, PROVIDER_EGTB, &p, text, &random, &r) == 1); CHECK(r.info.tb_wdl == scores[i].wdl);
    }
    STR(r.info.source, "ChessDB EGTB");
    CHECK(response(&cfg, PROVIDER_EGTB, &p, "{\"status\":\"unknown\",\"score\":30000,\"pv\":[\"b1b2\"]}", &random, &r) == 0);
    CHECK(response(&cfg, PROVIDER_EGTB, &p, "{\"status\":\"ok\",\"score\":30000,\"pv\":[\"b1c2\"]}", &random, &r) == 0);
    provider_string(&cfg, PROVIDER_EGTB, "move_quality", "suggest");
    data = "{\"status\":\"ok\",\"moves\":[{\"uci\":\"b1b8\",\"score\":29995},{\"uci\":\"b1b2\",\"score\":29990},{\"uci\":\"a1a2\",\"score\":0}]}";
    CHECK(response(&cfg, PROVIDER_EGTB, &p, data, &random, &r) == 1);
    CHECK(r.root_count == 2 && !*r.move && r.info.tb_wdl == 2);
    p = position(VAR_CHESS960, "4k3/8/8/8/8/8/8/R1K4R w AH - 0 1", "");
    provider_string(&cfg, PROVIDER_CHESSDB, "move_quality", "good");
    CHECK(response(&cfg, PROVIDER_CHESSDB, &p, "{\"status\":\"ok\",\"move\":\"c1g1\"}", &random, &r) == 1); STR(r.move, "c1h1");
    config_free(&cfg);
}
static void evaluate(MoveInfo *info, const char *text) { evaluation_parse(info, text, strlen(text)); }
static void test_evaluation_parse(void) {
    MoveInfo info = {0}; char text[512];
    evaluate(&info, "id name Test Engine"); CHECK(!info.has_score && !info.depth);
    evaluate(&info, "info depth 21 seldepth 32 multipv 1 score cp 58 nodes 123456 nps 654321 time 212 tbhits 5 wdl 300 400 300 pv e2e4 e7e5  \r\n");
    CHECK(info.has_score && !info.is_mate && !info.bound && info.score == 58 && !info.mate);
    CHECK(info.depth == 21 && info.seldepth == 32 && info.multipv == 1);
    CHECK(info.nodes == 123456 && info.nps == 654321 && info.time_ms == 212 && info.tbhits == 5);
    CHECK(info.has_wdl && info.wdl[0] == 300 && info.wdl[1] == 400 && info.wdl[2] == 300);
    STR(info.pv, "e2e4 e7e5");
    MoveInfo saved = info;
    evaluate(&info, "info depth 99 score cp -9000 nodes 99999999 multipv 2 pv d2d4 d7d5");
    CHECK(!memcmp(&info, &saved, sizeof(info)));
    evaluate(&info, "info multipv 3 depth 99 score mate -1 wdl 0 0 1000 pv g1f3");
    CHECK(!memcmp(&info, &saved, sizeof(info)));
    evaluate(&info, "info string score cp -10000 pv illegal data");
    CHECK(!memcmp(&info, &saved, sizeof(info)));
    evaluate(&info, "info depth 22 nodes 777");
    CHECK(info.depth == 22 && info.nodes == 777 && info.score == 58 && !strcmp(info.pv, "e2e4 e7e5"));
    evaluate(&info, "info score mate 0"); CHECK(info.has_score && info.is_mate && info.mate == 0 && info.score == -40000);
    CHECK(evaluation_string(&info, text, sizeof(text), 1) == 0 && strstr(text, "Evaluation: #0"));
    evaluate(&info, "info score mate 3"); CHECK(info.is_mate && info.mate == 3 && info.score == 40000);
    saved = info; evaluate(&info, "info score nonsense 3"); CHECK(!memcmp(&info, &saved, sizeof(info)));
    evaluate(&info, "info score cp invalid"); CHECK(!memcmp(&info, &saved, sizeof(info)));
    evaluate(&info, "info score cp 9223372036854775807"); CHECK(!memcmp(&info, &saved, sizeof(info)));
    evaluate(&info, "info score mate -4 lowerbound"); CHECK(info.is_mate && info.mate == -4 && info.score == -40000 && info.bound);
    evaluate(&info, "info depth 24"); CHECK(info.bound && info.score == -40000);
    evaluate(&info, "info score cp -42 upperbound"); CHECK(!info.is_mate && !info.mate && info.score == -42 && info.bound);
    evaluate(&info, "info score cp 0"); CHECK(!info.bound && info.score == 0);
    evaluate(&info, "info wdl 500 -1 501"); CHECK(!info.has_wdl);
    evaluate(&info, "info nodes 9223372036854775808 nps -1 time invalid depth 99999999999999");
    CHECK(info.nodes == 777 && info.nps == 654321 && info.time_ms == 212 && info.depth == 24);
    strcpy(info.source, "Polyglot");
    CHECK(evaluation_string(&info, text, sizeof(text), 0) == 0 && strstr(text, "score=+0.00") && strstr(text, "source=Polyglot"));
    CHECK(evaluation_string(&info, text, sizeof(text), 1) == 0 && strstr(text, "time 0.21s") && strstr(text, "Polyglot"));
    CHECK(evaluation_string(&info, text, 8, 1) == -1);
    memset(&info, 0, sizeof(info)); CHECK(evaluation_string(&info, text, sizeof(text), 1) == 0 && strstr(text, "Evaluation: ?"));
}
static void test_evaluation_decisions(void) {
    Config cfg = configuration(); Annotations history = {0}; MoveInfo info = {0};
    evaluate(&info, "info score cp -1000");
    CHECK(evaluation_decision(cfg.settings, &history, &info, 8) == 0);
    BOOLEAN(&cfg, "engine.draw_or_resign.resign_enabled", 1);
    INTEGER(&cfg, "engine.draw_or_resign.resign_moves", 3); INTEGER(&cfg, "engine.draw_or_resign.resign_score", -1000);
    CHECK(evaluation_decision(cfg.settings, &history, &info, 8) == 0);
    CHECK(annotations_put(&history, 0, &info) == 0);
    CHECK(evaluation_decision(cfg.settings, &history, &info, 8) == 0);
    CHECK(annotations_put(&history, 2, &info) == 0);
    CHECK(evaluation_decision(cfg.settings, &history, &info, 8) == 2);
    evaluate(&info, "info score cp -999"); CHECK(evaluation_decision(cfg.settings, &history, &info, 8) == 0);
    evaluate(&info, "info score cp -1001 lowerbound"); CHECK(evaluation_decision(cfg.settings, &history, &info, 8) == 0);
    evaluate(&info, "info score mate 0"); CHECK(evaluation_decision(cfg.settings, &history, &info, 8) == 2);
    evaluate(&info, "info score mate 2"); CHECK(evaluation_decision(cfg.settings, &history, &info, 8) == 0);
    evaluate(&info, "info score cp -999"); CHECK(annotations_put(&history, 2, &info) == 0 && history.count == 2);
    evaluate(&info, "info score cp -2000"); CHECK(evaluation_decision(cfg.settings, &history, &info, 8) == 0);
    CHECK(annotations_put(&history, 4, &info) == 0); CHECK(evaluation_decision(cfg.settings, &history, &info, 8) == 0);
    CHECK(annotations_put(&history, 6, &info) == 0); CHECK(evaluation_decision(cfg.settings, &history, &info, 8) == 2);
    /* Rolling back removes only annotations at/after the new position. */
    annotations_truncate(&history, 4); CHECK(history.count == 2 && history.items[1].ply == 2);
    CHECK(evaluation_decision(cfg.settings, &history, &info, 8) == 0);
    annotations_truncate(&history, 2); CHECK(history.count == 1 && history.items[0].ply == 0);
    CHECK(evaluation_decision(cfg.settings, &history, &info, 8) == 0);
    annotations_truncate(&history, 0); CHECK(!history.count);
    BOOLEAN(&cfg, "engine.draw_or_resign.offer_draw_enabled", 1);
    INTEGER(&cfg, "engine.draw_or_resign.offer_draw_moves", 3); INTEGER(&cfg, "engine.draw_or_resign.offer_draw_score", 20);
    INTEGER(&cfg, "engine.draw_or_resign.offer_draw_pieces", 10);
    evaluate(&info, "info score cp 20"); CHECK(annotations_put(&history, 10, &info) == 0);
    evaluate(&info, "info score cp -20"); CHECK(annotations_put(&history, 12, &info) == 0);
    evaluate(&info, "info score cp 0"); CHECK(evaluation_decision(cfg.settings, &history, &info, 10) == 1);
    CHECK(evaluation_decision(cfg.settings, &history, &info, 11) == 0);
    evaluate(&info, "info score cp 21"); CHECK(evaluation_decision(cfg.settings, &history, &info, 10) == 0);
    evaluate(&info, "info score cp -21"); CHECK(evaluation_decision(cfg.settings, &history, &info, 10) == 0);
    evaluate(&info, "info score cp 0 upperbound"); CHECK(evaluation_decision(cfg.settings, &history, &info, 10) == 0);
    evaluate(&info, "info score cp 0"); info.has_score = 0; CHECK(evaluation_decision(cfg.settings, &history, &info, 10) == 0);
    info.has_score = 1; history.items[1].info.has_score = 0;
    CHECK(evaluation_decision(cfg.settings, &history, &info, 10) == 0);
    history.items[1].info.has_score = 1; history.items[1].info.bound = 1;
    CHECK(evaluation_decision(cfg.settings, &history, &info, 10) == 0);
    annotations_truncate(&history, 0); info.has_tb = 1; info.tb_wdl = -2; info.has_score = 0;
    CHECK(evaluation_decision(cfg.settings, &history, &info, 32) == 2);
    BOOLEAN(&cfg, "engine.draw_or_resign.resign_for_egtb_minus_two", 0);
    CHECK(evaluation_decision(cfg.settings, &history, &info, 32) == 0);
    info.tb_wdl = 0; CHECK(evaluation_decision(cfg.settings, &history, &info, 32) == 1);
    BOOLEAN(&cfg, "engine.draw_or_resign.offer_draw_for_egtb_zero", 0);
    CHECK(evaluation_decision(cfg.settings, &history, &info, 32) == 0);
    info.tb_wdl = -1; CHECK(evaluation_decision(cfg.settings, &history, &info, 32) == 0);
    annotations_free(&history); CHECK(!history.items && !history.count && !history.capacity);
    config_free(&cfg);
}
static PolicyRateLimit rate_limit_response(const char *text, long status, int64_t retry_after, unsigned *step) {
    JsonTok tokens[128]; Json j = parsed(text, tokens, 128);
    return policy_challenge_rate_limit(&j, status, retry_after, step);
}
static void test_challenge_rate_limits(void) {
    unsigned step = 0; PolicyRateLimit limit;
    static const int64_t expected[] = {60,120,240,480,600,600,600};
    for (size_t i = 0; i < sizeof(expected)/sizeof(*expected); ++i) {
        limit = policy_challenge_rate_limit(NULL, 429, -1, &step);
        CHECK(limit.kind == POLICY_RATE_LIMIT_BOT && !limit.daily);
        CHECK(limit.delay_us == expected[i] * INT64_C(1000000));
    }
    CHECK(step == 4);
    limit = rate_limit_response("{\"error\":\"too many requests\"}", 429, INT64_C(1250000), &step);
    CHECK(limit.kind == POLICY_RATE_LIMIT_BOT && limit.delay_us == 1250000 && !limit.daily && step == 4);
    limit = policy_challenge_rate_limit(NULL, 429, 0, &step);
    CHECK(limit.kind == POLICY_RATE_LIMIT_BOT && limit.delay_us == 0 && step == 4);
    limit = policy_challenge_rate_limit(NULL, 429, INT64_MAX, &step);
    CHECK(limit.delay_us == INT64_MAX && step == 4);
    limit = policy_challenge_rate_limit(NULL, 200, -1, &step);
    CHECK(limit.kind == POLICY_RATE_LIMIT_NONE && !limit.delay_us && !limit.daily && !step);
    limit = policy_challenge_rate_limit(NULL, 429, -1, &step);
    CHECK(limit.delay_us == INT64_C(60000000) && step == 1);
    limit = policy_challenge_rate_limit(NULL, 503, -1, &step);
    CHECK(limit.kind == POLICY_RATE_LIMIT_NONE && step == 0);
    limit = policy_challenge_rate_limit(NULL, 429, -1, NULL);
    CHECK(limit.kind == POLICY_RATE_LIMIT_BOT && limit.delay_us == INT64_C(60000000));
    step = UINT_MAX; limit = policy_challenge_rate_limit(NULL, 429, -1, &step);
    CHECK(limit.delay_us == INT64_C(600000000) && step == 4);
    static const struct { const char *seconds; int64_t microseconds; } durations[] = {
        {"90", INT64_C(90000000)}, {"2.5", INT64_C(2500000)},
        {"1.25e1", INT64_C(12500000)}, {"0.000001", 1}, {"0.0000001", 1},
        {"0", 0}, {"-0.0", 0}, {"9223372036854.775807", INT64_MAX},
        {"9223372036854775807", INT64_MAX}, {"1e9999", INT64_MAX}, {"1e-9999", 1}
    };
    char text[512];
    for (size_t i = 0; i < sizeof(durations)/sizeof(*durations); ++i) {
        CHECK(snprintf(text, sizeof(text), "{\"error\":\"daily game limit\",\"ratelimit\":{\"key\":\"bot.vsBot.day\",\"seconds\":%s}}", durations[i].seconds) > 0);
        step = 3;
        limit = rate_limit_response(text, 429, 12, &step);
        CHECK(limit.kind == POLICY_RATE_LIMIT_BOT && limit.daily && limit.delay_us == durations[i].microseconds && step == 3);
        limit = rate_limit_response(text, 400, -1, &step);
        CHECK(limit.kind == POLICY_RATE_LIMIT_OPPONENT && limit.daily && limit.delay_us == durations[i].microseconds && step == 0);
        limit = rate_limit_response(text, 500, -1, &step);
        CHECK(limit.kind == POLICY_RATE_LIMIT_NONE && !limit.delay_us && !limit.daily && step == 0);
    }
    static const char *const invalid[] = {
        "{}", "[]", "null", "{\"error\":\"unclassified\"}",
        "{\"ratelimit\":{\"key\":\"bot.vsBot.day\",\"seconds\":10}}",
        "{\"error\":\"limit\",\"ratelimit\":{\"key\":\"other.limit\",\"seconds\":10}}",
        "{\"error\":\"limit\",\"ratelimit\":{\"key\":\"bot.vsBot.day\",\"seconds\":-1}}",
        "{\"error\":\"limit\",\"ratelimit\":{\"key\":\"bot.vsBot.day\",\"seconds\":-1e-9999}}",
        "{\"error\":\"limit\",\"ratelimit\":{\"key\":\"bot.vsBot.day\",\"seconds\":\"2.5\"}}",
        "{\"error\":\"limit\",\"ratelimit\":{\"key\":\"bot.vsBot.day\",\"seconds\":null}}",
        "{\"error\":\"limit\",\"ratelimit\":{\"key\":\"bot.vsBot.day\",\"seconds\":true}}",
        "{\"error\":\"limit\",\"ratelimit\":{\"key\":\"bot.vsBot.day\",\"seconds\":{}}}",
        "{\"error\":\"limit\",\"ratelimit\":{\"key\":\"bot.vsBot.day\"}}"
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i) {
        step = 1; limit = rate_limit_response(invalid[i], 429, -1, &step);
        CHECK(limit.kind == POLICY_RATE_LIMIT_BOT && !limit.daily && limit.delay_us == INT64_C(120000000) && step == 2);
        limit = rate_limit_response(invalid[i], 400, -1, &step);
        CHECK(limit.kind == POLICY_RATE_LIMIT_NONE && !limit.daily && !limit.delay_us && !step);
    }
}
int main(void) {
    test_challenge_parse(); test_challenge_policy(); test_queue(); test_block_lists();
    test_match_settings(); test_match_candidates(); test_decline_file();
    test_provider_queries(); test_chessdb_responses(); test_cloud_responses();
    test_explorer_responses(); test_egtb_responses();
    test_evaluation_parse(); test_evaluation_decisions(); test_challenge_rate_limits();
    printf("policy/provider/evaluation features: %u checks passed\n", checks);
    return 0;
}
