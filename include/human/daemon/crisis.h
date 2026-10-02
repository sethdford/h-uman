#ifndef HU_DAEMON_CRISIS_H
#define HU_DAEMON_CRISIS_H

/* SHIELD-005 inbound crisis handling for the reactive reply path, carved out of
 * hu_service_run (daemon.c is at its file-size ceiling). The tier comes from
 * the one self-harm detector (human/security/self_harm.h). */

#include "human/core/allocator.h"
#include "human/observer.h"
#include "human/security/self_harm.h"

#include <stdbool.h>
#include <stddef.h>

/* Classify one inbound batch. Logs one line when the tier is not NONE: the
 * tier, the score and a contact tag, never the text or the handle.
 * HU_CRISIS_TIERS off/shadow: any legacy self_harm hit is EXPLICIT (the legacy
 * crisis directive, byte-identical); shadow also logs the canonical tier.
 * live: the canonical tier. */
hu_self_harm_tier_t hu_daemon_inbound_crisis_tier(hu_allocator_t *alloc, const char *text,
                                                  size_t text_len, const char *who, size_t who_len,
                                                  hu_observer_t *obs);

/* Prepend the tier's directive to the reply's conversation context (*ctx is
 * owned, allocated len+1). NONE is a no-op. Returns true when the reply must be
 * forced (every tier but NONE: never a tapback or silence on distress). */
bool hu_daemon_crisis_prepend(hu_allocator_t *alloc, hu_self_harm_tier_t tier, char **ctx,
                              size_t *ctx_len);

/* Append the 988 resource line to an owned reply (*reply allocated len+1)
 * when the inbound tier is EXPLICIT and the reply lacks "988". Keyed to the
 * INBOUND tier only. Returns true when it appended. */
bool hu_daemon_crisis_ensure_resources(hu_allocator_t *alloc, hu_self_harm_tier_t inbound_tier,
                                       char **reply, size_t *reply_len);

/* True when the reply must not be sent: moderation flags violence, hate or
 * sexual content. Self-harm wording alone never blocks (it is usually the
 * model offering help), and nothing is substituted: a blocked reply is
 * dropped, never replaced with canned text. Logs the categories, not text. */
bool hu_daemon_reply_blocked(hu_allocator_t *alloc, const char *reply, size_t reply_len,
                             hu_observer_t *obs);

/* The last screen before the fallback send. A reply blocked by
 * hu_daemon_reply_blocked frees *owned (allocated *len + 1, may be NULL) and,
 * on an EXPLICIT turn, points *text at the crisis floor (caring line + 988)
 * and returns true — a crisis turn never ends in silence. On any other turn a
 * blocked reply returns false: send nothing. An unblocked reply is untouched. */
bool hu_daemon_crisis_screen(hu_allocator_t *alloc, hu_self_harm_tier_t inbound_tier,
                             const char **text, size_t *len, char **owned, hu_observer_t *obs);

#endif /* HU_DAEMON_CRISIS_H */
