#include "human/channels/imessage.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Test-only stub — replaces real exec(imsg) call for deterministic tests.
 * When set, hu_imessage_send_sticker calls this instead of running imsg.
 * Pass NULL to clear. */
static hu_error_t (*g_test_send_sticker_stub)(const char *target, size_t target_len,
                                              const char *sticker_path) = NULL;

#ifdef HU_IS_TEST
void hu_imessage_set_test_send_sticker_stub(hu_error_t (*stub)(const char *target,
                                                               size_t target_len,
                                                               const char *sticker_path)) {
    g_test_send_sticker_stub = stub;
}
#endif

/* Public: capability probe / test surface for native iMessage sticker send.
 *
 * IMPORTANT: macOS exposes NO automation API to send a native sticker/Memoji
 * balloon. AppleScript `send` handles only text + file attachments; the imsg
 * CLI has no sticker verb; BlueBubbles cannot send stickers; the Messages
 * sticker picker is UI-only and Memoji are generated client-side. This was
 * verified empirically — see
 * docs/investigations/imessage-sticker-memoji-feasibility.md (Phase 2: "Not
 * Feasible (send-side)"). The imessage vtable therefore sets .send_sticker =
 * NULL, and this function is unreachable in production.
 *
 * It is retained as a tested capability probe: it validates its inputs and the
 * test-stub mechanism, then returns HU_ERR_NOT_SUPPORTED in production. Do NOT
 * "wire it into the dispatcher" — there is no exec call to add. To send an
 * expressive image, use the regular .send attachment path (which renders as an
 * ordinary image bubble, not a native sticker balloon).
 *
 * Returns HU_OK only via a test stub; HU_ERR_INVALID_ARGUMENT for null/empty
 * args; HU_ERR_NOT_FOUND if the sticker file doesn't exist; HU_ERR_NOT_SUPPORTED
 * in production (no platform API). */
hu_error_t hu_imessage_send_sticker(void *ctx, const char *target, size_t target_len,
                                    const char *sticker_path, size_t sticker_path_len) {
    (void)ctx;
    (void)sticker_path_len; /* sticker_path is NUL-terminated; len informational */

    if (!target || !sticker_path || target_len == 0 || !sticker_path[0]) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Validate sticker file exists. */
    struct stat st;
    if (stat(sticker_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return HU_ERR_NOT_FOUND;
    }

    /* Test path: deterministic stub. */
    if (g_test_send_sticker_stub) {
        return g_test_send_sticker_stub(target, target_len, sticker_path);
    }

    /* Production: there is no platform API to send a native sticker (see the
     * function-doc comment above and the feasibility investigation). This is
     * terminal, not a TODO — return NOT_SUPPORTED. The vtable slot is NULL, so
     * the dispatcher never reaches here; expressive images go via .send. */
    return HU_ERR_NOT_SUPPORTED;
}
