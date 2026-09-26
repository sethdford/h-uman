#ifndef HU_DAEMON_HURT_HANDOFF_H
#define HU_DAEMON_HURT_HANDOFF_H

/*
 * Hurt-signal hand-off: when a 1:1 inbound message says the contact feels
 * hurt or worried ABOUT THE OWNER ("u mad at me?", "why are you being
 * short", "are we ok"), h-uman does not auto-reply. The owner is notified so
 * the reply comes from him.
 *
 * Why (2026-09-26): after 13 one-word auto-replies, a contact asked "U mad at
 * me?", "Why u being short?", "See this is why i get scared w u...". Every
 * one of those got another auto-reply ("Nah", "I'm sorry", "Hello!"). A
 * relationship-repair moment is the one reply that must be the real person.
 *
 * Detection favours recall: a false positive costs one hand-off (the owner
 * replies himself); a false negative sends a machine reply to someone who is
 * hurt. Phrases must address the owner in the second person, so "my boss is
 * mad at me" does not fire.
 *
 * Gated HU_HURT_HANDOFF (off|shadow|live, default off) per
 * .claude/rules/feature-gate-requires-measurement.md.
 */

#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when `text` reads as hurt or worry directed at the owner. Pure. */
bool hu_hurt_signal_detect(const char *text, size_t len);

/* Gate mode from HU_HURT_HANDOFF; default OFF. */
hu_gate_mode_t hu_hurt_handoff_mode(void);

/* Apply the gate to one inbound 1:1 batch:
 *   OFF    — no detection; returns false.
 *   SHADOW — detects and logs the message length only (never text or
 *            contact); returns false, so the reply proceeds unchanged.
 *   LIVE   — on a hit, logs, notifies the owner naming `contact_name`
 *            (never the message text) and returns true: the caller must
 *            not auto-reply to this batch.
 * `contact_name` may be NULL. */
bool hu_hurt_handoff_apply(hu_gate_mode_t mode, const char *text, size_t len,
                           const char *contact_name);

#ifdef HU_IS_TEST
/* Owner notifications requested since the last reset, and the last name. */
unsigned hu_hurt_handoff_test_notify_count(void);
const char *hu_hurt_handoff_test_last_name(void);
void hu_hurt_handoff_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_HURT_HANDOFF_H */
