#ifndef SC_SKILL_REGISTRY_H
#define SC_SKILL_REGISTRY_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

#define SC_SKILL_REGISTRY_URL "https://raw.githubusercontent.com/seaclaw/skill-registry/main/registry.json"

/**
 * Registry entry — one skill from the remote registry index.
 */
typedef struct sc_skill_registry_entry {
    char *name;
    char *description;
    char *version;
    char *author;
    char *url;
    char *tags;  /* comma-separated, or NULL */
} sc_skill_registry_entry_t;

/**
 * Search the registry index by query. Filters by name, description, tags.
 * Under SC_IS_TEST, returns mock data without network.
 * Caller frees entries with sc_skill_registry_entries_free.
 */
sc_error_t sc_skill_registry_search(sc_allocator_t *alloc, const char *query,
    sc_skill_registry_entry_t **out_entries, size_t *out_count);

void sc_skill_registry_entries_free(sc_allocator_t *alloc,
    sc_skill_registry_entry_t *entries, size_t count);

/**
 * Install a skill from the registry by name.
 * Under SC_IS_TEST, returns SC_OK without network or filesystem.
 */
sc_error_t sc_skill_registry_install(sc_allocator_t *alloc, const char *name);

/**
 * Uninstall (remove) an installed skill from disk.
 * Under SC_IS_TEST, returns SC_OK without filesystem.
 */
sc_error_t sc_skill_registry_uninstall(const char *name);

/**
 * Update all installed skills from the registry.
 * Under SC_IS_TEST, returns SC_OK without network.
 */
sc_error_t sc_skill_registry_update(sc_allocator_t *alloc);

/**
 * Get the installed skills directory path (~/.seaclaw/skills).
 * Writes to out, returns length written. Returns 0 if home not set.
 */
size_t sc_skill_registry_get_installed_dir(char *out, size_t out_len);

#endif /* SC_SKILL_REGISTRY_H */
