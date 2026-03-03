#ifndef SC_PLUGIN_H
#define SC_PLUGIN_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SC_PLUGIN_API_VERSION 1

typedef struct sc_plugin_info {
    const char *name;
    const char *version;
    const char *description;
    uint32_t api_version;
} sc_plugin_info_t;

typedef struct sc_plugin_registry sc_plugin_registry_t;

sc_plugin_registry_t *sc_plugin_registry_create(sc_allocator_t *alloc, uint32_t max_plugins);
void sc_plugin_registry_destroy(sc_plugin_registry_t *reg);

sc_error_t sc_plugin_register(sc_plugin_registry_t *reg, const sc_plugin_info_t *info,
                              const sc_tool_t *tools, size_t tools_count);

sc_error_t sc_plugin_get_tools(sc_plugin_registry_t *reg, sc_tool_t **out_tools, size_t *out_count);

size_t sc_plugin_count(sc_plugin_registry_t *reg);

sc_error_t sc_plugin_get_info(sc_plugin_registry_t *reg, size_t index, sc_plugin_info_t *out);

#endif
