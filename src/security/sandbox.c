#include "human/security/sandbox.h"
#include "human/security/sandbox_internal.h"

/* Create a noop sandbox (always available, no isolation).
 * Zig parity: createNoopSandbox. */
static hu_noop_sandbox_ctx_t g_noop_ctx;

hu_sandbox_t hu_sandbox_create_noop(void) {
    return hu_noop_sandbox_get(&g_noop_ctx);
}
