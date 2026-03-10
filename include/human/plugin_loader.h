#ifndef HU_PLUGIN_LOADER_H
#define HU_PLUGIN_LOADER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/plugin.h"
#include <stddef.h>

typedef struct hu_plugin_handle hu_plugin_handle_t;

hu_error_t hu_plugin_load(hu_allocator_t *alloc, const char *path, hu_plugin_host_t *host,
                          hu_plugin_info_t *info_out, hu_plugin_handle_t **out_handle);

void hu_plugin_unload(hu_plugin_handle_t *handle);

void hu_plugin_unload_all(void);

#endif /* HU_PLUGIN_LOADER_H */
