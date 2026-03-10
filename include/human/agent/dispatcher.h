#ifndef HU_AGENT_DISPATCHER_H
#define HU_AGENT_DISPATCHER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/tool.h"
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Agent Dispatcher — parallel tool call execution
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_dispatcher {
    uint32_t max_parallel; /* 1 = sequential; >1 = parallel (when supported) */
    uint32_t timeout_secs; /* per-tool timeout; 0 = no limit */
} hu_dispatcher_t;

typedef struct hu_dispatch_result {
    hu_tool_result_t *results; /* owned; same order as input calls */
    size_t count;
} hu_dispatch_result_t;

/* Create dispatcher with default config (max_parallel=1, timeout=0). */
void hu_dispatcher_default(hu_dispatcher_t *out);

/* Allocate and create dispatcher. Caller frees with hu_dispatcher_destroy. */
hu_error_t hu_dispatcher_create(hu_allocator_t *alloc, uint32_t max_parallel, uint32_t timeout_secs,
                                hu_dispatcher_t **out);

/* Destroy dispatcher allocated by hu_dispatcher_create. */
void hu_dispatcher_destroy(hu_allocator_t *alloc, hu_dispatcher_t *d);

/* Execute multiple tool calls. Uses pthreads for parallel when max_parallel > 1
 * and platform is POSIX; otherwise sequential. In HU_IS_TEST mode, always
 * sequential (no thread spawning). Caller must call hu_dispatch_result_free. */
hu_error_t hu_dispatcher_dispatch(hu_dispatcher_t *d, hu_allocator_t *alloc, hu_tool_t *tools,
                                  size_t tools_count, const hu_tool_call_t *calls,
                                  size_t calls_count, hu_dispatch_result_t *out);

/* Free results from hu_dispatcher_dispatch. */
void hu_dispatch_result_free(hu_allocator_t *alloc, hu_dispatch_result_t *r);

#endif /* HU_AGENT_DISPATCHER_H */
