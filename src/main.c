#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "config.h"
#include "engine.h"
#include "json.h"
#include "logging.h"
#include "book.h"
#include "archive.h"
#include "evaluation.h"
#include "hooks.h"
#include "policy.h"
#include "position.h"
#include "providers.h"
#include "settings.h"
#include "tablebase.h"
#include "util.h"

#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#if LIBCURL_VERSION_NUM < 0x074200
#error "libcurl 7.66.0 or newer is required (curl_multi_poll)"
#endif

#define MOVES_CAP 65536
#define LINE_CAP 262144
#define JSON_TOKENS 4096
#define ACTION_CAP 256
#define STREAM_IDLE_US 30000000LL
#define SECOND 1000000LL

typedef struct App App;
typedef struct Slot Slot;
enum HttpKind { H_NONE, H_ACCOUNT, H_EVENTS, H_GAME, H_MOVE, H_ACCEPT, H_DECLINE,
    H_UPGRADE, H_PLAYING, H_CHAT, H_ABORT, H_RESIGN, H_DRAW, H_TAKEBACK,
    H_SOURCE, H_ONLINE_LIST, H_ONLINE_BOTS, H_PUBLIC_PROFILE, H_CHALLENGE, H_CANCEL,
    H_EXPORT, H_CUSTOM, H_STATUS };
enum UciPhase { U_OFF, U_HELLO, U_READY, U_IDLE, U_GAME_READY, U_SEARCH, U_PONDER, U_STOPPING };
enum PendingState { P_NONE, P_READY, P_INFLIGHT, P_CONFIRM, P_RESYNC, P_BLOCKED };

typedef struct {
    App *app;
    CURL *easy, *cached;
    enum HttpKind kind;
    int slot;
    int auxiliary, authenticated, action_index, ndjson, parsed;
    uint64_t epoch, revision, action_serial;
    long status;
    int64_t retry_after_us, last_data_us, opened_us;
    char id[33], post_data[4096], error[CURL_ERROR_SIZE], etag[512];
    struct curl_slist *private_headers;
    char body[LINE_CAP];
    size_t used;
} Http;

typedef struct {
    enum PendingState state;
    uint64_t revision;
    int attempts, offer_draw;
    int64_t due_us, best_us, go_us, state_us;
    char move[8];
} Pending;

typedef struct {
    int used, reserved, ended, failed, full, synced, reconnect;
    int variant, ours, initial_side, ply;
    int correspondence, unlimited, opponent_bot, rated, draw_offer, takeback_offer;
    int takebacks, takeback_answered, greeted, result_sent, abort_requested, position_valid, finish_pending, retiring;
    int sources_stage, sources_attempts, out_of_book, engine_resigned, engine_draw;
    int64_t initial_ms, created_at, abort_due_us, disconnect_due_us, source_due_us, min_think_due_us;
    int64_t move_delay_us, finish_due_us, terminate_due_us;
    uint64_t epoch, revision;
    uint64_t sources_revision;
    char id[33], fen[512], moves[MOVES_CAP];
    char names[2][129], titles[2][32], opponent[65], speed[32], result[16], status[40], tournament[65], pending_finish_status[40];
    int ratings[2];
    char *full_json, *state_json;
    int64_t clocks[2], increments[2], clock_us, changed_us, reservation_us;
    Position position;
    MoveInfo info, last_info;
    Annotations annotations;
    ProviderResult source_result;
    Pending pending;
    char prediction[8], prediction_base[MOVES_CAP];
} Game;

struct Slot {
    App *app;
    int index;
    Engine engine;
    enum UciPhase phase;
    int supports_960, supports_ponder, ever_ready, failures, stream_failures;
    int supported_variants, has_variant_option, supports_opponent;
    char variant_names[10][32], engine_name[129];
    int io_failed, stop_discard, job_draw_offer;
    char fault[160];
    uint64_t ready_epoch, job_epoch, job_revision;
    int64_t deadline_us, restart_us, stream_due_us, go_us, state_us;
    char target[MOVES_CAP], ponder_base[MOVES_CAP];
    Http stream;
    Game game;
};

typedef struct {
    int used;
    enum HttpKind kind;
    int slot, attempts;
    uint64_t epoch, serial;
    char id[33], reason[32];
    char path[2048], body[4096];
    int post, survive_game, priority, auxiliary;
    Game *archive_snapshot;
    int64_t due_us, created_us;
} Action;

typedef struct {
    char id[33], opponent[65];
    int correspondence, my_turn, bot, background;
    int64_t due_us, seconds_left;
} DeferredStart;

typedef struct Parked {
    Game game;
    int64_t due_us;
    struct Parked *next;
} Parked;

struct App {
    Config cfg;
    CURLM *multi;
    struct curl_slist *headers;
    Slot *slots;
    Http events, request, background;
    Action actions[ACTION_CAP];
    DeferredStart deferred_starts[ACTION_CAP];
    Parked *parked;
    Policy policy;
    Tablebases tablebases;
    BookCollection books[10];
    BookOptions book_options;
    TablebaseOptions syzygy_options, gaviota_options;
    LbcHooks hooks;
    Archive *archive;
    char *adapter_argv[MAX_ENGINE_ARGS + 3];
    char adapter_path[4096];
    char account[65];
    int authenticated, fatal, work, event_failures, draining, upgrades_attempted;
    int matchmaking_stage, tablebases_initialized, hooks_initialized, takebacks_pruned, playing_loaded;
    char outgoing_id[33];
    int64_t event_due_us, blocked_until_us, account_due_us, playing_due_us, matchmaking_due_us;
    int64_t challenge_created_us, last_game_ended_us, profile_due_us, post_move_due_us, status_due_us;
    int64_t source_blocked_until[PROVIDER_DONE];
    unsigned challenge_backoff_step, enabled_sources;
    int local_book_enabled, local_syzygy_enabled, local_gaviota_enabled, fake_think;
    int draw_policy_enabled;
    int64_t configured_move_delay_us;
    uint64_t next_epoch, next_action, finished;
    char completed_ids[128][33];
    unsigned completed_cursor;
};

static volatile sig_atomic_t interrupted;
static void signal_stop(int sig) { (void)sig; if (interrupted < 2) ++interrupted; }

static int64_t now_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (int64_t)ts.tv_sec * SECOND + ts.tv_nsec / 1000;
}

static void log_msg(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logging_vwrite(fmt, args);
    va_end(args);
}

static int valid_id(const char *id) {
    return lbc_id(id) && strlen(id) <= 32;
}

static int valid_move_n(const char *s, size_t n) {
    if (n == 4 && strchr("PNBRQKpnbrqk", s[0]) && s[1] == '@' && s[2] >= 'a' && s[2] <= 'h' && s[3] >= '1' && s[3] <= '8') return 1;
    return (n == 4 || n == 5) && s[0] >= 'a' && s[0] <= 'h' &&
        s[1] >= '1' && s[1] <= '8' && s[2] >= 'a' && s[2] <= 'h' &&
        s[3] >= '1' && s[3] <= '8' &&
        (n == 4 || s[4] == 'q' || s[4] == 'r' || s[4] == 'b' || s[4] == 'n' || s[4] == 'k') &&
        (s[0] != s[2] || s[1] != s[3]);
}

static int count_moves(const char *moves) {
    int ply = 0;
    const char *p = moves;
    while (*p) {
        const char *end = strchr(p, ' ');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (!valid_move_n(p, n)) return -1;
        ++ply;
        if (!end) break;
        p = end + 1;
        if (!*p) return -1;
    }
    return ply;
}

static int append_move(char *out, size_t cap, const char *history, const char *move) {
    size_t h = strlen(history), m = strlen(move);
    if (h + m + (h ? 1 : 0) >= cap) return -1;
    memcpy(out, history, h);
    if (h) out[h++] = ' ';
    memcpy(out + h, move, m + 1);
    return 0;
}

/* Cache finished game IDs: account and game streams can arrive out of order.
 * Exclude local retirement and correspondence check-ins. */
static int recently_completed(const App *a, const char *id) {
    for (size_t i = 0; i < sizeof(a->completed_ids) / sizeof(a->completed_ids[0]); ++i)
        if (!strcmp(a->completed_ids[i], id)) return 1;
    return 0;
}

static void remember_completed(App *a, const char *id) {
    if (!recently_completed(a, id)) {
        strcpy(a->completed_ids[a->completed_cursor], id);
        a->completed_cursor = (a->completed_cursor + 1u) % 128u;
    }
    for (int i = 0; i < ACTION_CAP; ++i)
        if (!strcmp(a->deferred_starts[i].id, id)) a->deferred_starts[i].id[0] = 0;
}

static int variant_value(const Json *j, int token) {
    char name[32];
    return json_string(j, token, name, sizeof(name)) >= 0 ? config_variant(name) : 0;
}

static int our_turn(const Game *g) { return (g->initial_side ^ (g->ply & 1)) == g->ours; }

static int fen_side(const char *fen) {
    if (!strcmp(fen, "startpos")) return 0;
    int fields = 1;
    for (const unsigned char *p = (const unsigned char *)fen; *p; ++p) {
        if (*p < 32 || *p == 127) return -1;
        if (*p == ' ') ++fields;
    }
    const char *side = strchr(fen, ' ');
    if ((fields != 6 && fields != 7) || !side || (side[1] != 'w' && side[1] != 'b') || side[2] != ' ') return -1;
    return side[1] == 'b';
}

static int engine_send(Slot *s, const char *command);
static void request_stop(Slot *s, int discard);
static void archive_game(App *a, const Game *g, int complete);
static void save_takebacks(App *a, const Game *g);

static char *raw_object(const Json *j, int token) {
    if (token < 0 || token >= j->count) return NULL;
    size_t n = (size_t)(j->tokens[token].end - j->tokens[token].start);
    char *raw = malloc(n + 1);
    if (raw) { memcpy(raw, j->text + j->tokens[token].start, n); raw[n] = 0; }
    return raw;
}

static void game_dispose(Game *g) {
    free(g->full_json); free(g->state_json); annotations_free(&g->annotations);
    memset(g, 0, sizeof(*g));
}

static Slot *find_game(App *a, const char *id) {
    for (int i = 0; i < a->cfg.concurrency; ++i)
        if (a->slots[i].game.used && !strcmp(a->slots[i].game.id, id)) return &a->slots[i];
    return NULL;
}

static int active_games(const App *a, int bots_only) {
    int count = 0;
    for (int i = 0; i < a->cfg.concurrency; ++i) {
        const Game *g = &a->slots[i].game;
        if (g->used && !g->ended && (!bots_only || g->opponent_bot)) ++count;
    }
    return count;
}

static int opponent_engagements(const App *a, const char *name, const char *except) {
    int count = 0;
    for (int i = 0; i < a->cfg.concurrency; ++i) {
        const Game *g = &a->slots[i].game;
        if (g->used && !g->ended && !strcasecmp(g->opponent, name) && (!except || strcmp(g->id, except))) ++count;
    }
    for (const Parked *p = a->parked; p; p = p->next)
        if (!strcasecmp(p->game.opponent, name) && (!except || strcmp(p->game.id, except))) ++count;
    for (size_t i = 0; i < POLICY_QUEUE_CAP; ++i) {
        const Challenge *c = &a->policy.queue[i];
        if (c->used && !strcasecmp(c->opponent, name) && (!except || strcmp(c->id, except))) ++count;
    }
    for (size_t i = 0; i < ACTION_CAP; ++i) {
        const DeferredStart *d = &a->deferred_starts[i];
        if (*d->id && !strcasecmp(d->opponent, name) && (!except || strcmp(d->id, except))) ++count;
    }
    return count;
}

static Slot *new_game(App *a, const char *id, int reserved) {
    Slot *existing = find_game(a, id);
    if (existing) return existing;
    for (int i = 0; i < a->cfg.concurrency; ++i) {
        Slot *s = &a->slots[i];
        if (s->game.used) continue;
        game_dispose(&s->game);
        Parked **at = &a->parked;
        while (*at && strcmp((*at)->game.id, id)) at = &(*at)->next;
        if (*at) {
            Parked *old = *at; *at = old->next;
            s->game = old->game; free(old);
            s->game.synced = 0; s->game.reconnect = 0;
            memset(&s->game.pending, 0, sizeof(s->game.pending));
        }
        s->game.used = 1; s->game.reserved = reserved;
        s->game.epoch = ++a->next_epoch;
        if (!s->game.revision) s->game.revision = 1;
        s->game.reservation_us = now_us() + 30 * SECOND;
        strcpy(s->game.id, id);
        s->game.sources_revision = 0;
        s->stream_failures = 0; s->stream_due_us = now_us();
        a->work = 1;
        return s;
    }
    return NULL;
}

static Action *queue_request(App *a, enum HttpKind kind, const char *path, const char *body,
                             Slot *s, int survive_game, int priority) {
    for (int i = 0; i < ACTION_CAP; ++i) {
        Action *act = &a->actions[i];
        if (act->used && act->kind == kind && !strcmp(act->path, path) && !strcmp(act->body, body ? body : "")) return act;
    }
    for (int i = 0; i < ACTION_CAP; ++i) {
        Action *act = &a->actions[i];
        if (act->used) continue;
        memset(act, 0, sizeof(*act));
        if (lbc_copy(act->path, sizeof(act->path), path) || lbc_copy(act->body, sizeof(act->body), body ? body : "")) return NULL;
        act->used = 1; act->kind = kind; act->post = body != NULL; act->serial = ++a->next_action;
        act->slot = s ? s->index : -1; act->epoch = s ? s->game.epoch : 0;
        if (s) strcpy(act->id, s->game.id);
        act->survive_game = survive_game; act->priority = priority;
        act->due_us = act->created_us = now_us(); a->work = 1; return act;
    }
    log_msg("API action queue full"); return NULL;
}

static void add_action(App *a, enum HttpKind kind, const char *id, const char *reason, Slot *s) {
    char path[128], body[96];
    (void)snprintf(path, sizeof(path), "/api/challenge/%s/%s", id, kind == H_ACCEPT ? "accept" : "decline");
    (void)snprintf(body, sizeof(body), "reason=%s", reason ? reason : "generic");
    Action *act = queue_request(a, kind, path, kind == H_DECLINE ? body : "", s, kind == H_DECLINE, 3);
    if (act) { strcpy(act->id, id); if (reason) (void)lbc_copy(act->reason, sizeof(act->reason), reason); }
    else if (s && s->game.reserved) s->game.ended = 1;
}

static void game_action(Slot *s, enum HttpKind kind, const char *suffix, int survive) {
    char path[192];
    (void)snprintf(path, sizeof(path), "/api/bot/game/%s/%s", s->game.id, suffix);
    (void)queue_request(s->app, kind, path, "", s, survive, kind == H_CHAT ? 5 : 1);
}

static void chat_send(Slot *s, const char *room, const char *message, int survive) {
    if (!*message) return;
    char text[561], body[2048] = "", path[128];
    size_t n = 0; int characters = 0;
    while (message[n] && n < sizeof(text) - 1) {
        unsigned char c = (unsigned char)message[n];
        if ((c & 0xc0) != 0x80 && ++characters > 140) break;
        text[n] = c < 32 || c == 127 ? ' ' : (char)c; ++n;
    }
    text[n] = 0;
    if (lbc_form_add(body, sizeof(body), "room", room) || lbc_form_add(body, sizeof(body), "text", text)) return;
    (void)snprintf(path, sizeof(path), "/api/bot/game/%s/chat", s->game.id);
    (void)queue_request(s->app, H_CHAT, path, body, s, survive, 5);
}

static void greeting(Slot *s, const char *key, const char *room, int survive) {
    char path[96], text[2048] = "";
    (void)snprintf(path, sizeof(path), "greeting.%s", key);
    const char *p = settings_str(s->app->cfg.settings, path, ""); size_t used = 0;
    while (*p && used + 1 < sizeof(text)) {
        if (p[0] == '{' && p[1] == '{') { text[used++] = '{'; p += 2; continue; }
        if (p[0] == '}' && p[1] == '}') { text[used++] = '}'; p += 2; continue; }
        if (*p == '{') {
            const char *end = strchr(p, '}');
            if (!end) break;
            const char *value = "";
            if (end-p == 3 && !memcmp(p, "{me", 3)) value = s->game.names[s->game.ours];
            else if (end-p == 9 && !memcmp(p, "{opponent", 9)) value = s->game.names[!s->game.ours];
            size_t n = strlen(value);
            if (n >= sizeof(text) - used) break;
            memcpy(text + used, value, n); used += n; p = end + 1;
        } else text[used++] = *p++;
    }
    text[used] = 0; chat_send(s, room, text, survive);
}

static int takeback_path(App *a, const char *id, char *out, size_t cap) {
    const char *dir = settings_str(a->cfg.settings, "state_directory", "lichess_bot_auto_logs");
    return lbc_format(out, cap, "%s/takeback-count-%s.txt", dir, id);
}
static void read_takebacks(App *a, Game *g) {
    char path[4096]; if (takeback_path(a, g->id, path, sizeof(path))) return;
    FILE *file = fopen(path, "r"); if (!file) return;
    long n = 0; if (fscanf(file, "%ld", &n) == 1 && n >= 0 && n <= INT_MAX) g->takebacks = (int)n;
    (void)fclose(file);
}
static void save_takebacks(App *a, const Game *g) {
    char path[4096], count[32]; if (takeback_path(a, g->id, path, sizeof(path))) return;
    if (g->ended) { (void)unlink(path); return; }
    if (lbc_mkdirs(settings_str(a->cfg.settings, "state_directory", "lichess_bot_auto_logs"))) { log_msg("cannot create takeback record directory"); return; }
    int n = snprintf(count, sizeof(count), "%d\n", g->takebacks);
    if (n < 0 || lbc_atomic_write(path, count, (size_t)n)) log_msg("cannot save takeback count for %s", g->id);
}

static void finish_game(Slot *s, const char *status) {
    Game *g = &s->game; App *a = s->app;
    if (!g->used || g->ended) return;
    g->ended = 1; g->synced = 0; g->finish_pending = 0; g->retiring = 0;
    remember_completed(a, g->id);
    (void)lbc_copy(g->status, sizeof(g->status), status);
    if (!*g->result) (void)lbc_copy(g->result, sizeof(g->result), !strcmp(status, "draw") || !strcmp(status, "stalemate") || !strcmp(status, "timeout") ? "1/2-1/2" : "*");
    ++a->finished; a->last_game_ended_us = now_us();
    a->matchmaking_due_us = a->last_game_ended_us + lbc_seconds_us(settings_double(a->cfg.settings, "matchmaking.challenge_timeout", 30) * 60);
    if (g->full) {
        greeting(s, "goodbye", "player", 1); greeting(s, "goodbye_spectators", "spectator", 1);
        archive_game(a, g, 1); save_takebacks(a, g);
    }
    a->work = 1;
    log_msg("game %s finished: %s (%s)", g->id, status, g->result);
}

static void challenge(App *a, const Json *j, int object) {
    Challenge c;
    if (policy_challenge_parse(&c, j, object, now_us()) || !valid_id(c.id)) return;
    if (!strcasecmp(c.opponent, a->account) || find_game(a, c.id)) return;
    for (size_t i = 0; i < POLICY_QUEUE_CAP; ++i)
        if (a->policy.queue[i].used && !strcmp(a->policy.queue[i].id, c.id)) return;
    const char *reason = a->draining ? "later" : policy_challenge_reason(&a->policy, &a->cfg, &c, opponent_engagements(a, c.opponent, c.id), now_us());
    if (!reason) {
        int supported = 0;
        for (int i = 0; i < a->cfg.concurrency; ++i) supported |= a->slots[i].supported_variants;
        if (!(supported & c.variant)) reason = "variant";
        if (!reason && strcmp(c.fen, "startpos")) {
            Position p;
            if (!position_init(&p, (unsigned)c.variant, c.fen, NULL, 0) ||
                (p.chess960 && (!(supported & VAR_CHESS960) || !(a->cfg.variants & VAR_CHESS960)))) reason = "variant";
        }
    }
    if (!reason && a->hooks_initialized) {
        char *raw = raw_object(j, object);
        if (!raw || hooks_challenge(&a->hooks, raw) <= 0) reason = "generic";
        free(raw);
    }
    c.admitted = 1;
    if (!reason && policy_queue_add(&a->policy, &c)) reason = "later";
    if (reason) add_action(a, H_DECLINE, c.id, reason, NULL);
    else {
        policy_accepted(&a->policy, &a->cfg, &c, now_us());
        log_msg("queued challenge %s from %s", c.id, c.opponent); a->work = 1;
    }
}

static void accept_challenges(App *a) {
    if (a->draining || !a->cfg.accept) return;
    for (size_t i = 0; i < a->policy.list_count; ++i)
        if ((a->policy.lists[i].owners & 1) && !a->policy.lists[i].initialized) return;
    while (active_games(a, 0) < a->cfg.concurrency) {
        int reserved = (int)settings_int(a->cfg.settings, "challenge.games_reserved_for_humans", 0);
        Challenge *c = policy_queue_best(&a->policy, &a->cfg, a->cfg.concurrency - reserved - active_games(a, 1), now_us());
        if (!c) return;
        const char *reason = policy_challenge_reason(&a->policy, &a->cfg, c, opponent_engagements(a, c->opponent, c->id), now_us());
        if (reason) { add_action(a, H_DECLINE, c->id, reason, NULL); c->used = 0; continue; }
        Slot *s = new_game(a, c->id, 1); if (!s) return;
        s->game.opponent_bot = c->bot; s->game.correspondence = c->initial < 0; s->game.unlimited = c->days < 0 && c->initial < 0;
        strcpy(s->game.opponent, c->opponent);
        add_action(a, H_ACCEPT, c->id, NULL, s);
        c->used = 0;
        log_msg("accepting challenge %s", s->game.id);
    }
}

static int apply_history_suffix(Position *position, const char *moves) {
    const char *p = moves;
    while (*p) {
        size_t n = strcspn(p, " "); char move[8];
        if (!n || n >= sizeof(move)) return 0;
        memcpy(move, p, n); move[n] = 0;
        if (!position_apply_uci(position, move, NULL, 0)) return 0;
        p += n; if (*p == ' ') ++p;
    }
    return 1;
}

static int history_common_plies(const char *a, const char *b) {
    int count = 0;
    while (*a && *b) {
        size_t na = strcspn(a, " "), nb = strcspn(b, " ");
        if (na != nb || memcmp(a, b, na)) break;
        ++count; a += na; b += nb;
        if (*a == ' ') ++a;
        if (*b == ' ') ++b;
    }
    return count;
}

static void state_update(Slot *s, const Json *j, int object, int full) {
    Game *g = &s->game; App *a = s->app;
    char moves[MOVES_CAP], status[40];
    if (json_string(j, json_get(j, object, "status"), status, sizeof(status)) < 0 ||
        json_string(j, json_get(j, object, "moves"), moves, sizeof(moves)) < 0) goto invalid;
    int terminal = strcmp(status, "started") && strcmp(status, "created");
    if (terminal) {
        static const char *const endings[] = {"aborted", "mate", "resign", "stalemate", "timeout", "draw",
            "outoftime", "cheat", "noStart", "unknownFinish", "variantEnd"};
        int known = 0;
        for (size_t i = 0; i < sizeof(endings) / sizeof(*endings); ++i) if (!strcmp(status, endings[i])) known = 1;
        if (!known) goto invalid;
    }
    int ply = count_moves(moves);
    if (ply < 0) goto invalid;
    int64_t clocks[2], increments[2];
    static const char *const timekeys[] = {"wtime", "btime"}, *const inckeys[] = {"winc", "binc"};
    for (int i = 0; i < 2; ++i) {
        clocks[i] = lbc_json_int(j, object, timekeys[i], g->unlimited ? INT64_C(315360000000) : terminal ? g->clocks[i] : -1);
        increments[i] = lbc_json_int(j, object, inckeys[i], g->correspondence || terminal ? 0 : -1);
        if (clocks[i] < 0 || clocks[i] > INT64_C(315360000000) || increments[i] < 0 || increments[i] > INT_MAX) goto invalid;
    }
    int64_t now = now_us();
    int changed = strcmp(g->moves, moves) != 0;
    if (changed || !g->position_valid) {
        size_t oldlen = strlen(g->moves);
        int appended = !strncmp(moves, g->moves, oldlen) && (!oldlen || moves[oldlen] == ' ' || !moves[oldlen]);
        Position next;
        int valid;
        if (g->position_valid && appended) {
            next = g->position;
            const char *suffix = moves + oldlen; if (*suffix == ' ') ++suffix;
            valid = apply_history_suffix(&next, suffix);
        } else valid = position_from_history(&next, (unsigned)g->variant, g->fen, moves, NULL, 0);
        if (!valid) goto invalid;
        g->position = next; g->position_valid = 1;
        if (changed) {
            if (!appended) annotations_truncate(&g->annotations, history_common_plies(g->moves, moves));
            else if (g->pending.state != P_NONE && oldlen < strlen(moves)) {
                const char *nextmove = moves + oldlen + (oldlen ? 1 : 0);
                size_t n = strcspn(nextmove, " ");
                if (strlen(g->pending.move) != n || memcmp(g->pending.move, nextmove, n)) annotations_truncate(&g->annotations, g->ply);
            }
            strcpy(g->moves, moves); g->ply = ply; ++g->revision;
            memset(&g->pending, 0, sizeof(g->pending));
            g->changed_us = now; g->sources_revision = 0;
            g->engine_resigned = 0; g->engine_draw = 0;
        }
    }
    if (!g->changed_us) g->changed_us = now;
    memcpy(g->clocks, clocks, sizeof(clocks)); memcpy(g->increments, increments, sizeof(increments));
    g->clock_us = now;
    g->synced = !strcmp(status, "started") && !g->finish_pending && !g->retiring;
    (void)lbc_copy(g->status, sizeof(g->status), status);
    if (json_eq(j, json_get(j, object, "winner"), "white")) strcpy(g->result, "1-0");
    else if (json_eq(j, json_get(j, object, "winner"), "black")) strcpy(g->result, "0-1");
    else if (terminal && (!strcmp(status, "draw") || !strcmp(status, "stalemate") || !strcmp(status, "timeout"))) strcpy(g->result, "1/2-1/2");
    if (strcmp(a->cfg.protocol, "uci") || a->hooks_initialized) {
        char *raw = raw_object(j, object);
        if (raw) { free(g->state_json); g->state_json = raw; }
    }
    if (a->hooks_initialized && g->state_json) hooks_event(&a->hooks, "gameState", g->id, g->state_json);
    if (terminal) { finish_game(s, status); return; }
    if (full) {
        if (g->pending.state == P_RESYNC && g->pending.revision == g->revision) {
            if (g->pending.attempts >= 3) {
                g->pending.state = P_BLOCKED;
                log_msg("game %s: move retries exhausted; waiting for position change", g->id);
            } else g->pending.state = P_READY;
        }
        if (g->pending.state == P_CONFIRM) g->pending.due_us = now + 5 * SECOND;
    }
    g->draw_offer = lbc_json_bool(j, object, g->ours ? "wdraw" : "bdraw", 0);
    g->takeback_offer = lbc_json_bool(j, object, g->ours ? "wtakeback" : "btakeback", 0);
    if (!g->takeback_offer) g->takeback_answered = 0;
    if (g->takeback_offer && !g->takeback_answered && !our_turn(g)) {
        int accept = g->takebacks < settings_int(a->cfg.settings, "max_takebacks_accepted", 0);
        game_action(s, H_TAKEBACK, accept ? "takeback/yes" : "takeback/no", 0);
        g->takeback_answered = 1;
    }
    /* Renew deadlines on game states, not stream keepalives. */
    if (g->ply < 2) g->abort_due_us = now + lbc_seconds_us(settings_double(a->cfg.settings, "abort_time", 20));
    g->disconnect_due_us = now + lbc_seconds_us(settings_double(a->cfg.settings, "correspondence.disconnect_time", 300));
    int active = g->initial_side ^ (g->ply & 1);
    g->terminate_due_us = now + (g->clocks[active] + g->increments[active]) * 1000 + 60 * SECOND;
    a->work = 1;
    return;
invalid:
    log_msg("game %s: invalid or oversized game state; reconnecting", g->id);
    g->synced = 0; g->reconnect = 1; a->work = 1;
}

static void chat_message(Slot *s, const Json *j) {
    char name[129], room[32], text[4096];
    if (lbc_json_string(j, 0, "username", name, sizeof(name), NULL) ||
        lbc_json_string(j, 0, "room", room, sizeof(room), NULL) ||
        lbc_json_string(j, 0, "text", text, sizeof(text), NULL)) return;
    if (strcmp(room, "player") && strcmp(room, "spectator")) return;
    if (s->app->cfg.verbose) log_msg("chat %s [%s] %s: %s", s->game.id, room, name, text);
    if (s->app->hooks_initialized) {
        char *raw = raw_object(j, 0);
        if (raw) { hooks_event(&s->app->hooks, "chatLine", s->game.id, raw); free(raw); }
    }
    if (*text != '!') return;
    for (char *p = text; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 'a' - 'A');
    char reply[2048]; *reply = 0;
    if (!strcmp(text, "!help") || !strcmp(text, "!commands"))
        strcpy(reply, "Commands: !wait, !name, !eval, !queue");
    else if (!strcmp(text, "!wait") && s->game.ply < 2) {
        s->game.abort_due_us = now_us() + 60 * SECOND;
        s->game.disconnect_due_us = now_us() + 120 * SECOND;
        s->game.terminate_due_us = now_us() + 120 * SECOND;
        strcpy(reply, "Waiting 60 seconds.");
    } else if (!strcmp(text, "!name"))
        (void)snprintf(reply, sizeof(reply), "%s running %s (lichess-bot-c v%s)", s->app->policy.profile.name, s->engine_name, LBC_VERSION);
    else if (!strncmp(text, "!eval", 5)) {
        if (!strcasecmp(name, s->app->account) || !strcmp(room, "spectator"))
            (void)evaluation_string(&s->game.last_info, reply, sizeof(reply), 1);
        else strcpy(reply, "Evaluation is available to spectators and the bot owner.");
    } else if (!strcmp(text, "!queue")) {
        size_t used = 0;
        for (size_t i = 0; i < POLICY_QUEUE_CAP; ++i) if (s->app->policy.queue[i].used) {
            const char *opponent = s->app->policy.queue[i].opponent;
            int n = snprintf(reply + used, sizeof(reply) - used, "%s@%s", used ? ", " : "", opponent);
            if (n < 0 || (size_t)n >= sizeof(reply) - used) break;
            used += (size_t)n;
        }
        if (!used) strcpy(reply, "No challenges queued.");
    }
    chat_send(s, room, reply, 0);
}

static void game_message(Slot *s, const Json *j) {
    Game *g = &s->game; App *a = s->app;
    int type = json_get(j, 0, "type");
    if (json_eq(j, type, "gameFull")) {
        char id[33], white[65], black[65], fen[512];
        int variant = variant_value(j, json_get(j, json_get(j, 0, "variant"), "key"));
        if (json_string(j, json_get(j, 0, "id"), id, sizeof(id)) < 0 || strcmp(id, g->id) ||
            json_string(j, json_get(j, 0, "initialFen"), fen, sizeof(fen)) < 0 || fen_side(fen) < 0 ||
            !variant) goto unsupported;
        int players[2] = {json_get(j, 0, "white"), json_get(j, 0, "black")};
        int have_white = json_string(j, json_get(j, players[0], "id"), white, sizeof(white));
        int have_black = json_string(j, json_get(j, players[1], "id"), black, sizeof(black));
        int ours = -1;
        if (have_white >= 0 && !strcasecmp(white, a->account)) ours = 0;
        if (have_black >= 0 && !strcasecmp(black, a->account)) ours = 1;
        if (ours < 0) goto unsupported;
        if (g->full && (strcmp(g->fen, fen) || variant != g->variant || ours != g->ours)) goto unsupported;
        int is_new = !g->full;
        strcpy(g->fen, fen); g->initial_side = fen_side(fen);
        g->variant = variant; g->ours = ours; g->full = 1; g->reconnect = 0;
        for (int i = 0; i < 2; ++i) {
            (void)lbc_json_string(j, players[i], "name", g->names[i], sizeof(g->names[i]), i ? "Black" : "White");
            (void)lbc_json_string(j, players[i], "title", g->titles[i], sizeof(g->titles[i]), "");
            g->ratings[i] = (int)lbc_json_int(j, players[i], "rating", 0);
        }
        (void)lbc_json_string(j, players[!ours], "id", g->opponent, sizeof(g->opponent), "");
        g->opponent_bot = !strcmp(g->titles[!ours], "BOT") || json_get(j, players[!ours], "aiLevel") >= 0;
        (void)lbc_json_string(j, 0, "speed", g->speed, sizeof(g->speed), "blitz");
        g->correspondence = !strcmp(g->speed, "correspondence");
        int clock = json_get(j, 0, "clock");
        g->unlimited = g->correspondence && json_get(j, 0, "daysPerTurn") < 0;
        /* Correspondence gameFull may omit daysPerTurn; use the state clocks. */
        if (g->correspondence && lbc_json_int(j, json_get(j, 0, "state"), "wtime", -1) >= 0) g->unlimited = 0;
        g->initial_ms = lbc_json_int(j, clock, "initial", INT64_C(315360000000));
        g->rated = lbc_json_bool(j, 0, "rated", 0); g->created_at = lbc_json_int(j, 0, "createdAt", (int64_t)time(NULL) * 1000);
        (void)lbc_json_string(j, 0, "tournamentId", g->tournament, sizeof(g->tournament), "");
        char *raw = raw_object(j, 0);
        if (raw) { free(g->full_json); g->full_json = raw; }
        if (is_new) {
            read_takebacks(a, g);
            log_msg("game %s: %s, variant=%s, opponent=%s", g->id, ours ? "black" : "white", config_variant_name(variant), g->opponent);
            if (a->hooks_initialized && raw) hooks_event(&a->hooks, "gameFull", g->id, raw);
        }
        state_update(s, j, json_get(j, 0, "state"), 1);
    } else if (json_eq(j, type, "gameState") && g->full) state_update(s, j, 0, 0);
    else if (json_eq(j, type, "chatLine") && g->full) chat_message(s, j);
    else if (json_eq(j, type, "opponentGone") && a->hooks_initialized) {
        char *raw = raw_object(j, 0);
        if (raw) { hooks_event(&a->hooks, "opponentGone", g->id, raw); free(raw); }
    }
    return;
unsupported:
    log_msg("game %s: unsupported variant, FEN or account; no moves will be submitted", g->id);
    g->failed = 1; g->synced = 0;
}

static void defer_game(App *a, const Json *j, int object, const char *id, int64_t due, int discovery) {
    if (recently_completed(a, id)) return;
    for (int i = 0; i < ACTION_CAP; ++i) {
        DeferredStart *d = &a->deferred_starts[i];
        if (*d->id && strcmp(d->id, id)) continue;
        int existing = *d->id != 0;
        if (!existing) { memset(d, 0, sizeof(*d)); strcpy(d->id, id); }
        d->correspondence = json_eq(j, json_get(j, object, "speed"), "correspondence");
        d->my_turn = lbc_json_bool(j, object, "isMyTurn", 1);
        d->seconds_left = lbc_json_int(j, object, "secondsLeft", INT64_MAX/2);
        (void)lbc_json_string(j, json_get(j, object, "opponent"), "id", d->opponent, sizeof(d->opponent), "");
        if (!*d->opponent) (void)lbc_json_string(j, json_get(j, object, "opponent"), "username", d->opponent, sizeof(d->opponent), "");
        d->bot = json_eq(j, json_get(j, json_get(j, object, "opponent"), "title"), "BOT");
        if (!existing) {
            d->due_us = due;
            double checkin = settings_double(a->cfg.settings, "correspondence.checkin_period", 600);
            double minimum = 10 * (checkin + settings_double(a->cfg.settings, "correspondence.move_time", 60));
            if (discovery && d->correspondence && (!d->my_turn || (double)d->seconds_left > minimum)) {
                d->background = 1; d->due_us += lbc_seconds_us(checkin);
            }
        }
        a->work = 1; return;
    }
    log_msg("pending games queue full; cannot admit %s", id);
}

static void event_message(App *a, const Json *j) {
    int type = json_get(j, 0, "type");
    if (json_eq(j, type, "challenge")) { challenge(a, j, json_get(j, 0, "challenge")); return; }
    int start = json_eq(j, type, "gameStart"), finish = json_eq(j, type, "gameFinish");
    char id[33];
    int object = json_get(j, 0, start || finish ? "game" : "challenge");
    if (json_string(j, json_get(j, object, "id"), id, sizeof(id)) < 0 || !valid_id(id)) return;
    Slot *s = find_game(a, id);
    if (start) {
        policy_queue_remove(&a->policy, id);
        if (!strcmp(a->outgoing_id, id)) a->outgoing_id[0] = 0;
        if (!s) {
            int parked = 0;
            for (Parked *p = a->parked; p; p = p->next) if (!strcmp(p->game.id, id)) {
                parked = 1;
                if (lbc_json_bool(j, object, "isMyTurn", 0)) p->due_us = now_us();
                break;
            }
            if (!parked) defer_game(a, j, object, id, now_us(), 0);
        } else s->game.reserved = 0;
        for (int i = 0; i < ACTION_CAP; ++i)
            if (a->actions[i].used && (a->actions[i].kind == H_ACCEPT || a->actions[i].kind == H_DECLINE) && !strcmp(a->actions[i].id, id)) a->actions[i].used = 0;
        a->work = 1;
    } else if (finish) {
        if (s) {
            if (json_eq(j, json_get(j, object, "winner"), "white")) strcpy(s->game.result, "1-0");
            else if (json_eq(j, json_get(j, object, "winner"), "black")) strcpy(s->game.result, "0-1");
            char status[40]; (void)lbc_json_string(j, json_get(j, object, "status"), "name", status, sizeof(status), "gameFinish");
            if (s->game.full && !s->game.ended) {
                if (!s->game.finish_pending) s->game.finish_due_us = now_us() + 2 * SECOND;
                s->game.finish_pending = 1; s->game.synced = 0;
                (void)lbc_copy(s->game.pending_finish_status, sizeof(s->game.pending_finish_status), status);
                a->work = 1;
            } else finish_game(s, status);
        }
        Parked **at = &a->parked;
        while (*at) {
            Parked *p = *at;
            if (strcmp(p->game.id, id)) { at = &p->next; continue; }
            p->game.ended = 1;
            remember_completed(a, id);
            if (json_eq(j, json_get(j, object, "winner"), "white")) strcpy(p->game.result, "1-0");
            else if (json_eq(j, json_get(j, object, "winner"), "black")) strcpy(p->game.result, "0-1");
            (void)lbc_json_string(j, json_get(j, object, "status"), "name", p->game.status, sizeof(p->game.status), "gameFinish");
            if (!*p->game.result) (void)lbc_copy(p->game.result, sizeof(p->game.result),
                !strcmp(p->game.status, "draw") || !strcmp(p->game.status, "stalemate") || !strcmp(p->game.status, "timeout") ? "1/2-1/2" : "*");
            ++a->finished; a->last_game_ended_us = now_us();
            a->matchmaking_due_us = a->last_game_ended_us + lbc_seconds_us(settings_double(a->cfg.settings, "matchmaking.challenge_timeout", 30) * 60);
            archive_game(a, &p->game, 1); save_takebacks(a, &p->game);
            *at = p->next; game_dispose(&p->game); free(p); break;
        }
        for (int i = 0; i < ACTION_CAP; ++i) if (!strcmp(a->deferred_starts[i].id, id)) a->deferred_starts[i].id[0] = 0;
    } else if (json_eq(j, type, "challengeCanceled") || json_eq(j, type, "challengeDeclined")) {
        if (s && s->game.reserved) s->game.ended = 1;
        policy_queue_remove(&a->policy, id);
        if (!strcmp(a->outgoing_id, id)) a->outgoing_id[0] = 0;
        if (json_eq(j, type, "challengeDeclined")) policy_match_declined(&a->policy, &a->cfg, j, object, now_us());
        for (int i = 0; i < ACTION_CAP; ++i)
            if (a->actions[i].used && (a->actions[i].kind == H_ACCEPT || a->actions[i].kind == H_DECLINE) && !strcmp(a->actions[i].id, id)) a->actions[i].used = 0;
        a->work = 1;
    }
    if (a->hooks_initialized) {
        char kind[40]; (void)lbc_json_string(j, 0, "type", kind, sizeof(kind), "event");
        char *raw = raw_object(j, 0);
        if (raw) { hooks_event(&a->hooks, kind, id, raw); free(raw); }
    }
}

static void stream_line(Http *h, const char *line, size_t length) {
    if (length && line[length - 1] == '\r') --length;
    if (!length) return;
    JsonTok tokens[JSON_TOKENS]; Json j;
    if (json_parse(&j, line, length, tokens, JSON_TOKENS) || tokens[0].type != JSON_OBJECT) {
        if (h->kind == H_EVENTS || h->kind == H_GAME) log_msg("invalid JSON on %s stream", h->kind == H_EVENTS ? "event" : "game");
        if (h->kind == H_GAME) {
            Game *g = &h->app->slots[h->slot].game;
            g->reconnect = 1; g->synced = 0;
        }
        return;
    }
    if (h->kind == H_EVENTS) event_message(h->app, &j);
    else if (h->kind == H_ONLINE_BOTS) {
        char name[65];
        (void)lbc_json_string(&j, 0, "id", name, sizeof(name), "");
        if (!*name) (void)lbc_json_string(&j, 0, "username", name, sizeof(name), "");
        policy_match_consider(&h->app->policy, &h->app->cfg, &j, 0, opponent_engagements(h->app, name, NULL) > 0, now_us());
    } else if (h->kind == H_SOURCE && h->slot >= 0) {
        Game *g = &h->app->slots[h->slot].game;
        if (g->used && !g->ended && g->epoch == h->epoch && g->revision == h->revision) {
            ProviderResult value;
            if (provider_parse(&h->app->cfg, (enum ProviderKind)h->auxiliary, &g->position, &j, &h->app->policy.random, &value)) {
                g->source_result = value; h->parsed = 1;
            }
        }
    } else if (h->kind == H_GAME) {
        Slot *s = &h->app->slots[h->slot];
        if (s->game.used && s->game.epoch == h->epoch && !s->game.ended) game_message(s, &j);
    }
}

static size_t receive_data(char *data, size_t size, size_t count, void *user) {
    Http *h = user;
    if (size && count > SIZE_MAX / size) return 0;
    size_t total = size * count;
    h->last_data_us = now_us();
    if ((!h->ndjson && h->kind != H_EVENTS && h->kind != H_GAME) || h->status < 200 || h->status >= 300) {
        if (total >= sizeof(h->body) - h->used) return 0;
        memcpy(h->body + h->used, data, total); h->used += total; h->body[h->used] = 0;
        return total;
    }
    size_t left = total;
    while (left) {
        char *newline = memchr(data, '\n', left);
        size_t n = newline ? (size_t)(newline - data) : left;
        if (n >= sizeof(h->body) - h->used) return 0;
        memcpy(h->body + h->used, data, n); h->used += n;
        if (!newline) break;
        h->body[h->used] = 0;
        stream_line(h, h->body, h->used); h->used = 0;
        data += n + 1; left -= n + 1;
    }
    return total;
}

static size_t receive_header(char *data, size_t size, size_t count, void *user) {
    Http *h = user;
    if (size && count > SIZE_MAX / size) return 0;
    size_t n = size * count;
    h->last_data_us = now_us();
    if (n >= 5 && !memcmp(data, "HTTP/", 5)) {
        const char *space = memchr(data, ' ', n);
        if (space && data + n - space >= 4) {
            char code[4] = {space[1], space[2], space[3], 0};
            h->status = strtol(code, NULL, 10); h->used = 0;
        }
    } else if (n > 12 && !strncasecmp(data, "Retry-After:", 12)) {
        char value[128];
        size_t len = n - 12;
        if (len >= sizeof(value)) len = sizeof(value) - 1;
        memcpy(value, data + 12, len); value[len] = 0;
        char *end;
        errno = 0;
        double seconds = strtod(value, &end);
        int numeric = end != value;
        while (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t') ++end;
        if (!numeric || errno || *end || !isfinite(seconds) || seconds < 0) {
            time_t date = curl_getdate(value, NULL), current = time(NULL);
            if (date >= 0) h->retry_after_us = date > current ? lbc_seconds_us(difftime(date, current)) : 0;
        } else {
            h->retry_after_us = lbc_seconds_us(seconds);
        }
    }
    if (n > 5 && !strncasecmp(data, "ETag:", 5)) {
        size_t start = 5, end = n;
        while (start < end && (data[start] == ' ' || data[start] == '\t')) ++start;
        while (end > start && (data[end-1] == '\r' || data[end-1] == '\n' || data[end-1] == ' ')) --end;
        if (end - start < sizeof(h->etag)) { memcpy(h->etag, data + start, end - start); h->etag[end-start] = 0; }
    }
    return n;
}

static void http_close(App *a, Http *h) {
    if (h->easy) {
        (void)curl_multi_remove_handle(a->multi, h->easy);
        /* Reuse the handle and fixed options; easy marks an active transfer. */
        h->cached = h->easy; h->easy = NULL;
        (void)curl_easy_setopt(h->cached, CURLOPT_HTTPHEADER, NULL);
    }
    curl_slist_free_all(h->private_headers); h->private_headers = NULL;
}

static int http_open_ex(App *a, Http *h, enum HttpKind kind, const char *path, const char *body, Slot *s, int authenticated) {
    if (h->easy) { log_msg("internal error: occupied HTTP handle"); a->fatal = 1; return -1; }
    h->app = a; h->kind = kind; h->slot = s ? s->index : -1;
    h->epoch = s ? s->game.epoch : 0; h->revision = s ? s->game.revision : 0;
    h->status = 0; h->used = 0; h->body[0] = 0; h->error[0] = 0; h->etag[0] = 0;
    h->retry_after_us = -1; h->opened_us = h->last_data_us = now_us();
    h->id[0] = 0; if (s) strcpy(h->id, s->game.id);
    h->authenticated = authenticated; h->action_index = -1; h->action_serial = 0; h->auxiliary = 0; h->parsed = 0;
    h->ndjson = kind == H_ONLINE_BOTS;
    char url[4096];
    if (lbc_format(url, sizeof(url), "%s%s", *path == '/' ? a->cfg.base_url : "", path)) goto failed;
    int fresh = h->cached == NULL;
    h->easy = h->cached ? h->cached : curl_easy_init(); h->cached = NULL;
    if (!h->easy) goto failed;
#define SETOPT(option, value) do { if (curl_easy_setopt(h->easy, option, value) != CURLE_OK) goto failed; } while (0)
    SETOPT(CURLOPT_URL, url);
    SETOPT(CURLOPT_HTTPHEADER, authenticated ? a->headers : NULL);
    if (fresh) {
    SETOPT(CURLOPT_USERAGENT, "lichess-bot-c/" LBC_VERSION);
    SETOPT(CURLOPT_ACCEPT_ENCODING, "");
    SETOPT(CURLOPT_NOSIGNAL, 1L);
    SETOPT(CURLOPT_TCP_NODELAY, 1L);
    SETOPT(CURLOPT_TCP_KEEPALIVE, 1L);
    SETOPT(CURLOPT_TCP_KEEPIDLE, 30L);
    SETOPT(CURLOPT_TCP_KEEPINTVL, 15L);
    SETOPT(CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    SETOPT(CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    SETOPT(CURLOPT_FOLLOWLOCATION, 0L);
    SETOPT(CURLOPT_SSL_VERIFYPEER, 1L);
    SETOPT(CURLOPT_SSL_VERIFYHOST, 2L);
#if LIBCURL_VERSION_NUM >= 0x075500
    SETOPT(CURLOPT_PROTOCOLS_STR, "http,https");
#else
    SETOPT(CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    SETOPT(CURLOPT_WRITEFUNCTION, receive_data); SETOPT(CURLOPT_WRITEDATA, h);
    SETOPT(CURLOPT_HEADERFUNCTION, receive_header); SETOPT(CURLOPT_HEADERDATA, h);
    SETOPT(CURLOPT_PRIVATE, h); SETOPT(CURLOPT_ERRORBUFFER, h->error);
    }
    /* Reset request-specific headers, POST data, and timeouts on reuse. */
    SETOPT(CURLOPT_TIMEOUT_MS, kind == H_EVENTS || kind == H_GAME ? 0L : (long)a->cfg.http_timeout_ms);
    if (body) {
        if (lbc_copy(h->post_data, sizeof(h->post_data), body)) goto failed;
        SETOPT(CURLOPT_POST, 1L); SETOPT(CURLOPT_POSTFIELDS, h->post_data);
        SETOPT(CURLOPT_POSTFIELDSIZE, (long)strlen(h->post_data));
    } else {
        SETOPT(CURLOPT_POSTFIELDS, NULL);
        SETOPT(CURLOPT_POSTFIELDSIZE, 0L);
        SETOPT(CURLOPT_HTTPGET, 1L);
    }
#undef SETOPT
    if (curl_multi_add_handle(a->multi, h->easy) != CURLM_OK) goto failed;
    a->work = 1; return 0;
failed:
    if (h->easy) { curl_easy_cleanup(h->easy); h->easy = NULL; }
    log_msg("failed to initialize HTTP request"); a->fatal = 1; return -1;
}

static int http_open(App *a, Http *h, enum HttpKind kind, const char *path, const char *body, Slot *s) {
    return http_open_ex(a, h, kind, path, body, s, 1);
}

static int64_t backoff_us(int failures) {
    int shift = failures > 5 ? 5 : failures;
    if (shift < 0) shift = 0;
    return (int64_t)(1 << shift) * SECOND;
}

static void rate_limit(App *a, const Http *h) {
    int64_t delay = 60 * SECOND;
    if (h->retry_after_us > delay) delay = h->retry_after_us;
    JsonTok tokens[128]; Json j;
    if (!json_parse(&j, h->body, h->used, tokens, 128)) {
        int64_t seconds;
        if (!json_i64(&j, json_get(&j, json_get(&j, 0, "ratelimit"), "seconds"), &seconds) &&
            seconds > 0 && seconds < INT64_MAX / SECOND && seconds * SECOND > delay) delay = seconds * SECOND;
    }
    int64_t now = now_us();
    if (delay > INT64_MAX - now) delay = INT64_MAX - now;
    if (now + delay > a->blocked_until_us) a->blocked_until_us = now + delay;
    log_msg("HTTP 429: pausing new requests for at least %" PRId64 " seconds", delay / SECOND);
}

static void submit_archive(App *a, const Game *g, int complete, const char *official) {
    ArchiveGame record = {
        .id = g->id, .fen = g->fen, .moves = g->moves,
        .names = {g->names[0], g->names[1]}, .titles = {g->titles[0], g->titles[1]},
        .speed = g->speed, .result = *g->result ? g->result : "*", .status = g->status,
        .tournament = g->tournament, .ratings = {g->ratings[0], g->ratings[1]},
        .variant = g->variant, .ours = g->ours, .rated = g->rated, .finished = complete,
        .initial_ms = g->initial_ms, .increments = {g->increments[0], g->increments[1]},
        .created_at = g->created_at, .annotations = &g->annotations, .official_pgn = official
    };
    if (archive_submit(a->archive, &record)) log_msg("game %s: could not queue PGN archive", g->id);
}

static void archive_game(App *a, const Game *g, int complete) {
    if (!a->archive || !g->full) return;
    submit_archive(a, g, complete, NULL);
    if (!complete) return;
    char path[192];
    (void)snprintf(path, sizeof(path), "/game/export/%s?clocks=true&evals=true&literate=true", g->id);
    Action *act = queue_request(a, H_EXPORT, path, NULL, NULL, 1, 7);
    if (!act || act->archive_snapshot) return;
    Game *copy = malloc(sizeof(*copy));
    if (!copy) { act->used = 0; return; }
    *copy = *g; copy->full_json = NULL; copy->state_json = NULL;
    copy->annotations.items = NULL; copy->annotations.count = copy->annotations.capacity = 0;
    if (g->annotations.count) {
        copy->annotations.items = malloc(g->annotations.count * sizeof(*copy->annotations.items));
        if (!copy->annotations.items) { free(copy); act->used = 0; return; }
        memcpy(copy->annotations.items, g->annotations.items, g->annotations.count * sizeof(*g->annotations.items));
        copy->annotations.count = copy->annotations.capacity = g->annotations.count;
    }
    act->archive_snapshot = copy; strcpy(act->id, g->id);
}

static void clear_action(Action *act) {
    if (act->archive_snapshot) { game_dispose(act->archive_snapshot); free(act->archive_snapshot); act->archive_snapshot = NULL; }
    act->used = 0;
}

static void prune_takebacks(App *a, const Json *j, int array) {
    if (a->takebacks_pruned) return;
    /* Validate discovery before removing tracked games. */
    for (int t = array + 1; t < j->tokens[array].next; t = j->tokens[t].next) {
        char id[33];
        if ((lbc_json_string(j, t, "gameId", id, sizeof(id), NULL) && lbc_json_string(j, t, "id", id, sizeof(id), NULL)) || !valid_id(id)) return;
    }
    a->takebacks_pruned = 1;
    const char *directory = settings_str(a->cfg.settings, "state_directory", "lichess_bot_auto_logs");
    DIR *dir = opendir(directory); if (!dir) return;
    struct dirent *entry;
    const char *prefix = "takeback-count-", *suffix = ".txt";
    while ((entry = readdir(dir))) {
        size_t length = strlen(entry->d_name), start = strlen(prefix), end = strlen(suffix);
        if (length <= start + end || length - start - end > 32 || strncmp(entry->d_name, prefix, start) || strcmp(entry->d_name + length - end, suffix)) continue;
        char id[33]; memcpy(id, entry->d_name + start, length - start - end); id[length - start - end] = 0;
        if (!valid_id(id) || find_game(a, id)) continue;
        int active = 0;
        for (Parked *p = a->parked; p; p = p->next) if (!strcmp(p->game.id, id)) active = 1;
        for (size_t i = 0; i < ACTION_CAP; ++i) if (!strcmp(a->deferred_starts[i].id, id)) active = 1;
        for (int t = array + 1; !active && t < j->tokens[array].next; t = j->tokens[t].next) {
            char other[33];
            if (lbc_json_string(j, t, "gameId", other, sizeof(other), NULL)) (void)lbc_json_string(j, t, "id", other, sizeof(other), "");
            if (!strcmp(other, id)) active = 1;
        }
        if (!active) { char path[4096]; if (!lbc_format(path, sizeof(path), "%s/%s", directory, entry->d_name)) (void)unlink(path); }
    }
    (void)closedir(dir);
}

static void playing_response(App *a, const Json *j) {
    int array = json_get(j, 0, "nowPlaying");
    if (array < 0 || j->tokens[array].type != JSON_ARRAY) return;
    prune_takebacks(a, j, array);
    int64_t now = now_us();
    for (int t = array + 1; t < j->tokens[array].next; t = j->tokens[t].next) {
        char id[33];
        if (lbc_json_string(j, t, "gameId", id, sizeof(id), NULL) && lbc_json_string(j, t, "id", id, sizeof(id), NULL)) continue;
        if (!valid_id(id) || find_game(a, id)) continue;
        int parked = 0;
        for (Parked *p = a->parked; p; p = p->next) if (!strcmp(p->game.id, id)) {
            parked = 1;
            if (lbc_json_bool(j, t, "isMyTurn", 0)) p->due_us = now;
            break;
        }
        if (!parked) defer_game(a, j, t, id, now, 1);
    }
}

static void http_done(App *a, Http *h, CURLcode result) {
    (void)curl_easy_getinfo(h->easy, CURLINFO_RESPONSE_CODE, &h->status);
    /* The last NDJSON object may have no trailing newline. */
    if (h->ndjson && h->used && h->status >= 200 && h->status < 300) {
        stream_line(h, h->body, h->used); h->used = 0;
    }
    http_close(a, h);
    int64_t now = now_us();
    int ok = result == CURLE_OK && h->status >= 200 && h->status < 300;
    int transient = h->status == 429 || h->status >= 500 || result != CURLE_OK;
    if (h->status == 401 && h->authenticated && h->kind != H_SOURCE && h->kind != H_ONLINE_LIST) {
        log_msg("HTTP 401: verify the account token and bot:play permission"); a->fatal = 1; return;
    }
    if (h->status == 403 && (h->kind == H_ACCOUNT || h->kind == H_EVENTS)) {
        log_msg("HTTP 403: verify BOT account and token bot:play permission"); a->fatal = 1; return;
    }
    if (h->status == 429 && h->kind != H_SOURCE && h->kind != H_ONLINE_LIST && h->kind != H_CHALLENGE) rate_limit(a, h);
    if (h->kind == H_EVENTS) {
        if (now - h->opened_us > 60 * SECOND) a->event_failures = 0;
        a->event_due_us = now + backoff_us(a->event_failures++);
        log_msg("event stream closed (HTTP %ld, curl %d); reconnect scheduled", h->status, (int)result);
    } else if (h->kind == H_GAME) {
        Slot *s = &a->slots[h->slot]; Game *g = &s->game;
        if (!g->used || g->epoch != h->epoch || g->ended) return;
        g->synced = 0;
        if (h->status == 404 || h->status == 410) { finish_game(s, "unavailable"); return; }
        if (now - h->opened_us > 60 * SECOND) s->stream_failures = 0;
        s->stream_due_us = now + backoff_us(s->stream_failures++);
        log_msg("game %s: stream closed (HTTP %ld, curl %d); reconnect scheduled", g->id, h->status, (int)result);
    } else if (h->kind == H_ACCOUNT) {
        if (ok) {
            JsonTok tokens[JSON_TOKENS]; Json j;
            if (json_parse(&j, h->body, h->used, tokens, JSON_TOKENS) || policy_profile(&a->policy, &j, 0)) {
                log_msg("invalid account response"); a->fatal = 1; return;
            }
            (void)lbc_copy(a->account, sizeof(a->account), a->policy.profile.id);
            if (!json_eq(&j, json_get(&j, 0, "title"), "BOT")) {
                if (a->cfg.upgrade && !a->upgrades_attempted) {
                    a->upgrades_attempted = 1;
                    (void)queue_request(a, H_UPGRADE, "/api/bot/account/upgrade", "", NULL, 1, 0);
                    a->account_due_us = INT64_MAX;
                } else { log_msg("The token must belong to a BOT account. Use --upgrade only to convert an eligible account."); a->fatal = 1; }
                return;
            }
            int initial = !a->authenticated;
            a->authenticated = 1; a->profile_due_us = now + 300 * SECOND;
            if (initial) {
                a->event_due_us = now; a->playing_due_us = now; a->status_due_us = now + 60 * SECOND;
                char error[256] = "";
                a->archive = archive_create(a->cfg.settings, a->policy.profile.name, a->cfg.base_url, error, sizeof(error));
                if (*error) { log_msg("PGN archive: %s", error); a->fatal = 1; return; }
                log_msg("connected as %s; %d warm engine slot(s)", a->account, a->cfg.concurrency);
            }
            if (a->hooks_initialized) hooks_event(&a->hooks, "account", "", h->body);
        } else if (transient) {
            a->account_due_us = now + 5 * SECOND; a->profile_due_us = now + 60 * SECOND;
            log_msg("account request failed (HTTP %ld, curl %d); retry scheduled", h->status, (int)result);
        } else { log_msg("account request failed (HTTP %ld)", h->status); a->fatal = 1; }
    } else if (h->kind == H_MOVE) {
        Slot *s = &a->slots[h->slot]; Game *g = &s->game;
        if (!g->used || g->epoch != h->epoch || g->revision != h->revision || g->ended) {
            /* Do not retry a move after the game state has changed. */
            if (!ok && g->used && g->epoch == h->epoch && !g->ended) {
                size_t n = strcspn(h->body, "\r\n");
                log_msg("game %s: move rejected (HTTP %ld) after the game state changed; not retried: %.*s",
                        g->id, h->status, (int)(n < 160 ? n : 160), h->body);
            }
            return;
        }
        Pending *p = &g->pending;
        if (p->state != P_INFLIGHT) return;
        if (ok) { p->state = P_CONFIRM; p->due_us = now + 5 * SECOND; }
        else if (transient) {
            p->state = P_RESYNC; p->due_us = now + (int64_t)p->attempts * 250000;
            g->synced = 0; g->reconnect = 1;
            log_msg("game %s: uncertain move response (HTTP %ld, curl %d); checking fresh state before retry", g->id, h->status, (int)result);
        } else {
            p->state = P_BLOCKED; g->synced = 0; g->reconnect = 1;
            log_msg("game %s: move %s rejected (HTTP %ld); no repeated submission", g->id, p->move, h->status);
        }
    } else if (h->kind == H_SOURCE) {
        Slot *s = &a->slots[h->slot]; Game *g = &s->game;
        if (h->status == 429 && h->auxiliary > 0 && h->auxiliary < PROVIDER_DONE) {
            int64_t pause = h->retry_after_us > 60 * SECOND ? h->retry_after_us : 60 * SECOND;
            a->source_blocked_until[h->auxiliary] = pause > INT64_MAX - now ? INT64_MAX : now + pause;
        }
        if (!g->used || g->epoch != h->epoch || g->revision != h->revision || g->ended) return;
        int found = h->ndjson && h->parsed && (ok || result == CURLE_OPERATION_TIMEDOUT);
        if (ok && !h->ndjson) {
            JsonTok *tokens = malloc(32768 * sizeof(*tokens)); Json j;
            if (tokens) {
                if (!json_parse(&j, h->body, h->used, tokens, 32768)) found = provider_parse(&a->cfg, (enum ProviderKind)h->auxiliary, &g->position, &j, &a->policy.random, &g->source_result);
                free(tokens);
            }
        }
        if (found) {
            g->sources_stage = PROVIDER_DONE;
            log_msg("game %s: %s%s", g->id, g->source_result.info.source, g->source_result.root_count ? " root move suggestions" : " move");
        } else if (transient && h->status != 429 && g->sources_attempts < settings_int(a->cfg.settings, "engine.online_moves.max_retries", 2))
            g->source_due_us = now + 100000;
        else { ++g->sources_stage; g->sources_attempts = 0; }
    } else if (h->kind == H_ONLINE_LIST) {
        if (h->auxiliary >= 0 && (size_t)h->auxiliary < a->policy.list_count) {
            OnlineBlockList *list = &a->policy.lists[h->auxiliary];
            list->initialized = 1;
            if (h->status == 304 && result == CURLE_OK) { list->due_us = now + 1800 * SECOND; list->attempts = 0; }
            else if (ok && !policy_online_update(&a->policy, (size_t)h->auxiliary, h->body, h->used, now)) {
                (void)lbc_copy(list->etag, sizeof(list->etag), h->etag);
                log_msg("refreshed online block list (%zu usernames)", list->count);
            } else {
                list->due_us = now + (h->retry_after_us > 60 * SECOND ? h->retry_after_us : 60 * SECOND);
                log_msg("online block list unavailable (HTTP %ld); retaining previous list", h->status);
            }
        }
    } else if (h->kind == H_ONLINE_BOTS) {
        if (ok && *policy_match_finish(&a->policy)) a->matchmaking_stage = 2;
        else { a->matchmaking_stage = 0; a->matchmaking_due_us = now + 60 * SECOND; log_msg("matchmaking: no available matching opponent"); }
    } else if (h->kind == H_PUBLIC_PROFILE) {
        JsonTok tokens[JSON_TOKENS]; Json j;
        if (ok && !json_parse(&j, h->body, h->used, tokens, JSON_TOKENS) && !lbc_json_bool(&j, 0, "blocking", 0) && !lbc_json_bool(&j, 0, "disabled", 0)) a->matchmaking_stage = 3;
        else {
            (void)policy_filter(&a->policy, &a->cfg, a->policy.match.opponent, "", now + 86400 * SECOND, 0);
            a->matchmaking_stage = 0; a->matchmaking_due_us = now + 60 * SECOND;
        }
    } else if (h->kind == H_CHALLENGE) {
        JsonTok tokens[JSON_TOKENS]; Json j; char id[33] = "";
        int parsed = !json_parse(&j, h->body, h->used, tokens, JSON_TOKENS);
        PolicyRateLimit limit = policy_challenge_rate_limit(parsed ? &j : NULL, h->status, h->retry_after_us, &a->challenge_backoff_step);
        int64_t delay = limit.delay_us > INT64_MAX - now ? INT64_MAX - now : limit.delay_us;
        if (limit.kind == POLICY_RATE_LIMIT_BOT) {
            if (a->matchmaking_due_us < now + delay) a->matchmaking_due_us = now + delay;
            log_msg("matchmaking: our challenge requests are limited for %" PRId64 " seconds", delay / SECOND);
        } else if (limit.kind == POLICY_RATE_LIMIT_OPPONENT) {
            (void)policy_filter(&a->policy, &a->cfg, a->policy.match.opponent, "", now + delay, 0);
            log_msg("matchmaking: %s has reached its daily limit (%" PRId64 " seconds)", a->policy.match.opponent, delay / SECOND);
        }
        if (ok && parsed) {
            (void)lbc_json_string(&j, 0, "id", id, sizeof(id), "");
            if (!*id) (void)lbc_json_string(&j, json_get(&j, 0, "challenge"), "id", id, sizeof(id), "");
        }
        if (valid_id(id)) {
            if (!find_game(a, id)) strcpy(a->outgoing_id, id);
            log_msg("matchmaking: challenged %s (%s)", a->policy.match.opponent, id);
        } else if (limit.kind == POLICY_RATE_LIMIT_NONE) {
            (void)policy_filter(&a->policy, &a->cfg, a->policy.match.opponent, "", now + 86400 * SECOND, 0);
            log_msg("matchmaking: challenge failed (HTTP %ld)", h->status);
        }
        a->matchmaking_stage = 0;
        if (a->matchmaking_due_us < now + 60 * SECOND) a->matchmaking_due_us = now + 60 * SECOND;
    } else if (h->kind == H_PLAYING) {
        JsonTok tokens[JSON_TOKENS]; Json j;
        if (ok && !json_parse(&j, h->body, h->used, tokens, JSON_TOKENS)) playing_response(a, &j);
        a->playing_loaded = 1;
        a->playing_due_us = now + lbc_seconds_us(settings_double(a->cfg.settings, "correspondence.checkin_period", 600));
        if (a->playing_due_us < now + SECOND) a->playing_due_us = now + SECOND;
    } else if (h->kind == H_STATUS) {
        JsonTok tokens[128]; Json j;
        if (ok && !json_parse(&j, h->body, h->used, tokens, 128) && j.tokens[0].type == JSON_ARRAY && j.tokens[0].next > 1 && !lbc_json_bool(&j, 1, "online", 1)) {
            http_close(a, &a->events); a->event_due_us = now + SECOND;
            log_msg("account is reported offline; refreshing event stream");
        }
        a->status_due_us = now + 60 * SECOND;
    }
    if (h->action_index >= 0 && h->action_index < ACTION_CAP) {
        Action *act = &a->actions[h->action_index];
        if (act->used && act->serial == h->action_serial && act->kind == h->kind && !strcmp(act->id, h->id)) {
            Game *g = act->slot >= 0 ? &a->slots[act->slot].game : NULL;
            int current = g && g->used && g->epoch == act->epoch;
            if (h->kind == H_UPGRADE) {
                if (ok) { a->account_due_us = now; log_msg("account upgraded; verifying BOT profile"); }
                else { log_msg("account upgrade failed (HTTP %ld)", h->status); a->fatal = 1; }
            } else if (h->kind == H_TAKEBACK && current) {
                if ((ok || result != CURLE_OK) && strstr(act->path, "/yes")) {
                    ++g->takebacks; save_takebacks(a, g);
                    log_msg("game %s: takeback accepted (%d)", g->id, g->takebacks);
                } else if (ok) log_msg("game %s: takeback declined", g->id);
            } else if (h->kind == H_EXPORT && ok && act->archive_snapshot)
                submit_archive(a, act->archive_snapshot, 1, h->body);
            else if (h->kind == H_CANCEL && !strcmp(a->outgoing_id, act->id)) a->outgoing_id[0] = 0;
            if (!ok && transient && h->kind != H_TAKEBACK && h->kind != H_UPGRADE && act->attempts < 3) {
                act->due_us = now + backoff_us(act->attempts);
            } else {
                if (!ok) {
                    if (h->kind == H_ACCEPT && current && g->reserved) g->ended = 1;
                    log_msg("API request kind=%d game=%s failed (HTTP %ld, curl %d)", (int)act->kind, act->id, h->status, (int)result);
                }
                clear_action(act);
            }
        }
    }
    a->work = 1;
}

static void engine_problem(Slot *s, const char *reason) {
    if (s->io_failed) return;
    snprintf(s->fault, sizeof(s->fault), "%s", reason);
    s->io_failed = 1; s->app->work = 1;
}

static int engine_send(Slot *s, const char *command) {
    if (s->io_failed) return -1;
    if (s->app->cfg.engine_debug) {
        const char *line = command;
        while (*line) {
            size_t n = strcspn(line, "\r\n");
            if (n) log_msg("engine[%d] > %.*s", s->index, (int)n, line);
            line += n; while (*line == '\r' || *line == '\n') ++line;
        }
    }
    if (engine_queue(&s->engine, command, strlen(command))) {
        engine_problem(s, s->engine.error); return -1;
    }
    s->app->work = 1;
    return 0;
}

static void request_stop(Slot *s, int discard) {
    if (s->phase == U_STOPPING) { s->stop_discard |= discard; return; }
    if (s->phase != U_SEARCH && s->phase != U_PONDER) return;
    s->stop_discard = discard;
    s->phase = U_STOPPING;
    s->deadline_us = now_us() + 500000;
    (void)engine_send(s, "stop\n");
}

static int line_is(const char *line, size_t len, const char *word) {
    return strlen(word) == len && !memcmp(line, word, len);
}

static int variant_index(int variant) {
    int index = 0;
    while (variant > 1) { variant >>= 1; ++index; }
    return index;
}

static int game_960(const Slot *s) {
    return s->game.variant == VAR_CHESS960 || s->game.position.chess960 ||
           (s->game.variant == VAR_FROM_POSITION && s->supports_960);
}

static int game_ponder(const Slot *s) {
    if (s->game.correspondence) return settings_bool(s->app->cfg.settings, "correspondence.ponder", 0) ||
        settings_bool(s->app->cfg.settings, "correspondence.uci_ponder", 0);
    return s->app->cfg.ponder;
}

static void prepare_move(Slot *s, const char *move, const MoveInfo *info, const char *prediction) {
    Game *g = &s->game; App *a = s->app;
    char wire[8];
    if (!valid_move_n(move, strlen(move)) || !position_normalize_uci(&g->position, move, g->variant != VAR_STANDARD, wire) ||
        !position_is_legal(&g->position, wire)) {
        log_msg("game %s: invalid or illegal engine bestmove; ending game", g->id);
        g->pending.state = P_BLOCKED; g->pending.revision = g->revision; g->prediction[0] = 0;
        game_action(s, g->ply < 2 ? H_ABORT : H_RESIGN, g->ply < 2 ? "abort" : "resign", 0); return;
    }
    if (g->source_result.root_count) {
        int allowed = 0; char normalized[8];
        for (size_t i = 0; i < g->source_result.root_count; ++i)
            if (position_normalize_uci(&g->position, g->source_result.roots[i], g->variant != VAR_STANDARD, normalized) && !strcmp(normalized, wire)) allowed = 1;
        if (!allowed) { log_msg("game %s: engine ignored tablebase root restrictions", g->id); g->pending.state = P_BLOCKED; return; }
    }
    int decision = a->draw_policy_enabled ? evaluation_decision(a->cfg.settings, &g->annotations, info, position_piece_count(&g->position)) : 0;
    if ((decision == 2 || g->engine_resigned) && g->ply >= 2) {
        g->pending.state = P_BLOCKED; g->pending.revision = g->revision;
        game_action(s, H_RESIGN, "resign", 0); log_msg("game %s: resigning", g->id); return;
    }
    Pending *p = &g->pending; memset(p, 0, sizeof(*p));
    p->state = P_READY; p->revision = g->revision;
    p->best_us = p->due_us = now_us(); p->go_us = s->go_us; p->state_us = g->changed_us;
    if (p->due_us < g->min_think_due_us) p->due_us = g->min_think_due_us;
    p->offer_draw = decision == 1 || g->engine_draw;
    strcpy(p->move, wire); g->last_info = *info;
    if (annotations_put(&g->annotations, g->ply, info)) log_msg("game %s: annotation limit reached", g->id);
    if (a->cfg.verbose || info->has_score) { char text[256]; (void)evaluation_string(info, text, sizeof(text), 0); log_msg("game %s: %s %s", g->id, wire, text); }
    g->prediction[0] = 0;
    if (prediction && *prediction && game_ponder(s)) {
        Position next = g->position; char predicted[8];
        if (position_apply_uci(&next, wire, NULL, 0) &&
            position_normalize_uci(&next, prediction, g->variant != VAR_STANDARD, predicted) && position_is_legal(&next, predicted) &&
            !append_move(g->prediction_base, sizeof(g->prediction_base), g->moves, wire)) strcpy(g->prediction, predicted);
    }
    a->work = 1;
}

static void engine_line(void *context, const char *line, size_t length) {
    Slot *s = context; App *a = s->app;
    while (length && (*line == ' ' || *line == '\t')) { ++line; --length; }
    while (length && (line[length - 1] == ' ' || line[length - 1] == '\t')) --length;
    if (a->cfg.engine_debug) log_msg("engine[%d] < %.*s", s->index, (int)length, line);
    if (s->phase == U_HELLO) {
        const char *cap960 = "option name UCI_Chess960 type ", *capponder = "option name Ponder type ";
        const char *capvariant = "option name UCI_Variant type ", *capopponent = "option name UCI_Opponent type ";
        if (length >= strlen(cap960) && !strncasecmp(line, cap960, strlen(cap960))) s->supports_960 = 1;
        if (length >= strlen(capponder) && !strncasecmp(line, capponder, strlen(capponder))) s->supports_ponder = 1;
        if (length >= strlen(capopponent) && !strncasecmp(line, capopponent, strlen(capopponent))) s->supports_opponent = 1;
        if (length >= 8 && !strncmp(line, "id name ", 8)) {
            size_t n = length - 8; if (n >= sizeof(s->engine_name)) n = sizeof(s->engine_name) - 1;
            memcpy(s->engine_name, line + 8, n); s->engine_name[n] = 0;
        }
        if (length >= strlen(capvariant) && !strncasecmp(line, capvariant, strlen(capvariant))) {
            s->has_variant_option = 1; s->supported_variants = 0;
            const char *p = line;
            while ((p = strstr(p, " var "))) {
                p += 5; size_t n = strcspn(p, " \t\r\n"); char name[32];
                if (n >= sizeof(name)) continue;
                memcpy(name, p, n); name[n] = 0; int variant = config_variant(name);
                if (variant) { s->supported_variants |= variant; strcpy(s->variant_names[variant_index(variant)], name); }
            }
        }
        if (line_is(line, length, "uciok")) {
            if (!s->has_variant_option) s->supported_variants = VAR_STANDARD;
            if (s->supported_variants & VAR_STANDARD) s->supported_variants |= VAR_FROM_POSITION | (s->supports_960 ? VAR_CHESS960 : 0);
            char command[1200];
            for (int i = 0; i < a->cfg.option_count; ++i) {
                const char *option = a->cfg.options[i], *eq = strchr(option, '=');
                (void)snprintf(command, sizeof(command), "setoption name %.*s value %s\n", (int)(eq - option), option, eq + 1);
                if (engine_send(s, command)) return;
            }
            if (s->supports_960 && engine_send(s, "setoption name UCI_Chess960 value false\n")) return;
            if (s->supports_ponder && engine_send(s, a->cfg.ponder ? "setoption name Ponder value true\n" : "setoption name Ponder value false\n")) return;
            s->phase = U_READY; s->deadline_us = now_us() + (int64_t)a->cfg.engine_timeout_ms * 1000;
            (void)engine_send(s, "isready\n");
        }
        return;
    }
    if (line_is(line, length, "readyok") && (s->phase == U_READY || s->phase == U_GAME_READY)) {
        s->phase = U_IDLE; s->deadline_us = 0; s->failures = 0;
        if (!s->ever_ready && a->cfg.ponder && !s->supports_ponder) log_msg("engine slot %d does not advertise Ponder; using normal searches", s->index);
        s->ever_ready = 1; a->work = 1; return;
    }
    Game *g = &s->game;
    if (length >= 5 && !strncmp(line, "info ", 5)) {
        if (g->used && g->epoch == s->job_epoch && (s->phase == U_SEARCH || s->phase == U_PONDER || (s->phase == U_STOPPING && !s->stop_discard))) {
            if (line_is(line, length, "info string lbc_resign")) g->engine_resigned = 1;
            else if (line_is(line, length, "info string lbc_draw_offer")) g->engine_draw = 1;
            else if (line_is(line, length, "info string lbc_draw_accept")) g->engine_draw = 2;
            else evaluation_parse(&g->info, line, length);
        }
        return;
    }
    if (length < 9 || strncmp(line, "bestmove ", 9)) return;
    enum UciPhase old_phase = s->phase;
    if (old_phase != U_SEARCH && old_phase != U_STOPPING && old_phase != U_PONDER) return;
    s->phase = U_IDLE; s->deadline_us = 0; a->work = 1;
    if (old_phase == U_PONDER) {
        g->prediction[0] = 0; log_msg("engine slot %d returned bestmove before ponderhit/stop; discarding", s->index); return;
    }
    if ((old_phase == U_STOPPING && s->stop_discard) || !g->used || !g->synced || g->ended || g->failed ||
        g->epoch != s->job_epoch || g->revision != s->job_revision || !our_turn(g) || g->pending.state != P_NONE) return;
    if (g->engine_resigned && g->ply >= 2) {
        g->pending.state = P_BLOCKED; g->pending.revision = g->revision; game_action(s, H_RESIGN, "resign", 0); return;
    }
    if (g->engine_draw == 2 && g->draw_offer) {
        g->pending.state = P_BLOCKED; g->pending.revision = g->revision; game_action(s, H_DRAW, "draw/yes", 0); return;
    }
    char move[16] = {0}, keyword[16] = {0}, prediction[16] = {0};
    int fields = sscanf(line, "bestmove %15s %15s %15s", move, keyword, prediction);
    if (fields < 1 || !valid_move_n(move, strlen(move))) {
        log_msg("game %s: invalid or null engine bestmove; waiting for a fresh state", g->id);
        g->pending.state = P_BLOCKED; g->pending.revision = g->revision; g->reconnect = 1; g->synced = 0; g->prediction[0] = 0; return;
    }
    prepare_move(s, move, &g->info, fields == 3 && !strcmp(keyword, "ponder") ? prediction : NULL);
}

static int64_t remaining_ms(const Game *g, int color, int reserve, int64_t now) {
    int64_t remaining = g->clocks[color];
    int active = g->initial_side ^ (g->ply & 1);
    if (!g->unlimited && color == active && now > g->clock_us) remaining -= (now - g->clock_us) / 1000;
    remaining -= reserve; return remaining > 0 ? remaining : 1;
}

static int64_t movetime_limit(const Slot *s) {
    int64_t ms = s->app->cfg.movetime_ms;
    if (s->game.correspondence) {
        int64_t corr = lbc_seconds_us(settings_double(s->app->cfg.settings, "correspondence.move_time", 60)) / 1000;
        if (!ms || corr < ms) ms = corr;
    }
    if (s->game.ply < 2 && (!ms || ms > 10000)) ms = 10000;
    return ms;
}

static int64_t search_deadline(Slot *s, int64_t now) {
    int64_t budget = remaining_ms(&s->game, s->game.ours, s->app->cfg.overhead_ms, now), limit = movetime_limit(s);
    if (limit && budget > limit + 50) budget = limit + 50;
    return now + budget * 1000;
}

static int engine_history(Slot *s, const char *wire, char *out, size_t cap) {
    Game *g = &s->game;
    if (g->variant == VAR_STANDARD || game_960(s)) return lbc_copy(out, cap, wire);
    Position p;
    if (!position_init(&p, (unsigned)g->variant, g->fen, NULL, 0)) return -1;
    *out = 0;
    while (*wire) {
        char raw[8], normalized[8]; size_t n = strcspn(wire, " ");
        if (!n || n >= sizeof(raw)) return -1;
        memcpy(raw, wire, n); raw[n] = 0;
        if (!position_normalize_uci(&p, raw, 0, normalized)) return -1;
        size_t used = strlen(out);
        if (used + strlen(normalized) + 2 > cap) return -1;
        if (used) out[used++] = ' ';
        strcpy(out + used, normalized);
        if (!position_apply_uci(&p, raw, NULL, 0)) return -1;
        wire += n; if (*wire == ' ') ++wire;
    }
    return 0;
}

static int send_extension(Slot *s, const char *name, const char *json) {
    if (!strcmp(s->app->cfg.protocol, "uci") || !json) return 0;
    if (engine_send(s, name) || engine_send(s, " ") || engine_send(s, json) || engine_send(s, "\n")) return -1;
    return 0;
}

static int start_search(Slot *s, int ponder) {
    App *a = s->app; Game *g = &s->game;
    char commands[MOVES_CAP + 16384], limits[10240] = {0}, history[MOVES_CAP];
    const char *wire = g->moves;
    if (ponder) {
        if (append_move(s->target, sizeof(s->target), g->moves, g->prediction)) return -1;
        strcpy(s->ponder_base, g->moves); wire = s->target;
    } else strcpy(s->target, g->moves);
    if (engine_history(s, wire, history, sizeof(history))) return -1;
    int64_t now = now_us();
    int64_t wt = remaining_ms(g, 0, g->ours == 0 ? a->cfg.overhead_ms : 0, now);
    int64_t bt = remaining_ms(g, 1, g->ours == 1 ? a->cfg.overhead_ms : 0, now);
    size_t used = 0; int64_t ms = movetime_limit(s);
    if (ms) {
        int64_t available = g->ours ? bt : wt; if (ms > available) ms = available;
        if (ms < 1) ms = 1;
        used += (size_t)snprintf(limits + used, sizeof(limits) - used, " movetime %" PRId64, ms);
    }
    if (a->cfg.depth) used += (size_t)snprintf(limits + used, sizeof(limits) - used, " depth %d", a->cfg.depth);
    if (a->cfg.nodes) used += (size_t)snprintf(limits + used, sizeof(limits) - used, " nodes %" PRId64, a->cfg.nodes);
    if (!ponder && g->source_result.root_count) {
        used += (size_t)snprintf(limits + used, sizeof(limits) - used, " searchmoves");
        for (size_t i = 0; i < g->source_result.root_count; ++i) {
            char move[8];
            if (!position_normalize_uci(&g->position, g->source_result.roots[i], game_960(s), move)) return -1;
            int n = snprintf(limits + used, sizeof(limits) - used, " %s", move);
            if (n < 0 || (size_t)n >= sizeof(limits) - used) return -1;
            used += (size_t)n;
        }
    }
    const char *fen = g->fen;
    if (!strcmp(fen, "startpos") && g->variant != VAR_STANDARD && g->variant != VAR_FROM_POSITION && g->variant != VAR_CHESS960)
        fen = position_initial_fen((unsigned)g->variant);
    int n = snprintf(commands, sizeof(commands),
        "position %s%s%s%s\ngo%s wtime %" PRId64 " btime %" PRId64 " winc %" PRId64 " binc %" PRId64 "%s\n",
        !strcmp(fen, "startpos") ? "" : "fen ", fen, *history ? " moves " : "", history,
        ponder ? " ponder" : "", wt, bt, g->increments[0], g->increments[1], limits);
    if (n < 0 || (size_t)n >= sizeof(commands)) return -1;
    memset(&g->info, 0, sizeof(g->info));
    if (!ponder && g->source_result.root_count) (void)lbc_copy(g->info.source, sizeof(g->info.source), g->source_result.info.source);
    g->engine_resigned = g->engine_draw = 0;
    s->job_draw_offer = g->draw_offer;
    if (send_extension(s, "lbc_state", g->state_json)) return -1;
    if (strcmp(a->cfg.protocol, "uci") && engine_send(s, g->draw_offer ? "lbc_draw_offer true\n" : "lbc_draw_offer false\n")) return -1;
    s->phase = ponder ? U_PONDER : U_SEARCH;
    s->job_epoch = g->epoch; s->job_revision = g->revision;
    s->go_us = now; s->state_us = g->changed_us;
    s->deadline_us = ponder ? 0 : search_deadline(s, now);
    if (engine_send(s, commands)) return -1;
    /* Start the engine before optional lookups. Keep partial writes queued. */
    return engine_flush(&s->engine);
}

static void fail_engine(Slot *s) {
    log_msg("engine slot %d failed: %s", s->index, s->fault[0] ? s->fault : s->engine.error);
    engine_terminate(&s->engine);
    s->phase = U_OFF; s->ready_epoch = 0; s->game.prediction[0] = 0;
    s->io_failed = 0; s->fault[0] = 0;
    s->restart_us = now_us() + backoff_us(s->failures++);
    if (!s->ever_ready) s->app->fatal = 1;
    s->app->work = 1;
}

static int option_sink(void *user, const char *name, const char *value) {
    Slot *s = user; char command[2048];
    if (!name || !*name || strchr(name, '\n') || strchr(name, '\r') ||
        (value && (strchr(value, '\n') || strchr(value, '\r'))) ||
        !strcasecmp(name, "UCI_Chess960") || !strcasecmp(name, "UCI_Variant") || !strcasecmp(name, "Ponder") || !strcasecmp(name, "MultiPV")) return -1;
    if (lbc_format(command, sizeof(command), value ? "setoption name %s value %s\n" : "setoption name %s\n", name, value)) return -1;
    return engine_send(s, command);
}

static int configure_game(Slot *s, int64_t now) {
    App *a = s->app; Game *g = &s->game; char command[2048];
    int needs960 = game_960(s);
    if (!(s->supported_variants & g->variant) || (needs960 && !s->supports_960)) {
        log_msg("game %s needs engine support for %s%s", g->id, config_variant_name(g->variant), needs960 ? " and UCI_Chess960" : "");
        g->failed = 1; return -1;
    }
    if (s->has_variant_option) {
        int v = g->variant == VAR_FROM_POSITION || g->variant == VAR_CHESS960 ? VAR_STANDARD : g->variant;
        const char *value = s->variant_names[variant_index(v)];
        if (!*value) value = config_engine_variant_name(v);
        (void)snprintf(command, sizeof(command), "setoption name UCI_Variant value %s\n", value);
        if (engine_send(s, command)) return -1;
    }
    if (s->supports_960 && engine_send(s, needs960 ? "setoption name UCI_Chess960 value true\n" : "setoption name UCI_Chess960 value false\n")) return -1;
    if (s->supports_ponder && engine_send(s, game_ponder(s) ? "setoption name Ponder value true\n" : "setoption name Ponder value false\n")) return -1;
    if (strcmp(a->cfg.protocol, "uci")) {
        (void)snprintf(command, sizeof(command), "lbc_identity %s %s\n", a->policy.profile.name, a->cfg.base_url);
        if (engine_send(s, command) || send_extension(s, "lbc_game", g->full_json)) return -1;
        if (!strcmp(a->cfg.protocol, "homemade")) {
            char *options = malloc(60000), *draw = malloc(10000), *combined = malloc(72000);
            if (!options || !draw || !combined ||
                lbc_setting_json(settings_get(a->cfg.settings, "engine.homemade_options"), options, 60000) ||
                lbc_setting_json(settings_get(a->cfg.settings, "engine.draw_or_resign"), draw, 10000) ||
                lbc_format(combined, 72000, "{\"options\":%s,\"draw_or_resign\":%s}", options, draw) ||
                send_extension(s, "lbc_options", combined)) {
                free(options); free(draw); free(combined); return -1;
            }
            free(options); free(draw); free(combined);
        }
    }
    if (a->hooks_initialized && g->full_json && hooks_game_options(&a->hooks, g->full_json, option_sink, s)) {
        log_msg("game %s: custom game-specific options failed", g->id); g->failed = 1; return -1;
    }
    if (s->supports_opponent) {
        char rating[32];
        if (g->ratings[!g->ours]) (void)snprintf(rating, sizeof(rating), "%d", g->ratings[!g->ours]);
        else strcpy(rating, "none");
        (void)snprintf(command, sizeof(command), "setoption name UCI_Opponent value %s %s %s %s\n",
            *g->titles[!g->ours] ? g->titles[!g->ours] : "none", rating, g->opponent_bot ? "computer" : "human", g->names[!g->ours]);
        if (engine_send(s, command)) return -1;
    }
    if (strcmp(a->cfg.protocol, "uci")) {
        (void)snprintf(command, sizeof(command), "setoption name LBC_EngineRating value %d\n", g->ratings[g->ours]);
        if (engine_send(s, command)) return -1;
    }
    s->ready_epoch = g->epoch; s->phase = U_GAME_READY;
    s->deadline_us = now + (int64_t)a->cfg.engine_timeout_ms * 1000;
    return engine_send(s, "ucinewgame\nisready\n");
}

static void tablebase_result(Game *g, const TablebaseResult *tb) {
    static const int scores[] = {-9900, -500, 0, 500, 9900};
    ProviderResult *r = &g->source_result;
    memset(r, 0, sizeof(*r)); r->info.has_score = 1; r->info.has_tb = 1;
    r->info.score = scores[tb->wdl + 2]; r->info.tb_wdl = tb->wdl;
    if (tb->has_distance && tb->distance > INT_MIN && tb->distance < INT_MAX) {
        if (tb->source == TABLEBASE_SYZYGY) r->info.dtz = (int)tb->distance;
        else r->info.dtm = (int)tb->distance;
    }
    (void)lbc_copy(r->info.source, sizeof(r->info.source), tb->source == TABLEBASE_SYZYGY ? "Syzygy EGTB" : "Gaviota EGTB");
    if (tb->count > 1) { r->root_count = tb->count; memcpy(r->roots, tb->moves, tb->count * sizeof(*tb->moves)); }
    else if (tb->count) strcpy(r->move, tb->moves[0]);
    g->sources_stage = PROVIDER_DONE;
}

/* Try local sources once per history revision; schedule remote requests separately. */
static int prepare_sources(Slot *s) {
    App *a = s->app; Game *g = &s->game; int64_t now = now_us();
    if (g->sources_revision != g->revision) {
        g->sources_revision = g->revision; g->sources_stage = 0; g->sources_attempts = 0;
        g->source_due_us = now; memset(&g->source_result, 0, sizeof(g->source_result));
        g->min_think_due_us = now;
        if (g->ply > 9 && a->fake_think) {
            double seconds = (double)remaining_ms(g, g->ours, a->cfg.overhead_ms, now) / 1000.0 * 0.025 * pow(0.99, g->ply - 10);
            g->min_think_due_us += lbc_seconds_us(seconds);
        }
        if (!g->greeted && g->ply < 2) {
            greeting(s, "hello", "player", 0); greeting(s, "hello_spectators", "spectator", 0); g->greeted = 1;
        }
        if (a->local_book_enabled) {
            int variant = g->variant == VAR_FROM_POSITION ? (g->position.chess960 ? VAR_CHESS960 : VAR_STANDARD) : g->variant;
            BookResult result;
            if (book_choose(&a->books[variant_index(variant)], &g->position, &a->book_options, &a->policy.random, &result)) {
                strcpy(g->source_result.move, result.move); strcpy(g->source_result.info.source, "Polyglot");
                g->sources_stage = PROVIDER_DONE;
            }
        }
        if (g->sources_stage != PROVIDER_DONE && a->tablebases_initialized) {
            TablebaseResult result;
            if ((a->local_syzygy_enabled &&
                 tablebase_probe_syzygy(&a->tablebases, &g->position, &a->syzygy_options, &a->policy.random, &result)) ||
                (a->local_gaviota_enabled &&
                 tablebase_probe_gaviota(&a->tablebases, &g->position, &a->gaviota_options, &a->policy.random, &result))) tablebase_result(g, &result);
        }
        if (g->sources_stage == 0) g->sources_stage = PROVIDER_EGTB;
    }
    while (g->sources_stage < PROVIDER_DONE) {
        ProviderQuery query;
        if (!(a->enabled_sources & (1U << (unsigned)g->sources_stage)) || a->source_blocked_until[g->sources_stage] > now ||
            provider_query(&a->cfg, (enum ProviderKind)g->sources_stage, &g->position, a->account,
                remaining_ms(g, g->ours, a->cfg.overhead_ms, now), g->initial_ms, g->ply, g->out_of_book, &query) <= 0) {
            ++g->sources_stage; g->sources_attempts = 0; continue;
        }
        return 0;
    }
    if (*g->source_result.move) {
        s->go_us = g->changed_us; s->state_us = g->changed_us;
        prepare_move(s, g->source_result.move, &g->source_result.info, NULL); return 0;
    }
    if (g->sources_stage == PROVIDER_DONE && !g->source_result.root_count) { ++g->out_of_book; ++g->sources_stage; }
    return 1;
}

static void engine_tick(Slot *s) {
    App *a = s->app; Game *g = &s->game; int64_t now = now_us();
    if (s->io_failed) { fail_engine(s); return; }
    if (s->phase == U_OFF) {
        if (s->engine.pid > 0) (void)engine_reap(&s->engine);
        if (s->engine.pid > 0 || now < s->restart_us) return;
        char *const *argv = a->adapter_argv[0] ? a->adapter_argv : a->cfg.engine_argv;
        if (engine_spawn_ex(&s->engine, argv, a->cfg.engine_dir, a->cfg.silence_stderr)) {
            (void)snprintf(s->fault, sizeof(s->fault), "%s", s->engine.error); fail_engine(s); return;
        }
        s->supports_960 = s->supports_ponder = s->supports_opponent = s->has_variant_option = 0;
        s->supported_variants = 0; memset(s->variant_names, 0, sizeof(s->variant_names));
        strcpy(s->engine_name, "UCI engine");
        s->phase = U_HELLO; s->ready_epoch = 0;
        s->deadline_us = now + (int64_t)a->cfg.engine_timeout_ms * 1000;
        (void)engine_send(s, "uci\n"); return;
    }
    if (s->phase == U_HELLO || s->phase == U_READY || s->phase == U_GAME_READY || s->phase == U_STOPPING) {
        if (now >= s->deadline_us) engine_problem(s, "UCI response timeout");
        return;
    }
    int current = g->used && g->full && g->synced && !g->ended && !g->failed && !g->reserved;
    if (s->phase == U_PONDER) {
        if (!current || s->job_epoch != g->epoch) request_stop(s, 1);
        else if (our_turn(g) && strcmp(a->cfg.protocol, "uci") && s->job_draw_offer != g->draw_offer) request_stop(s, 1);
        else if (!strcmp(s->target, g->moves) && our_turn(g)) {
            s->phase = U_SEARCH; s->job_revision = g->revision;
            s->go_us = now; s->state_us = g->changed_us; s->deadline_us = search_deadline(s, now);
            g->sources_revision = g->revision; g->sources_stage = PROVIDER_DONE + 1;
            memset(&g->source_result, 0, sizeof(g->source_result));
            (void)engine_send(s, "ponderhit\n");
        } else if (strcmp(s->ponder_base, g->moves)) request_stop(s, 1);
        return;
    }
    if (s->phase == U_SEARCH) {
        if (!current || s->job_epoch != g->epoch || s->job_revision != g->revision || !our_turn(g)) request_stop(s, 1);
        else if (strcmp(a->cfg.protocol, "uci") && s->job_draw_offer != g->draw_offer) request_stop(s, 1);
        else if (now >= s->deadline_us) request_stop(s, 0);
        return;
    }
    if (s->phase != U_IDLE || !current) return;
    if (s->ready_epoch != g->epoch) {
        if (configure_game(s, now) && !g->failed) engine_problem(s, "could not configure game");
        return;
    }
    if (our_turn(g) && g->pending.state == P_NONE && now >= a->blocked_until_us && now >= g->move_delay_us) {
        if (prepare_sources(s) && start_search(s, 0)) engine_problem(s, "could not queue position/search");
    } else if (!our_turn(g) && game_ponder(s) && s->supports_ponder && *g->prediction &&
               !strcmp(g->moves, g->prediction_base)) {
        if (start_search(s, 1)) engine_problem(s, "could not queue ponder position");
    }
}

static void detach_game_actions(App *a, Slot *s) {
    for (int k = 0; k < ACTION_CAP; ++k) {
        Action *act = &a->actions[k];
        if (!act->used || act->slot != s->index || act->epoch != s->game.epoch) continue;
        if (act->survive_game) act->slot = -1;
        else clear_action(act);
    }
    if (a->request.easy && a->request.slot == s->index && a->request.epoch == s->game.epoch) {
        int i = a->request.action_index;
        if (i >= 0 && a->actions[i].used && a->actions[i].survive_game) a->request.slot = -1;
        else http_close(a, &a->request);
    }
}

static void reset_hook_engine(Slot *s) {
    if (!s->app->hooks_initialized) return;
    engine_terminate(&s->engine); s->phase = U_OFF; s->ready_epoch = 0;
    s->restart_us = now_us(); s->io_failed = 0; s->fault[0] = 0;
}

static void admit_deferred(App *a, int background, int64_t now) {
    for (;;) {
        DeferredStart *chosen = NULL;
        for (int i = 0; i < ACTION_CAP; ++i) {
            DeferredStart *d = &a->deferred_starts[i];
            if (!*d->id || d->background != background || d->due_us > now) continue;
            if (!chosen || d->seconds_left < chosen->seconds_left) chosen = d;
        }
        if (!chosen) break;
        Slot *s = new_game(a, chosen->id, 0); if (!s) break;
        s->game.reserved = 0; s->game.correspondence = chosen->correspondence;
        s->game.opponent_bot = chosen->bot; strcpy(s->game.opponent, chosen->opponent);
        chosen->id[0] = 0; a->work = 1;
    }
}

static void maintain_streams(App *a) {
    int64_t now = now_us();
    if (a->events.easy && now - a->events.last_data_us > STREAM_IDLE_US) {
        http_close(a, &a->events); a->event_due_us = now + backoff_us(a->event_failures++);
        log_msg("event stream idle; reconnect scheduled");
    }
    int ready = 1;
    for (int i = 0; i < a->cfg.concurrency; ++i) ready &= a->slots[i].ever_ready;
    if (a->authenticated && a->playing_loaded && ready && !a->events.easy && now >= a->event_due_us && now >= a->blocked_until_us)
        (void)http_open(a, &a->events, H_EVENTS, "/api/stream/event", NULL, NULL);
    for (int i = 0; i < a->cfg.concurrency; ++i) {
        Slot *s = &a->slots[i]; Game *g = &s->game;
        if (!g->used) continue;
        if (g->finish_pending && now >= g->finish_due_us) {
            log_msg("game %s: final-state synchronization timed out; using finish event metadata", g->id);
            g->result_sent = 1; /* Do not feed an unconfirmed final board into engine learning. */
            finish_game(s, g->pending_finish_status);
        }
        if (g->reserved && now >= g->reservation_us) {
            int still_queued = 0;
            for (int k = 0; k < ACTION_CAP; ++k)
                if (a->actions[k].used && a->actions[k].kind == H_ACCEPT && a->actions[k].slot == i && a->actions[k].epoch == g->epoch) still_queued = 1;
            if (still_queued) g->reservation_us = now + 30 * SECOND;
            else { log_msg("challenge %s expired before gameStart", g->id); g->ended = 1; }
        }
        if (g->ended) {
            http_close(a, &s->stream);
            if (s->phase == U_SEARCH || s->phase == U_PONDER) { request_stop(s, 1); continue; }
            if (s->phase == U_STOPPING || s->phase == U_GAME_READY) continue;
            if (!g->result_sent && g->full && s->phase == U_IDLE && s->ready_epoch == g->epoch && strcmp(a->cfg.protocol, "uci")) {
                char command[128];
                (void)send_extension(s, "lbc_state", g->state_json);
                (void)snprintf(command, sizeof(command), "lbc_result %s %s\nisready\n", *g->result ? g->result : "*", g->status);
                if (!engine_send(s, command)) { s->phase = U_GAME_READY; s->deadline_us = now + (int64_t)a->cfg.engine_timeout_ms * 1000; g->result_sent = 1; continue; }
            }
            detach_game_actions(a, s); reset_hook_engine(s);
            game_dispose(g); a->work = 1; continue;
        }
        if (g->reserved) continue;
        if (g->full && !g->finish_pending && !g->retiring && g->terminate_due_us && now >= g->terminate_due_us) {
            g->retiring = 1; g->synced = 0;
            if (g->ply < 2 && !g->abort_requested) { game_action(s, H_ABORT, "abort", 1); g->abort_requested = 1; }
            log_msg("game %s: terminating local session after inactivity", g->id);
        }
        if (g->full && g->synced && g->ply < 2 && g->abort_due_us && now >= g->abort_due_us && !g->abort_requested) {
            game_action(s, H_ABORT, "abort", 1); g->abort_requested = 1; g->retiring = 1; g->synced = 0;
            log_msg("game %s: aborting after inactivity", g->id);
        }
        if (g->full && (g->retiring || (g->synced && g->correspondence && !our_turn(g) && g->ply > 0 &&
            g->pending.state == P_NONE && now >= g->disconnect_due_us))) {
            if (s->phase == U_PONDER || s->phase == U_SEARCH) { request_stop(s, 1); continue; }
            if (s->phase == U_STOPPING || s->phase == U_GAME_READY) continue;
            Parked *p = g->correspondence && !g->abort_requested ? malloc(sizeof(*p)) : NULL;
            if (!p && !g->retiring) { g->disconnect_due_us = now + 60 * SECOND; continue; }
            archive_game(a, g, 0); http_close(a, &s->stream); detach_game_actions(a, s);
            if (p) {
                p->game = *g; p->game.synced = 0; p->game.prediction[0] = 0; p->game.retiring = 0;
                p->due_us = now + lbc_seconds_us(settings_double(a->cfg.settings, "correspondence.checkin_period", 600));
                if (p->due_us < now + SECOND) p->due_us = now + SECOND;
                p->next = a->parked; a->parked = p;
                log_msg("game %s: correspondence check-in scheduled; engine slot released", g->id);
                memset(g, 0, sizeof(*g));
            } else {
                /* Save unfinished games on local retirement so discovery can resume them. */
                log_msg("game %s: inactive session released its engine slot", g->id);
                game_dispose(g);
                a->last_game_ended_us = now;
                a->matchmaking_due_us = now + lbc_seconds_us(settings_double(a->cfg.settings, "matchmaking.challenge_timeout", 30) * 60);
                if (a->playing_due_us > now + 60 * SECOND) a->playing_due_us = now + 60 * SECOND;
            }
            reset_hook_engine(s); a->work = 1; continue;
        }
        if (g->pending.state == P_CONFIRM && now >= g->pending.due_us) {
            g->reconnect = 1; g->pending.due_us = now + 5 * SECOND;
            log_msg("game %s: refreshing state after accepted move; not replaying it", g->id);
        }
        if (s->stream.easy && now - s->stream.last_data_us > STREAM_IDLE_US) g->reconnect = 1;
        if (g->reconnect) {
            http_close(a, &s->stream); g->synced = 0; g->reconnect = 0;
            s->stream_due_us = now + 250000; a->work = 1;
        }
        if (!s->stream.easy && now >= s->stream_due_us && now >= a->blocked_until_us) {
            char path[128]; (void)snprintf(path, sizeof(path), "/api/bot/game/stream/%s", g->id);
            (void)http_open(a, &s->stream, H_GAME, path, NULL, s);
        }
    }
    /* Start accepted games before accepting new challenges. */
    admit_deferred(a, 0, now);
    if (!a->draining) {
        accept_challenges(a);
        admit_deferred(a, 1, now);
        for (;;) {
            Parked *chosen = NULL;
            for (Parked *p = a->parked; p; p = p->next) if (p->due_us <= now && (!chosen || p->due_us < chosen->due_us)) chosen = p;
            if (!chosen) break;
            char id[33]; strcpy(id, chosen->game.id);
            if (!new_game(a, id, 0)) break;
        }
    }
    if (a->archive) { char error[512]; if (archive_error(a->archive, error, sizeof(error))) log_msg("PGN archive: %s", error); }
}

static Slot *move_ready(App *a, int64_t now) {
    Slot *chosen = NULL;
    for (int i = 0; i < a->cfg.concurrency; ++i) {
        Slot *s = &a->slots[i]; Game *g = &s->game; Pending *p = &g->pending;
        if (g->used && !g->reserved && !g->ended && !g->failed && g->synced && p->state == P_READY &&
            p->revision == g->revision && our_turn(g) && now >= p->due_us && (!chosen || p->best_us < chosen->game.pending.best_us)) chosen = s;
    }
    return chosen;
}

static int matchmaking_allowed(App *a, int64_t now) {
    if (a->draining || !settings_bool(a->cfg.settings, "matchmaking.allow_matchmaking", 0) || *a->outgoing_id) return 0;
    int list_owners = settings_bool(a->cfg.settings, "matchmaking.include_challenge_block_list", 0) ? 3 : 2;
    for (size_t i = 0; i < a->policy.list_count; ++i)
        if ((a->policy.lists[i].owners & list_owners) && !a->policy.lists[i].initialized) return 0;
    int active = active_games(a, 0), bots = active_games(a, 1);
    int reserved = (int)settings_int(a->cfg.settings, "challenge.games_reserved_for_humans", 0);
    if (active >= a->cfg.concurrency || bots >= a->cfg.concurrency - reserved) return 0;
    if (active && !settings_bool(a->cfg.settings, "matchmaking.allow_during_games", 0)) return 0;
    if (bots && now - a->challenge_created_us < 600 * SECOND) return 0;
    for (size_t i = 0; i < POLICY_QUEUE_CAP; ++i) if (a->policy.queue[i].used) return 0;
    return 1;
}

static void start_background(App *a) {
    if (!a->authenticated || a->background.easy || a->draining ||
        (a->request.easy && a->request.kind == H_MOVE)) return;
    int index = policy_online_due(&a->policy, now_us()); if (index < 0) return;
    OnlineBlockList *list = &a->policy.lists[index];
    if (!http_open_ex(a, &a->background, H_ONLINE_LIST, list->url, NULL, NULL, 0)) {
        a->background.auxiliary = index; ++list->attempts;
        (void)curl_easy_setopt(a->background.easy, CURLOPT_TIMEOUT_MS, 15000L);
        if (*list->etag) {
            char header[544]; (void)snprintf(header, sizeof(header), "If-None-Match: %s", list->etag);
            a->background.private_headers = curl_slist_append(NULL, header);
            if (a->background.private_headers) (void)curl_easy_setopt(a->background.easy, CURLOPT_HTTPHEADER, a->background.private_headers);
        }
    }
}

static void start_request(App *a) {
    int64_t now = now_us();
    if (a->request.easy && a->request.kind == H_SOURCE && a->request.slot >= 0) {
        Game *g = &a->slots[a->request.slot].game;
        if (!g->used || g->ended || g->epoch != a->request.epoch || g->revision != a->request.revision)
            http_close(a, &a->request);
    }
    if (now < a->blocked_until_us) return;
    Slot *chosen = move_ready(a, now);
    if (a->request.easy) {
        /* Cancel optional lookups when a move is ready; never cancel a submitted move. */
        if (!chosen || (a->request.kind != H_SOURCE && a->request.kind != H_ONLINE_BOTS &&
            a->request.kind != H_PUBLIC_PROFILE && a->request.kind != H_EXPORT && a->request.kind != H_STATUS && a->request.kind != H_PLAYING)) return;
        Http *h = &a->request;
        if (h->kind == H_SOURCE && h->slot >= 0) {
            Game *g = &a->slots[h->slot].game;
            if (g->used && g->epoch == h->epoch && g->revision == h->revision) { ++g->sources_stage; g->sources_attempts = 0; }
        } else if (h->kind == H_ONLINE_BOTS || h->kind == H_PUBLIC_PROFILE) {
            a->matchmaking_stage = 0; a->matchmaking_due_us = now + SECOND;
        } else if (h->kind == H_EXPORT && h->action_index >= 0) a->actions[h->action_index].due_us = now + SECOND;
        http_close(a, h);
    }
    if (chosen) {
        Game *g = &chosen->game; Pending *p = &g->pending; char path[192];
        (void)snprintf(path, sizeof(path), "/api/bot/game/%s/move/%s%s", g->id, p->move, p->offer_draw ? "?offeringDraw=true" : "");
        p->state = P_INFLIGHT; ++p->attempts;
        g->move_delay_us = now + a->configured_move_delay_us;
        if (!http_open(a, &a->request, H_MOVE, path, "", chosen) && a->cfg.trace)
            log_msg("timing game=%s ply=%d move=%s state_to_go_us=%" PRId64 " engine_us=%" PRId64 " bestmove_to_post_us=%" PRId64,
                g->id, g->ply, p->move, p->go_us - p->state_us, p->best_us - p->go_us, now_us() - p->best_us);
        return;
    }
    Action *action = NULL;
    for (int i = 0; i < ACTION_CAP; ++i) {
        Action *act = &a->actions[i]; if (!act->used) continue;
        if (act->slot >= 0 && !act->survive_game) {
            Game *g = &a->slots[act->slot].game;
            if (!g->used || g->epoch != act->epoch || g->ended || (act->kind == H_ACCEPT && !g->reserved)) { clear_action(act); continue; }
        }
        if (now >= act->due_us && (!action || act->priority < action->priority ||
            (act->priority == action->priority && act->created_us < action->created_us))) action = act;
    }
    if (action && (a->authenticated || action->kind == H_UPGRADE)) {
        Action *act = action; ++act->attempts;
        if (!http_open(a, &a->request, act->kind, act->path, act->post ? act->body : NULL,
                       act->slot >= 0 ? &a->slots[act->slot] : NULL)) {
            strcpy(a->request.id, act->id); a->request.action_index = (int)(act - a->actions);
            a->request.action_serial = act->serial;
            if (act->kind == H_EXPORT) {
                for (struct curl_slist *h = a->headers; h; h = h->next) {
                    if (!strncasecmp(h->data, "Accept:", 7)) continue;
                    struct curl_slist *list = curl_slist_append(a->request.private_headers, h->data);
                    if (list) a->request.private_headers = list;
                }
                struct curl_slist *list = curl_slist_append(a->request.private_headers, "Accept: application/x-chess-pgn");
                if (list) { a->request.private_headers = list; (void)curl_easy_setopt(a->request.easy, CURLOPT_HTTPHEADER, list); }
            }
        }
        return;
    }
    if (!a->authenticated) {
        if (now >= a->account_due_us) (void)http_open(a, &a->request, H_ACCOUNT, "/api/account", NULL, NULL);
        return;
    }
    for (int i = 0; i < a->cfg.concurrency; ++i) {
        Slot *s = &a->slots[i]; Game *g = &s->game;
        if (!g->used || !g->full || !g->synced || g->ended || g->failed || !our_turn(g) || g->pending.state != P_NONE ||
            s->phase != U_IDLE || s->ready_epoch != g->epoch || g->sources_revision != g->revision ||
            g->sources_stage < PROVIDER_EGTB || g->sources_stage >= PROVIDER_DONE || now < g->source_due_us) continue;
        ProviderQuery query;
        if (provider_query(&a->cfg, (enum ProviderKind)g->sources_stage, &g->position, a->account,
            remaining_ms(g, g->ours, a->cfg.overhead_ms, now), g->initial_ms, g->ply, g->out_of_book, &query) <= 0) {
            ++g->sources_stage; a->work = 1; continue;
        }
        if (!http_open_ex(a, &a->request, H_SOURCE, query.url, NULL, s, query.authenticated)) {
            a->request.auxiliary = g->sources_stage; a->request.ndjson = query.ndjson; ++g->sources_attempts;
            (void)curl_easy_setopt(a->request.easy, CURLOPT_TIMEOUT_MS, (long)query.timeout_ms);
        }
        return;
    }
    if (*a->outgoing_id && (a->draining || now - a->challenge_created_us >= 25 * SECOND)) {
        char path[128]; (void)snprintf(path, sizeof(path), "/api/challenge/%s/cancel", a->outgoing_id);
        Action *act = queue_request(a, H_CANCEL, path, "", NULL, 1, 3);
        if (act) strcpy(act->id, a->outgoing_id);
        return;
    }
    if (a->draining) return;
    if (now >= a->playing_due_us) { (void)http_open(a, &a->request, H_PLAYING, "/api/account/playing", NULL, NULL); return; }
    if (now >= a->profile_due_us) { (void)http_open(a, &a->request, H_ACCOUNT, "/api/account", NULL, NULL); return; }
    if (now >= a->status_due_us) {
        char path[128]; (void)snprintf(path, sizeof(path), "/api/users/status?ids=%s", a->account);
        (void)http_open(a, &a->request, H_STATUS, path, NULL, NULL); return;
    }
    if (!matchmaking_allowed(a, now)) return;
    if (a->matchmaking_stage == 2) {
        char path[128]; (void)snprintf(path, sizeof(path), "/api/user/%s", a->policy.match.opponent);
        (void)http_open(a, &a->request, H_PUBLIC_PROFILE, path, NULL, NULL); return;
    }
    if (a->matchmaking_stage == 3) {
        char path[128], body[512];
        if (policy_match_body(&a->policy, body, sizeof(body))) { a->matchmaking_stage = 0; a->matchmaking_due_us = now + 60 * SECOND; return; }
        (void)snprintf(path, sizeof(path), "/api/challenge/%s", a->policy.match.opponent);
        a->challenge_created_us = now;
        (void)http_open(a, &a->request, H_CHALLENGE, path, body, NULL); return;
    }
    if (now >= a->matchmaking_due_us && now - a->challenge_created_us >= 60 * SECOND && !policy_match_begin(&a->policy, &a->cfg)) {
        a->matchmaking_stage = 1;
        log_msg("matchmaking: seeking %s %s using %s settings", a->policy.match.mode, a->policy.match.speed, a->policy.match.override_name);
        (void)http_open(a, &a->request, H_ONLINE_BOTS, "/api/bot/online", NULL, NULL);
    }
}

static int next_timeout(App *a) {
    int64_t now = now_us(), next = now + SECOND;
#define SOON(due) do { int64_t d_ = (due); if (d_ > now && d_ < next) next = d_; } while (0)
    SOON(a->blocked_until_us);
    if (!a->authenticated && !a->request.easy) SOON(a->account_due_us);
    if (a->authenticated && !a->events.easy) SOON(a->event_due_us);
    if (!a->draining) { SOON(a->playing_due_us); SOON(a->matchmaking_due_us); SOON(a->status_due_us); }
    if (*a->outgoing_id) SOON(a->challenge_created_us + 25 * SECOND);
    for (int i = 0; i < a->cfg.concurrency; ++i) {
        Slot *s = &a->slots[i]; Game *g = &s->game;
        if (s->phase == U_OFF) SOON(s->restart_us);
        else if (s->phase != U_IDLE && s->phase != U_PONDER) SOON(s->deadline_us);
        if (!g->used) continue;
        if (g->finish_pending) SOON(g->finish_due_us);
        if (!g->finish_pending && !g->retiring) SOON(g->terminate_due_us);
        if (g->reserved) SOON(g->reservation_us);
        else if (!s->stream.easy) SOON(s->stream_due_us);
        if (g->pending.state == P_READY || g->pending.state == P_CONFIRM) SOON(g->pending.due_us);
        if (g->ply < 2 && !g->abort_requested) SOON(g->abort_due_us);
        if (g->correspondence && !our_turn(g)) SOON(g->disconnect_due_us);
        SOON(g->source_due_us); SOON(g->move_delay_us);
    }
    if (!a->draining) for (Parked *p = a->parked; p; p = p->next) SOON(p->due_us);
    if (!a->draining) for (int i = 0; i < ACTION_CAP; ++i) if (*a->deferred_starts[i].id) SOON(a->deferred_starts[i].due_us);
    for (int i = 0; i < ACTION_CAP; ++i) if (a->actions[i].used) SOON(a->actions[i].due_us);
#undef SOON
    int64_t ms = (next - now + 999) / 1000; return ms > 0 ? (int)ms : 0;
}

static void hook_log(void *user, int level, const char *message) {
    App *a = user;
    if (level <= 1 || a->cfg.verbose) log_msg("custom handler: %s", message);
}
static int hook_submit(void *user, const char *method, const char *path, const char *body) {
    App *a = user;
    if (!method || !path || strncmp(path, "/api/", 5) || strchr(path, '\r') || strchr(path, '\n') ||
        strstr(path, "/../") || (strcmp(method, "POST") && strcmp(method, "GET"))) return -1;
    return queue_request(a, H_CUSTOM, path, !strcmp(method, "POST") ? body ? body : "" : NULL, NULL, 1, 4) ? 0 : -1;
}

static int absolute_path(const char *path, char *out, size_t cap) {
    if (*path == '/') return lbc_copy(out, cap, path);
    char cwd[4096]; if (!getcwd(cwd, sizeof(cwd))) return -1;
    return lbc_format(out, cap, "%s/%s", cwd, path);
}
static int runtime_file(const App *a, const char *argv0, const char *name, char *out, size_t cap) {
    const char *configured = settings_str(a->cfg.settings, "c_bridge.runtime_directory", "");
    char base[4096], candidate[8192];
    if (*configured) {
        if (absolute_path(configured, base, sizeof(base)) || lbc_format(out, cap, "%s/%s", base, name)) return -1;
        return access(out, R_OK) ? -1 : 0;
    }
    if (strchr(argv0, '/') && !absolute_path(argv0, base, sizeof(base))) {
        char *slash = strrchr(base, '/'); if (slash) *slash = 0;
    } else {
        ssize_t n = readlink("/proc/self/exe", base, sizeof(base) - 1);
        if (n > 0) { base[n] = 0; char *slash = strrchr(base, '/'); if (slash) *slash = 0; }
        else if (!getcwd(base, sizeof(base))) return -1;
    }
    static const char *const suffixes[] = {"", "/..", "/../libexec/lichess-bot-c"};
    for (size_t i = 0; i < sizeof(suffixes)/sizeof(*suffixes); ++i) {
        if (!lbc_format(candidate, sizeof(candidate), "%s%s/%s", base, suffixes[i], name) && !access(candidate, R_OK)) {
            char resolved[4096];
            if (realpath(candidate, resolved)) return lbc_copy(out, cap, resolved);
            return lbc_copy(out, cap, candidate);
        }
    }
    if (!absolute_path(name, out, cap) && !access(out, R_OK)) return 0;
    return -1;
}

static int initialize_extensions(App *a, const char *argv0) {
    Config *c = &a->cfg; char error[512] = "";
    if (policy_init(&a->policy, c, (uint64_t)now_us() ^ (uint64_t)getpid(), error, sizeof(error))) { log_msg("policy: %s", error); return -1; }
    for (int i = 0; i < 10; ++i) book_collection_init(&a->books[i]);
    a->book_options.selection = book_selection_from_name(settings_str(c->settings, "engine.polyglot.selection", "weighted_random"));
    a->book_options.normalization = book_normalization_from_name(settings_str(c->settings, "engine.polyglot.normalization", "none"));
    a->book_options.min_weight = settings_double(c->settings, "engine.polyglot.min_weight", 1);
    double max_depth = settings_double(c->settings, "engine.polyglot.max_depth", 8);
    a->book_options.max_depth = !isfinite(max_depth) || max_depth > UINT_MAX ? UINT_MAX : (unsigned)max_depth;
    if (settings_bool(c->settings, "engine.polyglot.enabled", 0)) {
        const Setting *books = settings_get(c->settings, "engine.polyglot.book");
        if (!books) books = settings_get(c->settings, "engine.polyglot.books");
        for (size_t i = 0; i < setting_size(books); ++i) {
            int variant = config_variant(setting_key(books, i)); if (!variant) continue;
            const Setting *paths = setting_at(books, i);
            for (size_t k = 0; k < setting_size(paths); ++k) {
                const char *path = setting_str(setting_at(paths, k), "");
                if (!book_collection_add(&a->books[variant_index(variant)], path, error, sizeof(error))) { log_msg("opening book: %s", error); return -1; }
            }
        }
    }
    const char *syzygy[64], *gaviota[64]; size_t counts[2] = {0,0};
    static const char *const path_names[] = {"engine.lichess_bot_tbs.syzygy.paths", "engine.lichess_bot_tbs.gaviota.paths"};
    static const char *const enable_names[] = {"engine.lichess_bot_tbs.syzygy.enabled", "engine.lichess_bot_tbs.gaviota.enabled"};
    for (int t = 0; t < 2; ++t) if (settings_bool(c->settings, enable_names[t], 0)) {
        const Setting *paths = settings_get(c->settings, path_names[t]);
        for (size_t i = 0; i < setting_size(paths); ++i) {
            if (counts[t] >= 64) { log_msg("at most 64 directories per tablebase type are supported"); return -1; }
            (t ? gaviota : syzygy)[counts[t]++] = setting_str(setting_at(paths, i), "");
        }
    }
    if (counts[0] || counts[1]) {
        if (!tablebase_init(&a->tablebases, syzygy, counts[0], gaviota, counts[1], error, sizeof(error))) { log_msg("tablebases: %s", error); return -1; }
        a->tablebases_initialized = 1;
    }
    a->syzygy_options = (TablebaseOptions){(int)settings_int(c->settings, "engine.lichess_bot_tbs.syzygy.max_pieces", 7),
        !strcmp(settings_str(c->settings, "engine.lichess_bot_tbs.syzygy.move_quality", "best"), "suggest"), 120};
    a->gaviota_options = (TablebaseOptions){(int)settings_int(c->settings, "engine.lichess_bot_tbs.gaviota.max_pieces", 5),
        !strcmp(settings_str(c->settings, "engine.lichess_bot_tbs.gaviota.move_quality", "best"), "suggest"),
        (int)settings_int(c->settings, "engine.lichess_bot_tbs.gaviota.min_dtm_to_consider_as_wdl_1", 120)};
    if (!strcmp(c->protocol, "xboard")) {
        if (runtime_file(a, argv0, "lbc-xboard", a->adapter_path, sizeof(a->adapter_path))) { log_msg("lbc-xboard adapter missing; run make all"); return -1; }
        a->adapter_argv[0] = a->adapter_path; a->adapter_argv[1] = "--";
        int n = 0; while (c->engine_argv[n]) { a->adapter_argv[n+2] = c->engine_argv[n]; ++n; }
        a->adapter_argv[n+2] = NULL;
    } else if (!strcmp(c->protocol, "homemade")) {
        if (runtime_file(a, argv0, "lbc-homemade", a->adapter_path, sizeof(a->adapter_path))) { log_msg("lbc-homemade adapter missing; run make all"); return -1; }
        size_t target = *settings_str(c->settings, "engine.interpreter", "") ? 1 + setting_size(settings_get(c->settings, "engine.interpreter_options")) : 0;
        if (target >= MAX_ENGINE_ARGS || !c->engine_argv[target]) { log_msg("native homemade engine target is missing"); return -1; }
        a->adapter_argv[0] = a->adapter_path; a->adapter_argv[1] = c->engine_argv[target];
    }
    const Setting *handlers = settings_get(c->settings, "extra_game_handlers");
    const char *hook_path = setting_str(handlers, settings_str(c->settings, "engine.hooks", ""));
    if (setting_type(handlers) == SETTING_MAP) hook_path = setting_str(setting_get(handlers, "path"), "");
    LbcHooksHostV1 host = {LBC_PLUGIN_ABI_V1, sizeof(host), a, hook_log, hook_submit};
    if (*hook_path) {
        char full[4096]; if (absolute_path(hook_path, full, sizeof(full))) return -1;
        if (hooks_open(&a->hooks, full, &host)) { log_msg("custom handlers: %s", a->hooks.error); return -1; }
        a->hooks_initialized = 1;
    }
    static const char *const source_paths[] = {"", "engine.online_moves.online_egtb.enabled",
        "engine.online_moves.chessdb_book.enabled", "engine.online_moves.lichess_cloud_analysis.enabled",
        "engine.online_moves.lichess_opening_explorer.enabled"};
    for (unsigned i = 1; i < PROVIDER_DONE; ++i)
        if (settings_bool(c->settings, source_paths[i], 0)) a->enabled_sources |= 1U << i;
    a->local_book_enabled = settings_bool(c->settings, "engine.polyglot.enabled", 0);
    a->local_syzygy_enabled = settings_bool(c->settings, "engine.lichess_bot_tbs.syzygy.enabled", 0);
    a->local_gaviota_enabled = settings_bool(c->settings, "engine.lichess_bot_tbs.gaviota.enabled", 0);
    a->fake_think = settings_bool(c->settings, "fake_think_time", 0);
    a->draw_policy_enabled = settings_bool(c->settings, "engine.draw_or_resign.resign_enabled", 0) ||
        settings_bool(c->settings, "engine.draw_or_resign.offer_draw_enabled", 0);
    a->configured_move_delay_us = lbc_seconds_us(settings_double(c->settings, "rate_limiting_delay", 0) / 1000.0);
    int64_t now = now_us();
    a->last_game_ended_us = a->challenge_created_us = now;
    a->matchmaking_due_us = now + lbc_seconds_us(settings_double(c->settings, "matchmaking.challenge_timeout", 30) * 60);
    if (settings_bool(c->settings, "matchmaking.start_immediately", 0)) {
        a->matchmaking_due_us = now; a->challenge_created_us = now - 60 * SECOND;
    }
    return 0;
}

static int run(App *a) {
    logging_defer(1);
    while (!a->fatal) {
        if (interrupted) {
            if (interrupted > 1 || !settings_bool(a->cfg.settings, "quit_after_all_games_finish", 0)) break;
            if (!a->draining) { a->draining = 1; log_msg("shutdown requested; finishing active games (signal again to force)"); }
        }
        a->work = 0;
        int running = 0;
        CURLMcode result = curl_multi_perform(a->multi, &running);
        if (result != CURLM_OK) { log_msg("curl_multi_perform: %s", curl_multi_strerror(result)); return 1; }
        CURLMsg *message; int left;
        while ((message = curl_multi_info_read(a->multi, &left))) {
            if (message->msg != CURLMSG_DONE) continue;
            Http *h = NULL; (void)curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &h);
            if (h) http_done(a, h, message->data.result);
        }
        for (int i = 0; i < a->cfg.concurrency; ++i) {
            Slot *s = &a->slots[i];
            if (s->phase != U_OFF && s->engine.output_fd >= 0 && engine_read(&s->engine, engine_line, s)) engine_problem(s, s->engine.error);
            if (s->phase != U_OFF && engine_reap(&s->engine) != 0) engine_problem(s, "engine process exited");
        }
        if (a->fatal) break;
        maintain_streams(a);
        for (int i = 0; i < a->cfg.concurrency; ++i) engine_tick(&a->slots[i]);
        if (a->fatal) break;
        /* Send positions before processing other API requests. */
        for (int i = 0; i < a->cfg.concurrency; ++i) {
            Slot *s = &a->slots[i];
            if (engine_wants_write(&s->engine) && engine_flush(&s->engine)) engine_problem(s, s->engine.error);
        }
        start_request(a); start_background(a);
        if (a->draining && !active_games(a, 0) && !a->request.easy && !*a->outgoing_id) {
            int pending = 0;
            for (int i = 0; i < a->cfg.concurrency; ++i) pending |= a->slots[i].game.used;
            for (int i = 0; i < ACTION_CAP; ++i) pending |= a->actions[i].used;
            if (!pending) break;
        }
        if (a->work) continue;
        /* Let curl attempt the move's first write before flushing logs. */
        logging_flush();
        struct curl_waitfd fds[MAX_GAMES * 2]; unsigned count = 0;
        for (int i = 0; i < a->cfg.concurrency; ++i) {
            Engine *e = &a->slots[i].engine;
            if (e->output_fd >= 0) fds[count++] = (struct curl_waitfd){e->output_fd, CURL_WAIT_POLLIN, 0};
            if (engine_wants_write(e)) fds[count++] = (struct curl_waitfd){e->input_fd, CURL_WAIT_POLLOUT, 0};
        }
        result = curl_multi_poll(a->multi, fds, count, next_timeout(a), NULL);
        if (result != CURLM_OK) { log_msg("curl_multi_poll: %s", curl_multi_strerror(result)); return 1; }
    }
    logging_defer(0);
    return a->fatal ? 1 : 0;
}

int main(int argc, char **argv) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    App *a = calloc(1, sizeof(*a));
    if (!a) { curl_global_cleanup(); return 1; }
    int parsed = config_parse(&a->cfg, argc, argv);
    if (parsed) { config_free(&a->cfg); free(a); curl_global_cleanup(); return parsed < 0 ? 2 : 0; }
    (void)unsetenv("LICHESS_BOT_TOKEN");
    char logging_error[512];
    const char *automatic_directory = a->cfg.disable_auto_logging ? NULL : settings_str(a->cfg.settings, "state_directory", "lichess_bot_auto_logs");
    if (logging_open(a->cfg.logfile, automatic_directory, a->cfg.settings, logging_error, sizeof(logging_error))) {
        log_msg("logging: %s", logging_error); a->fatal = 1; goto cleanup;
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action)); action.sa_handler = signal_stop; sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL); (void)sigaction(SIGTERM, &action, NULL);
    action.sa_handler = SIG_IGN; (void)sigaction(SIGPIPE, &action, NULL);
    a->slots = calloc((size_t)(a->cfg.concurrency ? a->cfg.concurrency : 1), sizeof(*a->slots));
    if (!a->slots) { log_msg("out of memory"); a->fatal = 1; goto cleanup; }
    for (int i = 0; i < a->cfg.concurrency; ++i) { a->slots[i].app = a; a->slots[i].index = i; engine_init(&a->slots[i].engine); }
    a->multi = curl_multi_init();
    if (!a->multi) { log_msg("out of memory"); a->fatal = 1; goto cleanup; }
    (void)curl_multi_setopt(a->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)a->cfg.concurrency + 5L);
    (void)curl_multi_setopt(a->multi, CURLMOPT_MAX_HOST_CONNECTIONS, (long)a->cfg.concurrency + 5L);
    (void)curl_multi_setopt(a->multi, CURLMOPT_MAXCONNECTS, (long)a->cfg.concurrency + 5L);
    char auth[TOKEN_CAP + 32];
    (void)snprintf(auth, sizeof(auth), "Authorization: Bearer %s", a->cfg.token);
    a->headers = curl_slist_append(NULL, auth);
    struct curl_slist *headers = a->headers ? curl_slist_append(a->headers, "Accept: application/x-ndjson, application/json") : NULL;
    if (!headers) { a->fatal = 1; goto cleanup; }
    a->headers = headers; headers = curl_slist_append(a->headers, "Expect:");
    if (!headers) { a->fatal = 1; goto cleanup; }
    a->headers = headers;
    memset(a->cfg.token, 0, sizeof(a->cfg.token)); memset(auth, 0, sizeof(auth));
    if (initialize_extensions(a, argv[0])) { a->fatal = 1; goto cleanup; }
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    log_msg("lichess-bot-c %s | libcurl %s | HTTP/2 %s | async DNS %s | protocol %s", LBC_VERSION, version->version,
            version->features & CURL_VERSION_HTTP2 ? "yes" : "no", version->features & CURL_VERSION_ASYNCHDNS ? "yes" : "no", a->cfg.protocol);
    a->fatal = run(a);
cleanup:
    if (a->multi) {
        http_close(a, &a->request); http_close(a, &a->events); http_close(a, &a->background);
        if (a->slots) for (int i = 0; i < a->cfg.concurrency; ++i) http_close(a, &a->slots[i].stream);
    }
    if (a->slots) for (int i = 0; i < a->cfg.concurrency; ++i) {
        Game *g = &a->slots[i].game;
        if (g->used && g->full && !g->ended) archive_game(a, g, 0);
        engine_terminate(&a->slots[i].engine);
    }
    if (a->hooks_initialized) hooks_close(&a->hooks);
    if (a->archive) { archive_destroy(a->archive); a->archive = NULL; }
    for (int i = 0; i < ACTION_CAP; ++i) clear_action(&a->actions[i]);
    while (a->parked) { Parked *next = a->parked->next; game_dispose(&a->parked->game); free(a->parked); a->parked = next; }
    if (a->tablebases_initialized) tablebase_close(&a->tablebases);
    for (int i = 0; i < 10; ++i) book_collection_close(&a->books[i]);
    policy_free(&a->policy);
    curl_easy_cleanup(a->request.cached); curl_easy_cleanup(a->events.cached); curl_easy_cleanup(a->background.cached);
    if (a->slots) for (int i = 0; i < a->cfg.concurrency; ++i) curl_easy_cleanup(a->slots[i].stream.cached);
    if (a->multi) curl_multi_cleanup(a->multi);
    curl_slist_free_all(a->headers);
    if (a->slots) for (int i = 0; i < a->cfg.concurrency; ++i) { (void)engine_reap(&a->slots[i].engine); game_dispose(&a->slots[i].game); }
    int exit_code = a->fatal ? 1 : 0;
    config_free(&a->cfg); free(a->slots); free(a); curl_global_cleanup();
    logging_close();
    return exit_code;
}
