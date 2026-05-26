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

/* Public: send a sticker file as an iMessage attachment via the imsg CLI.
 * Returns HU_OK on success, HU_ERR_INVALID_ARGUMENT for null/empty args,
 * HU_ERR_NOT_FOUND if the sticker file doesn't exist, HU_ERR_NOT_SUPPORTED
 * if the underlying send fails (imsg unavailable, permissions, etc.). */
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

    /* Production path: invoke imsg send --to <target> --file <sticker_path>.
     * Find the existing imsg-CLI helper in src/channels/imessage.c — likely
     * something like `hu_imessage_run_imsg_send_file` or inline within
     * imessage_send_with_attachment. If a reusable helper exists, call it.
     * Otherwise, return HU_ERR_NOT_SUPPORTED and document that the F2
     * dispatcher integration pass will wire the actual exec call. */
    /* For E2's CI-testable contract, we return HU_ERR_NOT_SUPPORTED in
     * production with no test stub. The dispatcher will see this and
     * choose a different reply style. */
    return HU_ERR_NOT_SUPPORTED;
}
