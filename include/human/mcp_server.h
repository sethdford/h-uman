#ifndef HU_MCP_SERVER_H
#define HU_MCP_SERVER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/mcp_resources.h"
#include "human/memory.h"
#include "human/tool.h"
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * MCP ENGINE — the JSON-RPC dispatcher that turns inbound MCP requests
 * into tool / resource / prompt operations and writes responses to stdout.
 *
 * Historical note: this type used to be called `hu_mcp_host_t`. The init #12
 * server-mode initiative will wrap this engine inside a public
 * `hu_mcp_server_t` vtable that layers consent + audit + rate-limit policy
 * on top of it. The deprecated shims below keep the old `hu_mcp_host_*`
 * names linking with a -Wdeprecated-declarations warning during the
 * cross-over so external callers have one release to migrate.
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_mcp_engine hu_mcp_engine_t;

hu_error_t hu_mcp_engine_create(hu_allocator_t *alloc, hu_tool_t *tools, size_t tool_count,
                                hu_memory_t *memory, hu_mcp_engine_t **out);

void hu_mcp_engine_set_resources(hu_mcp_engine_t *engine, hu_mcp_resource_registry_t *resources);
void hu_mcp_engine_set_prompts(hu_mcp_engine_t *engine, hu_mcp_prompt_registry_t *prompts);

hu_error_t hu_mcp_engine_run(hu_mcp_engine_t *engine);

void hu_mcp_engine_destroy(hu_mcp_engine_t *engine);

/* ── Deprecation shims for the old hu_mcp_host_* family ─────────────────────
 * These exist to keep out-of-tree callers compiling for one release while
 * they migrate to hu_mcp_engine_*. Internal call sites have already moved
 * over. The shims are intentionally `static inline` so they incur zero
 * runtime overhead and add no symbols at link time. The `deprecated`
 * attribute produces -Wdeprecated-declarations warnings (NOT -Werror) on
 * GCC/Clang; if the toolchain doesn't recognise the attribute we still
 * preserve source compatibility, just without the warning.
 * ────────────────────────────────────────────────────────────────────────── */

#if defined(__GNUC__) || defined(__clang__)
#define HU_MCP_HOST_DEPRECATED(new_name) \
    __attribute__((deprecated("renamed to " new_name)))
#else
#define HU_MCP_HOST_DEPRECATED(new_name)
#endif

typedef hu_mcp_engine_t hu_mcp_host_t;

HU_MCP_HOST_DEPRECATED("hu_mcp_engine_create")
static inline hu_error_t hu_mcp_host_create(hu_allocator_t *alloc, hu_tool_t *tools,
                                            size_t tool_count, hu_memory_t *memory,
                                            hu_mcp_host_t **out) {
    return hu_mcp_engine_create(alloc, tools, tool_count, memory, out);
}

HU_MCP_HOST_DEPRECATED("hu_mcp_engine_set_resources")
static inline void hu_mcp_host_set_resources(hu_mcp_host_t *srv,
                                             hu_mcp_resource_registry_t *resources) {
    hu_mcp_engine_set_resources(srv, resources);
}

HU_MCP_HOST_DEPRECATED("hu_mcp_engine_set_prompts")
static inline void hu_mcp_host_set_prompts(hu_mcp_host_t *srv,
                                           hu_mcp_prompt_registry_t *prompts) {
    hu_mcp_engine_set_prompts(srv, prompts);
}

HU_MCP_HOST_DEPRECATED("hu_mcp_engine_run")
static inline hu_error_t hu_mcp_host_run(hu_mcp_host_t *srv) {
    return hu_mcp_engine_run(srv);
}

HU_MCP_HOST_DEPRECATED("hu_mcp_engine_destroy")
static inline void hu_mcp_host_destroy(hu_mcp_host_t *srv) {
    hu_mcp_engine_destroy(srv);
}

#endif /* HU_MCP_SERVER_H */
