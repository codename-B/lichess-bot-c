#ifndef LBC_HOOKS_H
#define LBC_HOOKS_H
#include "../include/lbc_plugin.h"
typedef struct {
    void *library, *instance;
    const LbcHooksPluginV1 *plugin;
    LbcHooksHostV1 host;
    char error[256];
} LbcHooks;
/* 0 success/-1 failure; empty path means no hooks. Initialize storage to zero. */
int hooks_open(LbcHooks *hooks, const char *path, const LbcHooksHostV1 *host);
void hooks_close(LbcHooks *hooks);
int hooks_challenge(LbcHooks *hooks, const char *challenge_json);
int hooks_game_options(LbcHooks *hooks, const char *game_json,
                        LbcOptionSinkV1 sink, void *user);
void hooks_event(LbcHooks *hooks, const char *kind, const char *game_id, const char *json);
#endif
