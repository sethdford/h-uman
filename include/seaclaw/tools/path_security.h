#ifndef SC_TOOLS_PATH_SECURITY_H
#define SC_TOOLS_PATH_SECURITY_H

#include "seaclaw/core/allocator.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * Check if a relative path is safe (no traversal, no null bytes).
 * Rejects: "..", path traversal, absolute paths, URL-encoded traversal.
 */
bool sc_path_is_safe(const char *path);

/**
 * Check whether a resolved absolute path is allowed.
 * 1. System blocklist always rejects (/etc, /bin, C:\Windows, etc.)
 * 2. Workspace prefix matches -> allowed
 * 3. Any allowed_paths prefix matches -> allowed
 * allowed_paths entries can be "*" for allow-all (except system).
 */
bool sc_path_resolved_allowed(sc_allocator_t *alloc,
    const char *resolved,
    const char *ws_resolved,
    const char *const *allowed_paths,
    size_t allowed_count);

#endif /* SC_TOOLS_PATH_SECURITY_H */
