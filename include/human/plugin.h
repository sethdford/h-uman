#ifndef HU_PLUGIN_H
#define HU_PLUGIN_H

#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_PLUGIN_API_VERSION 1

typedef struct hu_plugin_info {
    const char *name;
    const char *version;
    const char *description;
    int api_version;
} hu_plugin_info_t;

typedef struct hu_plugin_host {
    hu_allocator_t *alloc;
    hu_error_t (*register_tool)(void *host_ctx, const char *name, void *tool_vtable);
    hu_error_t (*register_provider)(void *host_ctx, const char *name, void *provider_vtable);
    hu_error_t (*register_channel)(void *host_ctx, const hu_channel_t *channel);
    void *host_ctx;
} hu_plugin_host_t;

typedef hu_error_t (*hu_plugin_init_fn)(hu_plugin_host_t *host, hu_plugin_info_t *info_out);
typedef void (*hu_plugin_deinit_fn)(void);

typedef struct hu_plugin_registry hu_plugin_registry_t;

hu_plugin_registry_t *hu_plugin_registry_create(hu_allocator_t *alloc, uint32_t max_plugins);
void hu_plugin_registry_destroy(hu_plugin_registry_t *reg);

hu_error_t hu_plugin_register(hu_plugin_registry_t *reg, const hu_plugin_info_t *info,
                              const hu_tool_t *tools, size_t tools_count);

hu_error_t hu_plugin_get_tools(hu_plugin_registry_t *reg, hu_tool_t **out_tools, size_t *out_count);

size_t hu_plugin_count(hu_plugin_registry_t *reg);

hu_error_t hu_plugin_get_info(hu_plugin_registry_t *reg, size_t index, hu_plugin_info_t *out);

#endif
