#include "hooks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d failed: %s\n",__FILE__,__LINE__,#c); exit(1); } } while(0)
typedef struct { int options; } State;
static void logger(void *user, int level, const char *text) { (void)user; (void)level; fprintf(stderr, "%s\n", text); }
static int option(void *user, const char *name, const char *value) {
    State *state = user;
    CHECK(!strcmp(name, "Hash")); CHECK(!strcmp(value, "64"));
    ++state->options; return 0;
}
static const char *game = "{\"id\":\"abcd\",\"createdAt\":0,\"variant\":{\"key\":\"standard\",\"name\":\"Standard\"},\"white\":{\"name\":\"Bot\",\"title\":\"BOT\",\"rating\":1500},\"black\":{\"name\":\"Opponent\",\"rating\":1600},\"initialFen\":\"startpos\",\"state\":{\"moves\":\"\",\"wtime\":60000,\"btime\":60000,\"winc\":0,\"binc\":0,\"status\":\"started\"}}";
static const char *challenge = "{\"id\":\"abcd\",\"rated\":false,\"variant\":{\"key\":\"standard\"},\"perf\":{\"name\":\"Blitz\"},\"speed\":\"blitz\",\"color\":\"white\",\"challenger\":{\"name\":\"Opponent\",\"rating\":1600},\"timeControl\":{\"type\":\"clock\",\"limit\":60,\"increment\":1}}";
int main(int argc, char **argv) {
    CHECK(argc == 2);
    State state = {0}; LbcHooks hooks = {0};
    LbcHooksHostV1 host = { LBC_PLUGIN_ABI_V1, (uint32_t)sizeof(host), &state, logger, NULL };
    CHECK(hooks_open(&hooks, argv[1], &host) == 0);
    CHECK(hooks_challenge(&hooks, challenge));
    CHECK(hooks_game_options(&hooks, game, option, &state) == 0);
    CHECK(state.options == 1);
    hooks_event(&hooks, "gameStart", "abcd", "{}"); hooks_close(&hooks);
    CHECK(hooks_open(&hooks, "/nonexistent/lbc-hooks-test.so", &host) < 0);
    CHECK(*hooks.error); hooks_close(&hooks);
    CHECK(hooks_open(&hooks, "", &host) == 0);
    CHECK(hooks_challenge(&hooks, "{}")); CHECK(!hooks_game_options(&hooks, "{}", option, &state)); hooks_close(&hooks);
    puts("hooks: native ABI, lifecycle, overrides, and failures passed");
    return 0;
}
