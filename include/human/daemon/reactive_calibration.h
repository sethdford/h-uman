#ifndef HU_DAEMON_REACTIVE_CALIBRATION_H
#define HU_DAEMON_REACTIVE_CALIBRATION_H

#include "human/agent/length_policy.h"
#include "human/core/allocator.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_agent;
struct hu_channel;

/* Reactive reply length calibration (daemon.c step 2c), shared by the daemon
 * and the real-turn replay harness so both build the same conversation
 * context.
 *
 * Classifies `combined` (the batched inbound text) and appends the
 * calibration directive to *convo_ctx — or makes it the whole context when
 * *convo_ctx is NULL — joined by a blank line. Uses the persona contact
 * profile for `key` and the agent's relationship stage; `is_group` selects
 * the neutral ratio. `turn_cap` > 0 (a tight HU_LENGTH_POLICY cap from
 * hu_daemon_reply_budget) caps the directive's target; 0 = today's ratio.
 * On allocation failure, or when there is nothing to say,
 * *convo_ctx is left as it was. *convo_ctx is allocated on `alloc` with
 * length+1 bytes, the daemon's convention. */
void hu_daemon_append_length_calibration(hu_allocator_t *alloc, struct hu_agent *agent,
                                         const char *key, size_t key_len, const char *combined,
                                         size_t combined_len, bool is_group, uint32_t turn_cap,
                                         char **convo_ctx, size_t *convo_ctx_len);

/* Reactive reply budget (daemon.c step 4) — the one seam the daemon and the
 * replay harness share for reply length: the channel's response constraint,
 * then hu_length_policy_turn (F15 ratio, brief cap, and HU_LENGTH_POLICY per
 * hu_length_policy_mode()). `out->cap` is the turn's max_chars; pass
 * `out->tight ? out->cap : 0` to hu_daemon_append_length_calibration and
 * `out->tight` to agent->response_limit_tight. A NULL agent yields a zeroed
 * result. */
void hu_daemon_reply_budget(const struct hu_agent *agent, struct hu_channel *ch, const char *key,
                            size_t key_len, const char *combined, size_t combined_len,
                            bool is_group, bool brief_mode, hu_length_turn_result_t *out);

#endif /* HU_DAEMON_REACTIVE_CALIBRATION_H */
