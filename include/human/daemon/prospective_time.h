#ifndef HU_DAEMON_PROSPECTIVE_TIME_H
#define HU_DAEMON_PROSPECTIVE_TIME_H
/* Time-cued follow-ups for the proactive tick (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.2,
 * §4.4). The two legacy producers below were moved out of hu_service_run
 * (src/daemon.c) unchanged; HU_PROSPECTIVE_TIME chooses between them and the
 * v2 per-contact due set. */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

struct hu_agent;
struct hu_channel;

/* F20: this contact's due commitments as "COMMITMENT FOLLOW-UP: …" lines.
 * *ctx_out is heap (free with *ctx_len_out + 1) or NULL; ids_out[3] receives
 * the listed commitment ids, marked followed-up by the caller on delivery. */
void hu_daemon_prospective_commitment_ctx(hu_allocator_t *alloc, struct hu_agent *agent,
                                          const char *contact_id, int64_t now, char **ctx_out,
                                          size_t *ctx_len_out, int64_t ids_out[3],
                                          size_t *ids_count_out);

/* The proposer's due_followups section for this contact, written to buf[cap].
 * Returns the bytes written (0 = nothing to list; buf is then ""). *listed_id
 * receives the delayed_followups id the caller marks sent on delivery, and is
 * left unchanged when nothing is listed. `ch`/`target` are the send channel
 * and handle (history for the v2 fire-time check). */
size_t hu_daemon_prospective_due_followups(hu_allocator_t *alloc, struct hu_agent *agent,
                                           struct hu_channel *ch, const char *target,
                                           size_t target_len, const char *contact_id, int64_t now,
                                           char *buf, size_t cap, int64_t *listed_id);

/* HU_PROSPECTIVE_TIME=off|shadow|live (default off) selects what the two
 * producers above return:
 *   off    — the legacy producers, unchanged;
 *   shadow — the legacy producers, plus one read-only v2 time pass per contact
 *            per local day, logged as "prospective time shadow: …" (counts,
 *            then one "item: id=… verdict=…" line per judged intention, so a
 *            report can dedupe per intention across passes);
 *   live   — commitment_ctx returns nothing (commitments are mirrored into the
 *            typed store) and due_followups returns the v2 per-contact due set
 *            ("- <action>\n", at most one per contact per day, a stale
 *            relative day dropped from the line), leaving *listed_id
 *            untouched so the legacy mark-sent does not fire.
 * The first call logs one banner line for the gate state (OFF included).
 * HU_PROSPECTIVE_TIME stays in shadow until HU_PROSPECTIVE has passed its
 * promotion (spec §4.4) and then needs its own 7-day SHADOW read (§6 step 5). */

/* After a proactive message to `contact_id` was delivered (live only; off and
 * shadow do nothing): settle THIS contact's surfaced time intention against
 * the sent text — done when it carries the action (its commitment /
 * follow-up ledger rows retire with it), otherwise an attempt. Another
 * contact's rows are never read or settled. */
void hu_daemon_prospective_time_after_send(struct hu_agent *agent, const char *contact_id,
                                           const char *text, size_t text_len, int64_t now);

#endif /* HU_DAEMON_PROSPECTIVE_TIME_H */
