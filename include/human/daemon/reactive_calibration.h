#ifndef HU_DAEMON_REACTIVE_CALIBRATION_H
#define HU_DAEMON_REACTIVE_CALIBRATION_H

#include "human/core/allocator.h"
#include <stdbool.h>
#include <stddef.h>

struct hu_agent;

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

#endif /* HU_DAEMON_REACTIVE_CALIBRATION_H */
