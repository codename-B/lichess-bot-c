#include "evaluation.h"
#include "util.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int word(const char **cursor, const char *end, char *out, size_t cap) {
    const char *p = *cursor;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    const char *start = p;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
    *cursor = p;
    size_t n = (size_t)(p - start);
    if (!n || n >= cap) return 0;
    memcpy(out, start, n); out[n] = 0; return 1;
}
static int number(const char **cursor, const char *end, int64_t *out) {
    char buf[64], *tail;
    if (!word(cursor, end, buf, sizeof(buf))) return 0;
    errno = 0; long long n = strtoll(buf, &tail, 10);
    if (errno || *tail) return 0;
    *out = (int64_t)n; return 1;
}
void evaluation_parse(MoveInfo *info, const char *line, size_t length) {
    const char *p = line, *end = line + length; char key[64];
    MoveInfo next = *info; int secondary = 0;
    if (!word(&p, end, key, sizeof(key)) || strcmp(key, "info")) return;
    while (word(&p, end, key, sizeof(key))) {
        int64_t value;
        if (!strcmp(key, "string")) break;
        if (!strcmp(key, "pv")) {
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
            size_t n = (size_t)(end - p);
            while (n && (p[n-1] == '\r' || p[n-1] == '\n' || p[n-1] == ' ')) --n;
            if (n < sizeof(next.pv)) { memcpy(next.pv, p, n); next.pv[n] = 0; }
            break;
        }
        if (!strcmp(key, "score")) {
            char type[16];
            if (word(&p, end, type, sizeof(type)) && number(&p, end, &value) && value >= INT_MIN && value <= INT_MAX) {
                int is_mate = !strcmp(type, "mate");
                if (is_mate || !strcmp(type, "cp")) {
                    next.is_mate = is_mate;
                    next.has_score = 1; next.bound = 0;
                    next.mate = next.is_mate ? (int)value : 0;
                    next.score = next.is_mate ? (value > 0 ? 40000 : -40000) : (int)value;
                }
            }
        } else if (!strcmp(key, "upperbound") || !strcmp(key, "lowerbound")) next.bound = 1;
        else if (!strcmp(key, "wdl")) {
            int ok = 1;
            for (int i = 0; i < 3; ++i) {
                if (!number(&p, end, &value) || value < 0 || value > INT_MAX) { ok = 0; break; }
                next.wdl[i] = (int)value;
            }
            next.has_wdl = ok;
        } else if (!strcmp(key, "depth") || !strcmp(key, "seldepth") || !strcmp(key, "multipv") ||
                   !strcmp(key, "nodes") || !strcmp(key, "nps") || !strcmp(key, "time") || !strcmp(key, "tbhits")) {
            if (!number(&p, end, &value) || value < 0) continue;
            if (!strcmp(key, "depth") && value <= INT_MAX) next.depth = (int)value;
            else if (!strcmp(key, "seldepth") && value <= INT_MAX) next.seldepth = (int)value;
            else if (!strcmp(key, "multipv")) { secondary = value != 1; next.multipv = value <= INT_MAX ? (int)value : INT_MAX; }
            else if (!strcmp(key, "nodes")) next.nodes = value;
            else if (!strcmp(key, "nps")) next.nps = value;
            else if (!strcmp(key, "time")) next.time_ms = value;
            else if (!strcmp(key, "tbhits")) next.tbhits = value;
        }
    }
    if (!secondary) *info = next;
}
int evaluation_string(const MoveInfo *info, char *out, size_t cap, int for_chat) {
    char score[64];
    if (!info->has_score) (void)lbc_copy(score, sizeof(score), "?");
    else if (info->is_mate) (void)snprintf(score, sizeof(score), "#%d", info->mate);
    else (void)snprintf(score, sizeof(score), "%+.2f", (double)info->score / 100.0);
    if (for_chat) return lbc_format(out, cap, "Evaluation: %s, depth %d, nodes %" PRId64 ", time %.2fs%s%s",
        score, info->depth, info->nodes, (double)info->time_ms / 1000.0, *info->source ? ", " : "", info->source);
    return lbc_format(out, cap, "score=%s depth=%d nodes=%" PRId64 " nps=%" PRId64 " time_ms=%" PRId64 "%s%s",
        score, info->depth, info->nodes, info->nps, info->time_ms, *info->source ? " source=" : "", info->source);
}
int annotations_put(Annotations *a, int ply, const MoveInfo *info) {
    for (size_t i = 0; i < a->count; ++i) if (a->items[i].ply == ply) { a->items[i].info = *info; return 0; }
    if (a->count == a->capacity) {
        size_t cap = a->capacity ? a->capacity * 2 : 16;
        if (cap > 16384) return -1;
        MoveAnnotation *p = realloc(a->items, cap * sizeof(*p)); if (!p) return -1;
        a->items = p; a->capacity = cap;
    }
    a->items[a->count].ply = ply; a->items[a->count].info = *info; ++a->count; return 0;
}
void annotations_truncate(Annotations *a, int ply) {
    size_t used = 0;
    for (size_t i = 0; i < a->count; ++i) if (a->items[i].ply < ply) a->items[used++] = a->items[i];
    a->count = used;
}
void annotations_free(Annotations *a) { free(a->items); memset(a, 0, sizeof(*a)); }
int evaluation_decision(const Settings *s, const Annotations *a, const MoveInfo *current, int pieces) {
    if (current->has_tb) {
        if (current->tb_wdl == -2 && settings_bool(s, "engine.draw_or_resign.resign_enabled", 0) &&
            settings_bool(s, "engine.draw_or_resign.resign_for_egtb_minus_two", 1)) return 2;
        if (current->tb_wdl == 0 && settings_bool(s, "engine.draw_or_resign.offer_draw_enabled", 0) &&
            settings_bool(s, "engine.draw_or_resign.offer_draw_for_egtb_zero", 1)) return 1;
    }
    if (!current->has_score || current->bound) return 0;
    for (int resign = 1; resign >= 0; --resign) {
        if (!settings_bool(s, resign ? "engine.draw_or_resign.resign_enabled" : "engine.draw_or_resign.offer_draw_enabled", 0)) continue;
        int64_t needed = settings_int(s, resign ? "engine.draw_or_resign.resign_moves" : "engine.draw_or_resign.offer_draw_moves", resign ? 3 : 5);
        int64_t threshold = settings_int(s, resign ? "engine.draw_or_resign.resign_score" : "engine.draw_or_resign.offer_draw_score", resign ? -1000 : 0);
        if (!resign && pieces > settings_int(s, "engine.draw_or_resign.offer_draw_pieces", 10)) continue;
        if (needed < 1) needed = 1;
        if ((uint64_t)needed > a->count + 1) continue;
        int good = 1;
        for (int64_t n = 0; n < needed; ++n) {
            const MoveInfo *info = n == 0 ? current : &a->items[a->count - (size_t)n].info;
            int64_t score = info->score;
            if (!info->has_score || info->bound || (resign ? score > threshold : score > threshold || score < -threshold)) { good = 0; break; }
        }
        if (good) return resign ? 2 : 1;
    }
    return 0;
}
