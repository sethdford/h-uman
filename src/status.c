#include "seaclaw/status.h"
#include "seaclaw/version.h"
#include "seaclaw/config.h"
#include "seaclaw/channel_catalog.h"
#include "seaclaw/core/allocator.h"
#include <stdio.h>
#include <string.h>

sc_error_t sc_status_run(sc_allocator_t *alloc, char *buf, size_t buf_size) {
    if (!buf || buf_size < 128) return SC_ERR_INVALID_ARGUMENT;

    sc_config_t cfg = {0};
    sc_error_t err = sc_config_load(alloc, &cfg);
    if (err != SC_OK) {
        const char *ver = sc_version_string();
        (void)snprintf(buf, buf_size,
            "SeaClaw Status (no config found -- run onboard first)\n\nVersion: %s\n",
            ver ? ver : "0.1.0");
        return SC_OK;
    }

    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, buf_size - pos, "SeaClaw Status\n\n");
    pos += (size_t)snprintf(buf + pos, buf_size - pos, "Version:     %s\n", sc_version_string() ? sc_version_string() : "");
    pos += (size_t)snprintf(buf + pos, buf_size - pos, "Workspace:   %s\n", cfg.workspace_dir ? cfg.workspace_dir : "");
    pos += (size_t)snprintf(buf + pos, buf_size - pos, "Config:      %s\n\n", cfg.config_path ? cfg.config_path : "");
    pos += (size_t)snprintf(buf + pos, buf_size - pos, "Provider:    %s\n", cfg.default_provider ? cfg.default_provider : "");
    pos += (size_t)snprintf(buf + pos, buf_size - pos, "Model:       %s\n", cfg.default_model ? cfg.default_model : "(default)");
    pos += (size_t)snprintf(buf + pos, buf_size - pos, "Temperature: %.1f\n\n", cfg.default_temperature);
    pos += (size_t)snprintf(buf + pos, buf_size - pos, "Memory:      %s (auto-save: %s)\n",
        cfg.memory_backend ? cfg.memory_backend : "",
        cfg.memory_auto_save ? "on" : "off");
    pos += (size_t)snprintf(buf + pos, buf_size - pos, "Gateway:     %s:%u\n",
        cfg.gateway_host ? cfg.gateway_host : "127.0.0.1", (unsigned)cfg.gateway.port);

    size_t n_meta;
    const sc_channel_meta_t *meta = sc_channel_catalog_all(&n_meta);
    pos += (size_t)snprintf(buf + pos, buf_size - pos, "\nChannels:\n");
    char status_buf[64];
    for (size_t i = 0; i < n_meta && pos < buf_size - 64; i++) {
        const char *st = sc_channel_catalog_status_text(&cfg, &meta[i], status_buf, sizeof(status_buf));
        pos += (size_t)snprintf(buf + pos, buf_size - pos, "  %s: %s\n", meta[i].label, st);
    }

    sc_config_deinit(&cfg);
    return SC_OK;
}
