#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include <curl/curl.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Defaults from lichess-bot f2b38b4, excluding working_dir. */
static const char *const default_yaml_chunks[] = {
    "fake_think_time: false\n"
    "abort_time: 20\n"
    "move_overhead: 1000\n"
    "quit_after_all_games_finish: false\n"
    "rate_limiting_delay: 0\n"
    "pgn_directory: null\n"
    "pgn_file_grouping: game\n"
    "max_takebacks_accepted: 0\n",
    "engine:\n"
    "  dir: .\n"
    "  name: ''\n"
    "  protocol: uci\n"
    "  debug: false\n"
    "  interpreter: null\n"
    "  interpreter_options: []\n"
    "  working_dir: ''\n"
    "  silence_stderr: false\n"
    "  draw_or_resign:\n"
    "    offer_draw_enabled: false\n"
    "    offer_draw_for_egtb_zero: true\n"
    "    resign_enabled: false\n"
    "    resign_for_egtb_minus_two: true\n"
    "    resign_moves: 3\n"
    "    resign_score: -1000\n"
    "    offer_draw_moves: 5\n"
    "    offer_draw_score: 0\n"
    "    offer_draw_pieces: 10\n"
    "  online_moves:\n"
    "    max_out_of_book_moves: 10\n"
    "    max_retries: 2\n"
    "    max_depth: .inf\n"
    "    online_egtb:\n"
    "      enabled: false\n"
    "      source: lichess\n"
    "      min_time: 20\n"
    "      max_time: 10800\n"
    "      max_pieces: 7\n"
    "      move_quality: best\n"
    "    chessdb_book:\n"
    "      enabled: false\n"
    "      min_time: 20\n"
    "      max_time: 10800\n"
    "      move_quality: good\n"
    "      min_depth: 20\n"
    "    lichess_cloud_analysis:\n"
    "      enabled: false\n"
    "      min_time: 20\n"
    "      max_time: 10800\n"
    "      move_quality: best\n"
    "      min_depth: 20\n"
    "      min_knodes: 0\n"
    "      max_score_difference: 50\n"
    "    lichess_opening_explorer:\n"
    "      enabled: false\n"
    "      min_time: 20\n"
    "      max_time: 10800\n"
    "      source: masters\n"
    "      player_name: ''\n"
    "      sort: winrate\n"
    "      min_games: 10\n"
    "  lichess_bot_tbs:\n"
    "    syzygy:\n"
    "      enabled: false\n"
    "      max_pieces: 7\n"
    "      move_quality: best\n"
    "    gaviota:\n"
    "      enabled: false\n"
    "      max_pieces: 5\n"
    "      move_quality: best\n"
    "      min_dtm_to_consider_as_wdl_1: 120\n"
    "  polyglot:\n"
    "    enabled: false\n"
    "    max_depth: 8\n"
    "    selection: weighted_random\n"
    "    min_weight: 1\n"
    "    normalization: none\n"
    "  ponder: false\n"
    "  uci_ponder: false\n",
    "challenge:\n"
    "  variants:\n"
    "  - standard\n"
    "  time_controls:\n"
    "  - bullet\n"
    "  - blitz\n"
    "  - rapid\n"
    "  - classical\n"
    "  modes:\n"
    "  - casual\n"
    "  - rated\n"
    "  concurrency: 1\n"
    "  games_reserved_for_humans: 0\n"
    "  sort_by: best\n"
    "  preference: none\n"
    "  accept_bot: false\n"
    "  only_bot: false\n"
    "  max_increment: 180\n"
    "  min_increment: 0\n"
    "  max_base: .inf\n"
    "  min_base: 0\n"
    "  max_days: .inf\n"
    "  min_days: 1\n"
    "  block_list: []\n"
    "  online_block_list: []\n"
    "  allow_list: []\n"
    "  min_rating: 0\n"
    "  max_rating: 4000\n"
    "  rating_difference: null\n"
    "  max_simultaneous_games_per_user: 5\n"
    "  bullet_requires_increment: false\n"
    "  recent_bot_challenge_age: null\n"
    "  max_recent_bot_challenges: null\n",
    "correspondence:\n"
    "  checkin_period: 600\n"
    "  move_time: 60\n"
    "  disconnect_time: 300\n"
    "  ponder: false\n"
    "  uci_ponder: false\n",
    "matchmaking:\n"
    "  challenge_timeout: 30\n"
    "  block_list: []\n"
    "  online_block_list: []\n"
    "  include_challenge_block_list: false\n"
    "  challenge_decliner_file_name: false\n"
    "  challenge_filter: none\n"
    "  allow_matchmaking: false\n"
    "  challenge_initial_time:\n"
    "  - null\n"
    "  challenge_increment:\n"
    "  - null\n"
    "  challenge_days:\n"
    "  - null\n"
    "  opponent_min_rating: 600\n"
    "  opponent_max_rating: 4000\n"
    "  rating_preference: none\n"
    "  challenge_variant: random\n"
    "  challenge_mode: random\n"
    "  overrides: {}\n"
    "  allow_during_games: false\n"
    "  opponent_rating_difference: null\n",
    "greeting:\n"
    "  hello: ''\n"
    "  hello_spectators: ''\n"
    "  goodbye: ''\n"
    "  goodbye_spectators: ''\n",
    "c_bridge:\n"
    "  http_timeout_ms: 3000\n"
    "  engine_timeout_ms: 5000\n"
    "  trace: false\n"
    "  accept_challenges: true\n",
};

static int number(const char *arg, int64_t lo, int64_t hi, int64_t *out) {
    char *end;
    if (!arg || !*arg) return -1;
    for (const unsigned char *p = (const unsigned char *)arg; *p; ++p)
        if (*p < '0' || *p > '9') return -1;
    errno = 0;
    long long n = strtoll(arg, &end, 10);
    if (errno || *end || n < lo || n > hi) return -1;
    *out = (int64_t)n;
    return 0;
}

static int clean_line(const char *s) {
    for (; *s; ++s) if ((unsigned char)*s < 32 || (unsigned char)*s == 127) return 0;
    return 1;
}

static int validate_url(char *url) {
    /* Check the original URL before CURLU normalizes paths and empty queries. */
    if (!clean_line(url) || strchr(url, ' ') || strchr(url, '?') ||
        strchr(url, '#') || strchr(url, '@')) return -1;
    const char *authority = strstr(url, "://");
    if (!authority || !authority[3]) return -1;
    const char *raw_path = strchr(authority + 3, '/');
    if (raw_path && strcmp(raw_path, "/")) return -1;
    CURLU *u = curl_url();
    char *scheme = NULL, *host = NULL, *path = NULL, *extra = NULL;
    int valid = 0;
    if (!u || curl_url_set(u, CURLUPART_URL, url, CURLU_DISALLOW_USER) != CURLUE_OK) goto done;
    if (curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) != CURLUE_OK ||
        curl_url_get(u, CURLUPART_HOST, &host, 0) != CURLUE_OK) goto done;
    if (strcmp(scheme, "https") &&
        (strcmp(scheme, "http") ||
         (strcmp(host, "127.0.0.1") && strcmp(host, "[::1]")))) goto done;
    if (curl_url_get(u, CURLUPART_PATH, &path, 0) == CURLUE_OK && strcmp(path, "/")) goto done;
    const CURLUPart forbidden[] = {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_QUERY, CURLUPART_FRAGMENT};
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        if (curl_url_get(u, forbidden[i], &extra, 0) == CURLUE_OK) goto done;
    }
    valid = 1;
    size_t n = strlen(url);
    if (n && url[n - 1] == '/') url[n - 1] = 0;
done:
    curl_free(scheme); curl_free(host); curl_free(path); curl_free(extra);
    if (u) curl_url_cleanup(u);
    return valid ? 0 : -1;
}

void config_help(FILE *out) {
    fputs(
        "lichess-bot-c " LBC_VERSION " -- asynchronous Lichess engine bridge\n"
        "Usage: lichess-bot-c --config config.yml [overrides]\n"
        "   or: lichess-bot-c --engine PATH [options]\n\n"
        "Loads upstream lichess-bot YAML, retaining every section and engine option.\n"
        "Without --engine or --config, loads ./config.yml when present.\n"
        "Precedence: CLI > YAML; token-file > LICHESS_BOT_TOKEN > YAML token.\n\n"
        "  -c, --config PATH      Upstream-compatible YAML configuration\n"
        "  -e, --engine PATH      Override engine executable (resolved using PATH)\n"
        "  --protocol NAME        uci, xboard or homemade\n"
        "  --engine-arg ARG       Append one engine argument; repeatable, no shell\n"
        "  --engine-dir DIR       Engine working directory\n"
        "  --option NAME=VALUE    Override a protocol option; repeatable\n"
        "  --concurrency N        Concurrent games (1..16; YAML also permits 0)\n"
        "  --variants LIST        Comma-separated Lichess variant keys\n"
        "                         standard,fromPosition,chess960,antichess,atomic,\n"
        "                         crazyhouse,horde,kingOfTheHill,racingKings,threeCheck\n"
        "  --ponder               Think during opponent's turn\n"
        "  --no-ponder            Disable pondering from YAML\n"
        "  --overhead-ms N        Reserve own clock time; not a sleep\n"
        "  --movetime-ms N        Optional per-search time cap\n"
        "  --depth N              Optional search depth limit\n"
        "  --nodes N              Optional search node limit\n"
        "  --no-accept            Decline incoming challenges\n"
        "  --only-bots            Accept BOT challengers only\n"
        "  --casual-only          Accept unrated challenges only\n"
        "  --min-initial N        Minimum initial clock in seconds\n"
        "  --max-initial N        Maximum initial clock in seconds\n"
        "  --min-increment N      Minimum clock increment in seconds\n"
        "  --max-increment N      Maximum clock increment in seconds\n"
        "  --http-timeout-ms N    Finite request timeout (default 3000)\n"
        "  --engine-timeout-ms N  Engine handshake timeout (default 5000)\n"
        "  --base-url URL         HTTPS origin; HTTP permitted only on loopback\n"
        "  --token-file PATH      Token file; single trailing LF/CRLF permitted\n"
        "  -u, --upgrade          Request account upgrade to BOT\n"
        "  -v, --verbose          Detailed diagnostic logging\n"
        "  -l, --logfile PATH     Write diagnostic log to a file\n"
        "  --disable-auto-logging Disable automatic log files\n"
        "                         --disable_auto_logging is also accepted\n"
        "  --trace                Log per-move local timings\n"
        "  -h, --help             Show this help\n"
        "  --version              Show program and libcurl versions\n", out);
}

int config_variant(const char *name) {
    if (!name) return 0;
    static const struct { const char *name; int bit; } variants[] = {
        {"standard", VAR_STANDARD}, {"normal", VAR_STANDARD}, {"chess", VAR_STANDARD},
        {"fromPosition", VAR_FROM_POSITION}, {"fromposition", VAR_FROM_POSITION},
        {"chess960", VAR_CHESS960}, {"fischerandom", VAR_CHESS960},
        {"antichess", VAR_ANTICHESS}, {"giveaway", VAR_ANTICHESS}, {"suicide", VAR_ANTICHESS},
        {"atomic", VAR_ATOMIC}, {"crazyhouse", VAR_CRAZYHOUSE}, {"horde", VAR_HORDE},
        {"kingOfTheHill", VAR_KING_OF_THE_HILL}, {"kingofthehill", VAR_KING_OF_THE_HILL},
        {"racingKings", VAR_RACING_KINGS}, {"racingkings", VAR_RACING_KINGS},
        {"threeCheck", VAR_THREE_CHECK}, {"3check", VAR_THREE_CHECK}
    };
    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i)
        if (!strcmp(name, variants[i].name)) return variants[i].bit;
    return 0;
}

const char *config_variant_name(int variant) {
    switch (variant) {
    case VAR_STANDARD: return "standard";
    case VAR_FROM_POSITION: return "fromPosition";
    case VAR_CHESS960: return "chess960";
    case VAR_ANTICHESS: return "antichess";
    case VAR_ATOMIC: return "atomic";
    case VAR_CRAZYHOUSE: return "crazyhouse";
    case VAR_HORDE: return "horde";
    case VAR_KING_OF_THE_HILL: return "kingOfTheHill";
    case VAR_RACING_KINGS: return "racingKings";
    case VAR_THREE_CHECK: return "threeCheck";
    default: return NULL;
    }
}

const char *config_engine_variant_name(int variant) {
    switch (variant) {
    case VAR_STANDARD: case VAR_FROM_POSITION: return "chess";
    case VAR_CHESS960: return "chess";
    case VAR_ANTICHESS: return "antichess";
    case VAR_ATOMIC: return "atomic";
    case VAR_CRAZYHOUSE: return "crazyhouse";
    case VAR_HORDE: return "horde";
    case VAR_KING_OF_THE_HILL: return "kingofthehill";
    case VAR_RACING_KINGS: return "racingkings";
    case VAR_THREE_CHECK: return "3check";
    default: return NULL;
    }
}

void config_free(Config *c) {
    if (!c) return;
    settings_free(c->settings);
    for (size_t i = 0; i < c->owned_count; ++i) free(c->owned_strings[i]);
    memset(c, 0, sizeof(*c));
}

static char *config_string(Config *c, const char *format, ...) {
    if (c->owned_count >= MAX_CONFIG_STRINGS) return NULL;
    va_list args, copy;
    va_start(args, format); va_copy(copy, args);
    int n = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (n < 0 || n > 65535) { va_end(args); return NULL; }
    char *value = malloc((size_t)n + 1);
    if (value) {
        (void)vsnprintf(value, (size_t)n + 1, format, args);
        c->owned_strings[c->owned_count++] = value;
    }
    va_end(args);
    return value;
}

static int empty_value(const Setting *v) {
    return !v || setting_type(v) == SETTING_NULL ||
           (setting_type(v) == SETTING_STRING && !*setting_str(v, ""));
}

static int config_defaults(Settings *s, int cli_only) {
    char error[256];
    Settings *defaults = settings_load_string("{}", error, sizeof(error));
    if (!defaults) return -1;
    for (size_t i = 0; i < sizeof(default_yaml_chunks) / sizeof(default_yaml_chunks[0]); ++i) {
        Settings *chunk = settings_load_string(default_yaml_chunks[i], error, sizeof(error));
        int failed = !chunk || settings_apply_defaults(defaults, chunk);
        settings_free(chunk);
        if (failed) { settings_free(defaults); return -1; }
    }
    static const char *const force_empty[] = {
        "pgn_file_grouping", "max_takebacks_accepted", "engine.interpreter_options", "engine.working_dir",
        "engine.online_moves.max_retries", "engine.online_moves.max_depth", "challenge.block_list",
        "challenge.online_block_list", "challenge.allow_list", "challenge.min_rating", "challenge.max_rating",
        "correspondence.move_time", "matchmaking.challenge_timeout", "matchmaking.block_list",
        "matchmaking.online_block_list", "matchmaking.include_challenge_block_list",
        "matchmaking.challenge_decliner_file_name", "matchmaking.challenge_filter",
        "matchmaking.challenge_initial_time", "matchmaking.challenge_increment", "matchmaking.challenge_days",
        "matchmaking.opponent_min_rating", "matchmaking.opponent_max_rating", "matchmaking.overrides",
        "greeting.hello", "greeting.goodbye", "greeting.hello_spectators", "greeting.goodbye_spectators"
    };
    if (empty_value(settings_get(s, "matchmaking.challenge_filter")) &&
        !empty_value(settings_get(s, "matchmaking.delay_after_decline")) &&
        settings_set(s, "matchmaking.challenge_filter", settings_get(s, "matchmaking.delay_after_decline"))) goto fail;
    for (size_t i = 0; i < sizeof(force_empty) / sizeof(force_empty[0]); ++i)
        if (empty_value(settings_get(s, force_empty[i])) &&
            settings_set(s, force_empty[i], settings_get(defaults, force_empty[i]))) goto fail;
    if (settings_apply_defaults(s, defaults)) goto fail;
    if (settings_listify(s, "engine.interpreter_options", 1)) goto fail;
    /* Normalize book paths to ordered lists. */
    static const char *const book_maps[] = {"engine.polyglot.book", "engine.polyglot.books"};
    for (size_t i = 0; i < sizeof(book_maps) / sizeof(book_maps[0]); ++i) {
        const Setting *books = settings_get(s, book_maps[i]);
        if (!books || setting_type(books) == SETTING_NULL) continue;
        if (setting_type(books) != SETTING_MAP) goto fail;
        for (size_t j = 0; j < setting_size(books); ++j) {
            const Setting *value = setting_at(books, j);
            if (setting_type(value) == SETTING_LIST) continue;
            if (settings_map_set_list(s, books, setting_key(books, j), &value,
                                      setting_type(value) == SETTING_NULL ? 0 : 1)) goto fail;
        }
    }
    const char *const list_keys[] = {"challenge_initial_time", "challenge_increment", "challenge_days"};
    for (size_t i = 0; i < sizeof(list_keys) / sizeof(list_keys[0]); ++i) {
        char path[128]; (void)snprintf(path, sizeof(path), "matchmaking.%s", list_keys[i]);
        if (settings_listify(s, path, 0)) goto fail;
    }
    const Setting *overrides = settings_get(s, "matchmaking.overrides");
    if (setting_type(overrides) != SETTING_MAP) goto fail;
    for (size_t i = 0; i < setting_size(overrides); ++i) {
        const Setting *entry = setting_at(overrides, i);
        if (setting_type(entry) != SETTING_MAP) goto fail;
        for (size_t j = 0; j < sizeof(list_keys) / sizeof(list_keys[0]); ++j) {
            const Setting *v = setting_get(entry, list_keys[j]);
            if (!v || setting_type(v) == SETTING_LIST) continue;
            if (empty_value(v)) v = NULL;
            if (settings_map_set_list(s, entry, list_keys[j], &v, 1)) goto fail;
        }
    }
    if (settings_double(s, "matchmaking.challenge_timeout", 30) < 1 &&
        settings_set_int(s, "matchmaking.challenge_timeout", 1)) goto fail;
    if (empty_value(settings_get(s, "engine.working_dir"))) {
        char cwd[8192];
        if (!getcwd(cwd, sizeof(cwd)) || settings_set_str(s, "engine.working_dir", cwd)) goto fail;
    }
    if (cli_only && (settings_set_int(s, "move_overhead", 100) ||
        settings_set_int(s, "challenge.max_base", 1800) || settings_set_int(s, "challenge.max_increment", 60) ||
        settings_set_bool(s, "challenge.accept_bot", 1))) goto fail;
    settings_free(defaults);
    return 0;
fail:
    settings_free(defaults);
    return -1;
}

static int list_strings(const Setting *list, const char *const *allowed, size_t count) {
    if (setting_type(list) != SETTING_LIST) return -1;
    for (size_t i = 0; i < setting_size(list); ++i) {
        const Setting *item = setting_at(list, i);
        /* Upstream filters null entries from allow_list and block lists. */
        if (!allowed && setting_type(item) == SETTING_NULL) continue;
        const char *text = setting_str(item, NULL);
        if (!text) return -1;
        if (allowed) {
            size_t j = 0;
            for (; j < count; ++j) if (!strcmp(text, allowed[j])) break;
            if (j == count) return -1;
        }
    }
    return 0;
}

static int choice(Settings *s, const char *path, const char *choices) {
    const char *value = settings_str(s, path, NULL);
    if (!value) return -1;
    size_t length = strlen(value);
    for (const char *part = choices; *part;) {
        const char *end = strchr(part, '|');
        size_t n = end ? (size_t)(end - part) : strlen(part);
        if (length == n && !strncmp(value, part, n)) return 0;
        if (!end) break;
        part = end + 1;
    }
    return -1;
}

static int numeric(Settings *s, const char *path, double lo, double hi, int allow_null, int integral) {
    const Setting *v = settings_get(s, path);
    if (allow_null && setting_type(v) == SETTING_NULL) return 0;
    double n = setting_double(v, NAN);
    if (isnan(n) || n < lo || n > hi) goto invalid;
    if (integral && isfinite(n) && setting_int(v, INT64_MIN) == INT64_MIN) goto invalid;
    return 0;
invalid:
    fprintf(stderr, "Invalid numeric configuration: %s\n", path);
    return -1;
}

static int validate_settings(Config *c) {
    Settings *s = c->settings;
    static const char *const removed_extensions[] = {
        "engine.legacy_dir", "engine.legacy_python", "engine.homemade_module",
        "extra_game_handlers.module", "extra_game_handlers.upstream_dir", "extra_game_handlers.python"
    };
    for (size_t i = 0; i < sizeof(removed_extensions) / sizeof(removed_extensions[0]); ++i) {
        if (settings_get(s, removed_extensions[i])) {
            fprintf(stderr, "%s is no longer supported; use a native C extension.\n", removed_extensions[i]);
            return -1;
        }
    }
    if (settings_get(s, "extra_game_handlers.protocol") && choice(s, "extra_game_handlers.protocol", "native")) {
        fputs("Custom game handlers require a native C extension.\n", stderr);
        return -1;
    }
    static const struct { const char *path, *values; } choices[] = {
        {"engine.protocol", "uci|xboard|homemade"}, {"challenge.sort_by", "best|first"},
        {"challenge.preference", "none|human|bot"}, {"pgn_file_grouping", "game|opponent|all"},
        {"matchmaking.rating_preference", "none|high|low"}, {"matchmaking.challenge_mode", "casual|rated|random"},
        {"matchmaking.challenge_filter", "none|coarse|fine"},
        {"engine.polyglot.selection", "weighted_random|uniform_random|best_move"},
        {"engine.polyglot.normalization", "none|sum|max"},
        {"engine.online_moves.chessdb_book.move_quality", "all|good|best"},
        {"engine.online_moves.lichess_cloud_analysis.move_quality", "good|best"},
        {"engine.online_moves.online_egtb.move_quality", "best|suggest"},
        {"engine.online_moves.online_egtb.source", "lichess|chessdb"},
        {"engine.lichess_bot_tbs.syzygy.move_quality", "best|suggest"},
        {"engine.lichess_bot_tbs.gaviota.move_quality", "best|suggest"},
        {"engine.online_moves.lichess_opening_explorer.source", "lichess|masters|player"},
        {"engine.online_moves.lichess_opening_explorer.sort", "winrate|games_played"}
    };
    for (size_t i = 0; i < sizeof(choices) / sizeof(choices[0]); ++i) if (choice(s, choices[i].path, choices[i].values)) {
        fprintf(stderr, "Invalid configuration choice: %s (expected %s)\n", choices[i].path, choices[i].values);
        return -1;
    }
    static const char *const booleans[] = {
        "fake_think_time", "quit_after_all_games_finish", "engine.ponder", "engine.uci_ponder", "engine.debug",
        "engine.silence_stderr", "engine.polyglot.enabled", "engine.draw_or_resign.offer_draw_enabled",
        "engine.draw_or_resign.offer_draw_for_egtb_zero", "engine.draw_or_resign.resign_enabled",
        "engine.draw_or_resign.resign_for_egtb_minus_two", "engine.online_moves.chessdb_book.enabled",
        "engine.online_moves.lichess_cloud_analysis.enabled", "engine.online_moves.lichess_opening_explorer.enabled",
        "engine.online_moves.online_egtb.enabled", "engine.lichess_bot_tbs.syzygy.enabled", "engine.lichess_bot_tbs.gaviota.enabled",
        "challenge.accept_bot", "challenge.only_bot", "challenge.bullet_requires_increment",
        "correspondence.ponder", "correspondence.uci_ponder", "matchmaking.allow_matchmaking",
        "matchmaking.allow_during_games", "matchmaking.include_challenge_block_list", "c_bridge.trace", "c_bridge.accept_challenges"
    };
    for (size_t i = 0; i < sizeof(booleans) / sizeof(booleans[0]); ++i)
        if (setting_type(settings_get(s, booleans[i])) != SETTING_BOOL) {
            fprintf(stderr, "Configuration must be a YAML boolean: %s\n", booleans[i]); return -1;
        }
    static const struct { const char *path; double lo, hi; int nullable, integral; } numbers[] = {
        {"challenge.concurrency", 0, MAX_GAMES, 0, 1}, {"challenge.games_reserved_for_humans", 0, MAX_GAMES, 0, 1},
        {"challenge.min_base", 0, INFINITY, 0, 0}, {"challenge.max_base", 0, INFINITY, 0, 0},
        {"challenge.min_increment", 0, INFINITY, 0, 0}, {"challenge.max_increment", 0, INFINITY, 0, 0},
        {"challenge.min_days", 0, INFINITY, 0, 0}, {"challenge.max_days", 0, INFINITY, 0, 0},
        {"challenge.min_rating", 0, 4000, 0, 1}, {"challenge.max_rating", 0, 4000, 0, 1},
        {"challenge.rating_difference", 0, 4000, 1, 1}, {"challenge.max_simultaneous_games_per_user", 0, 100000, 0, 1},
        {"challenge.recent_bot_challenge_age", 0, INT_MAX, 1, 0}, {"challenge.max_recent_bot_challenges", 0, INT_MAX, 1, 1},
        {"move_overhead", 0, 60000, 0, 1}, {"rate_limiting_delay", 0, INT_MAX, 0, 1},
        {"abort_time", 0, INT_MAX, 0, 0}, {"max_takebacks_accepted", 0, INT_MAX, 0, 1},
        {"correspondence.move_time", 0, INT_MAX, 0, 0}, {"correspondence.checkin_period", 0, INT_MAX, 0, 0},
        {"correspondence.disconnect_time", 0, INT_MAX, 0, 0},
        {"matchmaking.challenge_timeout", 1, INT_MAX, 0, 0},
        {"matchmaking.opponent_min_rating", 0, 4000, 0, 1}, {"matchmaking.opponent_max_rating", 0, 4000, 0, 1},
        {"matchmaking.opponent_rating_difference", 0, 4000, 1, 1},
        {"engine.draw_or_resign.resign_score", -1000000, 1000000, 0, 1},
        {"engine.draw_or_resign.resign_moves", 1, INT_MAX, 0, 1},
        {"engine.draw_or_resign.offer_draw_score", 0, 1000000, 0, 1},
        {"engine.draw_or_resign.offer_draw_moves", 1, INT_MAX, 0, 1},
        {"engine.draw_or_resign.offer_draw_pieces", 0, 128, 0, 1},
        {"engine.polyglot.max_depth", 0, INFINITY, 0, 1}, {"engine.polyglot.min_weight", 0, INFINITY, 0, 0},
        {"engine.online_moves.max_out_of_book_moves", 0, INT_MAX, 0, 1},
        {"engine.online_moves.max_retries", 0, 10000, 0, 1}, {"engine.online_moves.max_depth", 0, INFINITY, 0, 1},
        {"engine.online_moves.online_egtb.max_pieces", 2, 8, 0, 1},
        {"engine.lichess_bot_tbs.syzygy.max_pieces", 2, 7, 0, 1},
        {"engine.lichess_bot_tbs.gaviota.max_pieces", 2, 5, 0, 1},
        {"engine.lichess_bot_tbs.gaviota.min_dtm_to_consider_as_wdl_1", 0, INT_MAX, 0, 1},
        {"engine.online_moves.chessdb_book.min_depth", 0, INT_MAX, 0, 1},
        {"engine.online_moves.lichess_cloud_analysis.min_depth", 0, INT_MAX, 0, 1},
        {"engine.online_moves.lichess_cloud_analysis.min_knodes", 0, INT_MAX, 0, 0},
        {"engine.online_moves.lichess_cloud_analysis.max_score_difference", 0, INT_MAX, 0, 1},
        {"engine.online_moves.lichess_opening_explorer.min_games", 0, INT_MAX, 0, 1},
        {"c_bridge.http_timeout_ms", 100, 60000, 0, 1}, {"c_bridge.engine_timeout_ms", 100, 60000, 0, 1}
    };
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i)
        if (numeric(s, numbers[i].path, numbers[i].lo, numbers[i].hi, numbers[i].nullable, numbers[i].integral)) return -1;
    if (settings_int(s, "challenge.games_reserved_for_humans", 0) > settings_int(s, "challenge.concurrency", 1)) {
        fputs("challenge.games_reserved_for_humans exceeds concurrency.\n", stderr); return -1;
    }
    const char *const speeds[] = {"ultraBullet", "bullet", "blitz", "rapid", "classical", "correspondence"};
    const char *const modes[] = {"casual", "rated"};
    if (list_strings(settings_get(s, "challenge.time_controls"), speeds, sizeof(speeds) / sizeof(speeds[0])) ||
        list_strings(settings_get(s, "challenge.modes"), modes, sizeof(modes) / sizeof(modes[0]))) return -1;
    const char *const lists[] = {"challenge.allow_list", "challenge.block_list", "challenge.online_block_list",
                                "matchmaking.block_list", "matchmaking.online_block_list"};
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); ++i)
        if (list_strings(settings_get(s, lists[i]), NULL, 0)) {
            fprintf(stderr, "Configuration must be a list of user names or URLs: %s\n", lists[i]); return -1;
        }
    const char *variant = settings_str(s, "matchmaking.challenge_variant", NULL);
    if (!variant || (strcmp(variant, "random") && !config_variant(variant))) return -1;
    const char *const times[] = {"matchmaking.challenge_initial_time", "matchmaking.challenge_increment", "matchmaking.challenge_days"};
    int available[3] = {0};
    for (size_t i = 0; i < 3; ++i) {
        const Setting *values = settings_get(s, times[i]);
        if (setting_type(values) != SETTING_LIST) return -1;
        for (size_t j = 0; j < setting_size(values); ++j) {
            const Setting *v = setting_at(values, j);
            if (setting_type(v) == SETTING_NULL) continue;
            int64_t n = setting_int(v, -1);
            if (n < (i == 2 ? 1 : 0) || n > (i == 2 ? 14 : 10800)) return -1;
            available[i] = 1;
        }
    }
    if (settings_bool(s, "matchmaking.allow_matchmaking", 0) && !(available[0] && available[1]) && !available[2]) {
        fputs("Matchmaking requires initial time and increment lists, or a days list.\n", stderr); return -1;
    }
    const char *const tb[] = {"engine.online_moves.online_egtb", "engine.lichess_bot_tbs.syzygy", "engine.lichess_bot_tbs.gaviota"};
    if (!strcmp(c->protocol, "xboard")) for (size_t i = 0; i < 3; ++i) {
        char enabled[128], quality[128];
        (void)snprintf(enabled, sizeof(enabled), "%s.enabled", tb[i]);
        (void)snprintf(quality, sizeof(quality), "%s.move_quality", tb[i]);
        if (settings_bool(s, enabled, 0) && !strcmp(settings_str(s, quality, "best"), "suggest")) {
            fputs("XBoard does not support tablebase move_quality: suggest.\n", stderr); return -1;
        }
    }
    return 0;
}

static int managed_option(const char *name, size_t length) {
    static const char *const names[] = {"UCI_Chess960", "Ponder", "UCI_Variant", "MultiPV"};
    while (length && *name == ' ') { ++name; --length; }
    while (length && name[length - 1] == ' ') --length;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (length == strlen(names[i]) && !strncasecmp(name, names[i], length)) return 1;
    return 0;
}

static int add_option(Config *c, const char *option, int replace) {
    const char *eq = strchr(option, '=');
    if (!eq || eq == option || strlen(option) > 1024 || !clean_line(option)) return -1;
    const char *name = option, *end = eq;
    while (name < end && *name == ' ') ++name;
    while (end > name && end[-1] == ' ') --end;
    if (end == name) return -1;
    if (managed_option(name, (size_t)(end - name))) return -1;
    if (replace) for (int i = 0; i < c->option_count; ++i) {
        const char *other = c->options[i], *other_end = strchr(other, '=');
        while (other < other_end && *other == ' ') ++other;
        while (other_end > other && other_end[-1] == ' ') --other_end;
        if (end - name == other_end - other && !strncasecmp(name, other, (size_t)(end - name))) {
            c->options[i] = option; return 0;
        }
    }
    if (c->option_count >= MAX_OPTIONS) return -1;
    c->options[c->option_count++] = option;
    return 0;
}

static int load_engine_options(Config *c) {
    char path[128]; (void)snprintf(path, sizeof(path), "engine.%s_options", c->protocol);
    const Setting *options = settings_get(c->settings, path);
    if (setting_type(options) == SETTING_NULL) return 0;
    if (setting_type(options) != SETTING_MAP) return -1;
    for (size_t i = 0; i < setting_size(options); ++i) {
        const char *name = setting_key(options, i);
        const Setting *value = setting_at(options, i);
        if (!strcmp(name, "go_commands") || managed_option(name, strlen(name))) continue;
        if (!strcmp(c->protocol, "xboard") && !strcmp(name, "egtpath")) {
            if (setting_type(value) != SETTING_MAP) return -1;
            for (size_t j = 0; j < setting_size(value); ++j) {
                const char *text = setting_str(setting_at(value, j), NULL);
                char *option = text ? config_string(c, "egtpath %s=%s", setting_key(value, j), text) : NULL;
                if (!option || add_option(c, option, 0)) return -1;
            }
            continue;
        }
        /* Plugins read nested values from Settings. */
        if (!strcmp(c->protocol, "homemade") &&
            (setting_type(value) == SETTING_MAP || setting_type(value) == SETTING_LIST)) continue;
        const char *text = setting_type(value) == SETTING_NULL ? "" : setting_text(value);
        char *option = text ? config_string(c, "%s=%s", name, text) : NULL;
        if (!option || add_option(c, option, 0)) return -1;
    }
    (void)snprintf(path, sizeof(path), "engine.%s_options.go_commands", c->protocol);
    const Setting *go = settings_get(c->settings, path);
    if (setting_type(go) == SETTING_NULL) return 0;
    if (setting_type(go) != SETTING_MAP) return -1;
    const Setting *value = setting_get(go, "depth");
    if (value && setting_type(value) != SETTING_NULL) {
        int64_t n = setting_int(value, -1);
        if (n < 1 || n > 256) return -1;
        c->depth = (int)n;
    }
    value = setting_get(go, "movetime");
    if (value && setting_type(value) != SETTING_NULL) {
        int64_t n = setting_int(value, -1);
        if (n < 1 || n > 3600000) return -1;
        c->movetime_ms = (int)n;
    }
    value = setting_get(go, "nodes");
    if (value && setting_type(value) != SETTING_NULL) {
        int64_t n = setting_int(value, -1);
        if (n < 1) return -1;
        c->nodes = n;
    }
    return 0;
}

static int apply_list_yaml(Settings *s, const char *path, const char *yaml) {
    Settings *temp = settings_load_string(yaml, NULL, 0);
    int result = temp ? settings_set(s, path, settings_get(temp, "value")) : -1;
    settings_free(temp);
    return result;
}

static int variant_list(Settings *s, const char *names) {
    size_t length = strlen(names);
    if (!length || length >= 256 || names[0] == ',' || names[length - 1] == ',' || strstr(names, ",,")) return -1;
    char input[256], yaml[1024]; strcpy(input, names);
    strcpy(yaml, "value: ["); size_t used = strlen(yaml);
    char *save = NULL;
    for (char *name = strtok_r(input, ",", &save); name; name = strtok_r(NULL, ",", &save)) {
        int bit = config_variant(name);
        if (!bit) return -1;
        int n = snprintf(yaml + used, sizeof(yaml) - used, "%s%s", used > 8 ? "," : "", config_variant_name(bit));
        if (n < 0 || (size_t)n >= sizeof(yaml) - used) return -1;
        used += (size_t)n;
    }
    if (used + 2 >= sizeof(yaml)) return -1;
    strcpy(yaml + used, "]");
    return apply_list_yaml(s, "challenge.variants", yaml);
}

static int integer_limit(Settings *s, const char *path, int fallback) {
    double n = settings_double(s, path, (double)fallback);
    return n >= INT_MAX ? INT_MAX : n <= 0 ? 0 : (int)n;
}

static int config_values(Config *c) {
    Settings *s = c->settings;
    const char *url = settings_str(s, "url", "https://lichess.org");
    if (!url || strlen(url) >= sizeof(c->base_url)) return -1;
    strcpy(c->base_url, url);
    c->protocol = settings_str(s, "engine.protocol", "uci");
    c->concurrency = (int)settings_int(s, "challenge.concurrency", 1);
    c->accept = settings_bool(s, "c_bridge.accept_challenges", 1);
    c->only_bots = settings_bool(s, "challenge.only_bot", 0);
    c->ponder = settings_bool(s, "engine.ponder", 0) || settings_bool(s, "engine.uci_ponder", 0);
    c->trace = settings_bool(s, "c_bridge.trace", 0);
    c->engine_debug = settings_bool(s, "engine.debug", 0);
    c->silence_stderr = settings_bool(s, "engine.silence_stderr", 0);
    c->overhead_ms = (int)settings_int(s, "move_overhead", 1000);
    c->http_timeout_ms = (int)settings_int(s, "c_bridge.http_timeout_ms", 3000);
    c->engine_timeout_ms = (int)settings_int(s, "c_bridge.engine_timeout_ms", 5000);
    c->min_initial = integer_limit(s, "challenge.min_base", 0);
    c->max_initial = integer_limit(s, "challenge.max_base", INT_MAX);
    c->min_increment = integer_limit(s, "challenge.min_increment", 0);
    c->max_increment = integer_limit(s, "challenge.max_increment", 180);
    c->engine_dir = settings_str(s, "engine.working_dir", NULL);
    const Setting *modes = settings_get(s, "challenge.modes");
    c->casual_only = 1;
    for (size_t i = 0; i < setting_size(modes); ++i)
        if (!strcmp(setting_str(setting_at(modes, i), ""), "rated")) c->casual_only = 0;
    const Setting *variants = settings_get(s, "challenge.variants");
    if (setting_type(variants) != SETTING_LIST) return -1;
    c->variants = 0;
    for (size_t i = 0; i < setting_size(variants); ++i) {
        int bit = config_variant(setting_str(setting_at(variants, i), NULL));
        if (!bit) return -1;
        c->variants |= bit;
    }
    return 0;
}

static int append_argv(Config *c, int *count, char *arg) {
    if (!arg || !*arg || *count >= MAX_ENGINE_ARGS) return -1;
    c->engine_argv[(*count)++] = arg;
    c->engine_argv[*count] = NULL;
    return 0;
}

static int build_engine_argv(Config *c, const char *executable, char **extra, int extra_count) {
    Settings *s = c->settings;
    int count = 0;
    const char *interpreter = settings_str(s, "engine.interpreter", NULL);
    if (interpreter && *interpreter) {
        if (append_argv(c, &count, config_string(c, "%s", interpreter))) return -1;
        const Setting *options = settings_get(s, "engine.interpreter_options");
        for (size_t i = 0; i < setting_size(options); ++i) {
            const char *arg = setting_str(setting_at(options, i), NULL);
            if (!arg || append_argv(c, &count, config_string(c, "%s", arg))) return -1;
        }
    }
    char *engine = NULL;
    if (executable) engine = config_string(c, "%s", executable);
    else {
        const char *dir = settings_str(s, "engine.dir", NULL), *name = settings_str(s, "engine.name", NULL);
        if (!dir || !name || !*name) return -1;
        if (name[0] == '/') engine = config_string(c, "%s", name);
        else if (dir[0] == '/') engine = config_string(c, "%s/%s", dir, name);
        else {
            char cwd[8192]; if (!getcwd(cwd, sizeof(cwd))) return -1;
            engine = config_string(c, "%s/%s/%s", cwd, dir, name);
        }
    }
    if (append_argv(c, &count, engine)) return -1;
    const Setting *options = settings_get(s, "engine.engine_options");
    if (setting_type(options) != SETTING_NULL && setting_type(options) != SETTING_MAP) return -1;
    for (size_t i = 0; i < setting_size(options); ++i) {
        const char *name = setting_key(options, i);
        const Setting *value = setting_at(options, i);
        const char *text = setting_text(value);
        if (setting_type(value) == SETTING_BOOL) text = setting_bool(value, 0) ? "True" : "False";
        char *arg = setting_type(value) == SETTING_NULL ? config_string(c, "--%s", name) :
                    text ? config_string(c, "--%s=%s", name, text) : NULL;
        if (append_argv(c, &count, arg)) return -1;
    }
    for (int i = 0; i < extra_count; ++i) {
        /* Preserve explicit empty arguments. */
        if (count >= MAX_ENGINE_ARGS) return -1;
        c->engine_argv[count++] = extra[i];
    }
    c->engine_argv[count] = NULL;
    return 0;
}

static int merge_blocklists(Settings *s) {
    if (!settings_bool(s, "matchmaking.include_challenge_block_list", 0)) return 0;
    const char *const names[] = {"block_list", "online_block_list"};
    for (size_t i = 0; i < 2; ++i) {
        char source[128], target[128];
        (void)snprintf(source, sizeof(source), "challenge.%s", names[i]);
        (void)snprintf(target, sizeof(target), "matchmaking.%s", names[i]);
        const Setting *from = settings_get(s, source), *to = settings_get(s, target);
        for (size_t j = 0; j < setting_size(from); ++j)
            if (settings_list_append(s, to, setting_at(from, j))) return -1;
    }
    return 0;
}

int config_parse(Config *c, int argc, char **argv) {
    memset(c, 0, sizeof(*c));
    enum { EARG = 1000, EDIR, OPTION, CONCURRENCY, VARIANTS, PONDER, OVERHEAD,
           MOVETIME, DEPTH, NODES, NOACCEPT, ONLYBOTS, CASUAL, MIN_INITIAL, MAX_INITIAL,
           MIN_INCREMENT, MAX_INCREMENT, HTTP_TIMEOUT, ENGINE_TIMEOUT, BASE_URL,
           TRACE, TOKEN_FILE, VERSION, PROTOCOL, NO_PONDER, DISABLE_AUTO_LOGGING };
    static const struct option opts[] = {
        {"engine", required_argument, NULL, 'e'}, {"help", no_argument, NULL, 'h'},
        {"config", required_argument, NULL, 'c'}, {"protocol", required_argument, NULL, PROTOCOL},
        {"engine-arg", required_argument, NULL, EARG}, {"engine-dir", required_argument, NULL, EDIR},
        {"option", required_argument, NULL, OPTION}, {"concurrency", required_argument, NULL, CONCURRENCY},
        {"variants", required_argument, NULL, VARIANTS}, {"ponder", no_argument, NULL, PONDER},
        {"no-ponder", no_argument, NULL, NO_PONDER}, {"overhead-ms", required_argument, NULL, OVERHEAD},
        {"movetime-ms", required_argument, NULL, MOVETIME}, {"depth", required_argument, NULL, DEPTH},
        {"nodes", required_argument, NULL, NODES}, {"no-accept", no_argument, NULL, NOACCEPT},
        {"only-bots", no_argument, NULL, ONLYBOTS}, {"casual-only", no_argument, NULL, CASUAL},
        {"min-initial", required_argument, NULL, MIN_INITIAL}, {"max-initial", required_argument, NULL, MAX_INITIAL},
        {"min-increment", required_argument, NULL, MIN_INCREMENT}, {"max-increment", required_argument, NULL, MAX_INCREMENT},
        {"http-timeout-ms", required_argument, NULL, HTTP_TIMEOUT}, {"engine-timeout-ms", required_argument, NULL, ENGINE_TIMEOUT},
        {"base-url", required_argument, NULL, BASE_URL}, {"trace", no_argument, NULL, TRACE},
        {"token-file", required_argument, NULL, TOKEN_FILE}, {"version", no_argument, NULL, VERSION},
        {"upgrade", no_argument, NULL, 'u'}, {"verbose", no_argument, NULL, 'v'}, {"logfile", required_argument, NULL, 'l'},
        {"disable-auto-logging", no_argument, NULL, DISABLE_AUTO_LOGGING},
        {"disable_auto_logging", no_argument, NULL, DISABLE_AUTO_LOGGING}, {NULL, 0, NULL, 0}
    };
    const char *config_file = NULL, *executable = NULL, *token_file = NULL;
    int key;
    optind = 0;
    while ((key = getopt_long(argc, argv, "he:c:uvl:", opts, NULL)) != -1) {
        if (key == 'h') { config_help(stdout); return 1; }
        if (key == VERSION) { printf("lichess-bot-c %s\n%s\n", LBC_VERSION, curl_version()); return 1; }
        if (key == 'c') config_file = optarg;
        if (key == 'e') executable = optarg;
        if (key == '?') goto invalid;
    }
    if (optind != argc) goto invalid;
    if (!config_file && !executable && access("config.yml", F_OK) == 0) config_file = "config.yml";
    c->config_file = config_file;
    char error[512];
    c->settings = config_file ? settings_load_file(config_file, error, sizeof(error)) : settings_load_string("{}", error, sizeof(error));
    if (!c->settings) { fprintf(stderr, "%s\n", error); goto failed; }
    if (setting_type(settings_get(c->settings, "")) != SETTING_MAP || config_defaults(c->settings, !config_file)) {
        fputs("Invalid YAML configuration structure or memory limit exceeded.\n", stderr); goto failed;
    }
    char *extra[MAX_ENGINE_ARGS]; int extra_count = 0;
    const char *cli_options[MAX_OPTIONS]; int cli_option_count = 0;
    int cli_depth = 0, cli_movetime = 0; int64_t cli_nodes = 0;
    optind = 0;
    while ((key = getopt_long(argc, argv, "he:c:uvl:", opts, NULL)) != -1) {
        int64_t n;
        Settings *s = c->settings;
        switch (key) {
        case 'e': executable = optarg; break;
        case 'c': break;
        case 'u': c->upgrade = 1; break;
        case 'v': c->verbose = 1; break;
        case 'l': c->logfile = optarg; break;
        case DISABLE_AUTO_LOGGING: c->disable_auto_logging = 1; break;
        case PROTOCOL: if (settings_set_str(s, "engine.protocol", optarg)) goto invalid; break;
        case EARG: if (extra_count >= MAX_ENGINE_ARGS - 1) goto invalid; extra[extra_count++] = optarg; break;
        case EDIR: if (settings_set_str(s, "engine.working_dir", optarg)) goto invalid; break;
        case OPTION:
            if (cli_option_count >= MAX_OPTIONS) goto invalid;
            cli_options[cli_option_count++] = optarg; break;
        case CONCURRENCY:
            if (number(optarg, 1, MAX_GAMES, &n) || settings_set_int(s, "challenge.concurrency", n)) goto invalid;
            break;
        case VARIANTS: if (variant_list(s, optarg)) goto invalid; break;
        case PONDER: case NO_PONDER:
            if (settings_set_bool(s, "engine.ponder", key == PONDER) || settings_set_bool(s, "engine.uci_ponder", 0)) goto invalid;
            break;
        case OVERHEAD:
            if (number(optarg, 0, 60000, &n) || settings_set_int(s, "move_overhead", n)) goto invalid;
            break;
        case MOVETIME: if (number(optarg, 1, 3600000, &n)) goto invalid; cli_movetime = (int)n; break;
        case DEPTH: if (number(optarg, 1, 256, &n)) goto invalid; cli_depth = (int)n; break;
        case NODES: if (number(optarg, 1, INT64_MAX, &cli_nodes)) goto invalid; break;
        case NOACCEPT: if (settings_set_bool(s, "c_bridge.accept_challenges", 0)) goto invalid; break;
        case ONLYBOTS: if (settings_set_bool(s, "challenge.only_bot", 1)) goto invalid; break;
        case CASUAL: if (apply_list_yaml(s, "challenge.modes", "value: [casual]")) goto invalid; break;
        case MIN_INITIAL: case MAX_INITIAL: case MIN_INCREMENT: case MAX_INCREMENT:
            if (number(optarg, 0, 10800, &n)) goto invalid;
            if (settings_set_int(s, key == MIN_INITIAL ? "challenge.min_base" : key == MAX_INITIAL ? "challenge.max_base" :
                key == MIN_INCREMENT ? "challenge.min_increment" : "challenge.max_increment", n)) goto invalid;
            break;
        case HTTP_TIMEOUT: case ENGINE_TIMEOUT:
            if (number(optarg, 100, 60000, &n) || settings_set_int(s,
                key == HTTP_TIMEOUT ? "c_bridge.http_timeout_ms" : "c_bridge.engine_timeout_ms", n)) goto invalid;
            break;
        case BASE_URL: if (settings_set_str(s, "url", optarg)) goto invalid; break;
        case TRACE: if (settings_set_bool(s, "c_bridge.trace", 1)) goto invalid; break;
        case TOKEN_FILE: token_file = optarg; break;
        default: goto invalid;
        }
    }
    if (config_values(c) || validate_settings(c) || load_engine_options(c)) goto invalid;
    if (c->min_initial > c->max_initial || c->min_increment > c->max_increment) goto invalid;
    if (validate_url(c->base_url)) {
        fputs("Base URL must be an HTTPS origin, or HTTP on 127.0.0.1 / [::1] for local tests.\n", stderr); goto failed;
    }
    for (int i = 0; i < cli_option_count; ++i) {
        if (add_option(c, cli_options[i], 1)) {
            fputs("Invalid protocol option or bridge-managed option (Ponder, UCI_Chess960, UCI_Variant, MultiPV).\n", stderr);
            goto failed;
        }
    }
    if (cli_depth) c->depth = cli_depth;
    if (cli_movetime) c->movetime_ms = cli_movetime;
    if (cli_nodes) c->nodes = cli_nodes;
    if (build_engine_argv(c, executable, extra, extra_count) || merge_blocklists(c->settings)) goto invalid;
    if (token_file) {
        FILE *fp = fopen(token_file, "rb");
        if (!fp) { perror("token file"); goto failed; }
        unsigned char raw[TOKEN_CAP + 2];
        size_t n = fread(raw, 1, sizeof(raw), fp);
        int read_failed = ferror(fp);
        fclose(fp);
        if (n == sizeof(raw) || read_failed) goto bad_token;
        if (n && raw[n - 1] == '\n') { --n; if (n && raw[n - 1] == '\r') --n; }
        if (!n || n >= sizeof(c->token)) goto bad_token;
        for (size_t i = 0; i < n; ++i) if (raw[i] <= 32 || raw[i] >= 127) goto bad_token;
        memcpy(c->token, raw, n); c->token[n] = 0;
    } else {
        const char *token = getenv("LICHESS_BOT_TOKEN");
        if (!token) token = settings_str(c->settings, "token", NULL);
        if (!token || !*token || strlen(token) >= sizeof(c->token)) goto bad_token;
        strcpy(c->token, token);
    }
    for (const unsigned char *p = (const unsigned char *)c->token; *p; ++p)
        if (*p <= 32 || *p >= 127) goto bad_token;
    return 0;
bad_token:
    fputs("Set a nonempty --token-file, LICHESS_BOT_TOKEN or YAML token (maximum 4095 bytes; no whitespace).\n", stderr);
    goto failed;
invalid:
    fputs("Invalid arguments or configuration. Run --help for usage.\n", stderr);
failed:
    config_free(c);
    return -1;
}

