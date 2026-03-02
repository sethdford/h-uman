#ifndef SC_STATUS_H
#define SC_STATUS_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"

/**
 * Run status report (version, workspace, config, provider, etc.).
 * Writes to provided buffer. In SC_IS_TEST skips file I/O.
 */
sc_error_t sc_status_run(sc_allocator_t *alloc, char *buf, size_t buf_size);

#endif /* SC_STATUS_H */
