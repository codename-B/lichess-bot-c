#ifndef LBC_PROVIDERS_H
#define LBC_PROVIDERS_H
#include "config.h"
#include "evaluation.h"
#include "json.h"
#include "position.h"
enum ProviderKind { PROVIDER_EGTB = 1, PROVIDER_CHESSDB, PROVIDER_CLOUD, PROVIDER_EXPLORER, PROVIDER_DONE };
typedef struct {
    enum ProviderKind kind;
    char url[4096];
    int authenticated, ndjson, timeout_ms;
} ProviderQuery;
typedef struct {
    char move[8], roots[POSITION_MAX_MOVES][8];
    size_t root_count;
    MoveInfo info;
} ProviderResult;
/* Returns 1 if enabled and eligible, 0 to skip this source, -1 invalid query. */
int provider_query(const Config *cfg, enum ProviderKind kind, const Position *position,
                    const char *account, int64_t remaining_ms, int64_t initial_ms,
                    int ply, int out_of_book, ProviderQuery *query);
/* Return 1 legal result, 0 miss/invalid data. No network or global state. */
int provider_parse(const Config *cfg, enum ProviderKind kind, const Position *position,
                    const Json *json, uint64_t *random, ProviderResult *result);
const char *provider_name(enum ProviderKind kind);
#endif
