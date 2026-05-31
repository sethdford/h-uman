#include "human/capabilities.h"
#include "human/channel_catalog.h"
#include "human/core/string.h"
#include "human/version.h"
#include <stdio.h>
#include <string.h>

/* Memory backend names (Human minimal set) */
static const char *MEMORY_BACKENDS[] = {"none", "markdown", "memory_lru", "sqlite"};
#define N_MEMORY_BACKENDS (sizeof(MEMORY_BACKENDS) / sizeof(MEMORY_BACKENDS[0]))

/* Core tool names */
static const char *CORE_TOOL_NAMES[] = {
    "shell",         "file_read",   "file_write",    "file_edit", "git",      "memory_store",
    "memory_recall", "memory_list", "memory_forget", "delegate",  "schedule", "spawn",
};
#define N_CORE_TOOLS (sizeof(CORE_TOOL_NAMES) / sizeof(CORE_TOOL_NAMES[0]))

static size_t append_json_string_array(char *out, size_t cap, size_t *pos, const char *const *names,
                                       size_t n) {
    *pos = hu_buf_appendf(out, cap, *pos, "[");
    for (size_t i = 0; i < n; i++) {
        if (i > 0)
            *pos = hu_buf_appendf(out, cap, *pos, ", ");
        *pos = hu_buf_appendf(out, cap, *pos, "\"%s\"", names[i]);
    }
    *pos = hu_buf_appendf(out, cap, *pos, "]");
    return *pos;
}

hu_error_t hu_capabilities_build_manifest_json(hu_allocator_t *alloc, const hu_config_t *cfg_opt,
                                               const hu_tool_t *runtime_tools,
                                               size_t runtime_tools_count, char **out_json) {
    if (!alloc || !out_json)
        return HU_ERR_INVALID_ARGUMENT;

    size_t cap = 4096;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    const char *version = hu_version_string();
    const char *backend = cfg_opt && cfg_opt->memory_backend ? cfg_opt->memory_backend : "";

    pos = hu_buf_appendf(buf, cap, pos,
                         "{\n  \"version\": \"%s\",\n  \"active_memory_backend\": \"%s\",\n  "
                         "\"channels\": [\n",
                         version, backend);

    size_t n_meta;
    const hu_channel_meta_t *meta = hu_channel_catalog_all(&n_meta);
    for (size_t i = 0; i < n_meta; i++) {
        bool enabled = hu_channel_catalog_is_build_enabled(meta[i].id);
        size_t cfg_count = cfg_opt ? hu_channel_catalog_configured_count(cfg_opt, meta[i].id) : 0;
        bool configured = enabled && cfg_count > 0;
        pos = hu_buf_appendf(buf, cap, pos,
                             "    {\"key\": \"%s\", \"label\": \"%s\", \"enabled_in_build\": %s, "
                             "\"configured\": %s, \"configured_count\": %zu}%s\n",
                             meta[i].key, meta[i].label, enabled ? "true" : "false",
                             configured ? "true" : "false", cfg_count, i + 1 < n_meta ? "," : "");
    }
    pos = hu_buf_appendf(buf, cap, pos, "  ],\n  \"memory_engines\": [\n");
    for (size_t i = 0; i < N_MEMORY_BACKENDS; i++) {
        bool configured = cfg_opt && cfg_opt->memory_backend &&
                          strcmp(cfg_opt->memory_backend, MEMORY_BACKENDS[i]) == 0;
        pos = hu_buf_appendf(buf, cap, pos,
                             "    {\"name\": \"%s\", \"enabled_in_build\": true, \"configured\": "
                             "%s}%s\n",
                             MEMORY_BACKENDS[i], configured ? "true" : "false",
                             i + 1 < N_MEMORY_BACKENDS ? "," : "");
    }
    pos = hu_buf_appendf(buf, cap, pos, "  ],\n  \"tools\": {\n");

    /* runtime_loaded */
    pos = hu_buf_appendf(buf, cap, pos, "    \"runtime_loaded\": ");
    if (runtime_tools && runtime_tools_count > 0) {
        pos = hu_buf_appendf(buf, cap, pos, "[");
        for (size_t i = 0; i < runtime_tools_count; i++) {
            const char *n = runtime_tools[i].vtable->name(runtime_tools[i].ctx);
            pos = hu_buf_appendf(buf, cap, pos, "%s\"%s\"", i ? ", " : "", n ? n : "?");
        }
        pos = hu_buf_appendf(buf, cap, pos, "]");
    } else
        pos = hu_buf_appendf(buf, cap, pos, "[]");
    pos = hu_buf_appendf(buf, cap, pos, ",\n    \"estimated_enabled_from_config\": ");
    append_json_string_array(buf, cap, &pos, CORE_TOOL_NAMES, N_CORE_TOOLS);
    pos = hu_buf_appendf(buf, cap, pos, "\n  }\n}\n");
    if (pos >= cap)
        pos = cap - 1;

    *out_json = buf;
    return HU_OK;
}

hu_error_t hu_capabilities_build_summary_text(hu_allocator_t *alloc, const hu_config_t *cfg_opt,
                                              const hu_tool_t *runtime_tools,
                                              size_t runtime_tools_count, char **out_text) {
    if (!alloc || !out_text)
        return HU_ERR_INVALID_ARGUMENT;

    char en[256] = {0}, dis[256] = {0}, cfg[256] = {0};
    size_t en_len = 0, dis_len = 0, cfg_len = 0;

    size_t n_meta;
    const hu_channel_meta_t *meta = hu_channel_catalog_all(&n_meta);
    for (size_t i = 0; i < n_meta; i++) {
        bool built = hu_channel_catalog_is_build_enabled(meta[i].id);
        bool has_cfg = cfg_opt && hu_channel_catalog_is_configured(cfg_opt, meta[i].id);
        const char *sep = (en_len || dis_len || cfg_len) ? ", " : "";
        if (built) {
            en_len = hu_buf_appendf(en, sizeof(en), en_len, "%s%s", sep, meta[i].key);
            if (has_cfg)
                cfg_len = hu_buf_appendf(cfg, sizeof(cfg), cfg_len, "%s%s", cfg_len ? ", " : "",
                                           meta[i].key);
        } else {
            dis_len = hu_buf_appendf(dis, sizeof(dis), dis_len, "%s%s", sep, meta[i].key);
        }
    }
    if (en_len == 0)
        snprintf(en, sizeof(en), "%s", "(none)");
    if (dis_len == 0)
        snprintf(dis, sizeof(dis), "%s", "(none)");
    if (cfg_len == 0)
        snprintf(cfg, sizeof(cfg), "%s", "(none)");

    const char *backend =
        cfg_opt && cfg_opt->memory_backend ? cfg_opt->memory_backend : "(unknown)";
    const char *tools_label = (runtime_tools && runtime_tools_count > 0)
                                  ? "tools (loaded)"
                                  : "tools (estimated from config)";

    char tools_buf[128] = "(none)";
    if (runtime_tools && runtime_tools_count > 0) {
        size_t t = 0;
        tools_buf[0] = '\0';
        for (size_t i = 0; i < runtime_tools_count; i++) {
            const char *n = runtime_tools[i].vtable->name(runtime_tools[i].ctx);
            t = hu_buf_appendf(tools_buf, sizeof(tools_buf), t, "%s%s", t ? ", " : "",
                               n ? n : "?");
        }
        if (t == 0)
            (void)hu_buf_appendf(tools_buf, sizeof(tools_buf), 0, "(none)");
    } else {
        size_t t = 0;
        tools_buf[0] = '\0';
        for (size_t i = 0; i < N_CORE_TOOLS; i++) {
            t = hu_buf_appendf(tools_buf, sizeof(tools_buf), t, "%s%s", t ? ", " : "",
                               CORE_TOOL_NAMES[i]);
        }
        if (t == 0)
            (void)hu_buf_appendf(tools_buf, sizeof(tools_buf), 0, "(none)");
    }

    size_t total = 512;
    char *out = (char *)alloc->alloc(alloc->ctx, total);
    if (!out)
        return HU_ERR_OUT_OF_MEMORY;

    int n = snprintf(out, total,
                     "Capabilities\n\nAvailable in this runtime:\n"
                     "  channels (build): %s\n"
                     "  channels (configured): %s\n"
                     "  memory engines (build): none, markdown, memory_lru, sqlite\n"
                     "  active memory backend: %s\n"
                     "  %s: %s\n\n"
                     "Not available in this runtime:\n"
                     "  channels (disabled in build): %s\n"
                     "  memory engines (disabled in build): (none)\n"
                     "  optional tools (disabled by config): (none)\n",
                     en, cfg, backend, tools_label, tools_buf, dis);

    if (n < 0 || (size_t)n >= total) {
        alloc->free(alloc->ctx, out, total);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out_text = out;
    return HU_OK;
}

hu_error_t hu_capabilities_build_prompt_section(hu_allocator_t *alloc, const hu_config_t *cfg_opt,
                                                const hu_tool_t *runtime_tools,
                                                size_t runtime_tools_count, char **out_text) {
    return hu_capabilities_build_summary_text(alloc, cfg_opt, runtime_tools, runtime_tools_count,
                                              out_text);
}
