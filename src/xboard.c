#define _POSIX_C_SOURCE 200809L
#include "xboard.h"
#include "engine.h"
#include "position.h"
#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define XB_LINE (128u * 1024u)
#define XB_HISTORY (64u * 1024u)
#define XB_OPTIONS 128

typedef struct {
    char name[128], type[16], value[2048], definition[3072];
    int configured;
} XOption;
enum XPhase { X_HELLO, X_IDLE, X_SEARCH, X_PONDER, X_ANALYZE,
              X_FINISH, X_STOP, X_HIT };
typedef struct {
    Engine engine;
    char **argv;
    enum XPhase phase;
    int failed, quitting, trace, uci_requested, uci_sent;
    int done, ping, setboard, usermove, san, colors, draw, name, ics;
    int memory, smp, analyze, nps, exclude, reuse;
    int ponder, chess960, new_pending, games, auto_ponder, draw_offered;
    int resigned, offered, accepted, stop_discard, valid_position;
    unsigned variant;
    char variants[1024], engine_name[256], egt[512];
    XOption options[XB_OPTIONS];
    size_t option_count;
    int own_rating, opponent_rating, opponent_computer, random;
    char opponent[256];
    uint32_t ping_serial, ready_ping, finish_ping;
    int ready_pending;
    int64_t hello_deadline, barrier_deadline;
    char input[XB_LINE + 1u];
    size_t input_length;
    char fen[POSITION_FEN_MAX], history[XB_HISTORY];
    char engine_fen[POSITION_FEN_MAX], engine_history[XB_HISTORY];
    char game_initial_fen[POSITION_FEN_MAX], state_history[XB_HISTORY];
    int state_history_valid;
    Position target, actual, search_position;
    char best[8], hint[8], pv_first[8], pv_second[8];
    char last_go[8192];
    int64_t time[2], increment[2], movetime, nodes;
    int depth, moves_to_go;
} XBoard;

static volatile sig_atomic_t interrupted;
static void stop_signal(int number) { (void)number; interrupted = 1; }
static int64_t clock_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int safe_text(const char *value) {
    return value && !strchr(value, '\n') && !strchr(value, '\r');
}
static void failure(XBoard *x, const char *message) {
    if (!x->failed) {
        fprintf(stderr, "lbc-xboard: %s\n", message);
        printf("info string lbc_error %s\n", message);
    }
    x->failed = 1;
}
static void send_engine(XBoard *x, const char *format, ...) {
    char command[8192];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(command, sizeof(command), format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(command)) {
        failure(x, "CECP command exceeds limit"); return;
    }
    if (x->trace) fprintf(stderr, "CECP > %s", command);
    if (engine_queue(&x->engine, command, (size_t)n))
        failure(x, x->engine.error);
}
static uint32_t ping_engine(XBoard *x) {
    if (++x->ping_serial == 0) ++x->ping_serial;
    send_engine(x, "ping %" PRIu32 "\n", x->ping_serial);
    x->barrier_deadline = clock_ms() + 10000;
    return x->ping_serial;
}
static int in_list(const char *list, const char *item) {
    size_t n = strlen(item);
    const char *p = list;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) ++p;
        const char *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        const char *trim = end;
        while (trim > p && isspace((unsigned char)trim[-1])) --trim;
        if ((size_t)(trim - p) == n && !strncasecmp(p, item, n)) return 1;
        p = end;
    }
    return 0;
}
static const char *cecp_variant(const XBoard *x, unsigned variant) {
    switch (variant) {
    case POSITION_STANDARD:
    case POSITION_FROM_POSITION:
        return x->chess960 ? "fischerandom" : "normal";
    case POSITION_CHESS960: return "fischerandom";
    case POSITION_ANTICHESS:
        return in_list(x->variants, "giveaway") ? "giveaway" : "suicide";
    case POSITION_ATOMIC: return "atomic";
    case POSITION_CRAZYHOUSE: return "crazyhouse";
    case POSITION_HORDE: return "horde";
    case POSITION_KING_OF_THE_HILL: return "kingofthehill";
    case POSITION_RACING_KINGS: return "racingkings";
    case POSITION_THREE_CHECK: return "3check";
    default: return "";
    }
}
static unsigned board_variant(const XBoard *x) {
    return x->chess960 && (x->variant == POSITION_STANDARD || x->variant == POSITION_FROM_POSITION)
        ? POSITION_CHESS960 : x->variant;
}
static int option_feature(XBoard *x, const char *value) {
    const char *kind = strstr(value, " -");
    if (!kind || kind == value || (size_t)(kind - value) >= sizeof(x->options[0].name)) return 0;
    char name[128], type[16];
    memcpy(name, value, (size_t)(kind - value)); name[kind - value] = 0;
    int offset = 0;
    if (sscanf(kind + 2, "%15s%n", type, &offset) != 1) return 0;
    const char *data = kind + 2 + offset;
    while (*data == ' ') ++data;
    if (strcmp(type, "spin") && strcmp(type, "slider") && strcmp(type, "check") &&
        strcmp(type, "string") && strcmp(type, "file") && strcmp(type, "path") &&
        strcmp(type, "combo") && strcmp(type, "button") && strcmp(type, "save") &&
        strcmp(type, "reset")) return 0;
    size_t i;
    for (i = 0; i < x->option_count; ++i)
        if (!strcmp(x->options[i].name, name)) break;
    if (i == x->option_count) {
        if (x->option_count == XB_OPTIONS) return 0;
        ++x->option_count;
    }
    XOption *o = &x->options[i];
    snprintf(o->name, sizeof(o->name), "%s", name);
    snprintf(o->type, sizeof(o->type), "%s", type);
    snprintf(o->definition, sizeof(o->definition), "%s", data);
    return 1;
}
static void advertise_option(const XOption *o) {
    if (!strcmp(o->type, "spin") || !strcmp(o->type, "slider")) {
        long value, minimum, maximum;
        if (sscanf(o->definition, "%ld %ld %ld", &value, &minimum, &maximum) == 3)
            printf("option name %s type spin default %ld min %ld max %ld\n", o->name, value, minimum, maximum);
    } else if (!strcmp(o->type, "check")) {
        printf("option name %s type check default %s\n", o->name, atoi(o->definition) ? "true" : "false");
    } else if (!strcmp(o->type, "button") || !strcmp(o->type, "save") || !strcmp(o->type, "reset")) {
        printf("option name %s type button\n", o->name);
    } else if (!strcmp(o->type, "combo")) {
        char choices[3072], normal[3072] = "", def[1024] = "";
        snprintf(choices, sizeof(choices), "%s", o->definition);
        char *p = choices;
        size_t used = 0;
        while (*p) {
            while (*p == ' ') ++p;
            char *end = strstr(p, " /// ");
            if (end) *end = 0;
            int selected = *p == '*';
            if (selected) ++p;
            if (selected || !*def) snprintf(def, sizeof(def), "%.*s", (int)sizeof(def) - 1, p);
            int n = snprintf(normal + used, sizeof(normal) - used, " var %s", p);
            if (n < 0 || (size_t)n >= sizeof(normal) - used) break;
            used += (size_t)n;
            if (!end) break;
            p = end + 5;
        }
        printf("option name %s type combo default %s%s\n", o->name, def, normal);
    } else {
        printf("option name %s type string default %s\n", o->name, *o->definition ? o->definition : "<empty>");
    }
}
static void advertise(XBoard *x) {
    if (!x->uci_requested || x->uci_sent || !x->done) return;
    if (!x->ping || !x->setboard) {
        failure(x, "CECP v2 requires ping=1 and setboard=1 (as upstream lichess-bot does)"); return;
    }
    printf("id name %s [native CECP adapter]\nid author lichess-bot-c\n", *x->engine_name ? x->engine_name : "XBoard engine");
    puts("option name Ponder type check default false");
    if (in_list(x->variants, "fischerandom")) puts("option name UCI_Chess960 type check default false");
    printf("option name UCI_Variant type combo default chess");
    for (unsigned v = 1; v <= POSITION_THREE_CHECK; v <<= 1) {
        if (v == POSITION_FROM_POSITION || v == POSITION_CHESS960) continue;
        if (in_list(x->variants, cecp_variant(x, v)))
            printf(" var %s", v == POSITION_STANDARD ? "chess" : position_variant_name(v));
    }
    putchar('\n');
    puts("option name UCI_Opponent type string default none none human unknown");
    puts("option name LBC_EngineRating type spin default 0 min 0 max 4000");
    puts("option name random type check default false");
    if (x->memory) puts("option name memory type spin default 16 min 1 max 2147483647");
    if (x->smp) puts("option name cores type spin default 1 min 1 max 1024");
    char egt[512]; snprintf(egt, sizeof(egt), "%s", x->egt);
    char *save = NULL;
    for (char *type = strtok_r(egt, ",", &save); type; type = strtok_r(NULL, ",", &save))
        printf("option name egtpath %s type string default <empty>\n", type);
    for (size_t i = 0; i < x->option_count; ++i) advertise_option(&x->options[i]);
    puts("uciok"); x->uci_sent = 1; x->phase = X_IDLE;
}

static void features(XBoard *x, const char *line) {
    const char *p = line;
    while (*p) {
        char key[128], value[4096]; size_t nk = 0, nv = 0;
        while (isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        while (*p && *p != '=' && !isspace((unsigned char)*p)) {
            if (nk + 1 >= sizeof(key)) { failure(x, "oversized feature key"); return; }
            key[nk++] = *p++;
        }
        key[nk] = 0;
        if (*p++ != '=') { failure(x, "malformed CECP feature"); return; }
        int quoted = *p == '"';
        if (quoted) ++p;
        while (*p && (quoted ? *p != '"' : !isspace((unsigned char)*p))) {
            if (*p == '\\' && p[1] && quoted && (p[1] == '\\' || p[1] == '"')) ++p;
            if (nv + 1 >= sizeof(value)) { failure(x, "oversized feature value"); return; }
            value[nv++] = *p++;
        }
        value[nv] = 0;
        if (quoted) {
            if (*p != '"') { failure(x, "unterminated CECP feature quote"); return; }
            ++p;
        }
        int v = atoi(value), accepted = 1;
        if (!strcmp(key, "ping")) x->ping = v != 0;
        else if (!strcmp(key, "setboard")) x->setboard = v != 0;
        else if (!strcmp(key, "usermove")) x->usermove = v != 0;
        else if (!strcmp(key, "san")) x->san = v != 0;
        else if (!strcmp(key, "colors")) x->colors = v != 0;
        else if (!strcmp(key, "draw")) x->draw = v != 0;
        else if (!strcmp(key, "name")) x->name = v != 0;
        else if (!strcmp(key, "ics")) x->ics = v != 0;
        else if (!strcmp(key, "memory")) x->memory = v != 0;
        else if (!strcmp(key, "smp")) x->smp = v != 0;
        else if (!strcmp(key, "analyze")) x->analyze = v != 0;
        else if (!strcmp(key, "nps")) x->nps = v != 0;
        else if (!strcmp(key, "exclude")) x->exclude = v != 0;
        else if (!strcmp(key, "reuse")) { x->reuse = 1; accepted = v != 0; }
        else if (!strcmp(key, "sigint")) accepted = v == 0;
        else if (!strcmp(key, "sigterm")) accepted = v != 0;
        else if (!strcmp(key, "myname")) snprintf(x->engine_name, sizeof(x->engine_name), "%.*s", (int)sizeof(x->engine_name) - 1, value);
        else if (!strcmp(key, "variants")) snprintf(x->variants, sizeof(x->variants), "%.*s", (int)sizeof(x->variants) - 1, value);
        else if (!strcmp(key, "egt")) snprintf(x->egt, sizeof(x->egt), "%.*s", (int)sizeof(x->egt) - 1, value);
        else if (!strcmp(key, "option")) accepted = option_feature(x, value);
        else if (!strcmp(key, "done")) {
            x->done = v != 0;
            x->hello_deadline = clock_ms() + (v ? 2000 : 3600000);
        } else if (strcmp(key, "debug") && strcmp(key, "time") && strcmp(key, "playother") &&
                   strcmp(key, "pause") && strcmp(key, "highlight") && strcmp(key, "setscore")) accepted = 0;
        else if (!strcmp(key, "playother") || !strcmp(key, "pause") || !strcmp(key, "highlight") || !strcmp(key, "setscore")) accepted = v == 0;
        send_engine(x, "%s %s\n", accepted ? "accepted" : "rejected", key);
        if (x->uci_sent && !strcmp(key, "option") && accepted)
            advertise_option(&x->options[x->option_count - 1]);
    }
    advertise(x);
}
static int append_history(char *history, size_t cap, const char *move) {
    size_t have = strlen(history), extra = strlen(move);
    if (have + extra + (have ? 1u : 0u) >= cap) return 0;
    if (have) history[have++] = ' ';
    memcpy(history + have, move, extra + 1);
    return 1;
}
static int convert_move(const Position *p, const char *text, int mode, char out[8]) {
    char parsed[8];
    if (position_normalize_uci(p, text, mode, out) && position_is_legal(p, out)) return 1;
    if (position_san_to_uci(p, text, parsed)) return position_normalize_uci(p, parsed, mode, out);
    return 0;
}
static int send_move(XBoard *x, Position *position, const char *move) {
    char text[64], normalized[8], err[160];
    if (!position_normalize_uci(position, move, x->chess960, normalized)) {
        failure(x, "cannot normalize CECP input move"); return 0;
    }
    if (x->san) {
        if (!position_uci_to_san(position, normalized, text, sizeof(text))) {
            failure(x, "cannot convert CECP input move to SAN"); return 0;
        }
    } else {
        snprintf(text, sizeof(text), "%s", normalized);
        if (position->chess960) {
            char san[64];
            if (position_uci_to_san(position, normalized, san, sizeof(san)) && !strncmp(san, "O-O", 3))
                snprintf(text, sizeof(text), "%s", !strncmp(san, "O-O-O", 5) ? "O-O-O" : "O-O");
        }
    }
    send_engine(x, "%s%s\n", x->usermove ? "usermove " : "", text);
    if (!position_apply_uci(position, normalized, err, sizeof(err))) { failure(x, err); return 0; }
    return !x->failed;
}
static void send_opponent(XBoard *x) {
    if (x->name && *x->opponent) send_engine(x, "name %s\n", x->opponent);
    if (x->own_rating || x->opponent_rating)
        send_engine(x, "rating %d %d\n", x->own_rating, x->opponent_rating);
    if (x->opponent_computer) send_engine(x, "computer\n");
}
static void new_game(XBoard *x) {
    send_engine(x, "easy\nforce\nnew\n");
    const char *v = cecp_variant(x, x->variant);
    if (strcmp(v, "normal")) {
        if (!in_list(x->variants, v)) { failure(x, "engine does not support requested variant"); return; }
        send_engine(x, "variant %s\n", v);
    }
    if (x->random) send_engine(x, "random\n");
    if (x->ics) send_engine(x, "ics lichess.org\n");
    send_opponent(x);
    send_engine(x, "force\n");
    x->engine_fen[0] = x->engine_history[0] = 0;
    x->auto_ponder = 0; x->new_pending = 0; x->draw_offered = 0;
    x->best[0] = x->hint[0] = 0; ++x->games;
}
static int history_suffix(XBoard *x, const char **suffix) {
    size_t n = strlen(x->engine_history);
    if (strcmp(x->fen, x->engine_fen) || strncmp(x->history, x->engine_history, n) ||
        (n && x->history[n] && x->history[n] != ' ')) return 0;
    *suffix = x->history + n;
    if (**suffix == ' ') ++*suffix;
    return 1;
}
static int same_position(const Position *a, const Position *b) {
    char fa[POSITION_FEN_MAX], fb[POSITION_FEN_MAX];
    return position_fen_ex(a, fa, sizeof(fa), 1, 1) &&
           position_fen_ex(b, fb, sizeof(fb), 1, 1) && !strcmp(fa, fb);
}
static int synchronize(XBoard *x) {
    const char *suffix;
    send_engine(x, "easy\nforce\n"); x->auto_ponder = 0;
    if (!history_suffix(x, &suffix)) {
        char err[160], fen[POSITION_FEN_MAX];
        if (!position_init(&x->actual, board_variant(x), x->fen, err, sizeof(err)) ||
            !position_fen_ex(&x->actual, fen, sizeof(fen), 1, x->chess960)) {
            failure(x, "could not build CECP initial FEN"); return 0;
        }
        x->actual.chess960 = x->chess960;
        send_engine(x, "setboard %s\n", fen);
        snprintf(x->engine_fen, sizeof(x->engine_fen), "%s", x->fen);
        x->engine_history[0] = 0; suffix = x->history;
    }
    char replay[XB_HISTORY]; snprintf(replay, sizeof(replay), "%s", suffix);
    char *save = NULL;
    for (char *move = strtok_r(replay, " ", &save); move; move = strtok_r(NULL, " ", &save)) {
        if (!send_move(x, &x->actual, move) || !append_history(x->engine_history, sizeof(x->engine_history), move)) return 0;
    }
    if (!same_position(&x->actual, &x->target)) { failure(x, "CECP position synchronization failed"); return 0; }
    return !x->failed;
}
static int64_t at_least_one(int64_t value) { return value > 0 ? value : 1; }
static void time_control(XBoard *x, int color, int live) {
    int64_t own = at_least_one(x->time[color]), increment = x->increment[color];
    if (!live && x->colors) send_engine(x, "%s\n", color ? "white" : "black");
    if (!live) {
        if (x->nodes > 0 && x->nps) {
            send_engine(x, "nps 1\nst %" PRId64 "\n", x->nodes);
        } else {
            if (x->nps) send_engine(x, "nps 0\n");
            send_engine(x, "level %d %" PRId64 ":%02" PRId64 " %" PRId64 ".%03" PRId64 "\n",
                x->moves_to_go, own / 60000, own / 1000 % 60, increment / 1000, increment % 1000);
            if (x->movetime > 0)
                send_engine(x, "st %" PRId64 ".%03" PRId64 "\n", x->movetime / 1000, x->movetime % 1000);
        }
        if (x->depth > 0) send_engine(x, "sd %d\n", x->depth);
    }
    send_engine(x, "time %" PRId64 "\notim %" PRId64 "\n", at_least_one(own / 10), at_least_one(x->time[!color] / 10));
    if (!live && x->colors) send_engine(x, "%s\n", color ? "black" : "white");
    if (x->draw_offered && x->draw) send_engine(x, "draw\n");
}
static void clear_search(XBoard *x) {
    x->best[0] = x->hint[0] = x->pv_first[0] = x->pv_second[0] = 0;
    x->resigned = x->offered = x->accepted = 0;
    x->finish_ping = 0;
}
static void parse_go(XBoard *x, const char *line) {
    x->time[0] = x->time[1] = 60000; x->increment[0] = x->increment[1] = 0;
    x->movetime = x->nodes = 0; x->depth = x->moves_to_go = 0;
    char copy[8192]; snprintf(copy, sizeof(copy), "%s", line);
    char *save = NULL;
    for (char *key = strtok_r(copy, " ", &save); key; key = strtok_r(NULL, " ", &save)) {
        int64_t *destination = NULL;
        if (!strcmp(key, "wtime")) destination = &x->time[0];
        if (!strcmp(key, "btime")) destination = &x->time[1];
        if (!strcmp(key, "winc")) destination = &x->increment[0];
        if (!strcmp(key, "binc")) destination = &x->increment[1];
        if (!strcmp(key, "movetime")) destination = &x->movetime;
        if (!strcmp(key, "nodes")) destination = &x->nodes;
        if (destination) {
            char *value = strtok_r(NULL, " ", &save);
            if (value) *destination = strtoll(value, NULL, 10);
        } else if (!strcmp(key, "depth") || !strcmp(key, "movestogo")) {
            int is_depth = !strcmp(key, "depth");
            char *value = strtok_r(NULL, " ", &save);
            if (value) { if (is_depth) x->depth = atoi(value); else x->moves_to_go = atoi(value); }
        }
    }
}
static void start_search(XBoard *x, const char *line) {
    if (x->phase != X_IDLE) { failure(x, "go received before previous search barrier"); return; }
    if (!x->valid_position) { failure(x, "go without a valid position"); return; }
    if (strlen(line) >= sizeof(x->last_go)) { failure(x, "go command too long"); return; }
    snprintf(x->last_go, sizeof(x->last_go), "%s", line);
    parse_go(x, line); clear_search(x); x->search_position = x->target;
    int pondering = strstr(line, " ponder") != NULL;
    const char *suffix = NULL;
    int incremental = x->auto_ponder && history_suffix(x, &suffix) && *suffix && !strchr(suffix, ' ');
    if (incremental) {
        Position after = x->actual;
        incremental = position_apply_uci(&after, suffix, NULL, 0) && same_position(&after, &x->target);
    }
    if (pondering && incremental) {
        /* CECP is already pondering; send the predicted move only on ponderhit. */
        x->phase = X_PONDER; return;
    }
    if (incremental && !x->nodes && !x->depth && !x->movetime) {
        time_control(x, x->target.turn, 1);
        x->phase = X_SEARCH;
        if (!send_move(x, &x->actual, suffix)) return;
        snprintf(x->engine_history, sizeof(x->engine_history), "%s", x->history);
        return;
    }
    if (!synchronize(x)) return;
    if (pondering) {
        if (x->analyze) { send_engine(x, "post\nanalyze\n"); x->phase = X_ANALYZE; }
        else x->phase = X_PONDER;
        return;
    }
    if (strstr(line, " searchmoves ")) {
        /* CECP root restrictions work only in analyze mode, not play mode. */
        failure(x, "CECP play does not support UCI searchmoves; use a UCI engine for restricted roots"); return;
    }
    time_control(x, x->target.turn, 0);
    send_engine(x, "post\n%s\ngo\n", x->ponder ? "hard" : "easy");
    x->auto_ponder = x->ponder; x->phase = X_SEARCH;
}
static void finish_search(XBoard *x) {
    if (x->resigned) puts("info string lbc_resign");
    if (x->accepted) puts("info string lbc_draw_accept");
    else if (x->offered) puts("info string lbc_draw_offer");
    const char *move = *x->best ? x->best : "0000";
    if (*x->hint && strcmp(move, "0000")) printf("bestmove %s ponder %s\n", move, x->hint);
    else printf("bestmove %s\n", move);
    x->phase = X_IDLE; x->finish_ping = 0; x->barrier_deadline = 0;
}
static void stop_search(XBoard *x) {
    if (x->phase == X_IDLE || x->phase == X_HELLO || x->phase == X_STOP || x->phase == X_FINISH) return;
    if (x->phase == X_ANALYZE) send_engine(x, "exit\n");
    if (x->phase == X_SEARCH) send_engine(x, "?\n");
    x->stop_discard = x->phase == X_PONDER || x->phase == X_ANALYZE;
    send_engine(x, "easy\nforce\n"); x->auto_ponder = 0;
    x->phase = X_STOP; x->finish_ping = ping_engine(x);
}
static void ponder_hit(XBoard *x) {
    if (x->phase == X_ANALYZE) {
        send_engine(x, "exit\neasy\nforce\n");
        x->phase = X_HIT; x->finish_ping = ping_engine(x); return;
    }
    if (x->phase != X_PONDER) return;
    x->phase = X_IDLE;
    char normal[8192]; snprintf(normal, sizeof(normal), "%s", x->last_go);
    char *p = strstr(normal, " ponder");
    if (p) memmove(p, p + 7, strlen(p + 7) + 1);
    start_search(x, normal);
}
static void post_info(XBoard *x, const char *line) {
    if (x->phase != X_SEARCH && x->phase != X_ANALYZE && x->phase != X_FINISH && x->phase != X_STOP) return;
    char depth_word[24], score_word[24]; int64_t centiseconds, nodes; int offset = 0;
    if (sscanf(line, "%23s %23s %" SCNd64 " %" SCNd64 "%n", depth_word, score_word, &centiseconds, &nodes, &offset) != 4) return;
    if (centiseconds < 0 || nodes < 0) return;
    char *end = NULL;
    long depth = strtol(depth_word, &end, 10);
    if (depth < 0 || end == depth_word || (*end && *end != '.' && *end != '+' && *end != '-')) return;
    long score = strtol(score_word, &end, 10);
    if (end == score_word || *end) return;
    /* Extended post fields precede the PV: seldepth, nps, reserved, tablebase hits. */
    const char *remainder = line + offset;
    int64_t seldepth = 0, nps = 0, hits = 0; size_t extras = 0;
    for (;;) {
        while (isspace((unsigned char)*remainder)) ++remainder;
        errno = 0; char *after = NULL;
        long long number = strtoll(remainder, &after, 10);
        if (after == remainder || errno || (*after && !isspace((unsigned char)*after))) break;
        if (number < 0) return;
        if (!extras) seldepth = (int64_t)number;
        else if (extras == 1) nps = (int64_t)number;
        else hits = (int64_t)number;
        ++extras; remainder = after;
    }
    char pv[8192] = "", text[XB_LINE + 1u];
    snprintf(text, sizeof(text), "%s", remainder);
    Position position = x->search_position;
    char *save = NULL; size_t used = 0, count = 0;
    for (char *word = strtok_r(text, " \t", &save); word; word = strtok_r(NULL, " \t", &save)) {
        char *number = word;
        while (isdigit((unsigned char)*number)) ++number;
        if (*number == '.') { while (*number == '.') ++number; word = number; if (!*word) continue; }
        char normalized[8];
        if (!convert_move(&position, word, x->chess960, normalized)) break;
        if (!count) snprintf(x->pv_first, sizeof(x->pv_first), "%s", normalized);
        if (count == 1) snprintf(x->pv_second, sizeof(x->pv_second), "%s", normalized);
        int n = snprintf(pv + used, sizeof(pv) - used, "%s%s", used ? " " : "", normalized);
        if (n < 0 || (size_t)n >= sizeof(pv) - used) break;
        used += (size_t)n; ++count;
        if (!position_apply_uci(&position, normalized, NULL, 0)) break;
    }
    if (score >= 100000 || score <= -100000) {
        long mate = score >= 100000 ? score - 100000 : score + 100000;
        printf("info depth %ld score mate %ld", depth, mate);
    } else printf("info depth %ld score cp %ld", depth, score);
    printf(" time %" PRId64 " nodes %" PRId64, centiseconds > INT64_MAX / 10 ? INT64_MAX : centiseconds * 10, nodes);
    if (extras) printf(" seldepth %" PRId64, seldepth);
    if (extras > 1) printf(" nps %" PRId64, nps);
    if (extras > 2) printf(" tbhits %" PRId64, hits);
    printf("%s%s\n", *pv ? " pv " : "", pv);
    if (x->nodes > 0 && nodes >= x->nodes && x->phase == X_SEARCH) stop_search(x);
}
static void received_move(XBoard *x, const char *text) {
    if (x->phase != X_SEARCH && x->phase != X_STOP) return;
    char word[64], move[8];
    if (sscanf(text, "%63s", word) != 1 || !convert_move(&x->search_position, word, x->chess960, move)) {
        failure(x, "CECP engine returned an illegal/unparseable move"); return;
    }
    if (x->stop_discard && x->phase == X_STOP) return;
    if (*x->best) { failure(x, "CECP engine returned two moves for one search"); return; }
    snprintf(x->best, sizeof(x->best), "%s", move);
    x->actual = x->search_position;
    if (!position_apply_uci(&x->actual, move, NULL, 0) ||
        !append_history(x->engine_history, sizeof(x->engine_history), move)) {
        failure(x, "CECP engine move exceeds history limit"); return;
    }
    if (!strcmp(move, x->pv_first) && *x->pv_second && position_is_legal(&x->actual, x->pv_second))
        snprintf(x->hint, sizeof(x->hint), "%s", x->pv_second);
    if (!x->finish_ping) x->finish_ping = ping_engine(x);
    if (x->phase != X_STOP) x->phase = X_FINISH;
}
static void engine_line(void *context, const char *line, size_t length) {
    XBoard *x = context;
    (void)length;
    while (isspace((unsigned char)*line)) ++line;
    if (x->trace) fprintf(stderr, "CECP < %s\n", line);
    if (!strncmp(line, "feature ", 8)) { features(x, line + 8); return; }
    if (*line == '#' || !*line) return;
    if (!strncmp(line, "pong ", 5)) {
        char *end = NULL; unsigned long n = strtoul(line + 5, &end, 10);
        if (*end || n > UINT32_MAX) return;
        if (x->ready_ping && (uint32_t)n == x->ready_ping) {
            x->ready_ping = 0; x->ready_pending = 0; puts("readyok");
        }
        if (x->finish_ping && (uint32_t)n == x->finish_ping) {
            if (x->phase == X_HIT) {
                x->phase = X_PONDER; x->finish_ping = 0; x->auto_ponder = 0; ponder_hit(x);
            } else {
                if (x->phase == X_STOP && !x->stop_discard && !*x->best && !x->resigned && !x->accepted && *x->pv_first) {
                    snprintf(x->best, sizeof(x->best), "%s", x->pv_first);
                    /* Engine has not committed this fallback move. */
                    x->engine_fen[0] = 0;
                }
                finish_search(x);
            }
        }
        return;
    }
    if (x->phase == X_HELLO) return;
    if (!strncmp(line, "move ", 5)) { received_move(x, line + 5); return; }
    if (!strncmp(line, "Hint: ", 6)) {
        if (*x->best) (void)convert_move(&x->actual, line + 6, x->chess960, x->hint);
        return;
    }
    if (!strcmp(line, "offer draw")) {
        if (x->draw_offered) {
            x->accepted = 1;
            if (x->phase == X_SEARCH) { send_engine(x, "easy\nforce\n"); x->auto_ponder = 0; x->phase = X_FINISH; x->finish_ping = ping_engine(x); }
        } else x->offered = 1;
        return;
    }
    if (!strcmp(line, "resign") ||
        ((!strncmp(line, "1-0", 3) || !strncmp(line, "0-1", 3)) && strstr(line, "resign"))) {
        x->resigned = 1; x->best[0] = 0; x->auto_ponder = 0;
        send_engine(x, "easy\nforce\n"); x->phase = X_FINISH;
        if (!x->finish_ping) x->finish_ping = ping_engine(x);
        return;
    }
    if ((!strncmp(line, "1-0", 3) && (!line[3] || isspace((unsigned char)line[3]))) ||
        (!strncmp(line, "0-1", 3) && (!line[3] || isspace((unsigned char)line[3]))) ||
        (!strncmp(line, "1/2-1/2", 7) && (!line[7] || isspace((unsigned char)line[7])))) {
        /* Wait for Lichess to confirm the result; preserve any preceding engine move. */
        if (x->phase == X_SEARCH || x->phase == X_FINISH || x->phase == X_STOP) {
            if (!strncmp(line, "1/2-1/2", 7) && x->draw_offered) x->accepted = 1;
            send_engine(x, "easy\nforce\n"); x->auto_ponder = 0;
            if (x->phase != X_STOP) x->phase = X_FINISH;
            if (!x->finish_ping) x->finish_ping = ping_engine(x);
        }
        return;
    }
    if (!strncasecmp(line, "Error", 5) || !strncasecmp(line, "Illegal move", 12)) {
        failure(x, line); return;
    }
    if (isdigit((unsigned char)*line)) {
        const char *p = line;
        while (isdigit((unsigned char)*p)) ++p;
        if (*p == '.') ++p;
        while (*p == ' ') ++p;
        if (!strncmp(p, "... ", 4)) received_move(x, p + 4);
        else post_info(x, line);
    }
}
static void position_command(XBoard *x, const char *line) {
    if (x->phase != X_IDLE) { failure(x, "position received while CECP search is active"); return; }
    char copy[XB_LINE + 1u];
    snprintf(copy, sizeof(copy), "%s", line);
    char *moves = strstr(copy, " moves ");
    if (moves) { *moves = 0; moves += 7; }
    const char *fen = !strcmp(copy, "startpos") ? "startpos" : !strncmp(copy, "fen ", 4) ? copy + 4 : NULL;
    if (!fen || strlen(fen) >= sizeof(x->fen) || (moves && strlen(moves) >= sizeof(x->history))) {
        failure(x, "invalid or oversized UCI position"); return;
    }
    char err[160];
    if (!position_from_history(&x->target, board_variant(x), fen, moves ? moves : "", err, sizeof(err))) {
        failure(x, err); return;
    }
    if (x->chess960) x->target.chess960 = 1;
    memcpy(x->fen, fen, strlen(fen) + 1);
    snprintf(x->history, sizeof(x->history), "%s", moves ? moves : "");
    x->valid_position = 1;
}
static void set_option(XBoard *x, const char *arg) {
    char copy[4096];
    if (strlen(arg) >= sizeof(copy)) { failure(x, "oversized UCI option"); return; }
    snprintf(copy, sizeof(copy), "%s", arg);
    char *value = strstr(copy, " value ");
    if (value) { *value = 0; value += 7; } else value = copy + strlen(copy);
    if (!strcmp(value, "<empty>")) *value = 0;
    if (!safe_text(copy) || !safe_text(value)) { failure(x, "invalid option line"); return; }
    if (!strcasecmp(copy, "Ponder")) { x->ponder = !strcasecmp(value, "true") || !strcmp(value, "1"); return; }
    if (!strcasecmp(copy, "UCI_Chess960")) { x->chess960 = !strcasecmp(value, "true") || !strcmp(value, "1"); return; }
    if (!strcasecmp(copy, "UCI_Variant")) {
        unsigned v = !strcmp(value, "chess") ? POSITION_STANDARD : position_variant_from_name(value);
        if (!v || !in_list(x->variants, cecp_variant(x, v))) { failure(x, "unsupported UCI_Variant for CECP engine"); return; }
        x->variant = v; return;
    }
    if (!strcasecmp(copy, "UCI_Opponent")) {
        char title[32], rating[32], kind[32]; int offset = 0;
        if (sscanf(value, "%31s %31s %31s %n", title, rating, kind, &offset) != 3) { failure(x, "invalid UCI_Opponent"); return; }
        x->opponent_rating = atoi(rating); x->opponent_computer = !strcmp(kind, "computer");
        snprintf(x->opponent, sizeof(x->opponent), "%.*s", (int)sizeof(x->opponent) - 1, value + offset);
        send_opponent(x); return;
    }
    if (!strcasecmp(copy, "LBC_EngineRating") || !strcmp(copy, "engine_rating")) { x->own_rating = atoi(value); return; }
    if (!strcmp(copy, "opponent_rating")) { x->opponent_rating = atoi(value); return; }
    if (!strcmp(copy, "name")) { snprintf(x->opponent, sizeof(x->opponent), "%.*s", (int)sizeof(x->opponent) - 1, value); return; }
    if (!strcmp(copy, "computer")) { x->opponent_computer = !strcasecmp(value, "true") || atoi(value) != 0; return; }
    if (!strcmp(copy, "random")) { x->random = !strcasecmp(value, "true") || atoi(value) != 0; return; }
    int custom = 0;
    for (size_t i = 0; i < x->option_count; ++i) if (!strcasecmp(x->options[i].name, copy)) { custom = 1; break; }
    if ((!strcasecmp(copy, "memory") || (!custom && !strcasecmp(copy, "Hash"))) && x->memory) { send_engine(x, "memory %s\n", value); return; }
    if ((!strcasecmp(copy, "cores") || (!custom && !strcasecmp(copy, "Threads"))) && x->smp) { send_engine(x, "cores %s\n", value); return; }
    if (!strncmp(copy, "egtpath ", 8)) {
        if (!in_list(x->egt, copy + 8)) { failure(x, "engine did not advertise requested tablebase format"); return; }
        send_engine(x, "%s %s\n", copy, value); return;
    }
    for (size_t i = 0; i < x->option_count; ++i) {
        XOption *o = &x->options[i];
        if (strcasecmp(o->name, copy)) continue;
        if (!strcmp(o->type, "check")) value = !strcasecmp(value, "true") || !strcmp(value, "1") ? "1" : "0";
        int button = !strcmp(o->type, "button") || !strcmp(o->type, "save") || !strcmp(o->type, "reset");
        send_engine(x, "option %s%s%s\n", o->name, button ? "" : "=", button ? "" : value);
        o->configured = 1; snprintf(o->value, sizeof(o->value), "%.*s", (int)sizeof(o->value) - 1, value);
        return;
    }
    failure(x, "configured option was not advertised by the CECP engine");
}
static void input_line(XBoard *x, const char *line) {
    while (*line == ' ' || *line == '\t') ++line;
    if (x->trace) fprintf(stderr, "UCI > %s\n", line);
    if (!strcmp(line, "uci")) { x->uci_requested = 1; advertise(x); }
    else if (!strcmp(line, "quit")) x->quitting = 1;
    else if (!strcmp(line, "isready")) {
        if (x->phase == X_SEARCH || x->phase == X_PONDER || x->phase == X_ANALYZE) puts("readyok");
        else { x->ready_pending = 1; if (x->done) x->ready_ping = ping_engine(x); }
    } else if (!strcmp(line, "ucinewgame")) {
        if (x->phase != X_IDLE) { failure(x, "ucinewgame before search finished"); return; }
        new_game(x);
    } else if (!strncmp(line, "setoption name ", 15)) set_option(x, line + 15);
    else if (!strncmp(line, "position ", 9)) position_command(x, line + 9);
    else if (!strncmp(line, "go", 2) && (!line[2] || line[2] == ' ')) start_search(x, line);
    else if (!strcmp(line, "stop")) stop_search(x);
    else if (!strcmp(line, "ponderhit")) ponder_hit(x);
    else if (!strncmp(line, "lbc_draw_offer ", 15)) x->draw_offered = !strcmp(line + 15, "true");
    else if (!strncmp(line, "lbc_game ", 9) || !strncmp(line, "lbc_state ", 10)) {
        int full = line[4] == 'g';
        const char *text = line + (full ? 9 : 10);
        Json json; JsonTok tokens[8192];
        if (json_parse(&json, text, strlen(text), tokens, 8192)) { failure(x, "invalid game metadata JSON"); return; }
        int state = full ? json_get(&json, 0, "state") : 0;
        if (full) {
            if (json_string(&json, json_get(&json, 0, "initialFen"), x->game_initial_fen, sizeof(x->game_initial_fen)) < 0)
                snprintf(x->game_initial_fen, sizeof(x->game_initial_fen), "startpos");
            x->state_history_valid = 0;
        }
        int moves = json_get(&json, state, "moves");
        if (moves >= 0) {
            if (json_string(&json, moves, x->state_history, sizeof(x->state_history)) < 0) { failure(x, "invalid state move history"); return; }
            x->state_history_valid = 1;
        }
    }
    else if (!strncmp(line, "lbc_result ", 11)) {
        char result[16]; int offset = 0;
        if (sscanf(line + 11, "%15s %n", result, &offset) != 1 ||
            (strcmp(result, "1-0") && strcmp(result, "0-1") && strcmp(result, "1/2-1/2") && strcmp(result, "*"))) {
            failure(x, "invalid lbc_result"); return;
        }
        char reason[1024]; snprintf(reason, sizeof(reason), "%.*s", (int)sizeof(reason) - 1, line + 11 + offset);
        for (char *p = reason; *p; ++p) if (*p == '{' || *p == '}') *p = ' ';
        if (x->state_history_valid && x->phase == X_IDLE) {
            const char *fen = *x->game_initial_fen ? x->game_initial_fen : *x->fen ? x->fen : "startpos";
            if (!position_from_history(&x->target, board_variant(x), fen, x->state_history, NULL, 0)) {
                failure(x, "invalid final game position"); return;
            }
            if (fen != x->fen) snprintf(x->fen, sizeof(x->fen), "%s", fen);
            snprintf(x->history, sizeof(x->history), "%s", x->state_history);
            if (!synchronize(x)) return;
        }
        send_engine(x, "easy\nforce\nresult %s {%s}\n", result, reason); x->auto_ponder = 0;
    }
}
static void read_input(XBoard *x) {
    char bytes[8192];
    ssize_t n = read(STDIN_FILENO, bytes, sizeof(bytes));
    if (n == 0) { x->quitting = 1; return; }
    if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) x->quitting = 1; return; }
    for (ssize_t i = 0; i < n; ++i) {
        if (bytes[i] == '\n') {
            if (x->input_length && x->input[x->input_length - 1] == '\r') --x->input_length;
            x->input[x->input_length] = 0; input_line(x, x->input); x->input_length = 0;
            if (x->failed || x->quitting) return;
        } else if (x->input_length < XB_LINE) x->input[x->input_length++] = bytes[i];
        else { failure(x, "UCI command exceeds input limit"); return; }
    }
}
int xboard_adapter_main(int argc, char **argv) {
    int first = 1, trace = 0;
    while (first < argc && strcmp(argv[first], "--")) {
        if (!strcmp(argv[first], "--trace")) trace = 1;
        else { fprintf(stderr, "usage: lbc-xboard [--trace] -- engine [args ...]\n"); return 2; }
        ++first;
    }
    if (++first >= argc) { fprintf(stderr, "usage: lbc-xboard [--trace] -- engine [args ...]\n"); return 2; }
    XBoard *x = calloc(1, sizeof(*x)); if (!x) return 1;
    x->argv = argv + first; x->trace = trace; x->phase = X_HELLO;
    x->variant = POSITION_STANDARD; x->draw = x->name = x->colors = x->reuse = 1;
    snprintf(x->variants, sizeof(x->variants), "normal");
    engine_init(&x->engine);
    if (engine_spawn_attached(&x->engine, x->argv, NULL)) { fprintf(stderr, "%s\n", x->engine.error); free(x); return 1; }
    (void)signal(SIGPIPE, SIG_IGN); (void)signal(SIGTERM, stop_signal); (void)signal(SIGINT, stop_signal);
    setbuf(stdout, NULL);
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) { engine_terminate(&x->engine); free(x); return 1; }
    send_engine(x, "xboard\nprotover 2\n"); x->hello_deadline = clock_ms() + 2000;
    while (!x->failed && !x->quitting && !interrupted) {
        if (engine_wants_write(&x->engine) && engine_flush(&x->engine)) { failure(x, x->engine.error); break; }
        int64_t now = clock_ms(), due = now + 60000;
        if (x->phase == X_HELLO) {
            if (now >= x->hello_deadline) { x->done = 1; advertise(x); if (!x->uci_sent) x->hello_deadline = now + 2000; }
            if (due > x->hello_deadline) due = x->hello_deadline;
        }
        if (x->finish_ping || x->ready_ping) {
            if (now >= x->barrier_deadline) { failure(x, "CECP ping barrier timed out"); break; }
            if (due > x->barrier_deadline) due = x->barrier_deadline;
        }
        int timeout = due <= now ? 0 : due - now > INT_MAX ? INT_MAX : (int)(due - now);
        struct pollfd fds[3] = {
            { STDIN_FILENO, POLLIN, 0 }, { x->engine.output_fd, POLLIN, 0 },
            { engine_wants_write(&x->engine) ? x->engine.input_fd : -1, POLLOUT, 0 }
        };
        int rc = poll(fds, 3, timeout);
        if (rc < 0) { if (errno == EINTR) continue; failure(x, "poll failed"); break; }
        if (fds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
            if (engine_read(&x->engine, engine_line, x)) { failure(x, x->engine.error); break; }
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) read_input(x);
        if (fds[2].revents & POLLOUT) (void)engine_flush(&x->engine);
    }
    if (!x->engine.dead) { send_engine(x, "quit\n"); (void)engine_flush(&x->engine); }
    /* Allow engine learning to finish. */
    int64_t end = clock_ms() + 300;
    while (engine_reap(&x->engine) == 0 && clock_ms() < end) {
        struct pollfd fd = { x->engine.output_fd, POLLIN, 0 };
        (void)poll(&fd, 1, 10);
        if (fd.revents & (POLLIN | POLLHUP | POLLERR)) (void)engine_read(&x->engine, NULL, NULL);
    }
    engine_terminate(&x->engine); int result = x->failed ? 1 : 0;
    free(x); return result;
}
