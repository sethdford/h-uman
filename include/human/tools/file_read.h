#ifndef HU_TOOLS_FILE_READ_H
#define HU_TOOLS_FILE_READ_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/security.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

hu_error_t hu_file_read_create(hu_allocator_t *alloc, const char *workspace_dir,
                               size_t workspace_dir_len, hu_security_policy_t *policy,
                               hu_tool_t *out);

/* Apply tools.max_file_size_bytes from config to a tool created by
 * hu_file_read_create. 0 keeps the built-in 1 MiB default. */
void hu_file_read_set_max_size(hu_tool_t *tool, uint32_t max_bytes);

/* Size predicate used by file_read: true when 0 < size <= limit, where limit
 * is configured_max, or the built-in 1 MiB default when configured_max == 0. */
bool hu_file_read_size_allowed(long size, uint32_t configured_max);

#endif /* HU_TOOLS_FILE_READ_H */
