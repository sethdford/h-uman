#ifndef HUMAN_CHANNELS_IMESSAGE_REPLY_H
#define HUMAN_CHANNELS_IMESSAGE_REPLY_H

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* Send `body` as a threaded inline reply to the inbound message with guid
 * `parent_msg_guid` on chat `target`. Tries Cmd-R via AX first; later
 * phases add AXShowMenu fallback (C2) and flat-send fallback (C3).
 *
 * Returns HU_OK on any tier succeeding; HU_ERR_NOT_SUPPORTED if no tier
 * worked (in C1 alone, "no tier worked" = Tier 1 failed).
 *
 * On non-macOS or when iMessage AX is disabled at compile time, returns
 * HU_ERR_NOT_SUPPORTED immediately. */
hu_error_t hu_imessage_reply(void *ctx, const char *target, size_t target_len,
                             const char *parent_msg_guid, size_t parent_msg_guid_len,
                             const char *body, size_t body_len);

/* Test-only stub interface — mirrors g_imessage_test_send_stub pattern
 * from src/channels/imessage.c. Replaces real AX calls so unit tests
 * can deterministically exercise the tier-escalation logic without
 * Messages.app present.
 *
 * The stub functions are called in tier order: tier1 first; if it
 * returns false, tier2 (when wired in C2); if false, tier3 (C3).
 * Pass NULL to clear. */
typedef bool (*hu_imessage_reply_tier_fn)(const char *parent_msg_guid, size_t parent_msg_guid_len,
                                          const char *body, size_t body_len);

/* Test-only — replaces the production flat-send fallback (Tier 3).
 * In production, Tier 3 calls the existing iMessage send path. */
typedef hu_error_t (*hu_imessage_reply_flat_send_fn)(const char *target, size_t target_len,
                                                     const char *body, size_t body_len);

/* Updated signature — takes THREE stubs now (tier1, tier2, flat-send). */
void hu_imessage_set_test_reply_stubs(hu_imessage_reply_tier_fn tier1,
                                      hu_imessage_reply_tier_fn tier2,
                                      hu_imessage_reply_flat_send_fn flat_send);

/* Test-only — returns the tier that was used by the most recent call,
 * one of: "cmdR" | "ax_menu" | "flat_fallback" | "" (none). */
const char *hu_imessage_test_last_reply_tier(void);

#if HU_IS_TEST
/* Test-only — reset / read the one-shot Tier-3 degradation WARN counter.
 * The count is 0 before any flat-fallback WARN fires and 1 afterward,
 * regardless of how many times the fallback is taken. */
void hu_imessage_test_reset_reply_warn(void);
int hu_imessage_test_reply_warn_count(void);
#endif

/* Production AX threaded-reply workers (implemented in imessage.c, where the
 * static AX helpers + chat.db access live). Only present in non-test macOS
 * builds with TAPBACK wiring compiled in; the test build stays on the stub
 * path so these are neither declared nor linked there.
 *   Tier 1: Cmd-R on the focused parent row → inline composer → type → send.
 *   Tier 2: AXShowMenu → click "Reply" → inline composer → type → send.
 * Each returns true on success; false to fall through to the next tier. */
#if defined(__APPLE__) && defined(HU_IMESSAGE_TAPBACK_ENABLED) && !HU_IS_TEST
bool hu_imessage_ax_reply_tier1_cmd_r(const char *target, size_t target_len,
                                      const char *parent_guid, size_t parent_guid_len,
                                      const char *body, size_t body_len);
bool hu_imessage_ax_reply_tier2_show_menu(const char *target, size_t target_len,
                                          const char *parent_guid, size_t parent_guid_len,
                                          const char *body, size_t body_len);
#endif

#endif
