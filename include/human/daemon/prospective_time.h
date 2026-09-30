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

#endif /* HU_DAEMON_PROSPECTIVE_TIME_H */
