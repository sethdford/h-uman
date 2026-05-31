#include "human/status.h"
#include "human/channel_catalog.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/version.h"
#include <stdio.h>
#include <string.h>

hu_error_t hu_status_run(hu_allocator_t *alloc, char *buf, size_t buf_size) {
    if (!buf || buf_size < 128)
        return HU_ERR_INVALID_ARGUMENT;

    hu_config_t cfg = {0};
    hu_error_t err = hu_config_load(alloc, &cfg);
    if (err != HU_OK) {
        const char *ver = hu_version_string();
        (void)snprintf(buf, buf_size,
                       "Human Status (no config found -- run onboard first)\n\nVersion: %s\n",
                       ver ? ver : "0.4.0");
        return HU_OK;
    }

    size_t pos = 0;
    const size_t cap = buf_size;
    pos = hu_buf_appendf(buf, cap, pos, "Human Status\n\n");
    if (pos >= cap)
        pos = cap - 1;
    pos = hu_buf_appendf(buf, cap, pos, "Version:     %s\n",
                         hu_version_string() ? hu_version_string() : "");
    if (pos >= cap)
        pos = cap - 1;
    pos = hu_buf_appendf(buf, cap, pos, "Workspace:   %s\n",
                         cfg.workspace_dir ? cfg.workspace_dir : "");
    if (pos >= cap)
        pos = cap - 1;
    pos = hu_buf_appendf(buf, cap, pos, "Config:      %s\n\n",
                         cfg.config_path ? cfg.config_path : "");
    if (pos >= cap)
        pos = cap - 1;
    pos = hu_buf_appendf(buf, cap, pos, "Provider:    %s\n",
                         cfg.default_provider ? cfg.default_provider : "");
    if (pos >= cap)
        pos = cap - 1;
    pos = hu_buf_appendf(buf, cap, pos, "Model:       %s\n",
                         cfg.default_model ? cfg.default_model : "(default)");
    if (pos >= cap)
        pos = cap - 1;
    pos = hu_buf_appendf(buf, cap, pos, "Temperature: %.1f\n\n",
                         cfg.default_temperature);
    if (pos >= cap)
        pos = cap - 1;
    pos = hu_buf_appendf(buf, cap, pos, "Memory:      %s (auto-save: %s)\n",
                         cfg.memory_backend ? cfg.memory_backend : "",
                         cfg.memory_auto_save ? "on" : "off");
    if (pos >= cap)
        pos = cap - 1;
    pos = hu_buf_appendf(buf, cap, pos, "Gateway:     %s:%u\n",
                         cfg.gateway_host ? cfg.gateway_host : "127.0.0.1",
                         (unsigned)cfg.gateway.port);
    if (pos >= cap)
        pos = cap - 1;

    size_t n_meta;
    const hu_channel_meta_t *meta = hu_channel_catalog_all(&n_meta);
    pos = hu_buf_appendf(buf, cap, pos, "\nChannels:\n");
    if (pos >= cap)
        pos = cap - 1;
    char status_buf[64];
    for (size_t i = 0; i < n_meta && pos < cap - 64; i++) {
        const char *st =
            hu_channel_catalog_status_text(&cfg, &meta[i], status_buf, sizeof(status_buf));
        pos = hu_buf_appendf(buf, cap, pos, "  %s: %s\n", meta[i].label, st);
        if (pos >= cap)
            pos = cap - 1;
    }

    hu_config_deinit(&cfg);
    return HU_OK;
}
