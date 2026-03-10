#ifndef HU_MCP_REGISTRY_H
#define HU_MCP_REGISTRY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

#define HU_MCP_REGISTRY_ENTRIES_MAX 16

typedef struct hu_mcp_registry_entry {
    char name[64];
    char command[256];
    char args[512];
    bool running;
    int pid;
} hu_mcp_registry_entry_t;

typedef struct hu_mcp_registry hu_mcp_registry_t;

hu_mcp_registry_t *hu_mcp_registry_create(hu_allocator_t *alloc);
void hu_mcp_registry_destroy(hu_mcp_registry_t *reg);

hu_error_t hu_mcp_registry_add(hu_mcp_registry_t *reg, const char *name, const char *command,
                               const char *args);
hu_error_t hu_mcp_registry_remove(hu_mcp_registry_t *reg, const char *name);
hu_error_t hu_mcp_registry_list(hu_mcp_registry_t *reg, hu_mcp_registry_entry_t *out, int max,
                                int *count);
hu_error_t hu_mcp_registry_start(hu_mcp_registry_t *reg, const char *name);
hu_error_t hu_mcp_registry_stop(hu_mcp_registry_t *reg, const char *name);

#endif /* HU_MCP_REGISTRY_H */
