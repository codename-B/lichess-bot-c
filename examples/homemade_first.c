#include "lbc_plugin.h"
#include <stdlib.h>
#include <string.h>

/* Choose the alphabetically first UCI move. */
static int search(void *instance, const LbcSearchV1 *q, LbcSearchResultV1 *r) {
    (void)instance;
    const char (*moves)[8] = q->root_moves ? q->root_moves : q->legal_moves;
    size_t count = q->root_moves ? q->root_count : q->legal_count;
    if (!count) return 0;
    size_t chosen = 0;
    for (size_t i = 1; i < count; ++i) if (strcmp(moves[i], moves[chosen]) < 0) chosen = i;
    memcpy(r->move, moves[chosen], sizeof(r->move));
    r->nodes = (int64_t)count; r->depth = 1;
    return 0;
}
static const LbcEnginePluginV1 plugin = {
    .abi_version = LBC_PLUGIN_ABI_V1, .struct_size = sizeof(LbcEnginePluginV1),
    .name = "Native FirstMove", .author = "lichess-bot-c",
    .search = search
};
const LbcEnginePluginV1 *lbc_engine_plugin_v1(void) { return &plugin; }
