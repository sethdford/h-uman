/* src/agent/turn/local_only_route.c — see include/human/agent/local_only_route.h. */
#include "human/agent/local_only_route.h"
#include "human/core/local_only_guard.h"

bool hu_local_only_router_defaults(hu_model_router_config_t *cfg, const char *primary_model,
                                   size_t primary_model_len) {
    if (!cfg || !primary_model || primary_model_len == 0 || !hu_local_only_enforced())
        return false;
    cfg->reflexive_model = primary_model;
    cfg->reflexive_model_len = primary_model_len;
    cfg->conversational_model = primary_model;
    cfg->conversational_model_len = primary_model_len;
    cfg->analytical_model = primary_model;
    cfg->analytical_model_len = primary_model_len;
    cfg->deep_model = primary_model;
    cfg->deep_model_len = primary_model_len;
    return true;
}
