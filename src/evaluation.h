#ifndef LBC_EVALUATION_H
#define LBC_EVALUATION_H
#include "settings.h"
#include <stddef.h>
#include <stdint.h>
#define LBC_INFO_PV_CAP 8192
typedef struct {
    int has_score, score, mate, is_mate, depth, seldepth, multipv, bound;
    int has_wdl, wdl[3], has_tb, tb_wdl, dtz, dtm;
    int64_t nodes, nps, time_ms, tbhits;
    char pv[LBC_INFO_PV_CAP], source[96];
} MoveInfo;
typedef struct { int ply; MoveInfo info; } MoveAnnotation;
typedef struct { MoveAnnotation *items; size_t count, capacity; } Annotations;
void evaluation_parse(MoveInfo *info, const char *line, size_t length);
int evaluation_string(const MoveInfo *info, char *out, size_t cap, int for_chat);
int annotations_put(Annotations *a, int ply, const MoveInfo *info);
void annotations_truncate(Annotations *a, int ply);
void annotations_free(Annotations *a);
/* 1 draw, 2 resign, 0 neither. Scores use the bot's perspective. */
int evaluation_decision(const Settings *settings, const Annotations *a,
                        const MoveInfo *current, int pieces);
#endif
