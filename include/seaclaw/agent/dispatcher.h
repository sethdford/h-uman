#ifndef SC_AGENT_DISPATCHER_H
#define SC_AGENT_DISPATCHER_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/provider.h"
#include "seaclaw/tool.h"
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Agent Dispatcher — parallel tool call execution
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_dispatcher {
    uint32_t max_parallel;  /* 1 = sequential; >1 = parallel (when supported) */
    uint32_t timeout_secs;   /* per-tool timeout; 0 = no limit */
} sc_dispatcher_t;

typedef struct sc_dispatch_result {
    sc_tool_result_t *results;  /* owned; same order as input calls */
    size_t count;
} sc_dispatch_result_t;

/* Create dispatcher with default config (max_parallel=1, timeout=0). */
void sc_dispatcher_default(sc_dispatcher_t *out);

/* Allocate and create dispatcher. Caller frees with sc_dispatcher_destroy. */
sc_error_t sc_dispatcher_create(sc_allocator_t *alloc,
    uint32_t max_parallel, uint32_t timeout_secs,
    sc_dispatcher_t **out);

/* Destroy dispatcher allocated by sc_dispatcher_create. */
void sc_dispatcher_destroy(sc_allocator_t *alloc, sc_dispatcher_t *d);

/* Execute multiple tool calls. Uses pthreads for parallel when max_parallel > 1
 * and platform is POSIX; otherwise sequential. In SC_IS_TEST mode, always
 * sequential (no thread spawning). Caller must call sc_dispatch_result_free. */
sc_error_t sc_dispatcher_dispatch(sc_dispatcher_t *d,
    sc_allocator_t *alloc,
    sc_tool_t *tools, size_t tools_count,
    const sc_tool_call_t *calls, size_t calls_count,
    sc_dispatch_result_t *out);

/* Free results from sc_dispatcher_dispatch. */
void sc_dispatch_result_free(sc_allocator_t *alloc, sc_dispatch_result_t *r);

#endif /* SC_AGENT_DISPATCHER_H */
