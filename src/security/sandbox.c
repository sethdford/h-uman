#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"

/* Create a noop sandbox (always available, no isolation).
 * Zig parity: createNoopSandbox. */
static sc_noop_sandbox_ctx_t g_noop_ctx;

sc_sandbox_t sc_sandbox_create_noop(void) {
    return sc_noop_sandbox_get(&g_noop_ctx);
}
