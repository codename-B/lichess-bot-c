#ifndef LBC_PLUGIN_H
#define LBC_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host pointers are borrowed during callbacks.
 * Authentication tokens are not exposed. */
#define LBC_PLUGIN_ABI_V1 UINT32_C(1)
#define LBC_MOVE_CAP 8
#define LBC_PV_CAP 4096

typedef struct {
    uint32_t abi_version, struct_size;
    void *user;
    int (*cancelled)(void *user); /* Safe from the search worker. */
    void (*log)(void *user, int level, const char *message);
} LbcEngineHostV1;

typedef struct {
    uint32_t abi_version, struct_size;
    const char *variant, *initial_fen, *fen, *history;
    const char (*legal_moves)[LBC_MOVE_CAP];
    size_t legal_count;
    const char (*root_moves)[LBC_MOVE_CAP]; /* NULL: all legal moves. */
    size_t root_count;
    int turn; /* 0 White; 1 Black. */
    int chess960;
    int64_t white_ms, black_ms, white_increment_ms, black_increment_ms;
    int64_t movetime_ms, nodes;
    int depth, moves_to_go, ponder, draw_offered;
    const char *game_json; /* Available via lbc_game; NULL if not supplied. */
    const char *state_json; /* Latest authoritative gameState, when supplied. */
    const char *options_json; /* Nested homemade options/draw policy, if supplied. */
} LbcSearchV1;

typedef struct {
    uint32_t abi_version, struct_size;
    char move[LBC_MOVE_CAP], ponder[LBC_MOVE_CAP];
    int resigned, draw_offered, draw_accepted;
    int has_score, score_cp, mate, depth, seldepth;
    int64_t nodes, time_ms, nps, tbhits;
    char pv[LBC_PV_CAP]; /* Space-separated UCI moves. */
} LbcSearchResultV1;

typedef struct {
    uint32_t abi_version, struct_size;
    const char *name, *author;
    void *(*create)(const LbcEngineHostV1 *host);
    void (*destroy)(void *engine);
    /* 0 success; nonzero unsupported option. */
    int (*configure)(void *engine, const char *name, const char *value);
    void (*new_game)(void *engine, const char *game_json);
    int (*search)(void *engine, const LbcSearchV1 *request, LbcSearchResultV1 *result);
    void (*result)(void *engine, const char *result, const char *reason);
    /* game/state/options payloads are JSON. identity is "username base_url";
     * opponent uses UCI_Opponent's title/rating/kind/name field format. */
    void (*notify)(void *engine, const char *event, const char *json);
} LbcEnginePluginV1;

/* Export an immutable descriptor, valid until dlclose.
 * Search runs on one worker, without concurrent lifecycle/configuration callbacks. */
typedef const LbcEnginePluginV1 *(*LbcGetEnginePluginV1)(void);
const LbcEnginePluginV1 *lbc_engine_plugin_v1(void);

typedef struct {
    uint32_t abi_version, struct_size;
    void *user;
    void (*log)(void *user, int level, const char *message);
    /* Queue an API request. Return 0 if queued, -1 if rejected.
     * The bot validates paths and handles authentication and rate limits. */
    int (*submit)(void *user, const char *method, const char *path, const char *body);
} LbcHooksHostV1;
typedef int (*LbcOptionSinkV1)(void *user, const char *name, const char *value);

typedef struct {
    uint32_t abi_version, struct_size;
    const char *name;
    void *(*create)(const LbcHooksHostV1 *host);
    void (*destroy)(void *hooks);
    /* 1: no extra restriction; 0: decline; -1: callback error (decline). */
    int (*is_supported_extra)(void *hooks, const char *challenge_json);
    /* Emit overrides via sink; 0 success, -1 failure. */
    int (*game_specific_options)(void *hooks, const char *game_json,
                                  LbcOptionSinkV1 sink, void *user);
    /* Runs synchronously on the network loop: keep callbacks short. */
    void (*event)(void *hooks, const char *kind, const char *game_id, const char *json);
} LbcHooksPluginV1;
typedef const LbcHooksPluginV1 *(*LbcGetHooksPluginV1)(void);
const LbcHooksPluginV1 *lbc_hooks_plugin_v1(void);

#ifdef __cplusplus
}
#endif
#endif
