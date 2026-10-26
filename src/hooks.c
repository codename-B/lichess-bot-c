#define _POSIX_C_SOURCE 200809L
#include "hooks.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int hooks_open(LbcHooks *hooks, const char *path, const LbcHooksHostV1 *host) {
    if (!hooks || !host) return -1;
    memset(hooks, 0, sizeof(*hooks));
    hooks->host = *host;
    hooks->host.abi_version = LBC_PLUGIN_ABI_V1;
    hooks->host.struct_size = (uint32_t)sizeof(hooks->host);
    if (!path || !*path) return 0;
    hooks->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!hooks->library) { snprintf(hooks->error, sizeof(hooks->error), "%s", dlerror()); return -1; }
    void *symbol = dlsym(hooks->library, "lbc_hooks_plugin_v1");
    LbcGetHooksPluginV1 get = NULL;
    if (sizeof(get) != sizeof(symbol)) { snprintf(hooks->error, sizeof(hooks->error), "unsupported function pointer ABI"); goto error; }
    memcpy(&get, &symbol, sizeof(get));
    if (!get) { snprintf(hooks->error, sizeof(hooks->error), "missing lbc_hooks_plugin_v1 symbol"); goto error; }
    hooks->plugin = get();
    if (!hooks->plugin || hooks->plugin->abi_version != LBC_PLUGIN_ABI_V1 ||
        hooks->plugin->struct_size < sizeof(LbcHooksPluginV1)) {
        snprintf(hooks->error, sizeof(hooks->error), "unsupported hooks ABI or truncated descriptor"); goto error;
    }
    if (hooks->plugin->create) hooks->instance = hooks->plugin->create(&hooks->host);
    return 0;
error:
    (void)dlclose(hooks->library); hooks->library = NULL; hooks->plugin = NULL;
    return -1;
}
void hooks_close(LbcHooks *hooks) {
    if (!hooks) return;
    if (hooks->plugin && hooks->plugin->destroy) hooks->plugin->destroy(hooks->instance);
    if (hooks->library) (void)dlclose(hooks->library);
    memset(hooks, 0, sizeof(*hooks));
}
int hooks_challenge(LbcHooks *hooks, const char *json) {
    if (!hooks || !hooks->plugin || !hooks->plugin->is_supported_extra) return 1;
    return hooks->plugin->is_supported_extra(hooks->instance, json) > 0;
}
int hooks_game_options(LbcHooks *hooks, const char *json, LbcOptionSinkV1 sink, void *user) {
    if (!hooks || !hooks->plugin || !hooks->plugin->game_specific_options) return 0;
    return hooks->plugin->game_specific_options(hooks->instance, json, sink, user);
}
void hooks_event(LbcHooks *hooks, const char *kind, const char *id, const char *json) {
    if (hooks && hooks->plugin && hooks->plugin->event) hooks->plugin->event(hooks->instance, kind, id, json);
}
