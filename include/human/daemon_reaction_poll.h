#ifndef HU_DAEMON_REACTION_POLL_H
#define HU_DAEMON_REACTION_POLL_H

#include "human/config.h"
#include "human/core/error.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Production tick: interval check uses cfg->poll_interval_seconds and
 * last_poll_unix_inout. Polls iMessage tapbacks when enabled; feeds events
 * to hu_reaction_handler_handle_event. now_unix is caller-supplied (tests
 * inject a fake clock). Advances *watermark_inout on each poll attempt. */
hu_error_t hu_daemon_tick_reaction_poll(const hu_reaction_collection_config_t *cfg,
                                        int64_t now_unix, int64_t *last_poll_unix_inout,
                                        int64_t *watermark_inout);

#ifdef HU_IS_TEST
hu_error_t hu_daemon_tick_for_test(const hu_config_t *cfg);
void hu_daemon_set_poll_call_counter_for_test(int *counter);
int hu_daemon_reaction_poll_get_count_for_test(void);
void hu_daemon_reaction_poll_reset_count_for_test(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_REACTION_POLL_H */
