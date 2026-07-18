#ifndef HU_DAEMON_LEARNING_TICK_H
#define HU_DAEMON_LEARNING_TICK_H

#include "human/core/error.h"
#include "human/memory.h"
#include <stddef.h>
#include <stdint.h>

struct hu_agent;
struct hu_allocator;
struct hu_contextual_bandit;

#ifdef __cplusplus
extern "C" {
#endif

/* Proactive-outcome pipeline (US-104 wiring, 2026-07-18 roadmap).
 *
 * The humanization bandit reads per-contact Beta(α,β) arms on the reply
 * path (hu_humanization_apply_bandit_override); these three functions are
 * the trainer side that makes those arms actually move:
 *
 *   record_send  — daemon proactive dispatch path, after a successful send
 *   mark_reply   — reactive path, on inbound from a contact (REPLY outcome)
 *   tick         — periodic: sweep >24h silences to IGNORED, then feed
 *                  resolved outcomes into the bandit + persist the arms
 *
 * `contact` must be the channel send target / session_key — the SAME
 * string the bandit read path hashes (hu_contact_handle_hash of the
 * inbound batch_key) so trainer and reader address one arm.
 *
 * All are best-effort: they no-op with HU_OK when SQLite is compiled out
 * or memory/db is absent, and never block the send path on failure. */
hu_error_t hu_daemon_proactive_outcome_record_send(hu_memory_t *memory, const char *channel,
                                                   const char *contact, size_t contact_len);

hu_error_t hu_daemon_proactive_outcome_mark_reply(hu_memory_t *memory, const char *channel,
                                                  const char *contact, size_t contact_len);

/* Rate-limited internally to once per 60s; pass the loop's current time. */
hu_error_t hu_daemon_proactive_outcome_tick(hu_memory_t *memory,
                                            struct hu_contextual_bandit *bandit, int64_t now);

/* Carved from daemon.c (2026-07-18, file-size ceiling): DPO consolidation
 * judge step on a 24h cadence. First call arms the timer without running
 * (judge_step blocks ~5 min on LLM scoring; see BUGFIX 2026-05-25). */
void hu_daemon_dpo_judge_tick(struct hu_agent *agent, struct hu_allocator *alloc, int64_t t);

#ifdef HU_IS_TEST
/* Re-arm the tick cadence + one-shot log guards between test cases. */
void hu_daemon_learning_tick_reset_for_test(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_LEARNING_TICK_H */
