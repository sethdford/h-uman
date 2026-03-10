#include "human/config.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include <string.h>

#include "config_internal.h"

void hu_config_deinit(hu_config_t *cfg) {
    if (!cfg)
        return;
    if (cfg->arena) {
        hu_arena_destroy(cfg->arena);
        cfg->arena = NULL;
    }
    memset(cfg, 0, sizeof(*cfg));
}

/* parse_* and hu_config_parse_json moved to config_parse.c */
/* hu_config_load, set_defaults, sync_*, load_json_file, hu_config_apply_env_* moved to
 * config_merge.c */
/* hu_config_save moved to config_serialize.c */
/* hu_config_get_*, hu_config_validate, hu_config_provider_requires_api_key moved to
 * config_getters.c */
