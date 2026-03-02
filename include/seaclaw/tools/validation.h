#ifndef SC_TOOLS_VALIDATION_H
#define SC_TOOLS_VALIDATION_H

#include "seaclaw/core/error.h"
#include <stddef.h>

/**
 * Path validation for file tools.
 * - Rejects paths containing ".." (path traversal)
 * - Rejects absolute paths outside workspace_dir when workspace_dir is set
 * - Rejects empty path
 * - Path length limited to 4096 bytes
 *
 * @param path Path to validate (null-terminated)
 * @param workspace_dir Base directory for relative paths (may be NULL to skip workspace check)
 * @param workspace_dir_len Length of workspace_dir
 * @return SC_OK if valid, SC_ERR_TOOL_VALIDATION on rejection
 */
sc_error_t sc_tool_validate_path(const char *path,
    const char *workspace_dir, size_t workspace_dir_len);

/**
 * URL validation for web tools.
 * - HTTPS only (rejects http://)
 * - Rejects private/internal IPs: 127.x, 10.x, 172.16-31.x, 192.168.x, ::1, fd*:*
 * - URL length limited to 8192 bytes
 *
 * @param url URL to validate (null-terminated)
 * @return SC_OK if valid, SC_ERR_TOOL_VALIDATION on rejection
 */
sc_error_t sc_tool_validate_url(const char *url);

#endif /* SC_TOOLS_VALIDATION_H */
