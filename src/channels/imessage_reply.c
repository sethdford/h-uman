#include "human/channels/imessage_reply.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include <stdio.h>
#include <string.h>

/* Test-only stubs — set by hu_imessage_set_test_reply_stubs. */
static hu_imessage_reply_tier_fn g_test_tier1 = NULL;
static hu_imessage_reply_tier_fn g_test_tier2 = NULL;
static hu_imessage_reply_flat_send_fn g_test_flat_send = NULL;
static char g_last_tier[32] = {0};

void hu_imessage_set_test_reply_stubs(hu_imessage_reply_tier_fn tier1,
                                      hu_imessage_reply_tier_fn tier2,
                                      hu_imessage_reply_flat_send_fn flat_send) {
    g_test_tier1 = tier1;
    g_test_tier2 = tier2;
    g_test_flat_send = flat_send;
}

const char *hu_imessage_test_last_reply_tier(void) {
    return g_last_tier;
}

#if defined(__APPLE__) && defined(HU_IMESSAGE_TAPBACK_ENABLED)
/* Real native impl. Mirrors ax_perform_tapback_on_row pattern in
 * src/channels/imessage.c. Real impl details:
 *
 * 1. ax_open_conversation(target, target_len)  // existing helper
 * 2. ax_find_message_group(window, parent_text_prefix, 0)  // existing
 *    — needs parent_text_prefix: a guid-to-text-prefix lookup OR pass
 *      the body's first 20 chars as a heuristic prefix
 * 3. AXUIElementPerformAction(msg_group, kAXRaiseAction)  // focus
 * 4. CGEventCreateKeyboardEvent for Cmd+R press + release
 * 5. Poll for AX text-field appearing under the parent row
 * 6. CGEventKeyboardSetUnicodeString for the body text
 * 7. Return key synth
 *
 * IMPORTANT for C1: stub this with `return false;` for now — the real
 * AX wiring requires testing on a live macOS box which is out-of-scope
 * for the unit test budget. Real impl lands in a follow-up integration
 * pass after C5 (when the full path is wired end-to-end).
 *
 * This is INTENTIONAL — Tier 1 falls through to Tier 2/3 until real
 * macOS testing happens. The contract (hu_imessage_reply returns proper
 * tier escalation results) is testable today via the stub mechanism. */
static bool ax_reply_tier1_cmd_r(const char *target, size_t target_len, const char *parent_guid,
                                 size_t parent_guid_len, const char *body, size_t body_len) {
    (void)target;
    (void)target_len;
    (void)parent_guid;
    (void)parent_guid_len;
    (void)body;
    (void)body_len;
    return false; /* Real CGEvent + AX wiring deferred to integration pass */
}

/* Real impl mirrors ax_perform_tapback_on_row in src/channels/imessage.c:
 *
 * 1. ax_open_conversation(target, target_len)
 * 2. ax_find_message_group(window, parent_text_prefix, 0)
 * 3. AXUIElementPerformAction(msg_group, kAXShowMenuAction)
 * 4. Iterate context menu items; match title.startswith("Reply")
 *    — handles "Reply…" (U+2026 ellipsis), "Reply..." (3 dots), localized
 * 5. AXUIElementPerformAction(menu_item, kAXPressAction)
 * 6. Poll for the inline composer (AX text field appearing under parent)
 * 7. CGEventKeyboardSetUnicodeString for body
 * 8. Return key synth
 *
 * Like Tier 1, stubbed to return false in C2 — real AX wiring is part of
 * the post-C5 integration pass on a live macOS box. The test-stub contract
 * is what's exercised in CI. */
static bool ax_reply_tier2_show_menu(const char *target, size_t target_len, const char *parent_guid,
                                     size_t parent_guid_len, const char *body, size_t body_len) {
    (void)target;
    (void)target_len;
    (void)parent_guid;
    (void)parent_guid_len;
    (void)body;
    (void)body_len;
    return false;
}
#endif

hu_error_t hu_imessage_reply(void *ctx, const char *target, size_t target_len,
                             const char *parent_msg_guid, size_t parent_msg_guid_len,
                             const char *body, size_t body_len) {
    (void)ctx;
    g_last_tier[0] = '\0';

    if (!target || !body || target_len == 0 || body_len == 0) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Tier 1: Cmd-R via AX. */
    bool t1_ok = false;
    if (g_test_tier1) {
        t1_ok = g_test_tier1(parent_msg_guid, parent_msg_guid_len, body, body_len);
    } else {
#if defined(__APPLE__) && defined(HU_IMESSAGE_TAPBACK_ENABLED)
        t1_ok = ax_reply_tier1_cmd_r(target, target_len, parent_msg_guid, parent_msg_guid_len, body,
                                     body_len);
#endif
    }
    if (t1_ok) {
        snprintf(g_last_tier, sizeof(g_last_tier), "cmdR");
        return HU_OK;
    }

    /* Tier 2: AXShowMenu → click "Reply…" menu item. */
    bool t2_ok = false;
    if (g_test_tier2) {
        t2_ok = g_test_tier2(parent_msg_guid, parent_msg_guid_len, body, body_len);
    } else {
#if defined(__APPLE__) && defined(HU_IMESSAGE_TAPBACK_ENABLED)
        t2_ok = ax_reply_tier2_show_menu(target, target_len, parent_msg_guid, parent_msg_guid_len,
                                         body, body_len);
#endif
    }
    if (t2_ok) {
        snprintf(g_last_tier, sizeof(g_last_tier), "ax_menu");
        return HU_OK;
    }

    /* Tier 3: flat-send fallback. Log WARN explaining the degradation
     * (per silent-config-gated-subsystems.md visibility discipline). */
    hu_log_warn("imessage", NULL,
                "threaded reply degraded to flat-send (parent_guid=%.*s reason=ax_unavailable)",
                (int)(parent_msg_guid_len > 40 ? 40 : parent_msg_guid_len),
                parent_msg_guid ? parent_msg_guid : "(null)");

    hu_error_t err = HU_ERR_NOT_SUPPORTED;
    if (g_test_flat_send) {
        err = g_test_flat_send(target, target_len, body, body_len);
    } else {
        /* In production, when no test stub is set, return NOT_SUPPORTED.
         * The dispatcher in F2 (daemon-level) will see NOT_SUPPORTED and
         * fall through to its own vtable->send call. This is cleaner
         * separation: this function tries the threaded path; the dispatcher
         * decides what to do on failure. */
        err = HU_ERR_NOT_SUPPORTED;
    }
    snprintf(g_last_tier, sizeof(g_last_tier), "flat_fallback");
    return err;
}
