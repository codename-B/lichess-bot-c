#include "lbc_plugin.h"
#include <stdlib.h>
#include <string.h>

/* Keep the descriptor immutable; store instance data in create(). */
typedef struct { LbcHooksHostV1 host; unsigned events; } State;
static void *create(const LbcHooksHostV1 *host) {
    State *state = calloc(1, sizeof(*state));
    if (state) state->host = *host;
    return state;
}
static void destroy(void *instance) { free(instance); }
static int supported(void *instance, const char *challenge_json) {
    (void)instance; (void)challenge_json;
    /* Determine whether to accept a challenge. */
    return 1;
}
static int options(void *instance, const char *game_json, LbcOptionSinkV1 sink, void *user) {
    (void)instance; (void)game_json;
    /* Set options for this game. */
    return sink(user, "Hash", "64");
}
static void event(void *instance, const char *kind, const char *game_id, const char *json) {
    State *state = instance;
    (void)kind; (void)game_id; (void)json;
    if (state) ++state->events;
    /* Queue API actions with state->host.submit(...). */
}
static const LbcHooksPluginV1 plugin = {
    .abi_version = LBC_PLUGIN_ABI_V1, .struct_size = sizeof(LbcHooksPluginV1),
    .name = "Example custom handlers", .create = create, .destroy = destroy,
    .is_supported_extra = supported, .game_specific_options = options, .event = event
};
const LbcHooksPluginV1 *lbc_hooks_plugin_v1(void) { return &plugin; }
