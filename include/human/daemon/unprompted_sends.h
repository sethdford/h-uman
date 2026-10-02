#ifndef HU_DAEMON_UNPROMPTED_SENDS_H
#define HU_DAEMON_UNPROMPTED_SENDS_H
/*
 * The non-proposer unprompted send paths, carved out of src/daemon.c
 * (file-size ratchet) and routed through the one gate stack
 * (human/daemon/unprompted_gate.h) — DEF-14, 2026-10-02:
 *   - F25 emotional check-in (templated, due 1–3 days after a heavy moment)
 *   - proactive photo share (Apple Photos album)
 *   - scheduled-queue delivery, where the read-no-reply bump lands
 */
#include "human/agent.h"
#include "human/channel.h"
#include "human/daemon.h"
#include "human/daemon_proactive.h"
#include "human/persona.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* F25: for each due emotional moment whose contact has a proactive channel,
 * send the templated check-in if the gate stack allows it. A denied moment
 * stays due (retried next pass), exactly like the old send-cap skip. */
void hu_daemon_f25_checkins_tick(hu_allocator_t *alloc, hu_agent_t *agent,
                                 hu_service_channel_t *channels, size_t channel_count,
                                 hu_proactive_context_t *pctx, int64_t now);

/* Photo share: gate, then send `media` (no text). Returns true only when the
 * channel accepted it; only then is recency/ledger charged. */
bool hu_daemon_photo_share_send(hu_allocator_t *alloc, hu_agent_t *agent, hu_channel_t *channel,
                                const hu_contact_profile_t *cp, const char *ch_name,
                                const char *target, size_t target_len, const char *const *media,
                                size_t media_count, int64_t now);

/* Hourly Apple Photos scan for `cp` (combined = their recent text). Real
 * Photos access, so compiled out under HU_IS_TEST. */
void hu_daemon_photo_share_tick(hu_allocator_t *alloc, hu_agent_t *agent, hu_channel_t *channel,
                                const hu_contact_profile_t *cp, const char *target,
                                size_t target_len, const char *combined, size_t combined_len,
                                int64_t now);

/* Deliver one entry popped from the scheduled queue. `kind` is the entry's
 * hu_unprompted_kind_t: 0 (owner-scheduled) keeps the historical pipeline
 * byte-for-byte; non-zero (the read-no-reply bump) runs the full gate stack
 * at send time instead of the bare sanitizer, and records the ledger row on
 * delivery. Returns true when the send was attempted (caller persists). */
bool hu_daemon_sched_deliver(hu_allocator_t *alloc, hu_agent_t *agent, hu_channel_t *channel,
                             const char *ch_name, const char *contact, char *msg, size_t msg_len,
                             size_t msg_cap, uint8_t kind, int64_t now);

#endif /* HU_DAEMON_UNPROMPTED_SENDS_H */
