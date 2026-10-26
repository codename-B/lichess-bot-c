#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "settings.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(test) do { ++checks; if (!(test)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); } } while (0)

static Settings *load(const char *yaml) {
    char error[512];
    Settings *s = settings_load_string(yaml, error, sizeof(error));
    if (!s) fprintf(stderr, "Unexpected YAML failure: %s\n", error);
    CHECK(s != NULL);
    return s;
}

static void reject(const char *yaml) {
    char error[512];
    Settings *s = settings_load_string(yaml, error, sizeof(error));
    CHECK(s == NULL);
    CHECK(*error != 0);
    settings_free(s);
}

static void test_types(void) {
    Settings *s = load(
        "text: hello\nquoted: 'false'\nexplicit: !!str 123\ntruth: YES\nfalsehood: Off\n"
        "nothing: null\ntilde: ~\nempty:\ninteger: -123\nhexadecimal: 0x2a\noctal: 010\n"
        "binary: 0b101\nsexagesimal: 1:02\nseparated: 1_024\nreal: 3.125\n"
        "maximum: 9223372036854775807\nminimum: -9223372036854775808\n"
        "infinity: .inf\nnegative_infinity: -.Inf\nnan: .nan\n"
        "list: [1, true, null, 'quoted words']\nmap: {spaced key: 'value', literal.dot: 7}\n"
        "folded: >-\n  line one\n  line two\n"
        "literal: |\n  line one\n  line two\nutf8: 'Bonne chance, \u265e'\n");
    CHECK(setting_type(settings_get(s, "text")) == SETTING_STRING);
    CHECK(!strcmp(settings_str(s, "quoted", ""), "false"));
    CHECK(!strcmp(settings_str(s, "explicit", ""), "123"));
    CHECK(settings_bool(s, "truth", 0) == 1);
    CHECK(settings_bool(s, "falsehood", 1) == 0);
    CHECK(!strcmp(setting_text(settings_get(s, "truth")), "true"));
    CHECK(setting_type(settings_get(s, "nothing")) == SETTING_NULL);
    CHECK(setting_type(settings_get(s, "empty")) == SETTING_NULL);
    CHECK(settings_int(s, "integer", 0) == -123);
    CHECK(settings_int(s, "hexadecimal", 0) == 42);
    CHECK(!strcmp(setting_text(settings_get(s, "hexadecimal")), "42"));
    CHECK(settings_int(s, "octal", 0) == 8);
    CHECK(settings_int(s, "binary", 0) == 5);
    CHECK(settings_int(s, "sexagesimal", 0) == 62);
    CHECK(settings_int(s, "separated", 0) == 1024);
    CHECK(settings_int(s, "maximum", 0) == INT64_MAX);
    CHECK(settings_int(s, "minimum", 0) == INT64_MIN);
    CHECK(settings_double(s, "real", 0) == 3.125);
    CHECK(settings_int(s, "real", 17) == 17);
    CHECK(isinf(settings_double(s, "infinity", 0)));
    CHECK(settings_double(s, "negative_infinity", 0) < 0);
    CHECK(isnan(settings_double(s, "nan", 0)));
    CHECK(setting_size(settings_get(s, "list")) == 4);
    CHECK(settings_int(s, "list[0]", 0) == 1);
    CHECK(settings_bool(s, "list.1", 0) == 1);
    CHECK(settings_get(s, "list[4]") == NULL);
    CHECK(settings_get(s, "list[-1]") == NULL);
    CHECK(settings_get(s, "list[999999999999999999999999]") == NULL);
    CHECK(setting_int(setting_get(settings_get(s, "map"), "literal.dot"), 0) == 7);
    CHECK(!strcmp(settings_str(s, "folded", ""), "line one line two"));
    CHECK(!strcmp(settings_str(s, "literal", ""), "line one\nline two\n"));
    CHECK(strstr(settings_str(s, "utf8", ""), "Bonne chance") != NULL);
    CHECK(settings_bool(s, "quoted", 99) == 99);
    CHECK(settings_int(s, "missing", 88) == 88);
    CHECK(setting_size(settings_get(s, "")) > 20);
    settings_free(s);
}

static void test_aliases(void) {
    Settings *s = load(
        "first: &first {one: 1, overlap: 'first'}\n"
        "second: &second {two: 2, overlap: 'second'}\n"
        "merged: {explicit: true, <<: [*first, *second], two: 3}\n"
        "before: {one: 4, <<: *first}\n"
        "list: &items [1, 2, 3]\ncopy: *items\n"
        "overwritten: &survives {value: 'original'}\noverwritten: {value: 'new'}\n"
        "surviving_alias: *survives\n");
    CHECK(settings_int(s, "merged.one", 0) == 1);
    CHECK(settings_int(s, "merged.two", 0) == 3);
    CHECK(!strcmp(settings_str(s, "merged.overlap", ""), "first"));
    CHECK(settings_int(s, "before.one", 0) == 4);
    CHECK(settings_int(s, "copy.2", 0) == 3);
    CHECK(!strcmp(settings_str(s, "surviving_alias.value", ""), "original"));
    CHECK(!strcmp(settings_str(s, "overwritten.value", ""), "new"));
    settings_free(s);
    reject("recursive: &cycle [*cycle]\n");
    reject("undefined: *missing\n");
    reject("merge: {<<: 42}\n");
    reject("merge: {<<: [{a: 1}, 2]}\n");
    char bomb[4096]; size_t used = 0;
    for (unsigned i = 0; i < 22; ++i) {
        int n = i ? snprintf(bomb + used, sizeof(bomb) - used,
            "a%u: &a%u [*a%u, *a%u, *a%u, *a%u]\n", i, i, i - 1, i - 1, i - 1, i - 1) :
            snprintf(bomb + used, sizeof(bomb) - used, "a0: &a0 [x, x, x, x]\n");
        CHECK(n > 0 && (size_t)n < sizeof(bomb) - used);
        used += (size_t)n;
    }
    reject(bomb);
}

static void test_errors(void) {
    reject("key: [1, 2\n");
    reject("key: !execute something\n");
    reject("true: value\n");
    reject("? [complex, key]\n: value\n");
    reject("---\none: 1\n---\ntwo: 2\n");
    reject("value: \"embedded\\0NUL\"\n");
    reject("value: !!int not-an-integer\n");
    char nesting[512]; size_t used = 0;
    for (unsigned i = 0; i < 70; ++i) nesting[used++] = '[';
    nesting[used++] = '0';
    for (unsigned i = 0; i < 70; ++i) nesting[used++] = ']';
    nesting[used] = 0;
    reject(nesting);
    Settings *s = load("");
    CHECK(setting_type(settings_get(s, "")) == SETTING_MAP);
    settings_free(s);
}

static void test_mutations(void) {
    Settings *s = load("engine: {Hash: 128, custom: 'kept'}\nexisting_null:\nscalar: 60\n");
    Settings *defaults = load("engine: {Hash: 64, Threads: 1}\nnew: true\nexisting_null: 17\n");
    CHECK(settings_apply_defaults(s, defaults) == 0);
    CHECK(settings_int(s, "engine.Hash", 0) == 128);
    CHECK(settings_int(s, "engine.Threads", 0) == 1);
    CHECK(settings_bool(s, "new", 0));
    CHECK(setting_type(settings_get(s, "existing_null")) == SETTING_NULL);
    CHECK(settings_set_bool(s, "new", 0) == 0);
    CHECK(settings_set_str(s, "engine.custom", "updated") == 0);
    CHECK(settings_set_int(s, "engine.Hash", 512) == 0);
    CHECK(settings_set_double(s, "engine.decimal", 1.25) == 0);
    CHECK(settings_listify(s, "scalar", 0) == 0);
    CHECK(settings_int(s, "scalar[0]", 0) == 60);
    CHECK(settings_listify(s, "existing_null", 1) == 0);
    CHECK(setting_size(settings_get(s, "existing_null")) == 0);
    CHECK(settings_list_append(s, settings_get(s, "scalar"), settings_get(s, "engine.Hash")) == 0);
    CHECK(settings_int(s, "scalar[1]", 0) == 512);
    CHECK(settings_map_set(s, settings_get(s, "engine"), "literal.with.dots", settings_get(s, "engine.Hash")) == 0);
    CHECK(setting_int(setting_get(settings_get(s, "engine"), "literal.with.dots"), 0) == 512);
    settings_free(defaults); settings_free(s);
}

static int parse_file(Config *c, const char *yaml, const char *const *extras, int extra_count) {
    char name[] = "/tmp/lbc-config-unit-XXXXXX";
    int fd = mkstemp(name);
    CHECK(fd >= 0);
    size_t length = strlen(yaml);
    CHECK(write(fd, yaml, length) == (ssize_t)length);
    CHECK(close(fd) == 0);
    char *argv[128] = {(char *)"test-settings", (char *)"--config", name};
    CHECK(extra_count < 124);
    for (int i = 0; i < extra_count; ++i) argv[i + 3] = (char *)extras[i];
    int result = config_parse(c, extra_count + 3, argv);
    CHECK(unlink(name) == 0);
    return result;
}

static void test_configuration(void) {
    const char *prior = getenv("LICHESS_BOT_TOKEN");
    char *saved = prior ? strdup(prior) : NULL;
    CHECK(setenv("LICHESS_BOT_TOKEN", "synthetic-settings-test-token", 1) == 0);
    Config c;
    const char *yaml =
        "token: 'yaml-token'\nurl: 'https://lichess.org/'\n"
        "engine:\n  dir: /bin\n  name: cat\n  protocol: uci\n  ponder: true\n"
        "  interpreter: /bin/sh\n  interpreter_options: '-e'\n"
        "  engine_options: {count: 5, flag: null, toggle: true}\n"
        "  uci_options:\n    Hash: 512\n    Threads: 4\n    Ponder: false\n    UCI_Variant: atomic\n"
        "    go_commands: {depth: 5, nodes: 17, movetime: 800}\n"
        "challenge:\n  concurrency: 3\n  variants: [standard, fromPosition, chess960, antichess, atomic, crazyhouse, horde, kingOfTheHill, racingKings, threeCheck]\n"
        "  block_list: [BotA]\n  online_block_list: ['https://example.org/bots.txt']\n"
        "matchmaking:\n  include_challenge_block_list: true\n  block_list: [BotB]\n"
        "  challenge_initial_time: 60\n  challenge_increment: 1\n  challenge_timeout: 0\n"
        "  delay_after_decline: fine\n  overrides:\n    'fast.mode': {challenge_initial_time: 10, challenge_increment: null, challenge_days: ''}\n"
        "greeting: {hello: 'Good luck {opponent}'}\n";
    const char *args[] = {"--concurrency", "4", "--overhead-ms", "7", "--no-ponder", "--option", "Hash=64", "--depth", "9"};
    CHECK(parse_file(&c, yaml, args, (int)(sizeof(args) / sizeof(args[0]))) == 0);
    CHECK(c.settings != NULL);
    CHECK(c.concurrency == 4 && settings_int(c.settings, "challenge.concurrency", 0) == 4);
    CHECK(c.variants == VAR_ALL);
    CHECK(c.overhead_ms == 7 && c.ponder == 0);
    CHECK(c.depth == 9 && c.nodes == 17 && c.movetime_ms == 800);
    CHECK(!strcmp(c.protocol, "uci"));
    CHECK(!strcmp(c.base_url, "https://lichess.org"));
    CHECK(!strcmp(c.token, "synthetic-settings-test-token"));
    CHECK(!strcmp(c.engine_argv[0], "/bin/sh"));
    CHECK(!strcmp(c.engine_argv[1], "-e"));
    CHECK(!strcmp(c.engine_argv[2], "/bin/cat"));
    CHECK(!strcmp(c.engine_argv[3], "--count=5"));
    CHECK(!strcmp(c.engine_argv[4], "--flag"));
    CHECK(!strcmp(c.engine_argv[5], "--toggle=True"));
    CHECK(c.engine_argv[6] == NULL);
    CHECK(c.option_count == 2);
    CHECK(!strcmp(c.options[0], "Hash=64"));
    CHECK(!strcmp(c.options[1], "Threads=4"));
    CHECK(settings_int(c.settings, "matchmaking.challenge_timeout", 0) == 1);
    CHECK(settings_int(c.settings, "matchmaking.challenge_initial_time[0]", 0) == 60);
    CHECK(!strcmp(settings_str(c.settings, "matchmaking.challenge_filter", ""), "fine"));
    CHECK(setting_size(settings_get(c.settings, "matchmaking.block_list")) == 2);
    CHECK(setting_size(settings_get(c.settings, "matchmaking.online_block_list")) == 1);
    const Setting *override = setting_get(settings_get(c.settings, "matchmaking.overrides"), "fast.mode");
    CHECK(setting_int(setting_at(setting_get(override, "challenge_initial_time"), 0), 0) == 10);
    CHECK(setting_size(setting_get(override, "challenge_increment")) == 1);
    CHECK(setting_type(setting_at(setting_get(override, "challenge_increment"), 0)) == SETTING_NULL);
    CHECK(setting_size(setting_get(override, "challenge_days")) == 1);
    CHECK(setting_type(setting_at(setting_get(override, "challenge_days"), 0)) == SETTING_NULL);
    CHECK(settings_int(c.settings, "abort_time", 0) == 20);
    CHECK(settings_int(c.settings, "engine.polyglot.max_depth", 0) == 8);
    CHECK(settings_bool(c.settings, "challenge.accept_bot", 1) == 0);
    config_free(&c);
    CHECK(c.settings == NULL && c.owned_count == 0);

    CHECK(unsetenv("LICHESS_BOT_TOKEN") == 0);
    CHECK(parse_file(&c, "token: yaml-token\nengine: {dir: /bin, name: cat}\n", NULL, 0) == 0);
    CHECK(!strcmp(c.token, "yaml-token"));
    config_free(&c);
    CHECK(setenv("LICHESS_BOT_TOKEN", "synthetic-settings-test-token", 1) == 0);

    const char *xboard = "engine: {dir: /bin, name: cat, protocol: xboard, xboard_options: {cores: 4, memory: 512, egtpath: {syzygy: '/path with spaces'}, go_commands: {depth: 4}}}\n";
    CHECK(parse_file(&c, xboard, NULL, 0) == 0);
    CHECK(!strcmp(c.protocol, "xboard"));
    CHECK(c.depth == 4 && c.option_count == 3);
    CHECK(!strcmp(c.options[2], "egtpath syzygy=/path with spaces"));
    config_free(&c);
    const char *homemade = "engine: {dir: '.', name: RandomMove, protocol: homemade, homemade_options: {seed: 4, custom: {nested: true}}}\n";
    CHECK(parse_file(&c, homemade, NULL, 0) == 0);
    CHECK(!strcmp(c.protocol, "homemade"));
    CHECK(settings_bool(c.settings, "engine.homemade_options.custom.nested", 0));
    config_free(&c);
    CHECK(parse_file(&c, "engine: {dir: '.', name: FirstMove, protocol: homemade}\nextra_game_handlers: {protocol: native, path: handlers.so}\n", NULL, 0) == 0);
    config_free(&c);

    const char *books = "engine: {dir: /bin, name: cat, polyglot: {min_weight: 65536, book: {standard: 'one book.bin', chess960: [first.bin, second.bin], giveaway: null, 'literal.variant': third.bin}, books: {atomic: atom.bin, horde: []}}}\n";
    CHECK(parse_file(&c, books, NULL, 0) == 0);
    CHECK(settings_double(c.settings, "engine.polyglot.min_weight", 0) == 65536);
    CHECK(setting_type(settings_get(c.settings, "engine.polyglot.book.standard")) == SETTING_LIST);
    CHECK(setting_size(settings_get(c.settings, "engine.polyglot.book.standard")) == 1);
    CHECK(!strcmp(settings_str(c.settings, "engine.polyglot.book.standard[0]", ""), "one book.bin"));
    CHECK(setting_size(settings_get(c.settings, "engine.polyglot.book.chess960")) == 2);
    CHECK(!strcmp(settings_str(c.settings, "engine.polyglot.book.chess960[0]", ""), "first.bin"));
    CHECK(!strcmp(settings_str(c.settings, "engine.polyglot.book.chess960[1]", ""), "second.bin"));
    CHECK(setting_type(settings_get(c.settings, "engine.polyglot.book.giveaway")) == SETTING_LIST);
    CHECK(setting_size(settings_get(c.settings, "engine.polyglot.book.giveaway")) == 0);
    CHECK(!strcmp(setting_str(setting_at(setting_get(settings_get(c.settings, "engine.polyglot.book"), "literal.variant"), 0), ""), "third.bin"));
    CHECK(!strcmp(settings_str(c.settings, "engine.polyglot.books.atomic[0]", ""), "atom.bin"));
    CHECK(setting_type(settings_get(c.settings, "engine.polyglot.books.horde")) == SETTING_LIST);
    CHECK(setting_size(settings_get(c.settings, "engine.polyglot.books.horde")) == 0);
    config_free(&c);

    const char *bad[] = {
        "engine: {dir: '.', name: FirstMove, protocol: homemade, legacy_dir: /old/bot}\n",
        "engine: {dir: '.', name: FirstMove, protocol: homemade, legacy_python: python3}\n",
        "engine: {dir: '.', name: FirstMove, protocol: homemade, homemade_module: homemade.py}\n",
        "engine: {dir: /bin, name: cat}\nextra_game_handlers: {protocol: python}\n",
        "engine: {dir: /bin, name: cat}\nextra_game_handlers: {module: handlers.py}\n",
        "engine: {dir: /bin, name: cat}\nextra_game_handlers: {upstream_dir: /old/bot}\n",
        "engine: {dir: /bin, name: cat}\nextra_game_handlers: {python: python3}\n",
        "engine: {dir: /bin, name: cat, protocol: unknown}\n",
        "engine: {dir: /bin, name: cat}\nchallenge: {concurrency: 17}\n",
        "engine: {dir: /bin, name: cat}\nchallenge: {concurrency: 1, games_reserved_for_humans: 2}\n",
        "engine: {dir: /bin, name: cat}\nchallenge: {sort_by: wrong}\n",
        "engine: {dir: /bin, name: cat}\nchallenge: {variants: [duck]}\n",
        "engine: {dir: /bin, name: cat}\nchallenge: {accept_bot: 'false'}\n",
        "engine: {dir: /bin, name: cat}\nmatchmaking: {allow_matchmaking: true}\n",
        "engine: {dir: /bin, name: cat, polyglot: {normalization: wrong}}\n",
        "engine: {dir: /bin, name: cat, polyglot: {min_weight: -1}}\n",
        "engine: {dir: /bin, name: cat, polyglot: {min_weight: .nan}}\n",
        "engine: {dir: /bin, name: cat}\nmove_overhead: -1\n",
        "engine: {dir: /bin, name: cat, uci_options: {Hash: \"64\\nquit\"}}\n",
        "engine: {dir: /bin, name: cat, protocol: xboard, lichess_bot_tbs: {syzygy: {enabled: true, move_quality: suggest}}}\n"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(parse_file(&c, bad[i], NULL, 0) == -1);
        CHECK(c.settings == NULL);
    }
    char *cli[] = {(char *)"test-settings", (char *)"--engine", (char *)"/bin/cat"};
    CHECK(config_parse(&c, 3, cli) == 0);
    CHECK(c.settings != NULL && c.overhead_ms == 100 && c.max_initial == 1800 && c.max_increment == 60);
    CHECK(settings_bool(c.settings, "challenge.accept_bot", 0));
    config_free(&c);
    for (int bit = 1; bit <= VAR_THREE_CHECK; bit <<= 1) {
        CHECK(config_variant(config_variant_name(bit)) == bit);
        CHECK(config_engine_variant_name(bit) != NULL);
    }
    if (saved) { CHECK(setenv("LICHESS_BOT_TOKEN", saved, 1) == 0); free(saved); }
    else CHECK(unsetenv("LICHESS_BOT_TOKEN") == 0);
}

int main(int argc, char **argv) {
    test_types(); test_aliases(); test_errors(); test_mutations(); test_configuration();
    if (argc > 1) {
        char error[512];
        Settings *s = settings_load_file(argv[1], error, sizeof(error));
        if (!s) fprintf(stderr, "%s\n", error);
        CHECK(s != NULL);
        CHECK(!strcmp(settings_str(s, "engine.protocol", ""), "uci"));
        CHECK(settings_int(s, "engine.uci_options.Threads", 0) == 4);
        CHECK(settings_int(s, "challenge.max_days", 0) == 14);
        settings_free(s);
    }
    printf("settings/config: %u checks passed\n", checks);
    return 0;
}
