/*
 * src/agent/hula_sdk.c — HuLa SDK v0.2.0 extern symbols.
 *
 * Houses the parts of the v0.2.0 SDK surface that must be real symbols
 * (not `static inline`), so language bindings have a single dlsym target.
 *
 * Defines:
 *   - struct hu_hula_ctx       (full layout — opaque to embedders)
 *   - hu_hula_ctx_create       (binding-friendly handle constructor)
 *   - hu_hula_ctx_destroy      (NULL-safe finalizer)
 *   - hu_hula_error_string     (hu_error_t -> identifier string)
 *
 * Backward-compatibility contract: every declaration here is additive over
 * v0.1.0. The existing `static inline` helpers in `include/human/hula_sdk.h`
 * (hu_hula_sdk_call / hu_hula_sdk_sequence / hu_hula_sdk_run_json) are NOT
 * touched — embedders that compiled against v0.1.0 continue to compile
 * unchanged against v0.2.0.
 *
 * Compiled with -Wall -Wextra -Wpedantic -Wswitch-enum -Werror.
 */

#include "human/hula_sdk.h"

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stddef.h>

/*
 * Full ctx layout — bindings never see this; they only hold `hu_hula_ctx_t *`.
 *
 * Kept minimal per the v0.2.0 story: only the embedded allocator. Anything
 * else is YAGNI until US-10.2 / US-10.3 prove a need.
 */
struct hu_hula_ctx {
    hu_allocator_t alloc;
};

hu_error_t hu_hula_ctx_create(hu_allocator_t *alloc, hu_hula_ctx_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!alloc->alloc || !alloc->free)
        return HU_ERR_INVALID_ARGUMENT;

    struct hu_hula_ctx *ctx = (struct hu_hula_ctx *)alloc->alloc(alloc->ctx, sizeof(*ctx));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;

    ctx->alloc = *alloc; /* copy vtable by value */
    *out = ctx;
    return HU_OK;
}

void hu_hula_ctx_destroy(hu_hula_ctx_t *ctx) {
    if (!ctx)
        return;

    /*
     * Capture the allocator locals BEFORE freeing the struct that holds
     * them — otherwise we'd dereference freed memory to call free().
     */
    hu_allocator_t alloc = ctx->alloc;
    alloc.free(alloc.ctx, ctx, sizeof(*ctx));
}

/*
 * Translate `hu_error_t` -> identifier string.
 *
 * The `default` arm is placed LAST and emits the "HU_ERR_UNKNOWN" sentinel
 * so the compiler's `-Wswitch-enum` warning (when enabled) still fires if a
 * future enum value is added without a matching case here. The pin-the-bug
 * regression test in `tests/test_hula_sdk_v2.c` asserts the sentinel literal
 * exactly so an accidental change (NULL, lowercased, etc.) fails loudly.
 */
const char *hu_hula_error_string(hu_error_t err) {
    switch (err) {
    case HU_OK:
        return "HU_OK";

    case HU_ERR_OUT_OF_MEMORY:
        return "HU_ERR_OUT_OF_MEMORY";
    case HU_ERR_INVALID_ARGUMENT:
        return "HU_ERR_INVALID_ARGUMENT";
    case HU_ERR_NOT_FOUND:
        return "HU_ERR_NOT_FOUND";
    case HU_ERR_ALREADY_EXISTS:
        return "HU_ERR_ALREADY_EXISTS";
    case HU_ERR_NOT_SUPPORTED:
        return "HU_ERR_NOT_SUPPORTED";
    case HU_ERR_PERMISSION_DENIED:
        return "HU_ERR_PERMISSION_DENIED";
    case HU_ERR_TIMEOUT:
        return "HU_ERR_TIMEOUT";
    case HU_ERR_IO:
        return "HU_ERR_IO";
    case HU_ERR_PARSE:
        return "HU_ERR_PARSE";

    case HU_ERR_CONFIG_INVALID:
        return "HU_ERR_CONFIG_INVALID";
    case HU_ERR_CONFIG_NOT_FOUND:
        return "HU_ERR_CONFIG_NOT_FOUND";

    case HU_ERR_PROVIDER_UNAVAILABLE:
        return "HU_ERR_PROVIDER_UNAVAILABLE";
    case HU_ERR_PROVIDER_AUTH:
        return "HU_ERR_PROVIDER_AUTH";
    case HU_ERR_PROVIDER_RATE_LIMITED:
        return "HU_ERR_PROVIDER_RATE_LIMITED";
    case HU_ERR_PROVIDER_RESPONSE:
        return "HU_ERR_PROVIDER_RESPONSE";

    case HU_ERR_CHANNEL_SEND:
        return "HU_ERR_CHANNEL_SEND";
    case HU_ERR_CHANNEL_START:
        return "HU_ERR_CHANNEL_START";
    case HU_ERR_CHANNEL_NOT_CONFIGURED:
        return "HU_ERR_CHANNEL_NOT_CONFIGURED";

    case HU_ERR_TOOL_EXECUTION:
        return "HU_ERR_TOOL_EXECUTION";
    case HU_ERR_TOOL_VALIDATION:
        return "HU_ERR_TOOL_VALIDATION";
    case HU_ERR_TOOL_NOT_FOUND:
        return "HU_ERR_TOOL_NOT_FOUND";

    case HU_ERR_MEMORY_STORE:
        return "HU_ERR_MEMORY_STORE";
    case HU_ERR_MEMORY_RECALL:
        return "HU_ERR_MEMORY_RECALL";
    case HU_ERR_MEMORY_BACKEND:
        return "HU_ERR_MEMORY_BACKEND";

    case HU_ERR_SECURITY_COMMAND_NOT_ALLOWED:
        return "HU_ERR_SECURITY_COMMAND_NOT_ALLOWED";
    case HU_ERR_SECURITY_HIGH_RISK_BLOCKED:
        return "HU_ERR_SECURITY_HIGH_RISK_BLOCKED";
    case HU_ERR_SECURITY_APPROVAL_REQUIRED:
        return "HU_ERR_SECURITY_APPROVAL_REQUIRED";
    case HU_ERR_SECURITY_RATE_LIMITED:
        return "HU_ERR_SECURITY_RATE_LIMITED";
    case HU_ERR_SECURITY_LOCKOUT:
        return "HU_ERR_SECURITY_LOCKOUT";

    case HU_ERR_PERIPHERAL_NOT_CONNECTED:
        return "HU_ERR_PERIPHERAL_NOT_CONNECTED";
    case HU_ERR_PERIPHERAL_IO:
        return "HU_ERR_PERIPHERAL_IO";
    case HU_ERR_PERIPHERAL_FLASH_FAILED:
        return "HU_ERR_PERIPHERAL_FLASH_FAILED";
    case HU_ERR_PERIPHERAL_DEVICE_NOT_FOUND:
        return "HU_ERR_PERIPHERAL_DEVICE_NOT_FOUND";

    case HU_ERR_TUNNEL_START_FAILED:
        return "HU_ERR_TUNNEL_START_FAILED";
    case HU_ERR_TUNNEL_URL_NOT_FOUND:
        return "HU_ERR_TUNNEL_URL_NOT_FOUND";

    case HU_ERR_GATEWAY_RATE_LIMITED:
        return "HU_ERR_GATEWAY_RATE_LIMITED";
    case HU_ERR_GATEWAY_BODY_TOO_LARGE:
        return "HU_ERR_GATEWAY_BODY_TOO_LARGE";
    case HU_ERR_GATEWAY_AUTH:
        return "HU_ERR_GATEWAY_AUTH";

    case HU_ERR_CRYPTO_ENCRYPT:
        return "HU_ERR_CRYPTO_ENCRYPT";
    case HU_ERR_CRYPTO_DECRYPT:
        return "HU_ERR_CRYPTO_DECRYPT";
    case HU_ERR_CRYPTO_HMAC:
        return "HU_ERR_CRYPTO_HMAC";

    case HU_ERR_JSON_PARSE:
        return "HU_ERR_JSON_PARSE";
    case HU_ERR_JSON_TYPE:
        return "HU_ERR_JSON_TYPE";
    case HU_ERR_JSON_DEPTH:
        return "HU_ERR_JSON_DEPTH";

    case HU_ERR_INTERNAL:
        return "HU_ERR_INTERNAL";
    case HU_ERR_SUBAGENT_TOO_MANY:
        return "HU_ERR_SUBAGENT_TOO_MANY";
    case HU_ERR_CANCELLED:
        return "HU_ERR_CANCELLED";

    case HU_ERR_LIMIT_REACHED:
        return "HU_ERR_LIMIT_REACHED";

    case HU_ERR_FLEET_DEPTH_EXCEEDED:
        return "HU_ERR_FLEET_DEPTH_EXCEEDED";
    case HU_ERR_FLEET_SPAWN_CAP:
        return "HU_ERR_FLEET_SPAWN_CAP";
    case HU_ERR_FLEET_BUDGET_EXCEEDED:
        return "HU_ERR_FLEET_BUDGET_EXCEEDED";

    case HU_ERR_COUNT:
        /* Sentinel terminator of the enum; not a real error. Fall through. */
        break;
    }
    return "HU_ERR_UNKNOWN";
}
