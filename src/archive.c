#define _POSIX_C_SOURCE 200809L
#include "archive.h"
#include "position.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PGN_LIMIT (8U * 1024U * 1024U)
#define PLY_LIMIT 16384U
#define TAG_LIMIT 256U
#define QUEUE_LIMIT 64U
#define QUEUE_BYTE_LIMIT (256U * 1024U * 1024U)

typedef struct { char *data; size_t used, capacity, column; int failed; } Buffer;
typedef struct { char *key, *value; } Tag;
typedef struct { char move[8]; char *comment, *extras; } PgnPly;
typedef struct {
    Tag tags[TAG_LIMIT]; size_t tag_count;
    PgnPly *plies; size_t count, capacity;
    char *leading_comment;
    int valid;
} Pgn;
typedef struct ArchiveJob {
    ArchiveGame game;
    Annotations annotations;
    char *strings[20]; size_t string_count, bytes;
    struct ArchiveJob *next;
} ArchiveJob;
struct Archive {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t work_ready, idle;
    ArchiveJob *head, *tail;
    size_t queue_size, queue_bytes;
    int busy, stopping;
    char *directory, *grouping, *bot_name, *base_url;
    char error[512];
};

static void set_error(char *error, size_t cap, const char *format, ...) {
    if (!error || !cap) return;
    va_list args; va_start(args, format);
    (void)vsnprintf(error, cap, format, args);
    va_end(args);
}

static int reserve(Buffer *b, size_t extra) {
    if (b->failed || extra > PGN_LIMIT || b->used > PGN_LIMIT - extra) { b->failed = 1; return -1; }
    size_t needed = b->used + extra + 1;
    if (needed <= b->capacity) return 0;
    size_t capacity = b->capacity ? b->capacity : 512;
    while (capacity < needed) capacity *= 2;
    char *data = realloc(b->data, capacity);
    if (!data) { b->failed = 1; return -1; }
    b->data = data; b->capacity = capacity;
    if (!b->used) *b->data = 0;
    return 0;
}

static int append_n(Buffer *b, const char *text, size_t length) {
    if (reserve(b, length)) return -1;
    memcpy(b->data + b->used, text, length);
    b->used += length; b->data[b->used] = 0;
    for (size_t i = 0; i < length; ++i) b->column = text[i] == '\n' ? 0 : b->column + 1;
    return 0;
}
static int append(Buffer *b, const char *text) { return append_n(b, text, strlen(text)); }
static int append_format(Buffer *b, const char *format, ...) {
    va_list args, copy; va_start(args, format); va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy); va_end(copy);
    if (length < 0 || reserve(b, (size_t)length)) { va_end(args); return -1; }
    (void)vsnprintf(b->data + b->used, b->capacity - b->used, format, args); va_end(args);
    for (size_t i = 0; i < (size_t)length; ++i) b->column = b->data[b->used + i] == '\n' ? 0 : b->column + 1;
    b->used += (size_t)length;
    return 0;
}
static void token(Buffer *b, const char *text) {
    size_t length = strlen(text);
    if (b->column && b->column + length + 1 > 100) (void)append(b, "\n");
    else if (b->column) (void)append(b, " ");
    (void)append(b, text);
}

static char *copy_n(const char *text, size_t length) {
    if (length > PGN_LIMIT) return NULL;
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length); copy[length] = 0;
    return copy;
}

static int join_text(char **target, const char *text, size_t length) {
    if (!length) return 0;
    size_t old = *target ? strlen(*target) : 0;
    if (length > PGN_LIMIT || old > PGN_LIMIT - length - 1) return -1;
    char *copy = realloc(*target, old + length + 2);
    if (!copy) return -1;
    if (old) copy[old++] = ' ';
    memcpy(copy + old, text, length); copy[old + length] = 0;
    *target = copy;
    return 0;
}

static void pgn_free(Pgn *p) {
    for (size_t i = 0; i < p->tag_count; ++i) { free(p->tags[i].key); free(p->tags[i].value); }
    for (size_t i = 0; i < p->count; ++i) { free(p->plies[i].comment); free(p->plies[i].extras); }
    free(p->plies); free(p->leading_comment);
    memset(p, 0, sizeof(*p));
}

static const char *tag_get(const Pgn *p, const char *key) {
    for (size_t i = 0; i < p->tag_count; ++i) if (!strcmp(p->tags[i].key, key)) return p->tags[i].value;
    return NULL;
}

static int tag_put(Pgn *p, const char *key, const char *value, int replace) {
    if (!value) return 0;
    for (size_t i = 0; i < p->tag_count; ++i) if (!strcmp(p->tags[i].key, key)) {
        if (!replace) return 0;
        char *copy = strdup(value); if (!copy) return -1;
        free(p->tags[i].value); p->tags[i].value = copy; return 0;
    }
    if (p->tag_count >= TAG_LIMIT) return -1;
    char *name = strdup(key), *copy = strdup(value);
    if (!name || !copy) { free(name); free(copy); return -1; }
    p->tags[p->tag_count++] = (Tag){name, copy}; return 0;
}

static int tag_fill(Pgn *p, const char *key, const char *value) {
    const char *old = tag_get(p, key);
    return tag_put(p, key, value, !old || !*old || *old == '?');
}

static const char *read_tags(Pgn *p, const char *text) {
    if (!text) return "";
    const unsigned char *bytes = (const unsigned char *)text;
    if (bytes[0] == 0xef && bytes[1] && bytes[1] == 0xbb && bytes[2] && bytes[2] == 0xbf) text += 3;
    for (;;) {
        while (isspace((unsigned char)*text)) ++text;
        if (*text != '[') return text;
        const char *start = ++text;
        while (isalnum((unsigned char)*text) || *text == '_') ++text;
        size_t length = (size_t)(text - start);
        if (!length || length >= 128) return NULL;
        char name[128]; memcpy(name, start, length); name[length] = 0;
        while (isspace((unsigned char)*text)) ++text;
        if (*text++ != '"') return NULL;
        Buffer value = {0};
        while (*text && *text != '"') {
            if (*text == '\\' && text[1]) ++text;
            if (append_n(&value, text++, 1)) break;
        }
        if (value.failed || *text != '"') { free(value.data); return NULL; }
        ++text;
        while (isspace((unsigned char)*text)) ++text;
        if (*text++ != ']' || tag_put(p, name, value.data ? value.data : "", 1)) { free(value.data); return NULL; }
        free(value.data);
    }
}

static int pgn_add_move(Pgn *p, const char *move) {
    if (p->count >= PLY_LIMIT) return -1;
    if (p->count == p->capacity) {
        size_t capacity = p->capacity ? p->capacity * 2 : 128;
        PgnPly *plies = realloc(p->plies, capacity * sizeof(*plies));
        if (!plies) return -1;
        p->plies = plies; p->capacity = capacity;
    }
    PgnPly *ply = &p->plies[p->count++];
    memset(ply, 0, sizeof(*ply));
    (void)snprintf(ply->move, sizeof(ply->move), "%s", move);
    return 0;
}

static int is_result(const char *text) {
    return text && (!strcmp(text, "*") || !strcmp(text, "1-0") || !strcmp(text, "0-1") || !strcmp(text, "1/2-1/2"));
}

static const char *variation_end(const char *text) {
    unsigned depth = 0; int comment = 0, semicolon = 0;
    for (; *text; ++text) {
        if (semicolon) { if (*text == '\n') semicolon = 0; continue; }
        if (comment) { if (*text == '}') comment = 0; continue; }
        if (*text == '{') { comment = 1; continue; }
        if (*text == ';') { semicolon = 1; continue; }
        if (*text == '(') { if (++depth > 64) return NULL; }
        else if (*text == ')') { if (!depth || !--depth) return text + 1; }
    }
    return NULL;
}

static void parse_pgn(Pgn *p, const char *text, const ArchiveGame *game) {
    if (!text || !*text || strnlen(text, PGN_LIMIT + 1U) > PGN_LIMIT) return;
    const char *cursor = read_tags(p, text);
    if (!cursor) return;
    unsigned variant = (unsigned)game->variant;
    const char *variant_tag = tag_get(p, "Variant");
    if (variant_tag) {
        unsigned parsed_variant = position_variant_from_name(variant_tag);
        if (parsed_variant) variant = parsed_variant;
    }
    const char *fen = tag_get(p, "FEN");
    if (!fen) fen = game->fen;
    Position board;
    if (!position_init(&board, variant, fen, NULL, 0)) return;
    while (*cursor) {
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (!*cursor) break;
        if (*cursor == '%' && (cursor == text || cursor[-1] == '\n')) {
            while (*cursor && *cursor != '\n') ++cursor;
            continue;
        }
        if (*cursor == '{' || *cursor == ';') {
            int brace = *cursor == '{';
            const char *start = ++cursor;
            while (*cursor && *cursor != (brace ? '}' : '\n')) ++cursor;
            if (brace && !*cursor) return;
            char **comment = p->count ? &p->plies[p->count - 1].comment : &p->leading_comment;
            if (join_text(comment, start, (size_t)(cursor - start))) return;
            if (*cursor) ++cursor;
            continue;
        }
        if (*cursor == '(') {
            const char *end = variation_end(cursor);
            if (!end || !p->count) return;
            if (join_text(&p->plies[p->count - 1].extras, cursor, (size_t)(end - cursor))) return;
            cursor = end; continue;
        }
        const char *start = cursor;
        while (*cursor && !isspace((unsigned char)*cursor) && !strchr("{}();", *cursor)) ++cursor;
        if (cursor == start) return;
        size_t length = (size_t)(cursor - start);
        if (length >= 128) return;
        char symbol[128]; memcpy(symbol, start, length); symbol[length] = 0;
        if (is_result(symbol)) { (void)tag_put(p, "Result", symbol, 1); break; }
        if (*symbol == '$' || !strcmp(symbol, "!") || !strcmp(symbol, "?") || !strcmp(symbol, "!!") ||
            !strcmp(symbol, "??") || !strcmp(symbol, "!?") || !strcmp(symbol, "?!")) {
            if (p->count && join_text(&p->plies[p->count - 1].extras, symbol, length)) return;
            continue;
        }
        char *san = symbol;
        while (isdigit((unsigned char)*san)) ++san;
        if (*san == '.') { while (*san == '.') ++san; }
        else san = symbol;
        if (!*san || !strcmp(san, "e.p.")) continue;
        char move[8];
        if (!position_san_to_uci(&board, san, move) || !position_apply_uci(&board, move, NULL, 0) || pgn_add_move(p, move)) return;
    }
    p->valid = 1;
}

static int parse_history(Pgn *p, const ArchiveGame *game, char *error, size_t error_cap) {
    Position board;
    if (!position_init(&board, (unsigned)game->variant, game->fen, error, error_cap)) return -1;
    const char *cursor = game->moves ? game->moves : "";
    while (*cursor) {
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (!*cursor) break;
        const char *start = cursor;
        while (*cursor && !isspace((unsigned char)*cursor)) ++cursor;
        size_t length = (size_t)(cursor - start);
        if (!length || length >= 8) return -1;
        char raw[8], move[8]; memcpy(raw, start, length); raw[length] = 0;
        if (!position_normalize_uci(&board, raw, board.chess960, move) ||
            !position_apply_uci(&board, move, error, error_cap) || pgn_add_move(p, move)) return -1;
    }
    p->valid = 1;
    return 0;
}

static const char *pretty_variant(int variant) {
    switch (variant) {
    case POSITION_CHESS960: return "Chess960";
    case POSITION_ANTICHESS: return "Antichess";
    case POSITION_ATOMIC: return "Atomic";
    case POSITION_CRAZYHOUSE: return "Crazyhouse";
    case POSITION_HORDE: return "Horde";
    case POSITION_KING_OF_THE_HILL: return "King of the Hill";
    case POSITION_RACING_KINGS: return "Racing Kings";
    case POSITION_THREE_CHECK: return "Three-check";
    default: return "Standard";
    }
}

static int compatible(const Pgn *a, const Pgn *b, size_t through) {
    if (!a->valid || !b->valid || a->count < through || b->count < through) return 0;
    for (size_t i = 0; i < through; ++i) if (strcmp(a->plies[i].move, b->plies[i].move)) return 0;
    return 1;
}

static int copy_tags(Pgn *dst, const Pgn *src, int replace) {
    for (size_t i = 0; i < src->tag_count; ++i) if (tag_put(dst, src->tags[i].key, src->tags[i].value, replace)) return -1;
    return 0;
}

static int prepare_headers(Pgn *headers, const ArchiveGame *game, const char *base_url) {
    char value[512];
    const char *variant = pretty_variant(game->variant);
    const char *category = game->variant == POSITION_STANDARD || game->variant == POSITION_FROM_POSITION ?
                           (game->speed && *game->speed ? game->speed : "standard") : variant;
    (void)snprintf(value, sizeof(value), "%s %s game", game->rated ? "Rated" : "Casual", category);
    size_t start = game->rated ? 6 : 7;
    value[start] = (char)toupper((unsigned char)value[start]);
    if (tag_fill(headers, "Event", value)) return -1;
    (void)snprintf(value, sizeof(value), "%s%s%s", base_url ? base_url : "https://lichess.org",
                   base_url && *base_url && base_url[strlen(base_url) - 1] == '/' ? "" : "/", game->id ? game->id : "unknown");
    if (tag_fill(headers, "Site", value) || tag_put(headers, "LichessGameId", game->id ? game->id : "unknown", 1) ||
        tag_put(headers, "LichessGameFinished", game->finished ? "1" : "0", 1)) return -1;
    char date[32] = "????.??.??", clock[32] = "??:??:??";
    if (game->created_at > 0) {
        time_t seconds = (time_t)(game->created_at / 1000); struct tm utc;
        if (gmtime_r(&seconds, &utc)) {
            (void)strftime(date, sizeof(date), "%Y.%m.%d", &utc);
            (void)strftime(clock, sizeof(clock), "%H:%M:%S", &utc);
        }
    }
    if (tag_fill(headers, "Date", date) || tag_fill(headers, "Round", "-") ||
        tag_fill(headers, "UTCDate", date) || tag_fill(headers, "UTCTime", clock) || tag_fill(headers, "Variant", variant)) return -1;
    for (int color = 0; color < 2; ++color) {
        if (tag_fill(headers, color ? "Black" : "White", game->names[color] && *game->names[color] ? game->names[color] : "?")) return -1;
        if (game->ratings[color] > 0) {
            (void)snprintf(value, sizeof(value), "%d", game->ratings[color]);
            if (tag_fill(headers, color ? "BlackElo" : "WhiteElo", value)) return -1;
        }
        if (game->titles[color] && *game->titles[color] && tag_fill(headers, color ? "BlackTitle" : "WhiteTitle", game->titles[color])) return -1;
    }
    const char *result = tag_get(headers, "Result");
    if (is_result(game->result) && strcmp(game->result, "*")) result = game->result;
    if (!is_result(result)) result = "*";
    if (tag_put(headers, "Result", result, 1)) return -1;
    if (game->fen && *game->fen && strcmp(game->fen, "startpos")) {
        if (tag_put(headers, "SetUp", "1", 1) || tag_put(headers, "FEN", game->fen, 1)) return -1;
    }
    if (!game->speed || strcmp(game->speed, "correspondence")) {
        if (game->initial_ms >= 0 && game->increments[0] >= 0) {
            char initial[64], increment[64];
            (void)snprintf(initial, sizeof(initial), "%" PRId64, game->initial_ms / 1000);
            (void)snprintf(increment, sizeof(increment), "%" PRId64, game->increments[0] / 1000);
            if (game->initial_ms % 1000) (void)snprintf(initial, sizeof(initial), "%" PRId64 ".%03" PRId64,
                                                      game->initial_ms / 1000, game->initial_ms % 1000);
            if (game->increments[0] % 1000) (void)snprintf(increment, sizeof(increment), "%" PRId64 ".%03" PRId64,
                                                         game->increments[0] / 1000, game->increments[0] % 1000);
            (void)snprintf(value, sizeof(value), "%s+%s", initial, increment);
            if (tag_fill(headers, "TimeControl", value)) return -1;
        }
    }
    if (game->status && *game->status) {
        const char *termination = !strcmp(game->status, "outoftime") ? "time forfeit" :
                                  !strcmp(game->status, "aborted") ? "abandoned" :
                                  !strcmp(game->status, "noStart") ? "abandoned" :
                                  game->finished ? "normal" : "unterminated";
        if (tag_fill(headers, "Termination", termination)) return -1;
    }
    if (game->tournament && *game->tournament && tag_fill(headers, "Tournament", game->tournament)) return -1;
    return 0;
}

static void emit_tag(Buffer *b, const char *key, const char *value) {
    (void)append_format(b, "[%s \"", key);
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '\\' || *p == '"') (void)append(b, "\\");
        char ch = *p < 32 || *p == 127 ? ' ' : (char)*p;
        (void)append_n(b, &ch, 1);
    }
    (void)append(b, "\"]\n");
}

static const MoveInfo *annotation_at(const ArchiveGame *game, size_t ply) {
    if (!game->annotations) return NULL;
    for (size_t i = 0; i < game->annotations->count; ++i)
        if (game->annotations->items[i].ply >= 0 && (size_t)game->annotations->items[i].ply == ply)
            return &game->annotations->items[i].info;
    return NULL;
}

static void clean_comment(Buffer *out, const char *text, int remove_eval, int remove_clock) {
    if (!text) return;
    if (out->used && *text) (void)append(out, " ");
    for (const char *p = text; *p;) {
        if ((remove_eval && (!strncmp(p, "[%eval ", 7) || !strncmp(p, "[%source ", 9))) ||
            (remove_clock && !strncmp(p, "[%clk ", 6))) {
            const char *end = strchr(p, ']');
            if (end) { p = end + 1; continue; }
        }
        unsigned char ch = (unsigned char)*p++;
        if (ch == '{' || ch == '}') ch = ch == '{' ? '(' : ')';
        if (ch < 32 || ch == 127) ch = ' ';
        char value = (char)ch; (void)append_n(out, &value, 1);
    }
}

static void emit_comment(Buffer *out, Buffer *comment) {
    if (!comment->used) return;
    if (out->column) (void)append(out, " ");
    (void)append(out, "{ "); (void)append_n(out, comment->data, comment->used); (void)append(out, " }");
}

static void add_evaluation(Buffer *comment, const MoveInfo *info, int ours) {
    if (!info) return;
    if (info->has_score) {
        int64_t sign = ours == 1 ? -1 : 1;
        if (comment->used) (void)append(comment, " ");
        if (info->is_mate) (void)append_format(comment, "[%%eval #%" PRId64, (int64_t)info->mate * sign);
        else (void)append_format(comment, "[%%eval %.2f", (double)((int64_t)info->score * sign) / 100.0);
        if (info->depth > 0) (void)append_format(comment, ",%d", info->depth);
        (void)append(comment, "]");
    }
    if (*info->source) {
        if (comment->used) (void)append(comment, " ");
        (void)append(comment, "[%source ");
        for (const unsigned char *p = (const unsigned char *)info->source; *p; ++p) {
            char ch = *p < 32 || strchr("{}[]", *p) ? ' ' : (char)*p;
            (void)append_n(comment, &ch, 1);
        }
        (void)append(comment, "]");
    }
    if (info->has_tb) {
        if (comment->used) (void)append(comment, " ");
        (void)append_format(comment, "Tablebase WDL %d, DTZ %d, DTM %d", info->tb_wdl, info->dtz, info->dtm);
    }
}

static char *render_pv(const Position *before, const MoveInfo *info) {
    if (!info || !*info->pv) return NULL;
    Position board = *before;
    Buffer result = {0}; (void)append(&result, "(");
    const char *cursor = info->pv; size_t count = 0;
    while (*cursor && count < 1024) {
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (!*cursor) break;
        const char *start = cursor;
        while (*cursor && !isspace((unsigned char)*cursor)) ++cursor;
        size_t length = (size_t)(cursor - start);
        if (length >= 8) break;
        char move[8], san[64], prefix[32]; memcpy(move, start, length); move[length] = 0;
        if (!position_uci_to_san(&board, move, san, sizeof(san))) break;
        if (board.turn == 0 || !count) {
            (void)snprintf(prefix, sizeof(prefix), "%d%s", board.fullmove_number, board.turn ? "..." : ".");
            if (!count) (void)append(&result, prefix); else token(&result, prefix);
        }
        token(&result, san);
        if (!position_apply_uci(&board, move, NULL, 0)) break;
        ++count;
    }
    (void)append(&result, ")");
    if (!count || result.failed) { free(result.data); return NULL; }
    return result.data;
}

static char *render_game(const ArchiveGame *game, const char *base_url, const char *previous,
                          char *error, size_t error_cap) {
    if (error && error_cap) *error = 0;
    if (!game || !game->id || game->variant <= 0) { set_error(error, error_cap, "Invalid archive game snapshot"); return NULL; }
    Pgn history = {0}, official = {0}, old = {0}, headers = {0};
    Buffer output = {0};
    parse_pgn(&official, game->official_pgn, game);
    parse_pgn(&old, previous, game);
    if (parse_history(&history, game, error, error_cap)) goto fail;
    size_t compared = official.count < history.count ? official.count : history.count;
    if (!compatible(&official, &history, compared)) pgn_free(&official);
    Pgn *mainline = &history;
    if (official.valid && official.count > history.count && compatible(&official, &history, history.count)) mainline = &official;
    size_t common_old = 0, common_official = 0;
    while (common_old < old.count && common_old < mainline->count &&
           !strcmp(old.plies[common_old].move, mainline->plies[common_old].move)) ++common_old;
    while (common_official < official.count && common_official < mainline->count &&
           !strcmp(official.plies[common_official].move, mainline->plies[common_official].move)) ++common_official;
    if (copy_tags(&headers, &old, 0) || copy_tags(&headers, &official, 1) || prepare_headers(&headers, game, base_url)) goto fail;
    const char *const roster[] = {"Event", "Site", "Date", "Round", "White", "Black", "Result"};
    for (size_t i = 0; i < sizeof(roster) / sizeof(roster[0]); ++i) emit_tag(&output, roster[i], tag_get(&headers, roster[i]));
    for (size_t i = 0; i < headers.tag_count; ++i) {
        size_t j = 0;
        for (; j < sizeof(roster) / sizeof(roster[0]); ++j) if (!strcmp(headers.tags[i].key, roster[j])) break;
        if (j == sizeof(roster) / sizeof(roster[0])) emit_tag(&output, headers.tags[i].key, headers.tags[i].value);
    }
    (void)append(&output, "\n");
    Buffer leading = {0};
    clean_comment(&leading, old.leading_comment, 0, 0);
    if (!old.leading_comment || !official.leading_comment || strcmp(old.leading_comment, official.leading_comment))
        clean_comment(&leading, official.leading_comment, 0, 0);
    emit_comment(&output, &leading); free(leading.data);
    Position board;
    if (!position_init(&board, (unsigned)game->variant, game->fen, error, error_cap)) goto fail;
    for (size_t i = 0; i < mainline->count; ++i) {
        const char *move = mainline->plies[i].move;
        char san[64], prefix[32];
        if (!position_uci_to_san(&board, move, san, sizeof(san))) { set_error(error, error_cap, "Cannot convert move %zu to SAN", i + 1); goto fail; }
        if (board.turn == 0 || !i) {
            (void)snprintf(prefix, sizeof(prefix), "%d%s", board.fullmove_number, board.turn ? "..." : ".");
            token(&output, prefix);
        }
        token(&output, san);
        const MoveInfo *info = annotation_at(game, i);
        const PgnPly *prior = i < common_old ? &old.plies[i] : NULL;
        const PgnPly *server = i < common_official ? &official.plies[i] : NULL;
        Buffer comment = {0};
        if (prior) clean_comment(&comment, prior->comment, info != NULL,
                                 server && server->comment && strstr(server->comment, "[%clk "));
        if (server && (!prior || !prior->comment || !server->comment || strcmp(prior->comment, server->comment)))
            clean_comment(&comment, server->comment, info != NULL, 0);
        add_evaluation(&comment, info, game->ours);
        emit_comment(&output, &comment);
        if (comment.failed) output.failed = 1;
        free(comment.data);
        if (prior && prior->extras) token(&output, prior->extras);
        if (server && server->extras && (!prior || !prior->extras || strcmp(prior->extras, server->extras))) token(&output, server->extras);
        char *pv = render_pv(&board, info);
        if (pv && !(prior && prior->extras && strstr(prior->extras, pv)) && !(server && server->extras && strstr(server->extras, pv))) token(&output, pv);
        free(pv);
        if (!position_apply_uci(&board, move, error, error_cap)) goto fail;
    }
    token(&output, tag_get(&headers, "Result"));
    (void)append(&output, "\n\n");
    if (output.failed) goto fail;
    pgn_free(&history); pgn_free(&official); pgn_free(&old); pgn_free(&headers);
    return output.data;
fail:
    if (error && error_cap && !*error) set_error(error, error_cap, "Unable to generate PGN within archive limits");
    pgn_free(&history); pgn_free(&official); pgn_free(&old); pgn_free(&headers);
    free(output.data); return NULL;
}

char *archive_render_pgn(const ArchiveGame *game, const char *base_url, char *error, size_t error_cap) {
    return render_game(game, base_url, NULL, error, error_cap);
}

static void job_free(ArchiveJob *job) {
    if (!job) return;
    for (size_t i = 0; i < job->string_count; ++i) free(job->strings[i]);
    free(job->annotations.items); free(job);
}
static char *job_string(ArchiveJob *job, const char *value, size_t limit) {
    if (!value) return NULL;
    size_t length = strnlen(value, limit + 1);
    if (length > limit || job->string_count >= sizeof(job->strings) / sizeof(job->strings[0])) return NULL;
    char *copy = copy_n(value, length);
    if (copy) { job->strings[job->string_count++] = copy; job->bytes += length + 1; }
    return copy;
}

static ArchiveJob *job_copy(const ArchiveGame *game) {
    if (!game || !game->id || !lbc_id(game->id) || strlen(game->id) > 32 || game->variant <= 0 ||
        (game->ours != 0 && game->ours != 1)) return NULL;
    ArchiveJob *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->game = *game; job->bytes = sizeof(*job);
#define COPY_FIELD(field, limit) do { if (game->field && !(job->game.field = job_string(job, game->field, limit))) goto fail; } while (0)
    COPY_FIELD(id, 32); COPY_FIELD(fen, 511); COPY_FIELD(moves, 65535);
    COPY_FIELD(names[0], 512); COPY_FIELD(names[1], 512); COPY_FIELD(titles[0], 128); COPY_FIELD(titles[1], 128);
    COPY_FIELD(speed, 64); COPY_FIELD(result, 16); COPY_FIELD(status, 64); COPY_FIELD(tournament, 128);
    COPY_FIELD(official_pgn, PGN_LIMIT);
#undef COPY_FIELD
    job->game.annotations = &job->annotations;
    if (game->annotations && game->annotations->count) {
        size_t count = game->annotations->count;
        if (count > PLY_LIMIT || !game->annotations->items) goto fail;
        job->annotations.items = malloc(count * sizeof(*job->annotations.items));
        if (!job->annotations.items) goto fail;
        memcpy(job->annotations.items, game->annotations->items, count * sizeof(*job->annotations.items));
        job->annotations.count = job->annotations.capacity = count;
        job->bytes += count * sizeof(*job->annotations.items);
    }
    return job;
fail:
    job_free(job); return NULL;
}

static char *archive_path(const Archive *a, const ArchiveGame *game, int single) {
    char white[256], black[256], bot[256], opponent[256], file[1024];
    lbc_safe_filename(white, sizeof(white), game->names[0] ? game->names[0] : "Unknown White");
    lbc_safe_filename(black, sizeof(black), game->names[1] ? game->names[1] : "Unknown Black");
    lbc_safe_filename(bot, sizeof(bot), a->bot_name);
    lbc_safe_filename(opponent, sizeof(opponent), game->names[game->ours ? 0 : 1] ? game->names[game->ours ? 0 : 1] : "Unknown");
    if (single || !game->finished || !strcmp(a->grouping, "game"))
        (void)snprintf(file, sizeof(file), "%s vs %s - %s.pgn", white, black, game->id);
    else if (!strcmp(a->grouping, "opponent")) (void)snprintf(file, sizeof(file), "%s games vs. %s.pgn", bot, opponent);
    else (void)snprintf(file, sizeof(file), "%s games.pgn", bot);
    size_t length = strlen(a->directory) + strlen(file) + 2;
    char *path = malloc(length);
    if (path) (void)snprintf(path, length, "%s/%s", a->directory, file);
    return path;
}

/* Scan PGN records without loading the whole file.
 * Comments and nested variations cannot start a record. */
typedef struct { FILE *fp; char *pending; } RecordReader;
static int read_record(RecordReader *reader, Buffer *out) {
    int movetext = 0, comment = 0, variation = 0;
    char *line = reader->pending; size_t capacity = line ? strlen(line) + 1 : 0;
    reader->pending = NULL;
    for (;;) {
        ssize_t length;
        if (line && !out->used && capacity) length = (ssize_t)strlen(line);
        else length = getline(&line, &capacity, reader->fp);
        if (length < 0) { free(line); return ferror(reader->fp) ? -1 : out->used ? 1 : 0; }
        const char *start = line; while (*start && isspace((unsigned char)*start)) ++start;
        if (*start == '[' && movetext && !comment && !variation) {
            reader->pending = line; return 1;
        }
        if (append_n(out, line, (size_t)length)) { free(line); return -1; }
        if (*start && *start != '[' && *start != '%') movetext = 1;
        if (movetext) for (const char *p = line; *p; ++p) {
            if (comment) { if (*p == '}') comment = 0; continue; }
            if (*p == ';' || (*p == '%' && p == line)) break;
            if (*p == '{') comment = 1;
            else if (*p == '(') ++variation;
            else if (*p == ')' && variation) --variation;
        }
        /* Pending buffer consumed; resume reading. */
        if (!*start && !movetext && out->used == (size_t)length) { out->used = 0; if (out->data) *out->data = 0; }
        if (line) { free(line); line = NULL; capacity = 0; }
    }
}

static int record_matches(const char *text, const char *id) {
    Pgn p = {0}; int matches = 0;
    if (read_tags(&p, text)) {
        const char *value = tag_get(&p, "LichessGameId");
        if (value && !strcmp(value, id)) matches = 1;
        value = tag_get(&p, "Site");
        if (!matches && value) {
            const char *tail = strrchr(value, '/');
            if (tail && !strcmp(tail + 1, id)) matches = 1;
        }
    }
    pgn_free(&p); return matches;
}

static char *previous_record(const char *path, const char *id) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    RecordReader reader = {.fp = fp};
    char *result = NULL;
    for (;;) {
        Buffer record = {0}; int status = read_record(&reader, &record);
        if (status <= 0) { free(record.data); break; }
        if (record_matches(record.data, id)) { result = record.data; break; }
        free(record.data);
    }
    free(reader.pending); fclose(fp); return result;
}

static int record_finished(const char *text) {
    Pgn p = {0}; int finished = 0;
    if (text && read_tags(&p, text)) {
        const char *flag = tag_get(&p, "LichessGameFinished"), *result = tag_get(&p, "Result");
        finished = (flag && !strcmp(flag, "1")) || (is_result(result) && strcmp(result, "*"));
    }
    pgn_free(&p); return finished;
}

static int lock_path(const char *path) {
    size_t length = strlen(path) + 6;
    char *name = malloc(length); if (!name) return -1;
    (void)snprintf(name, length, "%s.lock", path);
    int fd = open(name, O_CREAT | O_RDWR | O_CLOEXEC, 0600); free(name);
    if (fd < 0) return -1;
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    while (fcntl(fd, F_SETLKW, &lock)) {
        if (errno != EINTR) { close(fd); return -1; }
    }
    return fd;
}

static int update_group(const char *path, const char *id, const char *pgn) {
    int lock = lock_path(path); if (lock < 0) return -1;
    size_t length = strlen(path) + 16;
    char *temporary = malloc(length);
    if (!temporary) { close(lock); return -1; }
    (void)snprintf(temporary, length, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(temporary), result = -1, replaced = 0;
    FILE *out = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!out) { if (fd >= 0) close(fd); goto done; }
    FILE *in = fopen(path, "rb");
    if (!in && errno != ENOENT) goto close_out;
    if (in) {
        RecordReader reader = {.fp = in};
        for (;;) {
            Buffer record = {0}; int status = read_record(&reader, &record);
            if (status <= 0) {
                free(record.data);
                if (status < 0) { free(reader.pending); fclose(in); goto close_out; }
                break;
            }
            int same = record_matches(record.data, id);
            const char *text = same ? (replaced ? "" : pgn) : record.data;
            if (same) replaced = 1;
            int failed = fputs(text, out) == EOF;
            if (!same && record.used && record.data[record.used - 1] != '\n') failed |= fputc('\n', out) == EOF;
            free(record.data);
            if (failed) { free(reader.pending); fclose(in); goto close_out; }
        }
        free(reader.pending); fclose(in);
    }
    if (!replaced && fputs(pgn, out) == EOF) goto close_out;
    if (fflush(out) || fsync(fileno(out))) goto close_out;
    result = 0;
close_out:
    if (fclose(out)) result = -1;
    if (!result && rename(temporary, path)) result = -1;
done:
    if (result) (void)unlink(temporary);
    free(temporary); close(lock); return result;
}

static int write_job(Archive *a, const ArchiveJob *job, char *error, size_t error_cap) {
    if (lbc_mkdirs(a->directory)) { set_error(error, error_cap, "Cannot create PGN directory: %s", strerror(errno)); return -1; }
    char *path = archive_path(a, &job->game, 0), *single = archive_path(a, &job->game, 1);
    if (!path || !single) { free(path); free(single); set_error(error, error_cap, "Out of memory naming PGN"); return -1; }
    char *previous = previous_record(single, job->game.id);
    if (!previous && strcmp(path, single)) previous = previous_record(path, job->game.id);
    if (!previous && !job->game.finished && strcmp(a->grouping, "game")) {
        ArchiveGame completed = job->game; completed.finished = 1;
        char *grouped = archive_path(a, &completed, 0);
        if (grouped) { previous = previous_record(grouped, job->game.id); free(grouped); }
    }
    if (!job->game.finished && record_finished(previous)) {
        free(previous); free(path); free(single); return 0;
    }
    char *pgn = render_game(&job->game, a->base_url, previous, error, error_cap);
    free(previous);
    int result = -1;
    if (pgn) {
        result = !strcmp(path, single) ? lbc_atomic_write(path, pgn, strlen(pgn)) : update_group(path, job->game.id, pgn);
        if (result) set_error(error, error_cap, "Cannot write PGN for %s: %s", job->game.id, strerror(errno));
        else if (strcmp(path, single)) (void)unlink(single);
    }
    free(pgn); free(path); free(single); return result;
}

static void *archive_worker(void *argument) {
    Archive *a = argument;
    pthread_mutex_lock(&a->mutex);
    for (;;) {
        while (!a->head && !a->stopping) pthread_cond_wait(&a->work_ready, &a->mutex);
        if (!a->head && a->stopping) break;
        ArchiveJob *job = a->head;
        a->head = job->next; if (!a->head) a->tail = NULL;
        --a->queue_size; a->queue_bytes -= job->bytes; a->busy = 1;
        pthread_mutex_unlock(&a->mutex);
        char error[512] = "";
        int failed = write_job(a, job, error, sizeof(error));
        job_free(job);
        pthread_mutex_lock(&a->mutex);
        a->busy = 0;
        if (failed) (void)snprintf(a->error, sizeof(a->error), "%s", error);
        if (!a->head) pthread_cond_broadcast(&a->idle);
    }
    pthread_cond_broadcast(&a->idle);
    pthread_mutex_unlock(&a->mutex);
    return NULL;
}

Archive *archive_create(const Settings *settings, const char *bot_name, const char *base_url,
                        char *error, size_t error_cap) {
    if (error && error_cap) *error = 0;
    const char *directory = settings_str(settings, "pgn_directory", NULL);
    if (!directory || !*directory) return NULL;
    const char *grouping = settings_str(settings, "pgn_file_grouping", "game");
    if (strcmp(grouping, "game") && strcmp(grouping, "opponent") && strcmp(grouping, "all")) {
        set_error(error, error_cap, "Invalid PGN grouping"); return NULL;
    }
    Archive *a = calloc(1, sizeof(*a));
    if (!a) { set_error(error, error_cap, "Out of memory creating PGN writer"); return NULL; }
    a->directory = strdup(directory); a->grouping = strdup(grouping);
    a->bot_name = strdup(bot_name ? bot_name : "Bot"); a->base_url = strdup(base_url ? base_url : "https://lichess.org");
    int mutex_ready = 0, work_ready = 0, idle_ready = 0;
    if (!a->directory || !a->grouping || !a->bot_name || !a->base_url) goto fail;
    if (pthread_mutex_init(&a->mutex, NULL)) goto fail;
    mutex_ready = 1;
    if (pthread_cond_init(&a->work_ready, NULL)) goto fail;
    work_ready = 1;
    if (pthread_cond_init(&a->idle, NULL)) goto fail;
    idle_ready = 1;
    if (pthread_create(&a->thread, NULL, archive_worker, a)) goto fail;
    return a;
fail:
    if (idle_ready) pthread_cond_destroy(&a->idle);
    if (work_ready) pthread_cond_destroy(&a->work_ready);
    if (mutex_ready) pthread_mutex_destroy(&a->mutex);
    free(a->directory); free(a->grouping); free(a->bot_name); free(a->base_url); free(a);
    set_error(error, error_cap, "Cannot start PGN writer thread"); return NULL;
}

int archive_submit(Archive *a, const ArchiveGame *game) {
    if (!a) return 0;
    ArchiveJob *job = job_copy(game);
    if (!job) return -1;
    pthread_mutex_lock(&a->mutex);
    if (a->stopping || job->bytes > QUEUE_BYTE_LIMIT) goto failed;
    ArchiveJob *before = NULL;
    for (ArchiveJob *old = a->head; old; before = old, old = old->next) if (!strcmp(old->game.id, game->id)) {
        /* Keep completed results when a late suspended snapshot arrives. */
        if (old->game.finished && !game->finished) { pthread_mutex_unlock(&a->mutex); job_free(job); return 0; }
        if (a->queue_bytes - old->bytes > QUEUE_BYTE_LIMIT - job->bytes) goto failed;
        job->next = old->next;
        if (before) before->next = job; else a->head = job;
        if (a->tail == old) a->tail = job;
        a->queue_bytes = a->queue_bytes - old->bytes + job->bytes;
        pthread_mutex_unlock(&a->mutex); job_free(old); return 0;
    }
    if (a->queue_size >= QUEUE_LIMIT || a->queue_bytes > QUEUE_BYTE_LIMIT - job->bytes) goto failed;
    if (a->tail) a->tail->next = job; else a->head = job;
    a->tail = job; ++a->queue_size; a->queue_bytes += job->bytes;
    pthread_cond_signal(&a->work_ready); pthread_mutex_unlock(&a->mutex); return 0;
failed:
    pthread_mutex_unlock(&a->mutex); job_free(job); return -1;
}

int archive_error(Archive *a, char *error, size_t error_cap) {
    if (error && error_cap) *error = 0;
    if (!a) return 0;
    pthread_mutex_lock(&a->mutex);
    int available = *a->error != 0;
    if (available) { set_error(error, error_cap, "%s", a->error); *a->error = 0; }
    pthread_mutex_unlock(&a->mutex); return available;
}

void archive_flush(Archive *a) {
    if (!a) return;
    pthread_mutex_lock(&a->mutex);
    while (a->head || a->busy) pthread_cond_wait(&a->idle, &a->mutex);
    pthread_mutex_unlock(&a->mutex);
}

void archive_destroy(Archive *a) {
    if (!a) return;
    pthread_mutex_lock(&a->mutex); a->stopping = 1; pthread_cond_signal(&a->work_ready); pthread_mutex_unlock(&a->mutex);
    pthread_join(a->thread, NULL);
    pthread_cond_destroy(&a->idle); pthread_cond_destroy(&a->work_ready); pthread_mutex_destroy(&a->mutex);
    free(a->directory); free(a->grouping); free(a->bot_name); free(a->base_url); free(a);
}
