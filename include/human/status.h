#ifndef HU_STATUS_H
#define HU_STATUS_H

#include "human/core/allocator.h"
#include "human/core/error.h"

/**
 * Run status report (version, workspace, config, provider, etc.).
 * Writes to provided buffer. In HU_IS_TEST skips file I/O.
 */
hu_error_t hu_status_run(hu_allocator_t *alloc, char *buf, size_t buf_size);

#endif /* HU_STATUS_H */
