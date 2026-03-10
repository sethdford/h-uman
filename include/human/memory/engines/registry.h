#ifndef HU_MEMORY_ENGINES_REGISTRY_H
#define HU_MEMORY_ENGINES_REGISTRY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Backend capabilities and descriptor
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_backend_capabilities {
    bool supports_keyword_rank;
    bool supports_session_store;
    bool supports_transactions;
    bool supports_outbox;
} hu_backend_capabilities_t;

typedef struct hu_backend_descriptor {
    const char *name;
    const char *label;
    bool auto_save_default;
    hu_backend_capabilities_t capabilities;
    bool needs_db_path;
    bool needs_workspace;
} hu_backend_descriptor_t;

/* Find backend descriptor by name. Returns NULL if not found. */
const hu_backend_descriptor_t *hu_registry_find_backend(const char *name, size_t name_len);

/* Check if name is a known backend (enabled or not). */
bool hu_registry_is_known_backend(const char *name, size_t name_len);

/* Map backend name to engine token string. Returns NULL if unknown. */
const char *hu_registry_engine_token_for_backend(const char *name, size_t name_len);

/* Format comma-separated list of enabled backend names. Caller frees result. */
char *hu_registry_format_enabled_backends(hu_allocator_t *alloc);

#endif /* HU_MEMORY_ENGINES_REGISTRY_H */
