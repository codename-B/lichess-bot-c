#define _POSIX_C_SOURCE 200809L
#include "archive.h"
#include "position.h"
#include "settings.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(test) do { ++checks; if (!(test)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); } } while (0)

static ArchiveGame sample_game(void) {
    ArchiveGame game = {0};
    game.id = "archive01"; game.fen = "startpos";
    game.moves = "e2e4 e7e5 g1f3 b8c6 f1b5";
    game.names[0] = "Bot"; game.names[1] = "Opponent";
    game.titles[0] = "BOT"; game.ratings[0] = 2100; game.ratings[1] = 2200;
    game.speed = "blitz"; game.result = "*"; game.status = "started";
    game.variant = POSITION_STANDARD; game.ours = 0; game.rated = 1;
    game.initial_ms = 1800000; game.increments[0] = game.increments[1] = 1000;
    game.created_at = 1792430400000LL;
    return game;
}

static char *read_file(const char *path) {
    FILE *fp = fopen(path, "rb"); CHECK(fp != NULL);
    CHECK(fseek(fp, 0, SEEK_END) == 0); long length = ftell(fp); CHECK(length >= 0);
    CHECK(fseek(fp, 0, SEEK_SET) == 0);
    char *text = malloc((size_t)length + 1); CHECK(text != NULL);
    CHECK(fread(text, 1, (size_t)length, fp) == (size_t)length);
    text[length] = 0; CHECK(fclose(fp) == 0); return text;
}

static unsigned occurrences(const char *text, const char *needle) {
    unsigned count = 0; size_t n = strlen(needle);
    while ((text = strstr(text, needle))) { ++count; text += n; }
    return count;
}

static char *save_render(const char *root, const char *name, const ArchiveGame *game, const char *expected_moves) {
    char error[512], path[4096];
    char *pgn = archive_render_pgn(game, "https://lichess.org", error, sizeof(error));
    if (!pgn) fprintf(stderr, "Render error: %s\n", error);
    CHECK(pgn != NULL);
    CHECK(lbc_format(path, sizeof(path), "%s/%s.pgn", root, name) == 0);
    CHECK(lbc_atomic_write(path, pgn, strlen(pgn)) == 0);
    CHECK(lbc_format(path, sizeof(path), "%s/%s.uci", root, name) == 0);
    CHECK(lbc_atomic_write(path, expected_moves, strlen(expected_moves)) == 0);
    return pgn;
}

static void test_render(const char *root) {
    ArchiveGame game = sample_game();
    MoveAnnotation items[2] = {0};
    items[0].ply = 0; items[0].info.has_score = 1; items[0].info.score = 25; items[0].info.depth = 12;
    strcpy(items[0].info.pv, "e2e4 e7e5 g1f3"); strcpy(items[0].info.source, "UCI engine");
    items[1].ply = 2; items[1].info.has_score = 1; items[1].info.score = 31; items[1].info.depth = 13;
    strcpy(items[1].info.pv, "g1f3 b8c6 f1b5"); strcpy(items[1].info.source, "Lichess cloud");
    Annotations annotations = {.items = items, .count = 2, .capacity = 2};
    game.annotations = &annotations;
    char *pgn = save_render(root, "standard", &game, game.moves);
    CHECK(strstr(pgn, "[TimeControl \"1800+1\"]") != NULL);
    CHECK(strstr(pgn, "[WhiteElo \"2100\"]") != NULL);
    CHECK(strstr(pgn, "[%eval 0.25,12]") != NULL);
    CHECK(strstr(pgn, "(1. e4 e5 2. Nf3)") != NULL);
    CHECK(strstr(pgn, "[%source Lichess cloud]") != NULL);
    free(pgn);

    game.ours = 1; game.moves = "e2e4 e7e5";
    annotations.count = 1; items[0].ply = 1; items[0].info.score = 50; items[0].info.depth = 8;
    strcpy(items[0].info.pv, "e7e5 g1f3");
    pgn = save_render(root, "black_pov", &game, game.moves);
    CHECK(strstr(pgn, "[%eval -0.50,8]") != NULL); free(pgn);
    items[0].info.is_mate = 1; items[0].info.mate = 3;
    pgn = save_render(root, "black_mate", &game, game.moves);
    CHECK(strstr(pgn, "[%eval #-3,8]") != NULL); free(pgn);

    game = sample_game(); game.annotations = NULL;
    game.moves = "e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5a4 g8f6 e1g1";
    pgn = save_render(root, "castling", &game, game.moves);
    CHECK(strstr(pgn, "O-O") != NULL); free(pgn);
    game.fen = "4k3/8/8/8/8/8/4P3/4K3 b - - 0 37";
    game.moves = "e8d7 e2e4"; game.variant = POSITION_FROM_POSITION;
    pgn = save_render(root, "black_start_fen", &game, game.moves);
    CHECK(strstr(pgn, "[SetUp \"1\"]") != NULL);
    CHECK(strstr(pgn, "37... Kd7 38. e4") != NULL); free(pgn);
    game.fen = "8/P7/7k/8/8/8/8/7K w - - 0 1"; game.moves = "a7a8q";
    pgn = save_render(root, "promotion", &game, game.moves);
    CHECK(strstr(pgn, "a8=Q") != NULL); free(pgn);

    game = sample_game(); game.id = "export01"; game.moves = "f2f3 e7e5 g2g4";
    game.result = "0-1"; game.finished = 1; game.status = "mate";
    game.official_pgn =
        "[Event \"Official server event\"]\n[Site \"https://lichess.org/export01\"]\n"
        "[Date \"2026.10.19\"]\n[Round \"-\"]\n[White \"Bot\"]\n[Black \"Opponent\"]\n"
        "[Result \"0-1\"]\n[Opening \"Barnes Opening\"]\n[ECO \"A00\"]\n\n"
        "{Public introduction} 1.f3 {Public note [%clk 0:02:59]} (1. e4 e5) e5 $1 "
        "2.g4 {[%clk 0:02:55]} Qh4# {Final server comment} 0-1\n";
    pgn = save_render(root, "official_merge", &game, "f2f3 e7e5 g2g4 d8h4");
    CHECK(strstr(pgn, "[Event \"Official server event\"]") != NULL);
    CHECK(strstr(pgn, "[Opening \"Barnes Opening\"]") != NULL);
    CHECK(strstr(pgn, "Public introduction") != NULL && strstr(pgn, "Final server comment") != NULL);
    CHECK(strstr(pgn, "[%clk 0:02:59]") != NULL);
    CHECK(strstr(pgn, "Qh4#") != NULL);
    CHECK(strstr(pgn, "(1. e4 e5)") != NULL && strstr(pgn, "$1") != NULL); free(pgn);

    game = sample_game(); game.moves = "e2e4";
    game.official_pgn = "[Event \"Stale export\"]\n[Result \"1-0\"]\n\n1. d4 d5 1-0\n";
    pgn = save_render(root, "divergent_export", &game, game.moves);
    CHECK(strstr(pgn, "Stale export") == NULL);
    CHECK(strstr(pgn, "[Result \"*\"]") != NULL); free(pgn);

    game = sample_game(); game.names[0] = "Name\"with\\escapes\nand newline"; game.moves = "";
    pgn = save_render(root, "escaped_headers", &game, "");
    CHECK(strstr(pgn, "Name\\\"with\\\\escapes and newline") != NULL); free(pgn);

    for (unsigned variant = 1; variant <= POSITION_THREE_CHECK; variant <<= 1) {
        Position board; CHECK(position_init(&board, variant, "startpos", NULL, 0));
        char history[128] = "";
        for (int ply = 0; ply < 4; ++ply) {
            char moves[POSITION_MAX_MOVES][8]; size_t count = position_legal_moves(&board, moves, POSITION_MAX_MOVES);
            CHECK(count > 0 && count <= POSITION_MAX_MOVES);
            if (ply) strcat(history, " ");
            strcat(history, moves[count / 2]);
            CHECK(position_apply_uci(&board, moves[count / 2], NULL, 0));
        }
        game = sample_game(); game.variant = (int)variant; game.moves = history;
        char name[64]; CHECK(lbc_format(name, sizeof(name), "variant_%u", variant) == 0);
        pgn = save_render(root, name, &game, history); free(pgn);
    }
    game = sample_game(); game.variant = POSITION_CRAZYHOUSE;
    game.fen = "4k3/8/8/8/8/8/8/4K3[N] w - - 0 1"; game.moves = "N@f7";
    pgn = save_render(root, "crazyhouse_drop", &game, game.moves);
    CHECK(strstr(pgn, "N@f7") != NULL); free(pgn);
    game = sample_game(); game.variant = POSITION_ANTICHESS;
    game.fen = "8/P7/8/8/8/8/8/7r w - - 0 1"; game.moves = "a7a8k";
    pgn = save_render(root, "antichess_king_promotion", &game, game.moves);
    CHECK(strstr(pgn, "a8=K") != NULL); free(pgn);

    game = sample_game(); game.moves = "e2e5";
    char error[512]; pgn = archive_render_pgn(&game, NULL, error, sizeof(error));
    CHECK(pgn == NULL && *error); free(pgn);
}

static Settings *archive_settings(const char *directory, const char *grouping) {
    Settings *s = settings_load_string("{}", NULL, 0); CHECK(s != NULL);
    CHECK(settings_set_str(s, "pgn_directory", directory) == 0);
    CHECK(settings_set_str(s, "pgn_file_grouping", grouping) == 0);
    return s;
}

static void test_worker(const char *root, const char *grouping) {
    char directory[4096], single[4096], grouped[4096], error[512];
    CHECK(lbc_format(directory, sizeof(directory), "%s/worker_%s", root, grouping) == 0);
    Settings *s = archive_settings(directory, grouping);
    Archive *archive = archive_create(s, "Bot", "https://lichess.org", error, sizeof(error));
    CHECK(archive != NULL && !*error);
    ArchiveGame game = sample_game(); game.id = "dedup01"; game.moves = "e2e4";
    MoveAnnotation item = {.ply = 0}; item.info.has_score = 1; item.info.score = 25; item.info.depth = 12;
    strcpy(item.info.pv, "e2e4 e7e5"); strcpy(item.info.source, "Retained engine");
    Annotations annotations = {.items = &item, .count = 1, .capacity = 1}; game.annotations = &annotations;
    char mutable_history[32] = "e2e4"; game.moves = mutable_history;
    CHECK(archive_submit(archive, &game) == 0);
    strcpy(mutable_history, "d2d4"); item.info.score = 9999;
    archive_flush(archive); CHECK(archive_error(archive, error, sizeof(error)) == 0);
    CHECK(lbc_format(single, sizeof(single), "%s/Bot vs Opponent - dedup01.pgn", directory) == 0);
    char *text = read_file(single);
    CHECK(strstr(text, "[%eval 0.25,12]") != NULL);
    CHECK(strstr(text, "e4") != NULL && strstr(text, "d4") == NULL); free(text);

    game.moves = "e2e4 e7e5"; game.finished = 1; game.status = "draw"; game.result = "1/2-1/2";
    game.annotations = NULL;
    game.official_pgn = "[Site \"https://lichess.org/dedup01\"]\n[Result \"1/2-1/2\"]\n\n1. e4 {Server clock [%clk 0:29:59]} e5 1/2-1/2\n";
    CHECK(archive_submit(archive, &game) == 0); archive_flush(archive);
    CHECK(archive_error(archive, error, sizeof(error)) == 0);
    if (!strcmp(grouping, "all")) CHECK(lbc_format(grouped, sizeof(grouped), "%s/Bot games.pgn", directory) == 0);
    else if (!strcmp(grouping, "opponent")) CHECK(lbc_format(grouped, sizeof(grouped), "%s/Bot games vs. Opponent.pgn", directory) == 0);
    else CHECK(lbc_copy(grouped, sizeof(grouped), single) == 0);
    text = read_file(grouped);
    CHECK(strstr(text, "Retained engine") != NULL && strstr(text, "[%eval 0.25,12]") != NULL);
    CHECK(strstr(text, "[%clk 0:29:59]") != NULL); free(text);
    if (strcmp(grouping, "game")) CHECK(access(single, F_OK) != 0);

    for (int i = 0; i < 8; ++i) CHECK(archive_submit(archive, &game) == 0);
    archive_flush(archive); CHECK(archive_error(archive, error, sizeof(error)) == 0);
    archive_destroy(archive);
    archive = archive_create(s, "Bot", "https://lichess.org", error, sizeof(error)); CHECK(archive != NULL);
    CHECK(archive_submit(archive, &game) == 0); archive_flush(archive);
    text = read_file(grouped);
    CHECK(occurrences(text, "[LichessGameId \"dedup01\"]") == 1);
    CHECK(strstr(text, "[%eval 0.25,12]") != NULL); free(text);

    game.id = "dedup02"; game.official_pgn = NULL;
    CHECK(archive_submit(archive, &game) == 0); archive_flush(archive);
    CHECK(archive_error(archive, error, sizeof(error)) == 0);
    if (strcmp(grouping, "game")) {
        text = read_file(grouped);
        CHECK(occurrences(text, "[Event ") == 2);
        CHECK(occurrences(text, "[LichessGameId \"dedup01\"]") == 1);
        CHECK(occurrences(text, "[LichessGameId \"dedup02\"]") == 1); free(text);
    }
    game.id = "dedup01"; game.finished = 0; game.moves = ""; game.result = "*"; game.status = "started";
    CHECK(archive_submit(archive, &game) == 0); archive_flush(archive);
    if (strcmp(grouping, "game")) CHECK(access(single, F_OK) != 0);
    text = read_file(grouped);
    CHECK(strstr(text, "[LichessGameFinished \"1\"]") != NULL);
    CHECK(strstr(text, "[LichessGameFinished \"0\"]") == NULL);
    free(text);
    archive_destroy(archive); settings_free(s);
}

static void test_sanitization_and_errors(const char *root) {
    char directory[4096], path[4096], error[512];
    CHECK(lbc_format(directory, sizeof(directory), "%s/unsafe_names", root) == 0);
    Settings *s = archive_settings(directory, "opponent");
    Archive *a = archive_create(s, "Bot", "https://lichess.org", error, sizeof(error)); CHECK(a != NULL);
    ArchiveGame game = sample_game(); game.names[1] = "../../Opp|onent?";
    game.finished = 1; game.result = "1-0";
    CHECK(archive_submit(a, &game) == 0); archive_flush(a);
    CHECK(archive_error(a, error, sizeof(error)) == 0);
    CHECK(lbc_format(path, sizeof(path), "%s/Bot games vs. .._.._Opp_onent_.pgn", directory) == 0);
    CHECK(access(path, F_OK) == 0);
    game.id = "../escape"; CHECK(archive_submit(a, &game) == -1);
    archive_destroy(a); settings_free(s);

    CHECK(lbc_format(path, sizeof(path), "%s/not_a_directory", root) == 0);
    CHECK(lbc_atomic_write(path, "x", 1) == 0);
    s = archive_settings(path, "game");
    a = archive_create(s, "Bot", NULL, error, sizeof(error)); CHECK(a != NULL);
    game = sample_game(); CHECK(archive_submit(a, &game) == 0); archive_flush(a);
    CHECK(archive_error(a, error, sizeof(error)) == 1 && *error);
    CHECK(archive_error(a, error, sizeof(error)) == 0);
    archive_destroy(a); settings_free(s);
    s = settings_load_string("{}", NULL, 0); CHECK(s != NULL);
    CHECK(archive_create(s, "Bot", NULL, error, sizeof(error)) == NULL && !*error);
    CHECK(archive_submit(NULL, &game) == 0); archive_flush(NULL); archive_destroy(NULL); settings_free(s);
}

int main(int argc, char **argv) {
    char temporary[] = "/tmp/lbc-archive-tests-XXXXXX";
    const char *root = argc > 1 ? argv[1] : mkdtemp(temporary);
    CHECK(root != NULL && lbc_mkdirs(root) == 0);
    test_render(root);
    test_worker(root, "game"); test_worker(root, "opponent"); test_worker(root, "all");
    test_sanitization_and_errors(root);
    printf("archive: %u checks passed; PGN fixtures: %s\n", checks, root);
    return 0;
}
