#ifndef LICHESS_POSITION_H
#define LICHESS_POSITION_H

#include <stddef.h>
#include <stdint.h>

/* Values match config.h. All public functions are allocation-free. */
enum {
    POSITION_STANDARD = 1, POSITION_FROM_POSITION = 2,
    POSITION_CHESS960 = 4, POSITION_ANTICHESS = 8,
    POSITION_ATOMIC = 16, POSITION_CRAZYHOUSE = 32,
    POSITION_HORDE = 64, POSITION_KING_OF_THE_HILL = 128,
    POSITION_RACING_KINGS = 256, POSITION_THREE_CHECK = 512
};
enum { POSITION_PAWN=1, POSITION_KNIGHT, POSITION_BISHOP,
       POSITION_ROOK, POSITION_QUEEN, POSITION_KING };
#define POSITION_MAX_MOVES 1024
#define POSITION_FEN_MAX 512
#define POSITION_START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

typedef struct {
    /* a1=0 .. h8=63; positive=White, negative=Black; P N B R Q K=1..6. */
    int8_t board[64];
    uint64_t castling; /* Bit for each rook retaining a castling right. */
    uint64_t promoted;
    unsigned variant;
    unsigned ply; /* Moves applied since the initial FEN, including nulls. */
    int turn; /* 0=White, 1=Black. */
    int chess960;
    int ep_square; /* -1 or a1=0. Kept even when capture is unavailable. */
    int halfmove_clock;
    int fullmove_number;
    unsigned pockets[2][7];
    int remaining_checks[2];
} Position;

unsigned position_variant_from_name(const char *name);
const char *position_variant_name(unsigned variant);
const char *position_initial_fen(unsigned variant);
/* Return 1 on success, 0 on failure; errors are optional. */
int position_init(Position *p, unsigned variant, const char *initial_fen,
                  char *err, size_t err_cap);
int position_from_history(Position *p, unsigned variant, const char *initial_fen,
                          const char *history, char *err, size_t err_cap);
/* Accept both king-to-rook and unambiguous king-to-final castling forms. */
int position_apply_uci(Position *p, const char *uci, char *err, size_t err_cap);
/* Requires a move from position_legal_moves() on this position. */
int position_apply_generated_uci(Position *p, const char *uci);
int position_is_legal(const Position *p, const char *uci);
int position_normalize_uci(const Position *p, const char *uci,
                           int chess960_mode, char out[8]);
/* FEN matches python-chess default: legal EP, X-FEN rights, variant fields. */
int position_fen(const Position *p, char *out, size_t cap);
/* Raw EP and Shredder-FEN castling rights for engines. */
int position_fen_ex(const Position *p, char *out, size_t cap,
                    int raw_en_passant, int shredder_castling);
/* Return total count; write min(count, cap) strings. Canonical UCI uses p->chess960. */
size_t position_legal_moves(const Position *p, char (*moves)[8], size_t cap);
int position_san_to_uci(const Position *p, const char *san, char out[8]);
int position_uci_to_san(const Position *p, const char *uci, char *out, size_t cap);
int position_in_check(const Position *p, int color);
int position_piece_count(const Position *p);
int position_is_capture(const Position *p, const char *uci);
int position_is_zeroing(const Position *p, const char *uci);
/* Variant end only, independent of 50 moves and repetition. */
int position_variant_end(const Position *p);
/* -1 ongoing, 0 draw, 1 White wins, 2 Black wins. */
int position_outcome(const Position *p);
uint64_t position_polyglot_hash(const Position *p);

#endif
