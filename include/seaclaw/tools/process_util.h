#ifndef SC_TOOLS_PROCESS_UTIL_H
#define SC_TOOLS_PROCESS_UTIL_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/process_util.h"
#include <stddef.h>

/* Tool-friendly process run: wraps sc_process_run with tool defaults. */
sc_error_t sc_tools_process_run(sc_allocator_t *alloc,
    const char *const *argv,
    const char *cwd,
    size_t max_output_bytes,
    sc_run_result_t *out);

#endif /* SC_TOOLS_PROCESS_UTIL_H */
