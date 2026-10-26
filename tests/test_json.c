#include "json.h"

#ifdef NDEBUG
#undef NDEBUG /* Keep assertions enabled. */
#endif
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static Json json;
static JsonTok tokens[1024];

static void parse(const char *text)
{
    assert(json_parse(&json, text, strlen(text), tokens, 1024) == 0);
    assert(json.count > 0);
    assert(tokens[0].next == json.count);
}

static void reject(const char *text)
{
    assert(json_parse(&json, text, strlen(text), tokens, 1024) == -1);
    assert(json.count == 0);
}

static void test_lichess(void)
{
    static const char full[] =
        "{\"type\":\"gameFull\",\"id\":\"abcdefgh\","
        "\"variant\":{\"key\":\"standard\",\"name\":\"Standard\"},"
        "\"clock\":{\"initial\":60000,\"increment\":1000},\"speed\":\"bullet\","
        "\"rated\":false,\"createdAt\":1729108800123,"
        "\"white\":{\"id\":\"fast_bot\",\"name\":\"Fast_Bot\",\"title\":\"BOT\",\"rating\":1500},"
        "\"black\":{\"id\":\"opponent\",\"name\":\"Opponent\",\"rating\":1600},"
        "\"initialFen\":\"startpos\","
        "\"state\":{\"type\":\"gameState\",\"moves\":\"e2e4 e7e5 g1f3\","
        "\"wtime\":58123,\"btime\":59000,\"winc\":1000,\"binc\":1000,\"status\":\"started\"},"
        "\"tournamentId\":null}";
    int state, boolean, white;
    int64_t time;
    char moves[64];
    parse(full);
    assert(json_eq(&json, json_get(&json, 0, "type"), "gameFull"));
    assert(json_get(&json, 0, "wtime") == -1);
    assert(json_get(&json, 0, "name") == -1);
    state = json_get(&json, 0, "state");
    assert(tokens[state].type == JSON_OBJECT);
    assert(json_eq(&json, json_get(&json, state, "type"), "gameState"));
    assert(json_i64(&json, json_get(&json, state, "wtime"), &time) == 0);
    assert(time == 58123);
    assert(json_string(&json, json_get(&json, state, "moves"), moves, sizeof moves) == 14);
    assert(strcmp(moves, "e2e4 e7e5 g1f3") == 0);
    assert(json_bool(&json, json_get(&json, 0, "rated"), &boolean) == 0 && !boolean);
    assert(json_i64(&json, json_get(&json, 0, "createdAt"), &time) == 0);
    assert(time == INT64_C(1729108800123));
    white = json_get(&json, 0, "white");
    assert(json_eq(&json, json_get(&json, white, "id"), "fast_bot"));
    assert(tokens[json_get(&json, 0, "tournamentId")].type == JSON_NULL);

    parse("{\"type\":\"gameState\",\"moves\":\"e2e4 e7e5 g1f3 b8c6\","
          "\"wtime\":57000,\"btime\":58000,\"winc\":1000,\"binc\":1000,"
          "\"status\":\"mate\",\"winner\":\"white\",\"wdraw\":true}");
    assert(json_eq(&json, json_get(&json, 0, "status"), "mate"));
    assert(json_bool(&json, json_get(&json, 0, "wdraw"), &boolean) == 0 && boolean);
}

static void test_structure(void)
{
    int nested, array, first;
    parse(" \r\n{\"nested\":{\"key\":\"wrong\"},\"array\":[{\"key\":\"also wrong\"},2],"
          "\"key\":\"right\",\"last\":[]}\t");
    nested = json_get(&json, 0, "nested");
    array = json_get(&json, 0, "array");
    assert(json_eq(&json, json_get(&json, 0, "key"), "right"));
    assert(json_eq(&json, json_get(&json, nested, "key"), "wrong"));
    first = array + 1;
    assert(tokens[first].type == JSON_OBJECT);
    assert(tokens[tokens[first].next].type == JSON_NUMBER);
    assert(tokens[tokens[tokens[first].next].next].type == JSON_STRING);
    assert(tokens[json_get(&json, 0, "last")].type == JSON_ARRAY);
    assert(json_get(&json, array, "key") == -1);
    parse("{\"k\\u0065y\":\"yes\",\"key\":\"second\"}");
    assert(json_eq(&json, json_get(&json, 0, "key"), "yes"));
    parse("[[],{},true,false,null,0,\"\"]");
    assert(json.count == 8);
    parse("\"\"");
    assert(json_eq(&json, 0, ""));
    parse("null");
    assert(tokens[0].type == JSON_NULL);
}

static void test_strings(void)
{
    char out[128];
    static const char utf8[] = "\xc2\xa2\xe2\x82\xac\xf0\x9f\x8e\xb2";
    int index;
    parse("{\"s\":\"quote: \\\" slash: \\/ backslash: \\\\ controls: \\b\\f\\n\\r\\t\"}");
    index = json_get(&json, 0, "s");
    assert(json_string(&json, index, out, sizeof out) >= 0);
    assert(strcmp(out, "quote: \" slash: / backslash: \\ controls: \b\f\n\r\t") == 0);
    assert(json_eq(&json, index, out));
    parse("\"\\u00a2\\u20AC\\uD83C\\uDFB2\"");
    assert(json_string(&json, 0, out, sizeof out) == 9);
    assert(strcmp(out, utf8) == 0);
    assert(!json_eq(&json, 0, utf8));
    parse("\"\xc2\xa2\xe2\x82\xac\xf0\x9f\x8e\xb2\"");
    assert(json_string(&json, 0, out, sizeof out) == 9);
    assert(strcmp(out, utf8) == 0);
    parse("\"\\uD800\\uDC00\\uDBFF\\uDFFF\"");
    assert(json_string(&json, 0, out, sizeof out) == 8);
    assert(memcmp(out, "\xf0\x90\x80\x80\xf4\x8f\xbf\xbf", 8) == 0);
    parse("\"abc\"");
    memset(out, 'X', sizeof out);
    assert(json_string(&json, 0, out, 4) == 3);
    assert(out[4] == 'X');
    assert(json_string(&json, 0, out, 3) == -1 && out[0] == 0);
    out[0] = 'X';
    assert(json_string(&json, 0, out, 0) == -1 && out[0] == 'X');
    assert(json_string(&json, 0, NULL, 4) == -1);
    parse("\"\"");
    assert(json_string(&json, 0, out, 1) == 0 && out[0] == 0);
    parse("\"a\\u0000b\"");
    assert(json_string(&json, 0, out, sizeof out) == -1 && out[0] == 0);
    assert(!json_eq(&json, 0, "a"));
    assert(!json_eq(&json, 0, "a\0b"));
}

static void test_numbers(void)
{
    static const char *valid[] = {
        "0", "-0", "1", "-1", "10", "-10", "0.1", "-0.2", "1e1", "1E+10",
        "1e-10", "-12.34E+56", "1234567890123456789012345678901234567890"
    };
    static const char *not_integer[] = {
        "1.0", "1e0", "9223372036854775808", "-9223372036854775809",
        "99999999999999999999999999999999999999999999999999999999999999999999"
    };
    size_t i;
    int64_t n;
    int b;
    for (i = 0; i < sizeof valid / sizeof valid[0]; ++i) parse(valid[i]);
    parse("9223372036854775807");
    assert(json_i64(&json, 0, &n) == 0 && n == INT64_MAX);
    parse("-9223372036854775808");
    assert(json_i64(&json, 0, &n) == 0 && n == INT64_MIN);
    parse("-0");
    assert(json_i64(&json, 0, &n) == 0 && n == 0);
    for (i = 0; i < sizeof not_integer / sizeof not_integer[0]; ++i) {
        parse(not_integer[i]);
        n = 123;
        assert(json_i64(&json, 0, &n) == -1 && n == 123);
    }
    parse("true");
    assert(json_bool(&json, 0, &b) == 0 && b == 1);
    assert(json_i64(&json, 0, &n) == -1);
    parse("false");
    assert(json_bool(&json, 0, &b) == 0 && b == 0);
    parse("0");
    b = 9;
    assert(json_bool(&json, 0, &b) == -1 && b == 9);
}

static void test_invalid(void)
{
    static const char *invalid[] = {
        "", " ", "\vnull", "\fnull", "00", "01", "-01", "+1", ".1", "1.",
        "-.1", "--1", "-", "1e", "1e+", "1e-", "1.e1", "nan", "NaN", "Infinity",
        "[", "{", "[1,]", "[,1]", "[1 2]", "[1,,2]", "{,}", "{\"a\":}",
        "{\"a\":1,}", "{\"a\" 1}", "{a:1}", "{1:2}", "{\"a\":1 \"b\":2}",
        "[}", "{]", "[truefalse]", "falsex", "True", "nul", "null null", "{}[]",
        "\"", "\"abc", "\"abc\\", "\"\\a\"", "\"\\x00\"", "\"\\u\"", "\"\\u000\"",
        "\"\\uGGGG\"", "\"\\uD800\"", "\"\\uD800\\uD800\"", "\"\\uDC00\"",
        "\"\\uD800\\u1234\"", "\"\\uD800x\\uDC00\"", "\"line\nbreak\"", "\"\x01\"",
        "\"\x80\"", "\"\xc0\x80\"", "\"\xc1\xbf\"", "\"\xc2\"", "\"\xe0\x80\x80\"",
        "\"\xed\xa0\x80\"", "\"\xf0\x80\x80\x80\"", "\"\xf4\x90\x80\x80\"",
        "\"\xf5\x80\x80\x80\"", "\"\xe2\x82\"", "\"\xc2x\"", "\"\xff\"",
        "/*comment*/null", "{'a':1}", "[1,/*x*/2]"
    };
    size_t i;
    char with_nul[] = {'"', 'a', 0, 'b', '"'};
    char trailing_nul[] = {'{', '}', 0};
    for (i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) reject(invalid[i]);
    assert(json_parse(&json, with_nul, sizeof with_nul, tokens, 1024) == -1);
    assert(json_parse(&json, trailing_nul, sizeof trailing_nul, tokens, 1024) == -1);
    assert(json_parse(&json, trailing_nul, 2, tokens, 1024) == 0);
}

static void test_boundaries(void)
{
    static const char example[] =
        "{\"type\":\"gameState\",\"moves\":\"e2e4 e7e5\",\"nested\":[1,{\"x\":\"\\uD83C\\uDFB2\"}]}";
    char nested[2 * (JSON_MAX_DEPTH + 1) + 1];
    char out[4];
    int depth, i, needed;
    size_t cut;
    for (cut = 0; cut < sizeof example - 1; ++cut) {
        assert(json_parse(&json, example, cut, tokens, 1024) == -1);
    }
    parse(example);
    needed = json.count;
    for (i = 0; i < needed; ++i) {
        assert(json_parse(&json, example, sizeof example - 1, tokens, i) == -1);
    }
    assert(json_parse(&json, example, sizeof example - 1, tokens, needed) == 0);
    for (depth = JSON_MAX_DEPTH; depth <= JSON_MAX_DEPTH + 1; ++depth) {
        for (i = 0; i < depth; ++i) nested[i] = '[';
        nested[depth] = '0';
        for (i = 0; i < depth; ++i) nested[depth + i + 1] = ']';
        assert(json_parse(&json, nested, (size_t)(2 * depth + 1), tokens, 1024) ==
               (depth == JSON_MAX_DEPTH ? 0 : -1));
    }
    assert(json_parse(NULL, "0", 1, tokens, 1024) == -1);
    assert(json_parse(&json, NULL, 1, tokens, 1024) == -1);
    assert(json_parse(&json, "0", 1, NULL, 1024) == -1);
    assert(json_parse(&json, "0", 1, tokens, -1) == -1);
    assert(json_parse(&json, "0", (size_t)INT_MAX + 1, tokens, 1024) == -1);
    parse("{} ");
    assert(json_get(&json, -1, "x") == -1);
    assert(json_get(&json, json.count, "x") == -1);
    assert(json_get(&json, 0, NULL) == -1);
    assert(json_get(NULL, 0, "x") == -1);
    assert(!json_eq(&json, -1, "x"));
    assert(!json_eq(&json, json.count, "x"));
    assert(json_string(&json, -1, out, sizeof out) == -1 && !out[0]);
    assert(json_string(&json, 0, out, sizeof out) == -1 && !out[0]);
    assert(json_i64(&json, -1, NULL) == -1);
    assert(json_bool(&json, -1, NULL) == -1);
}

int main(void)
{
    test_lichess();
    test_structure();
    test_strings();
    test_numbers();
    test_invalid();
    test_boundaries();
    puts("json: all tests passed");
    return 0;
}
