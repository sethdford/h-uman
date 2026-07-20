/* Promise-keeper (blueprint mechanism #2, outbound half).
 *
 * The inbound F20 hook already stores THEIR commitments; this module scans
 * the final outbound reply for OUR commitments ("i'll send it tomorrow"),
 * stores them (who="me") and schedules the delayed follow-up, so the
 * commitments table becomes a two-sided kept/broken ledger.
 *
 * Gated on HU_PROMISE_KEEPER (off|shadow|on, default OFF): stored promises
 * feed the proposer's due_followups context and can change future sends, so
 * promotion to "on" is gated on the scoped proactivity trial observation
 * per feature-gate-requires-measurement: the filtered SHADOW stream must
 * show >=80% genuine-commitment precision over a week before flipping live. */
#ifndef HUMAN_DAEMON_PROMISE_KEEPER_H
#define HUMAN_DAEMON_PROMISE_KEEPER_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hu_promise_keeper_mode {
    HU_PROMISE_KEEPER_OFF = 0,
    HU_PROMISE_KEEPER_SHADOW,
    HU_PROMISE_KEEPER_LIVE,
} hu_promise_keeper_mode_t;

/* Pure mode parse ("on" -> LIVE, "shadow" -> SHADOW, anything else / NULL -> OFF). */
hu_promise_keeper_mode_t hu_promise_keeper_mode_from_env(const char *env_value);

/* Pure predicate: true iff the detected commitment description is a bare
 * courtesy invitation — it starts with the word-bounded phrase "let me know",
 * no deadline parsed from the reply (deadline_ts <= 0), and no first-person
 * deliverable marker (i'll / i will / gonna) after the prefix. Shadow evidence
 * 2026-07-19: 5 of 8 distinct captured promises were this shape. */
bool hu_promise_keeper_is_courtesy_invitation(const char *desc, size_t desc_len,
                                              int64_t deadline_ts);

/* Scan one outbound reply. SHADOW logs what it would store; LIVE stores the
 * commitment and (when a deadline parses) schedules the follow-up. OFF is a
 * no-op. stored_out (optional) reports whether a commitment row was written.
 * memory is the hu_memory_t* used by the superhuman commitment API. */
hu_error_t hu_daemon_promise_keeper_scan_outbound(void *memory, hu_allocator_t *alloc,
                                                  const char *contact_id, size_t contact_id_len,
                                                  const char *reply, size_t reply_len,
                                                  hu_promise_keeper_mode_t mode, void *observer,
                                                  bool *stored_out);

#ifdef __cplusplus
}
#endif

#endif /* HUMAN_DAEMON_PROMISE_KEEPER_H */
