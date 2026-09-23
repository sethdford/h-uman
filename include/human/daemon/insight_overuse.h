#ifndef HUMAN_DAEMON_INSIGHT_OVERUSE_H
#define HUMAN_DAEMON_INSIGHT_OVERUSE_H

/* Insight overuse — the PAS-style counter-metric for the specificity push.
 *
 * Controlling and Assessing Appropriate Persona Use (arXiv 2609.04676, EMNLP
 * 2026) finds models surface persona attributes regardless of whether the
 * turn calls for them. HU_INSIGHT_STREAM went live 2026-09-06 to move the
 * specificity axis; without a counter-metric "generic" can be traded for
 * "name-dropping" and nothing would notice. This module measures, per reply:
 *
 *   injected  content tokens (>=4 chars, no digits, not a stop word) in the
 *             insight block the loader put in THIS turn's prompt
 *   surfaced  those tokens that appear (whole word, case-folded) in the reply
 *   prompted  surfaced tokens that also appear in the contact's inbound text
 *
 * unprompted = surfaced - prompted is the overuse signal: memory the model
 * brought up on its own. Whether that is too much is a comparison against
 * Seth's own unprompted rate (scripts/insight_overuse_report.py), not a fixed
 * threshold.
 *
 * Gate: HU_INSIGHT_OVERUSE off|shadow|live (hu_gate_mode_from_env, default
 * OFF). SHADOW and LIVE both only log one line per reply — this subsystem
 * changes nothing that is sent. LIVE is reserved for the closed loop
 * (adapting HU_INSIGHT_MAX_ITEMS) which is gated on the comparison above. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include "human/memory.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_insight_overuse {
    size_t injected;
    size_t surfaced;
    size_t prompted;
} hu_insight_overuse_t;

/* HU_INSIGHT_OVERUSE, parsed per gate_mode.h; unset -> OFF. */
hu_gate_mode_t hu_insight_overuse_mode(void);

/* Pure count over the three texts. insights may be NULL/empty (-> all zero).
 * Tokens are runs of [A-Za-z0-9'] of length >= 4; all-digit runs and a small
 * stop list are skipped; a token is counted once however often it appears
 * in the block. Match is whole-word, case-insensitive. */
hu_error_t hu_insight_overuse_count(const char *insights, size_t insights_len, const char *inbound,
                                    size_t inbound_len, const char *reply, size_t reply_len,
                                    hu_insight_overuse_t *out);

/* Re-render the contact's insight block exactly as memory_loader.c injected
 * it this turn (same repo query, same budget) and log one line. OFF -> no
 * work, HU_OK. If the insight stream itself is not LIVE nothing was injected:
 * zeros, no log. out is optional. Never changes what is sent. */
hu_error_t hu_daemon_insight_overuse_scan(hu_memory_t *memory, hu_allocator_t *alloc,
                                          const char *contact_id, size_t contact_id_len,
                                          const char *inbound, size_t inbound_len,
                                          const char *reply, size_t reply_len, hu_gate_mode_t mode,
                                          void *observer, hu_insight_overuse_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HUMAN_DAEMON_INSIGHT_OVERUSE_H */
