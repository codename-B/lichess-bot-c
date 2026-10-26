#ifndef LBC_ARCHIVE_H
#define LBC_ARCHIVE_H

#include "evaluation.h"
#include "settings.h"
#include <stddef.h>
#include <stdint.h>

typedef struct Archive Archive;
/* Strings and annotations are borrowed; archive_submit copies them.
 * Annotation ply is zero-based from fen; scores use the bot's perspective. */
typedef struct {
    const char *id, *fen, *moves;
    const char *names[2], *titles[2];
    const char *speed, *result, *status, *tournament;
    int ratings[2], variant, ours, rated, finished;
    int64_t initial_ms, increments[2], created_at;
    const Annotations *annotations;
    /* Server PGN: merge tags, comments, clocks, and final moves if history matches. */
    const char *official_pgn;
} ArchiveGame;

/* Write PGN records to files as games finish, on a worker thread.
 * NULL with an empty error means pgn_directory is disabled. */
Archive *archive_create(const Settings *settings, const char *bot_name,
                        const char *base_url, char *error, size_t error_cap);
/* 0 accepted/disabled, -1 invalid snapshot, allocation failure or full queue. */
int archive_submit(Archive *archive, const ArchiveGame *game);
/* Return and clear the latest asynchronous error. */
int archive_error(Archive *archive, char *error, size_t error_cap);
/* Waits until queued writes finish. Destroy also drains the queue. */
void archive_flush(Archive *archive);
void archive_destroy(Archive *archive);

/* Caller frees the returned PGN. */
char *archive_render_pgn(const ArchiveGame *game, const char *base_url,
                          char *error, size_t error_cap);

#endif
