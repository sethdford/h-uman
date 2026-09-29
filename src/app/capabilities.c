#include "human/capabilities.h"
#include "human/channel_catalog.h"
#include "human/core/string.h"

/* channel_catalog.h is the single source of truth for which channels are
 * compiled into this build (see src/channels/channel_catalog.c) — the
 * catalog it returns already excludes anything not compiled in, so no
 * separate is_build_enabled() filter is needed here. */
size_t hu_capabilities_channels_built_list(char *out, size_t out_cap, bool json) {
    if (!out || out_cap == 0)
        return 0;

    size_t n = 0;
    const hu_channel_meta_t *catalog = hu_channel_catalog_all(&n);

    size_t pos = 0;
    if (json)
        pos = hu_buf_appendf(out, out_cap, pos, "[");
    for (size_t i = 0; i < n; i++) {
        if (i > 0)
            pos = hu_buf_appendf(out, out_cap, pos, ", ");
        pos = hu_buf_appendf(out, out_cap, pos, json ? "\"%s\"" : "%s", catalog[i].key);
    }
    if (json)
        pos = hu_buf_appendf(out, out_cap, pos, "]");
    else if (n == 0)
        pos = hu_buf_appendf(out, out_cap, pos, "(none)");

    return pos;
}

size_t hu_capabilities_channels_configured_count(const hu_config_t *cfg_opt) {
    if (!cfg_opt)
        return 0;

    size_t n = 0;
    const hu_channel_meta_t *catalog = hu_channel_catalog_all(&n);
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (hu_channel_catalog_is_configured(cfg_opt, catalog[i].id))
            count++;
    }
    return count;
}
