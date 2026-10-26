#define _POSIX_C_SOURCE 200809L
#include "homemade.h"
#include "position.h"
#include "../include/lbc_plugin.h"

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define HOME_HISTORY (64u * 1024u)
#define HOME_GAME (96u * 1024u)
#define HOME_INPUT (128u * 1024u)
typedef struct {
    void *library, *instance;
    const LbcEnginePluginV1 *plugin;
    LbcEngineHostV1 api;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    atomic_int cancel;
    int thread_created, running, released, job_pending, shutdown, fatal, chess960, draw_offered, valid_position;
    unsigned variant;
    Position position;
    char initial_fen[POSITION_FEN_MAX], fen[POSITION_FEN_MAX], history[HOME_HISTORY], game_json[HOME_GAME];
    char state_json[HOME_HISTORY + 2048u];
    char options_json[HOME_GAME];
    char legal[POSITION_MAX_MOVES][8], roots[POSITION_MAX_MOVES][8];
    LbcSearchV1 request;
    int builtin;
    uint64_t random_state;
} Homemade;

static int cancelled(void *user) {
    Homemade *h = user; return atomic_load_explicit(&h->cancel, memory_order_relaxed);
}
static void engine_log(void *user, int level, const char *message) {
    (void)user;
    fprintf(stderr, "homemade[%d]: %s\n", level, message ? message : "");
}
static int64_t milliseconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void fail(Homemade *h, const char *message) {
    fprintf(stderr, "lbc-homemade: %s\n", message);
    printf("info string lbc_error %s\n", message);
    h->fatal = 1;
}
static uint64_t next_random(Homemade *h) {
    uint64_t x = h->random_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    h->random_state = x;
    return x * UINT64_C(2685821657736338717);
}
static int builtin_search(Homemade *h, LbcSearchResultV1 *result) {
    const LbcSearchV1 *q = &h->request;
    const char (*moves)[8] = q->root_moves ? q->root_moves : q->legal_moves;
    size_t count = q->root_moves ? q->root_count : q->legal_count;
    if (!count) return 0;
    size_t selected = 0;
    int random = h->builtin == 1;
    if (h->builtin == 4) {
        int64_t own = q->movetime_ms > 0 ? q->movetime_ms : q->turn ? q->black_ms : q->white_ms;
        int64_t inc = q->movetime_ms > 0 ? 0 : q->turn ? q->black_increment_ms : q->white_increment_ms;
        random = own / 60 + inc > 10000;
        result->draw_accepted = q->draw_offered;
    }
    if (random) {
        uint64_t bound = (uint64_t)count;
        uint64_t threshold = (UINT64_C(0) - bound) % bound, value;
        do { value = next_random(h); } while (value < threshold);
        selected = (size_t)(value % bound);
    } else if (h->builtin == 2) {
        char best[64] = "";
        for (size_t i = 0; i < count; ++i) {
            char san[64];
            if (position_uci_to_san(&h->position, moves[i], san, sizeof(san)) && (!*best || strcmp(san, best) < 0)) {
                snprintf(best, sizeof(best), "%s", san); selected = i;
            }
        }
    } else {
        for (size_t i = 1; i < count; ++i) if (strcmp(moves[i], moves[selected]) < 0) selected = i;
    }
    snprintf(result->move, sizeof(result->move), "%s", moves[selected]);
    result->nodes = (int64_t)count; result->depth = 1;
    snprintf(result->pv, sizeof(result->pv), "%s", result->move);
    return 0;
}
static int root_contains(const Homemade *h, const char *move) {
    if (!h->request.root_moves) return 1;
    for (size_t i = 0; i < h->request.root_count; ++i) if (!strcmp(h->roots[i], move)) return 1;
    return 0;
}
static void output_result(Homemade *h, LbcSearchResultV1 *r, int code) {
    r->move[sizeof(r->move) - 1] = 0; r->ponder[sizeof(r->ponder) - 1] = 0;
    r->pv[sizeof(r->pv) - 1] = 0;
    if (code || r->abi_version != LBC_PLUGIN_ABI_V1 || r->struct_size < sizeof(*r)) {
        printf("info string lbc_error homemade search returned error %d\nbestmove 0000\n", code); return;
    }
    if (r->resigned) puts("info string lbc_resign");
    if (r->draw_accepted) puts("info string lbc_draw_accept");
    else if (r->draw_offered) puts("info string lbc_draw_offer");
    if (r->has_score || r->depth || r->nodes || r->time_ms || *r->pv) {
        printf("info depth %d seldepth %d nodes %" PRId64 " time %" PRId64 " nps %" PRId64 " tbhits %" PRId64,
               r->depth, r->seldepth, r->nodes, r->time_ms, r->nps, r->tbhits);
        if (r->has_score) printf(" score %s %d", r->mate ? "mate" : "cp", r->mate ? r->mate : r->score_cp);
        /* Emit only legal moves from a plugin's PV. */
        char copy[LBC_PV_CAP]; snprintf(copy, sizeof(copy), "%s", r->pv);
        char *save = NULL; Position board = h->position; int have = 0;
        for (char *move = strtok_r(copy, " \t\r\n", &save); move; move = strtok_r(NULL, " \t\r\n", &save)) {
            char normalized[8];
            if (!position_normalize_uci(&board, move, h->chess960, normalized) || !position_is_legal(&board, normalized)) break;
            printf("%s%s", have ? " " : " pv ", normalized); have = 1;
            if (!position_apply_uci(&board, normalized, NULL, 0)) break;
        }
        putchar('\n');
    }
    char move[8] = "0000", ponder[8] = "";
    if (!r->resigned && *r->move && strcmp(r->move, "0000")) {
        if (!position_normalize_uci(&h->position, r->move, h->chess960, move) ||
            !position_is_legal(&h->position, move) || !root_contains(h, move)) {
            puts("info string lbc_error homemade returned an illegal or excluded move"); snprintf(move, sizeof(move), "0000");
        } else if (*r->ponder) {
            Position after = h->position;
            if (position_apply_uci(&after, move, NULL, 0) &&
                position_normalize_uci(&after, r->ponder, h->chess960, ponder) && position_is_legal(&after, ponder)) { }
            else ponder[0] = 0;
        }
    }
    if (*ponder) printf("bestmove %s ponder %s\n", move, ponder);
    else printf("bestmove %s\n", move);
}
static void *search_worker(void *user) {
    Homemade *h = user;
    (void)pthread_mutex_lock(&h->mutex);
    for (;;) {
        while (!h->job_pending && !h->shutdown) (void)pthread_cond_wait(&h->condition, &h->mutex);
        if (h->shutdown) break;
        h->job_pending = 0;
        (void)pthread_mutex_unlock(&h->mutex);
        LbcSearchResultV1 result = {0};
        result.abi_version = LBC_PLUGIN_ABI_V1; result.struct_size = (uint32_t)sizeof(result);
        int64_t began = milliseconds();
        int code = h->builtin ? builtin_search(h, &result) : h->plugin->search(h->instance, &h->request, &result);
        if (!result.time_ms) result.time_ms = milliseconds() - began;
        (void)pthread_mutex_lock(&h->mutex);
        while (h->request.ponder && !h->released && !cancelled(h)) (void)pthread_cond_wait(&h->condition, &h->mutex);
        output_result(h, &result, code);
        h->running = 0;
        (void)pthread_cond_broadcast(&h->condition);
    }
    (void)pthread_mutex_unlock(&h->mutex);
    return NULL;
}
static void join_search(Homemade *h, int cancel) {
    if (!h->thread_created) return;
    (void)pthread_mutex_lock(&h->mutex);
    if (cancel) {
        atomic_store_explicit(&h->cancel, 1, memory_order_relaxed);
        h->released = 1;
        (void)pthread_cond_broadcast(&h->condition);
    }
    while (h->running) (void)pthread_cond_wait(&h->condition, &h->mutex);
    (void)pthread_mutex_unlock(&h->mutex);
}
static int is_running(Homemade *h) {
    (void)pthread_mutex_lock(&h->mutex); int running = h->running;
    (void)pthread_mutex_unlock(&h->mutex); return running;
}
static int64_t number(const char *word) {
    if (!word) return 0;
    errno = 0; char *end = NULL; long long n = strtoll(word, &end, 10);
    return errno || *end || n < 0 ? 0 : (int64_t)n;
}
static int go_keyword(const char *word) {
    static const char *const keys[] = { "wtime", "btime", "winc", "binc", "movetime", "nodes", "depth", "movestogo", "ponder", "infinite", "mate", "searchmoves" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) if (!strcmp(word, keys[i])) return 1;
    return 0;
}
static void start_search(Homemade *h, char *line) {
    if (is_running(h)) { fail(h, "go before previous search stopped"); return; }
    join_search(h, 0);
    if (!h->valid_position) { fail(h, "go without a valid position"); return; }
    LbcSearchV1 *q = &h->request; memset(q, 0, sizeof(*q));
    q->abi_version = LBC_PLUGIN_ABI_V1; q->struct_size = (uint32_t)sizeof(*q);
    q->variant = position_variant_name(h->variant); q->initial_fen = h->initial_fen;
    q->fen = h->fen; q->history = h->history; q->turn = h->position.turn; q->chess960 = h->chess960;
    q->game_json = *h->game_json ? h->game_json : NULL;
    q->state_json = *h->state_json ? h->state_json : NULL;
    q->options_json = *h->options_json ? h->options_json : NULL;
    q->legal_count = position_legal_moves(&h->position, h->legal, POSITION_MAX_MOVES);
    if (q->legal_count > POSITION_MAX_MOVES) { fail(h, "legal move capacity exceeded"); return; }
    /* C11 requires an explicit const cast for pointers to arrays. */
    q->legal_moves = (const char (*)[8])(const void *)h->legal;
    q->draw_offered = h->draw_offered;
    char *save = NULL;
    for (char *key = strtok_r(line, " ", &save); key; ) {
        int64_t *dst = NULL;
        if (!strcmp(key, "wtime")) dst = &q->white_ms;
        if (!strcmp(key, "btime")) dst = &q->black_ms;
        if (!strcmp(key, "winc")) dst = &q->white_increment_ms;
        if (!strcmp(key, "binc")) dst = &q->black_increment_ms;
        if (!strcmp(key, "movetime")) dst = &q->movetime_ms;
        if (!strcmp(key, "nodes")) dst = &q->nodes;
        if (dst) *dst = number(strtok_r(NULL, " ", &save));
        else if (!strcmp(key, "ponder")) q->ponder = 1;
        else if (!strcmp(key, "depth") || !strcmp(key, "movestogo")) {
            int depth = !strcmp(key, "depth"); int64_t n = number(strtok_r(NULL, " ", &save));
            if (n > INT_MAX) n = INT_MAX;
            if (depth) q->depth = (int)n; else q->moves_to_go = (int)n;
        } else if (!strcmp(key, "searchmoves")) {
            q->root_moves = (const char (*)[8])(const void *)h->roots;
            key = strtok_r(NULL, " ", &save);
            while (key && !go_keyword(key)) {
                char move[8];
                if (q->root_count >= POSITION_MAX_MOVES ||
                    !position_normalize_uci(&h->position, key, h->chess960, move) || !position_is_legal(&h->position, move)) {
                    fail(h, "invalid root move restriction"); return;
                }
                snprintf(h->roots[q->root_count++], sizeof(h->roots[0]), "%s", move);
                key = strtok_r(NULL, " ", &save);
            }
            continue;
        }
        key = strtok_r(NULL, " ", &save);
    }
    atomic_store_explicit(&h->cancel, 0, memory_order_relaxed);
    (void)pthread_mutex_lock(&h->mutex);
    h->released = 0; h->running = 1; h->job_pending = 1;
    (void)pthread_cond_broadcast(&h->condition);
    (void)pthread_mutex_unlock(&h->mutex);
}
static void parse_position(Homemade *h, char *line) {
    if (is_running(h)) { fail(h, "position while search active"); return; }
    join_search(h, 0);
    char *moves = strstr(line, " moves ");
    if (moves) { *moves = 0; moves += 7; }
    const char *fen = !strcmp(line, "startpos") ? "startpos" : !strncmp(line, "fen ", 4) ? line + 4 : NULL;
    if (!fen || strlen(fen) >= sizeof(h->initial_fen) || (moves && strlen(moves) >= sizeof(h->history))) {
        fail(h, "invalid or oversized position"); return;
    }
    char err[160];
    unsigned board_variant = h->chess960 && (h->variant == POSITION_STANDARD || h->variant == POSITION_FROM_POSITION)
        ? POSITION_CHESS960 : h->variant;
    if (!position_from_history(&h->position, board_variant, fen, moves ? moves : "", err, sizeof(err))) { fail(h, err); return; }
    if (h->chess960) h->position.chess960 = 1;
    if (!position_fen_ex(&h->position, h->fen, sizeof(h->fen), 1, h->chess960)) { fail(h, "could not encode current FEN"); return; }
    memcpy(h->initial_fen, fen, strlen(fen) + 1);
    snprintf(h->history, sizeof(h->history), "%s", moves ? moves : "");
    h->valid_position = 1;
}
static void set_option(Homemade *h, char *arg) {
    if (is_running(h)) { fail(h, "configure while search active"); return; }
    join_search(h, 0);
    char *value = strstr(arg, " value ");
    if (value) { *value = 0; value += 7; } else value = arg + strlen(arg);
    if (!strcmp(value, "<empty>")) *value = 0;
    if (!strcasecmp(arg, "UCI_Variant")) {
        unsigned variant = !strcmp(value, "chess") ? POSITION_STANDARD : position_variant_from_name(value);
        if (!variant) fail(h, "unknown chess variant"); else h->variant = variant;
    } else if (!strcasecmp(arg, "UCI_Chess960")) h->chess960 = !strcasecmp(value, "true");
    else if (!strcasecmp(arg, "Seed") && h->builtin) { uint64_t seed = (uint64_t)number(value); h->random_state = seed ? seed : 1; }
    else if (!strcasecmp(arg, "Ponder")) { /* go controls pondering. */ }
    else if (h->plugin && h->plugin->configure && h->plugin->configure(h->instance, arg, value)) fail(h, "homemade plugin rejected option");
    if (h->plugin && h->plugin->notify && !strcasecmp(arg, "UCI_Opponent")) h->plugin->notify(h->instance, "opponent", value);
}
static int load_plugin(Homemade *h, const char *path) {
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    static const char *const names[] = { "", "RandomMove", "Alphabetical", "FirstMove", "ComboEngine" };
    for (int i = 1; i <= 4; ++i) if (!strcmp(base, names[i])) { h->builtin = i; return 0; }
    h->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h->library) { fprintf(stderr, "lbc-homemade: %s\n", dlerror()); return -1; }
    void *symbol = dlsym(h->library, "lbc_engine_plugin_v1");
    LbcGetEnginePluginV1 get = NULL;
    if (sizeof(get) != sizeof(symbol)) return -1;
    memcpy(&get, &symbol, sizeof(get));
    if (!get) { fprintf(stderr, "lbc-homemade: missing lbc_engine_plugin_v1 export\n"); return -1; }
    h->plugin = get();
    if (!h->plugin || h->plugin->abi_version != LBC_PLUGIN_ABI_V1 ||
        h->plugin->struct_size < sizeof(LbcEnginePluginV1) || !h->plugin->search) {
        fprintf(stderr, "lbc-homemade: invalid plugin ABI or missing search callback\n"); h->plugin = NULL; return -1;
    }
    if (h->plugin->create) h->instance = h->plugin->create(&h->api);
    return 0;
}
int homemade_adapter_main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: lbc-homemade plugin.so|RandomMove|Alphabetical|FirstMove|ComboEngine\n"); return 2;
    }
    Homemade *h = calloc(1, sizeof(*h)); if (!h) return 1;
    h->variant = POSITION_STANDARD;
    h->random_state = (uint64_t)milliseconds() ^ UINT64_C(0x3141592653589793);
    atomic_init(&h->cancel, 0);
    h->api = (LbcEngineHostV1){ LBC_PLUGIN_ABI_V1, (uint32_t)sizeof(h->api), h, cancelled, engine_log };
    if (pthread_mutex_init(&h->mutex, NULL) || pthread_cond_init(&h->condition, NULL)) { free(h); return 1; }
    if (load_plugin(h, argv[1])) { if (h->library) (void)dlclose(h->library); free(h); return 1; }
    if (pthread_create(&h->thread, NULL, search_worker, h)) {
        fail(h, "could not create persistent search worker");
    } else h->thread_created = 1;
    setbuf(stdout, NULL);
    char *line = NULL; size_t capacity = 0;
    ssize_t length;
    while (!h->fatal && (length = getline(&line, &capacity, stdin)) >= 0) {
        if ((size_t)length > HOME_INPUT) { fail(h, "input command exceeds limit"); break; }
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = 0;
        if (!strcmp(line, "uci")) {
            printf("id name %s\nid author %s\n", h->builtin ? argv[1] : h->plugin->name ? h->plugin->name : "Native homemade",
                   h->builtin ? "lichess-bot-c" : h->plugin->author ? h->plugin->author : "User plugin");
            puts("option name UCI_Chess960 type check default false\noption name Ponder type check default false");
            puts("option name UCI_Variant type combo default chess var chess var antichess var atomic var crazyhouse var horde var kingOfTheHill var racingKings var threeCheck");
            puts("option name UCI_Opponent type string default none none human unknown");
            if (h->builtin) puts("option name Seed type spin default 1 min 1 max 2147483647");
            puts("uciok");
        } else if (!strcmp(line, "isready")) {
            (void)pthread_mutex_lock(&h->mutex); puts("readyok"); (void)pthread_mutex_unlock(&h->mutex);
        }
        else if (!strcmp(line, "quit")) break;
        else if (!strcmp(line, "stop")) join_search(h, 1);
        else if (!strcmp(line, "ponderhit")) {
            (void)pthread_mutex_lock(&h->mutex); h->released = 1;
            (void)pthread_cond_broadcast(&h->condition); (void)pthread_mutex_unlock(&h->mutex);
        } else if (!strcmp(line, "ucinewgame")) {
            join_search(h, 1); h->draw_offered = 0;
            if (h->plugin && h->plugin->new_game) h->plugin->new_game(h->instance, *h->game_json ? h->game_json : NULL);
        } else if (!strncmp(line, "position ", 9)) parse_position(h, line + 9);
        else if (!strncmp(line, "go", 2) && (!line[2] || line[2] == ' ')) start_search(h, line + 2);
        else if (!strncmp(line, "setoption name ", 15)) set_option(h, line + 15);
        else if (!strncmp(line, "lbc_draw_offer ", 15)) h->draw_offered = !strcmp(line + 15, "true");
        else if (!strncmp(line, "lbc_options ", 12)) {
            if (is_running(h)) { fail(h, "nested options while search active"); continue; }
            if (strlen(line + 12) >= sizeof(h->options_json)) { fail(h, "nested options exceed limit"); continue; }
            snprintf(h->options_json, sizeof(h->options_json), "%s", line + 12);
            if (h->plugin && h->plugin->notify) h->plugin->notify(h->instance, "options", h->options_json);
        }
        else if (!strncmp(line, "lbc_identity ", 13)) {
            if (is_running(h)) { fail(h, "identity while search active"); continue; }
            if (h->plugin && h->plugin->notify) h->plugin->notify(h->instance, "identity", line + 13);
        } else if (!strncmp(line, "lbc_state ", 10)) {
            if (is_running(h)) { fail(h, "state metadata while search active"); continue; }
            if (strlen(line + 10) >= sizeof(h->state_json)) { fail(h, "state metadata exceeds limit"); continue; }
            snprintf(h->state_json, sizeof(h->state_json), "%s", line + 10);
            if (h->plugin && h->plugin->notify) h->plugin->notify(h->instance, "state", h->state_json);
        }
        else if (!strncmp(line, "lbc_game ", 9)) {
            if (is_running(h)) { fail(h, "game metadata while search active"); continue; }
            if (strlen(line + 9) >= sizeof(h->game_json)) { fail(h, "game metadata exceeds limit"); continue; }
            snprintf(h->game_json, sizeof(h->game_json), "%s", line + 9);
            if (h->plugin && h->plugin->notify) h->plugin->notify(h->instance, "game", h->game_json);
        } else if (!strncmp(line, "lbc_result ", 11)) {
            join_search(h, 1);
            char *reason = strchr(line + 11, ' ');
            if (reason) *reason++ = 0;
            if (h->plugin && h->plugin->result) h->plugin->result(h->instance, line + 11, reason ? reason : "");
        }
    }
    join_search(h, 1); free(line);
    if (h->thread_created) {
        (void)pthread_mutex_lock(&h->mutex); h->shutdown = 1;
        (void)pthread_cond_broadcast(&h->condition); (void)pthread_mutex_unlock(&h->mutex);
        (void)pthread_join(h->thread, NULL);
    }
    if (h->plugin && h->plugin->destroy) h->plugin->destroy(h->instance);
    if (h->library) (void)dlclose(h->library);
    (void)pthread_cond_destroy(&h->condition); (void)pthread_mutex_destroy(&h->mutex);
    int result = h->fatal ? 1 : 0; free(h); return result;
}
