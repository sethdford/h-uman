#include "human/channels/imessage_reply.h"
#include "human/channels/imessage_action.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/time.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

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

/* One-shot guard for the Tier-3 degradation WARN: a busy reply path that
 * keeps falling through to flat-send should log the "AX unavailable"
 * explanation exactly once per process, not on every message. Increments
 * only on the single emitted line so the test can assert "exactly one WARN
 * across N failures". */
static int g_flat_fallback_warn_emitted = 0;

#if HU_IS_TEST
void hu_imessage_test_reset_reply_warn(void) {
    g_flat_fallback_warn_emitted = 0;
}
int hu_imessage_test_reply_warn_count(void) {
    return g_flat_fallback_warn_emitted;
}
#endif

/* Emit one telemetry line for the completed reply. Best-effort:
 * telemetry failure never blocks the reply. */
static void emit_telemetry(const char *tier_used, int send_result, int64_t elapsed_ms,
                           const char *target, size_t target_len) {
    hu_imessage_action_log_t log = {0};
    log.ts_unix = (int64_t)time(NULL);
    /* For C4, target is used as a rough chat-id-hash. F2's dispatcher
     * will replace this with a real hash. Keep it bounded. */
    char chat_buf[33] = {0};
    size_t n = target_len < 32 ? target_len : 32;
    memcpy(chat_buf, target, n);
    log.target_chat_id_hash = chat_buf;
    /* Facts are not yet built at this layer — F2 will pass them in.
     * For C4 emit with zero-valued facts; the line still has the
     * tier_used + elapsed_ms which is the value-add of this hop. */
    log.style_chosen = HU_REPLY_STYLE_THREADED; /* always THREADED at this entry */
    log.send_result = send_result;
    log.tier_used = tier_used;
    log.elapsed_ms = (int)elapsed_ms;
    /* Best-effort: ignore return; logging is non-fatal. */
    (void)hu_imessage_action_log_jsonl(&log);
}

/* The real Tier 1 / Tier 2 AX workers live in src/channels/imessage.c
 * (hu_imessage_ax_reply_tier1_cmd_r / hu_imessage_ax_reply_tier2_show_menu),
 * alongside the static AX helpers and chat.db access they depend on. They
 * are declared in imessage_reply.h under the same
 * (__APPLE__ && HU_IMESSAGE_TAPBACK_ENABLED && !HU_IS_TEST) guard. In test
 * builds those symbols don't exist, so the tier-escalation logic below is
 * exercised purely through the g_test_tier1 / g_test_tier2 stubs. */

hu_error_t hu_imessage_reply(void *ctx, const char *target, size_t target_len,
                             const char *parent_msg_guid, size_t parent_msg_guid_len,
                             const char *body, size_t body_len) {
    (void)ctx;
    g_last_tier[0] = '\0';

    if (!target || !body || target_len == 0 || body_len == 0) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Record start time for elapsed_ms. */
    int64_t ts_start_ms = hu_time_get_current_ms();

    /* Tier 1: Cmd-R via AX. */
    bool t1_ok = false;
    if (g_test_tier1) {
        t1_ok = g_test_tier1(parent_msg_guid, parent_msg_guid_len, body, body_len);
    } else {
#if defined(__APPLE__) && defined(HU_IMESSAGE_TAPBACK_ENABLED) && !HU_IS_TEST
        t1_ok = hu_imessage_ax_reply_tier1_cmd_r(target, target_len, parent_msg_guid,
                                                 parent_msg_guid_len, body, body_len);
#endif
    }
    if (t1_ok) {
        snprintf(g_last_tier, sizeof(g_last_tier), "cmdR");
        int64_t ts_end_ms = hu_time_get_current_ms();
        emit_telemetry("cmdR", 0, ts_end_ms - ts_start_ms, target, target_len);
        return HU_OK;
    }

    /* Tier 2: AXShowMenu → click "Reply…" menu item. */
    bool t2_ok = false;
    if (g_test_tier2) {
        t2_ok = g_test_tier2(parent_msg_guid, parent_msg_guid_len, body, body_len);
    } else {
#if defined(__APPLE__) && defined(HU_IMESSAGE_TAPBACK_ENABLED) && !HU_IS_TEST
        t2_ok = hu_imessage_ax_reply_tier2_show_menu(target, target_len, parent_msg_guid,
                                                     parent_msg_guid_len, body, body_len);
#endif
    }
    if (t2_ok) {
        snprintf(g_last_tier, sizeof(g_last_tier), "ax_menu");
        int64_t ts_end_ms = hu_time_get_current_ms();
        emit_telemetry("ax_menu", 0, ts_end_ms - ts_start_ms, target, target_len);
        return HU_OK;
    }

    /* Tier 3: flat-send fallback. Log WARN explaining the degradation
     * (per silent-config-gated-subsystems.md visibility discipline), but
     * only once per process to avoid spamming the log on every message. */
    if (g_flat_fallback_warn_emitted == 0) {
        hu_log_warn("imessage", NULL,
                    "threaded reply degraded to flat-send (parent_guid=%.*s reason=ax_unavailable)",
                    (int)(parent_msg_guid_len > 40 ? 40 : parent_msg_guid_len),
                    parent_msg_guid ? parent_msg_guid : "(null)");
        g_flat_fallback_warn_emitted = 1;
    }

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
    int64_t ts_end_ms = hu_time_get_current_ms();
    emit_telemetry("flat_fallback", (int)err, ts_end_ms - ts_start_ms, target, target_len);
    return err;
}
