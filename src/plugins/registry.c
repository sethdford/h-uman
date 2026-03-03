#include "seaclaw/core/string.h"
#include "seaclaw/plugin.h"
#include <stdlib.h>
#include <string.h>

typedef struct sc_plugin_entry {
    sc_plugin_info_t info;
    char *name;
    char *version;
    char *description;
    sc_tool_t *tools;
    size_t tools_count;
    bool used;
} sc_plugin_entry_t;

struct sc_plugin_registry {
    sc_allocator_t *alloc;
    sc_plugin_entry_t *entries;
    uint32_t max_plugins;
    size_t count;
};

sc_plugin_registry_t *sc_plugin_registry_create(sc_allocator_t *alloc, uint32_t max_plugins) {
    if (!alloc || max_plugins == 0)
        return NULL;
    sc_plugin_registry_t *r = (sc_plugin_registry_t *)alloc->alloc(alloc->ctx, sizeof(*r));
    if (!r)
        return NULL;
    memset(r, 0, sizeof(*r));
    r->alloc = alloc;
    r->max_plugins = max_plugins;
    r->entries =
        (sc_plugin_entry_t *)alloc->alloc(alloc->ctx, max_plugins * sizeof(sc_plugin_entry_t));
    if (!r->entries) {
        alloc->free(alloc->ctx, r, sizeof(*r));
        return NULL;
    }
    memset(r->entries, 0, max_plugins * sizeof(sc_plugin_entry_t));
    return r;
}

void sc_plugin_registry_destroy(sc_plugin_registry_t *reg) {
    if (!reg)
        return;
    for (uint32_t i = 0; i < reg->max_plugins; i++) {
        sc_plugin_entry_t *e = &reg->entries[i];
        if (!e->used)
            continue;
        if (e->name)
            reg->alloc->free(reg->alloc->ctx, e->name, strlen(e->name) + 1);
        if (e->version)
            reg->alloc->free(reg->alloc->ctx, e->version, strlen(e->version) + 1);
        if (e->description)
            reg->alloc->free(reg->alloc->ctx, e->description, strlen(e->description) + 1);
        if (e->tools)
            reg->alloc->free(reg->alloc->ctx, e->tools, e->tools_count * sizeof(sc_tool_t));
    }
    reg->alloc->free(reg->alloc->ctx, reg->entries, reg->max_plugins * sizeof(sc_plugin_entry_t));
    reg->alloc->free(reg->alloc->ctx, reg, sizeof(*reg));
}

sc_error_t sc_plugin_register(sc_plugin_registry_t *reg, const sc_plugin_info_t *info,
                              const sc_tool_t *tools, size_t tools_count) {
    if (!reg || !info || !info->name)
        return SC_ERR_INVALID_ARGUMENT;
    if (info->api_version != SC_PLUGIN_API_VERSION)
        return SC_ERR_INVALID_ARGUMENT;
    uint32_t slot = reg->max_plugins;
    for (uint32_t i = 0; i < reg->max_plugins; i++)
        if (!reg->entries[i].used) {
            slot = i;
            break;
        }
    if (slot >= reg->max_plugins)
        return SC_ERR_OUT_OF_MEMORY;
    sc_plugin_entry_t *e = &reg->entries[slot];
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->name = sc_strndup(reg->alloc, info->name, strlen(info->name));
    e->version =
        info->version ? sc_strndup(reg->alloc, info->version, strlen(info->version)) : NULL;
    e->description = info->description
                         ? sc_strndup(reg->alloc, info->description, strlen(info->description))
                         : NULL;
    e->info.name = e->name;
    e->info.version = e->version;
    e->info.description = e->description;
    e->info.api_version = info->api_version;
    if (tools && tools_count > 0) {
        e->tools = (sc_tool_t *)reg->alloc->alloc(reg->alloc->ctx, tools_count * sizeof(sc_tool_t));
        if (e->tools) {
            memcpy(e->tools, tools, tools_count * sizeof(sc_tool_t));
            e->tools_count = tools_count;
        }
    }
    reg->count++;
    return SC_OK;
}

sc_error_t sc_plugin_get_tools(sc_plugin_registry_t *reg, sc_tool_t **out_tools,
                               size_t *out_count) {
    if (!reg || !out_tools || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_tools = NULL;
    *out_count = 0;
    size_t total = 0;
    for (uint32_t i = 0; i < reg->max_plugins; i++)
        if (reg->entries[i].used)
            total += reg->entries[i].tools_count;
    if (total == 0)
        return SC_OK;
    sc_tool_t *tools = (sc_tool_t *)reg->alloc->alloc(reg->alloc->ctx, total * sizeof(sc_tool_t));
    if (!tools)
        return SC_ERR_OUT_OF_MEMORY;
    size_t j = 0;
    for (uint32_t i = 0; i < reg->max_plugins; i++) {
        sc_plugin_entry_t *e = &reg->entries[i];
        if (!e->used || !e->tools)
            continue;
        memcpy(&tools[j], e->tools, e->tools_count * sizeof(sc_tool_t));
        j += e->tools_count;
    }
    *out_tools = tools;
    *out_count = total;
    return SC_OK;
}

size_t sc_plugin_count(sc_plugin_registry_t *reg) {
    return reg ? reg->count : 0;
}

sc_error_t sc_plugin_get_info(sc_plugin_registry_t *reg, size_t index, sc_plugin_info_t *out) {
    if (!reg || !out)
        return SC_ERR_INVALID_ARGUMENT;
    size_t j = 0;
    for (uint32_t i = 0; i < reg->max_plugins; i++) {
        if (!reg->entries[i].used)
            continue;
        if (j == index) {
            *out = reg->entries[i].info;
            return SC_OK;
        }
        j++;
    }
    return SC_ERR_NOT_FOUND;
}
