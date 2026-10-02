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

#endif /* HU_DAEMON_CRISIS_H */
