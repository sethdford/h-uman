#include "seaclaw/tools/process_util.h"

sc_error_t sc_tools_process_run(sc_allocator_t *alloc,
    const char *const *argv,
    const char *cwd,
    size_t max_output_bytes,
    sc_run_result_t *out) {
    if (!alloc || !argv || !argv[0] || !out) return SC_ERR_INVALID_ARGUMENT;
    if (max_output_bytes == 0) max_output_bytes = 1048576;
    return sc_process_run(alloc, argv, cwd, max_output_bytes, out);
}
