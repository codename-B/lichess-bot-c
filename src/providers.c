#include "providers.h"
#include "util.h"
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *provider_name(enum ProviderKind kind) {
    switch (kind) {
        case PROVIDER_EGTB: return "Online EGTB";
        case PROVIDER_CHESSDB: return "ChessDB";
        case PROVIDER_CLOUD: return "Lichess Cloud Analysis";
        case PROVIDER_EXPLORER: return "Lichess Opening Explorer";
        default: return "Engine";
    }
}
static const Setting *configuration(const Config *cfg, enum ProviderKind kind) {
    static const char *const paths[] = {"", "engine.online_moves.online_egtb", "engine.online_moves.chessdb_book",
        "engine.online_moves.lichess_cloud_analysis", "engine.online_moves.lichess_opening_explorer"};
    return kind > 0 && kind < PROVIDER_DONE ? settings_get(cfg->settings, paths[kind]) : NULL;
}
static int standard(const Position *p) { return p->variant == VAR_STANDARD || p->variant == VAR_FROM_POSITION || p->variant == VAR_CHESS960; }
static const char *api_variant(const Position *p) { return standard(p) ? "standard" : config_variant_name((int)p->variant); }
static int op1(const Position *p) {
    if (position_piece_count(p) != 8 || p->castling) return 0;
    int counts[2] = {0, 0};
    for (int i = 0; i < 64; ++i) if (p->board[i]) ++counts[p->board[i] < 0];
    if (counts[0] <= 2 || counts[1] <= 2) return 0;
    for (int sq = 0; sq < 64; ++sq) if (p->board[sq] == POSITION_PAWN)
        for (int other = sq + 8; other < 64; other += 8) if (p->board[other] == -POSITION_PAWN) return 1;
    return 0;
}
int provider_query(const Config *cfg, enum ProviderKind kind, const Position *p, const char *account,
                    int64_t remaining_ms, int64_t initial_ms, int ply, int out_of_book, ProviderQuery *q) {
    const Setting *s = configuration(cfg, kind);
    if (!setting_bool(setting_get(s, "enabled"), 0)) return 0;
    double min_time = setting_double(setting_get(s, "min_time"), 20), max_time = setting_double(setting_get(s, "max_time"), 10800);
    if ((double)remaining_ms < min_time * 1000.0 || (double)initial_ms > max_time * 1000.0) return 0;
    if (kind != PROVIDER_EGTB && ((double)ply > settings_double(cfg->settings, "engine.online_moves.max_depth", INFINITY) * 2.0 - 1.0 ||
        out_of_book >= settings_int(cfg->settings, "engine.online_moves.max_out_of_book_moves", 10))) return 0;
    char fen[POSITION_FEN_MAX], params[3072] = "", site[1024] = "";
    if (!position_fen(p, fen, sizeof(fen))) return -1;
    memset(q, 0, sizeof(*q)); q->kind = kind; q->timeout_ms = 2000;
    const char *quality = setting_str(setting_get(s, "move_quality"), kind == PROVIDER_CHESSDB ? "good" : "best");
    const char *source = setting_str(setting_get(s, "source"), "lichess");
    if (kind == PROVIDER_EGTB) {
        int pieces = position_piece_count(p);
        if (p->castling || pieces > setting_int(setting_get(s, "max_pieces"), 7)) return 0;
        if (!strcmp(source, "lichess")) {
            if ((!standard(p) && p->variant != VAR_ANTICHESS && p->variant != VAR_ATOMIC) ||
                pieces > (standard(p) ? 8 : 6) || (pieces == 8 && !op1(p))) return 0;
            if (lbc_format(site, sizeof(site), "https://tablebase.lichess.ovh/%s", api_variant(p)) ||
                lbc_form_add(params, sizeof(params), "fen", fen)) return -1;
            q->authenticated = 1;
        } else if (!strcmp(source, "chessdb")) {
            if (!standard(p) || pieces > 7) return 0;
            (void)lbc_copy(site, sizeof(site), "https://www.chessdb.cn/cdb.php");
            if (lbc_form_add(params, sizeof(params), "action", !strcmp(quality, "suggest") ? "queryall" : "querypv") ||
                lbc_form_add(params, sizeof(params), "board", fen) || lbc_form_add(params, sizeof(params), "json", "1")) return -1;
        } else return 0;
    } else if (kind == PROVIDER_CHESSDB) {
        if (!standard(p)) return 0;
        (void)lbc_copy(site, sizeof(site), "https://www.chessdb.cn/cdb.php");
        const char *action = !strcmp(quality, "best") ? "querypv" : !strcmp(quality, "all") ? "query" : "querybest";
        if (lbc_form_add(params, sizeof(params), "action", action) || lbc_form_add(params, sizeof(params), "board", fen) ||
            lbc_form_add(params, sizeof(params), "json", "1")) return -1;
    } else if (kind == PROVIDER_CLOUD) {
        if (lbc_format(site, sizeof(site), "%s/api/cloud-eval", cfg->base_url) ||
            lbc_form_add(params, sizeof(params), "fen", fen) || lbc_form_add(params, sizeof(params), "variant", api_variant(p)) ||
            lbc_form_add(params, sizeof(params), "multiPv", !strcmp(quality, "best") ? "1" : "5")) return -1;
        q->authenticated = 1;
    } else if (kind == PROVIDER_EXPLORER) {
        source = setting_str(setting_get(s, "source"), "masters");
        if (!strcmp(source, "masters") && !standard(p)) return 0;
        if (strcmp(source, "masters") && strcmp(source, "lichess") && strcmp(source, "player")) return 0;
        if (lbc_format(site, sizeof(site), "https://explorer.lichess.ovh/%s", source) ||
            lbc_form_add(params, sizeof(params), "fen", fen) || lbc_form_add(params, sizeof(params), "moves", "100")) return -1;
        if (strcmp(source, "masters") && (lbc_form_add(params, sizeof(params), "variant", api_variant(p)) ||
            lbc_form_add(params, sizeof(params), "recentGames", "0"))) return -1;
        if (!strcmp(source, "lichess") && lbc_form_add(params, sizeof(params), "topGames", "0")) return -1;
        if (!strcmp(source, "player")) {
            const char *player = setting_str(setting_get(s, "player_name"), "");
            if (lbc_form_add(params, sizeof(params), "player", *player ? player : account) ||
                lbc_form_add(params, sizeof(params), "color", p->turn ? "black" : "white")) return -1;
            q->ndjson = 1;
        }
        q->authenticated = 1;
    } else return 0;
    /* Never send the Lichess token to an overridden endpoint. */
    const char *override = setting_str(setting_get(s, "url"), "");
    if (*override) {
        if ((strncmp(override, "https://", 8) && strncmp(override, "http://", 7)) ||
            lbc_copy(site, sizeof(site), override)) return -1;
        q->authenticated = 0;
    }
    if (lbc_format(q->url, sizeof(q->url), "%s%c%s", site, strchr(site, '?') ? '&' : '?', params)) return -1;
    return 1;
}
static int array_first(const Json *j, int array) {
    return array >= 0 && j->tokens[array].type == JSON_ARRAY && array + 1 < j->tokens[array].next ? array + 1 : -1;
}
static int move_at(const Json *j, int token, const Position *p, char out[8]) {
    char raw[16];
    return json_string(j, token, raw, sizeof(raw)) >= 0 && position_normalize_uci(p, raw, p->chess960, out) && position_is_legal(p, out);
}
static void pv_array(const Json *j, int array, char *out, size_t cap) {
    *out = 0; int t = array_first(j, array);
    for (; t >= 0 && t < j->tokens[array].next; t = j->tokens[t].next) {
        char move[16]; size_t used = strlen(out);
        if (json_string(j, t, move, sizeof(move)) < 0 || used + strlen(move) + 2 >= cap) break;
        if (used) out[used++] = ' ';
        memcpy(out + used, move, strlen(move) + 1);
    }
}
static int first_pv(const Position *p, const char *pv, char out[8]) {
    char move[16]; size_t n = strcspn(pv, " \t\r\n");
    if (!n || n >= sizeof(move)) return 0;
    memcpy(move, pv, n); move[n] = 0;
    return position_normalize_uci(p, move, p->chess960, out) && position_is_legal(p, out);
}
static int category_wdl(const Json *j, int token, int *wdl) {
    static const struct { const char *name; int score; } values[] = {
        {"loss", -2}, {"maybe-loss", -1}, {"blessed-loss", -1}, {"draw", 0},
        {"cursed-win", 1}, {"maybe-win", 1}, {"win", 2}};
    for (size_t i = 0; i < sizeof(values)/sizeof(*values); ++i) if (json_eq(j, token, values[i].name)) { *wdl = values[i].score; return 1; }
    return 0;
}
static int chessdb_wdl(int64_t score) { return score < -20000 ? -2 : score < 0 ? -1 : !score ? 0 : score <= 20000 ? 1 : 2; }
static int tablebase_distance(const Json *j, int object, const char *key) {
    int64_t distance = lbc_json_int(j, object, key, 0);
    /* Reject distances that overflow MoveInfo when stored or negated. */
    return distance < -(int64_t)INT_MAX || distance > INT_MAX ? 0 : (int)-distance;
}
static void tb_score(MoveInfo *info, int wdl, const char *source) {
    static const int scores[] = {-9900, -500, 0, 500, 9900};
    info->has_tb = 1; info->tb_wdl = wdl; info->has_score = 1; info->score = scores[wdl+2];
    (void)lbc_copy(info->source, sizeof(info->source), source);
}
static int parse_egtb(const Setting *s, const Position *p, const Json *j, ProviderResult *r) {
    int lichess = !strcmp(setting_str(setting_get(s, "source"), "lichess"), "lichess");
    int suggest = !strcmp(setting_str(setting_get(s, "move_quality"), "best"), "suggest");
    if (!lichess && !json_eq(j, json_get(j, 0, "status"), "ok")) return 0;
    if (!lichess && !suggest) {
        int64_t score;
        int pv = json_get(j, 0, "pv");
        if (json_i64(j, json_get(j, 0, "score"), &score) || !move_at(j, array_first(j, pv), p, r->move)) return 0;
        tb_score(&r->info, chessdb_wdl(score), "ChessDB EGTB");
        pv_array(j, pv, r->info.pv, sizeof(r->info.pv)); return 1;
    }
    int moves = json_get(j, 0, "moves"), first = array_first(j, moves), best = -3;
    if (first < 0) return 0;
    for (int t = first; t < j->tokens[moves].next; t = j->tokens[t].next) {
        int wdl; char move[8];
        if (lichess) {
            if (!category_wdl(j, json_get(j, t, "category"), &wdl)) continue;
            wdl = -wdl;
        } else {
            int64_t score;
            if (json_i64(j, json_get(j, t, "score"), &score)) continue;
            wdl = chessdb_wdl(score);
        }
        if (!move_at(j, json_get(j, t, "uci"), p, move)) continue;
        if (best == -3) {
            best = wdl; (void)lbc_copy(r->move, sizeof(r->move), move);
            r->info.dtz = tablebase_distance(j, t, position_piece_count(p) == 8 ? "dtc" : "dtz");
            r->info.dtm = tablebase_distance(j, t, "dtm");
        }
        if (!suggest) break;
        if (wdl == best && r->root_count < POSITION_MAX_MOVES) strcpy(r->roots[r->root_count++], move);
    }
    if (best == -3) return 0;
    tb_score(&r->info, best, lichess ? "Lichess EGTB" : "ChessDB EGTB");
    if (r->root_count > 1) *r->move = 0;
    else r->root_count = 0;
    return 1;
}
static int parse_chessdb(const Setting *s, const Position *p, const Json *j, ProviderResult *r) {
    if (!json_eq(j, json_get(j, 0, "status"), "ok")) return 0;
    if (!strcmp(setting_str(setting_get(s, "move_quality"), "good"), "best")) {
        int64_t depth = lbc_json_int(j, 0, "depth", -1);
        if (depth < setting_int(setting_get(s, "min_depth"), 20) || depth > INT_MAX) return 0;
        pv_array(j, json_get(j, 0, "pv"), r->info.pv, sizeof(r->info.pv));
        if (!first_pv(p, r->info.pv, r->move)) return 0;
        int64_t score;
        if (!json_i64(j, json_get(j, 0, "score"), &score) && score >= INT_MIN && score <= INT_MAX) {
            r->info.has_score = 1; r->info.score = (int)score;
        }
        r->info.depth = (int)depth;
    } else if (!move_at(j, json_get(j, 0, "move"), p, r->move)) return 0;
    (void)lbc_copy(r->info.source, sizeof(r->info.source), "ChessDB"); return 1;
}
static int cloud_score(const Json *j, int object, int *is_mate, int *mate, int *score) {
    int64_t value;
    if (!json_i64(j, json_get(j, object, "cp"), &value) && value >= -1000000 && value <= 1000000) {
        *is_mate = 0; *mate = 0; *score = (int)value; return 1;
    }
    if (!json_i64(j, json_get(j, object, "mate"), &value) && value >= -1000000 && value <= 1000000) {
        *is_mate = 1; *mate = (int)value; *score = value > 0 ? 40000 : -40000; return 1;
    }
    return 0;
}
static int parse_cloud(const Setting *s, const Position *p, const Json *j, uint64_t *random, ProviderResult *r) {
    int64_t depth = lbc_json_int(j, 0, "depth", -1), knodes = lbc_json_int(j, 0, "knodes", -1);
    if (depth < setting_int(setting_get(s, "min_depth"), 20) || depth > INT_MAX ||
        knodes < setting_int(setting_get(s, "min_knodes"), 0) || knodes > INT64_MAX/1000) return 0;
    int pvs = json_get(j, 0, "pvs"), first = array_first(j, pvs);
    if (first < 0) return 0;
    int best_mate, best_is_mate, best_score;
    if (!cloud_score(j, first, &best_is_mate, &best_mate, &best_score)) return 0;
    (void)best_mate; (void)best_is_mate;
    int choose_best = !strcmp(setting_str(setting_get(s, "move_quality"), "best"), "best");
    int64_t difference = setting_int(setting_get(s, "max_score_difference"), 50);
    uint64_t count = 0;
    for (int t = first; t < j->tokens[pvs].next; t = j->tokens[t].next) {
        int score, mate, is_mate; char pv[LBC_INFO_PV_CAP], move[8];
        if (!cloud_score(j, t, &is_mate, &mate, &score) ||
            lbc_json_string(j, t, "moves", pv, sizeof(pv), NULL) || !first_pv(p, pv, move)) continue;
        if (p->turn ? (int64_t)score > (int64_t)best_score + difference : (int64_t)score < (int64_t)best_score - difference) continue;
        if (!choose_best && lbc_random_below(random, ++count) != 0) continue;
        strcpy(r->move, move); strcpy(r->info.pv, pv);
        r->info.has_score = 1; r->info.is_mate = is_mate;
        r->info.mate = p->turn ? -mate : mate; r->info.score = p->turn ? -score : score;
        if (choose_best) break;
    }
    if (!*r->move) return 0;
    r->info.depth = (int)depth; r->info.nodes = knodes * 1000;
    (void)lbc_copy(r->info.source, sizeof(r->info.source), "Lichess Cloud Analysis"); return 1;
}
static int parse_explorer(const Setting *s, const Position *p, const Json *j, ProviderResult *r) {
    int array = json_get(j, 0, "moves"), first = array_first(j, array);
    if (first < 0) return 0;
    int winrate = !strcmp(setting_str(setting_get(s, "sort"), "winrate"), "winrate");
    int64_t min_games = setting_int(setting_get(s, "min_games"), 10);
    double best_a = -1, best_b = -1;
    for (int t = first; t < j->tokens[array].next; t = j->tokens[t].next) {
        int64_t white = lbc_json_int(j, t, "white", -1), black = lbc_json_int(j, t, "black", -1), draws = lbc_json_int(j, t, "draws", -1);
        if (white < 0 || black < 0 || draws < 0 || white > INT64_MAX/3 || black > INT64_MAX/3 || draws > INT64_MAX/3) continue;
        int64_t games = white + black + draws;
        if (games < 1 || games < min_games) continue;
        char move[8]; if (!move_at(j, json_get(j, t, "uci"), p, move)) continue;
        double rate = ((double)(p->turn ? black : white) + (double)draws * 0.5) / (double)games;
        double a = winrate ? rate : (double)games, b = winrate ? (double)games : rate;
        if (a > best_a || (a == best_a && (b > best_b || (b == best_b && strcmp(move, r->move) > 0)))) {
            best_a = a; best_b = b; strcpy(r->move, move);
        }
    }
    if (!*r->move) return 0;
    (void)lbc_format(r->info.source, sizeof(r->info.source), "Lichess Opening Explorer (%s)", setting_str(setting_get(s, "source"), "masters"));
    return 1;
}
int provider_parse(const Config *cfg, enum ProviderKind kind, const Position *p, const Json *j, uint64_t *random, ProviderResult *r) {
    memset(r, 0, sizeof(*r));
    if (!j->count || j->tokens[0].type != JSON_OBJECT) return 0;
    const Setting *s = configuration(cfg, kind);
    switch (kind) {
        case PROVIDER_EGTB: return parse_egtb(s, p, j, r);
        case PROVIDER_CHESSDB: return parse_chessdb(s, p, j, r);
        case PROVIDER_CLOUD: return parse_cloud(s, p, j, random, r);
        case PROVIDER_EXPLORER: return parse_explorer(s, p, j, r);
        default: return 0;
    }
}
