#ifndef HU_SKILL_REGISTRY_H
#define HU_SKILL_REGISTRY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

#define HU_SKILL_REGISTRY_URL \
    "https://raw.githubusercontent.com/human/skill-registry/main/registry.json"

/**
 * Registry entry — one skill from the remote registry index.
 */
typedef struct hu_skill_registry_entry {
    char *name;
    char *description;
    char *version;
    char *author;
    char *url;
    char *tags; /* comma-separated, or NULL */
} hu_skill_registry_entry_t;

/**
 * Search the registry index by query. Filters by name, description, tags.
 * Under HU_IS_TEST, returns mock data without network.
 * Caller frees entries with hu_skill_registry_entries_free.
 */
hu_error_t hu_skill_registry_search(hu_allocator_t *alloc, const char *query,
                                    hu_skill_registry_entry_t **out_entries, size_t *out_count);

void hu_skill_registry_entries_free(hu_allocator_t *alloc, hu_skill_registry_entry_t *entries,
                                    size_t count);

/**
 * Install a skill from the registry by name.
 * Under HU_IS_TEST, returns HU_OK without network or filesystem.
 */
hu_error_t hu_skill_registry_install(hu_allocator_t *alloc, const char *name);

/**
 * Uninstall (remove) an installed skill from disk.
 * Under HU_IS_TEST, returns HU_OK without filesystem.
 */
hu_error_t hu_skill_registry_uninstall(const char *name);

/**
 * Update all installed skills from the registry.
 * Under HU_IS_TEST, returns HU_OK without network.
 */
hu_error_t hu_skill_registry_update(hu_allocator_t *alloc);

/**
 * Get the installed skills directory path (~/.human/skills).
 * Writes to out, returns length written. Returns 0 if home not set.
 */
size_t hu_skill_registry_get_installed_dir(char *out, size_t out_len);

/**
 * Publish a skill from a directory (validates .skill.json or SKILL.md, prints
 * contribute instructions). Under HU_IS_TEST, returns HU_OK without filesystem.
 */
hu_error_t hu_skill_registry_publish(hu_allocator_t *alloc, const char *skill_dir);

#endif /* HU_SKILL_REGISTRY_H */
