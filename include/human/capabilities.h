#ifndef HU_CAPABILITIES_H
#define HU_CAPABILITIES_H

#include "human/config.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * The single source of truth for "which channels can this build talk on" —
 * both the CLI (`human capabilities`) and the RPC (`admin.capabilities`)
 * render channel lists through this helper instead of hardcoding names.
 *
 * Writes the catalog channel keys (channel_catalog.h) compiled into this
 * build into out[out_cap], always NUL-terminated. When json is true, writes
 * a JSON string array (e.g. ["cli","imessage"], or "[]" if empty);
 * otherwise a comma-separated list (e.g. "cli, imessage", or "(none)" if
 * empty). Returns the number of bytes written, excluding the terminator.
 * No-op (returns 0) if out is NULL or out_cap is 0.
 */
size_t hu_capabilities_channels_built_list(char *out, size_t out_cap, bool json);

/**
 * Number of catalog channels that are both compiled into this build and
 * configured under cfg_opt (see hu_channel_catalog_is_configured). Returns
 * 0 if cfg_opt is NULL.
 */
size_t hu_capabilities_channels_configured_count(const hu_config_t *cfg_opt);

#endif /* HU_CAPABILITIES_H */
