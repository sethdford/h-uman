#ifndef HU_DAEMON_REACTIVE_CALIBRATION_H
#define HU_DAEMON_REACTIVE_CALIBRATION_H

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
 * the neutral ratio. On allocation failure, or when there is nothing to say,
 * *convo_ctx is left as it was. *convo_ctx is allocated on `alloc` with
 * length+1 bytes, the daemon's convention. */
void hu_daemon_append_length_calibration(hu_allocator_t *alloc, struct hu_agent *agent,
                                         const char *key, size_t key_len, const char *combined,
                                         size_t combined_len, bool is_group, char **convo_ctx,
                                         size_t *convo_ctx_len);

/* Reactive reply budget (daemon.c step 4), shared with the replay harness:
 * the channel's response constraint, lowered to the F15 ratio (relational for
 * a 1:1 thread, neutral for a group), then to the brief cap when `brief_mode`.
 * 0 = unlimited. This is the seam reply-length policy hooks (HU_LENGTH_POLICY,
 * PR #580): a length policy belongs here, not inline in daemon.c, so the
 * replay harness measures it. */
uint32_t hu_daemon_reply_budget(const struct hu_agent *agent, struct hu_channel *ch,
                                const char *key, size_t key_len, size_t combined_len, bool is_group,
                                bool brief_mode);

#endif /* HU_DAEMON_REACTIVE_CALIBRATION_H */
