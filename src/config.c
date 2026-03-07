#include "seaclaw/config.h"
#include "seaclaw/core/arena.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include <string.h>

#include "config_internal.h"

void sc_config_deinit(sc_config_t *cfg) {
    if (!cfg)
        return;
    if (cfg->arena) {
        sc_arena_destroy(cfg->arena);
        cfg->arena = NULL;
    }
    memset(cfg, 0, sizeof(*cfg));
}

/* parse_* and sc_config_parse_json moved to config_parse.c */
/* sc_config_load, set_defaults, sync_*, load_json_file, sc_config_apply_env_* moved to
 * config_merge.c */
/* sc_config_save moved to config_serialize.c */
/* sc_config_get_*, sc_config_validate, sc_config_provider_requires_api_key moved to
 * config_getters.c */
