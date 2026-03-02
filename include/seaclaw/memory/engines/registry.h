#ifndef SC_MEMORY_ENGINES_REGISTRY_H
#define SC_MEMORY_ENGINES_REGISTRY_H

#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Backend capabilities and descriptor
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_backend_capabilities {
    bool supports_keyword_rank;
    bool supports_session_store;
    bool supports_transactions;
    bool supports_outbox;
} sc_backend_capabilities_t;

typedef struct sc_backend_descriptor {
    const char *name;
    const char *label;
    bool auto_save_default;
    sc_backend_capabilities_t capabilities;
    bool needs_db_path;
    bool needs_workspace;
} sc_backend_descriptor_t;

/* Find backend descriptor by name. Returns NULL if not found. */
const sc_backend_descriptor_t *sc_registry_find_backend(const char *name, size_t name_len);

/* Check if name is a known backend (enabled or not). */
bool sc_registry_is_known_backend(const char *name, size_t name_len);

/* Map backend name to engine token string. Returns NULL if unknown. */
const char *sc_registry_engine_token_for_backend(const char *name, size_t name_len);

/* Format comma-separated list of enabled backend names. Caller frees result. */
char *sc_registry_format_enabled_backends(sc_allocator_t *alloc);

#endif /* SC_MEMORY_ENGINES_REGISTRY_H */
