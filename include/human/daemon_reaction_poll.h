#ifndef HU_DAEMON_REACTION_POLL_H
#define HU_DAEMON_REACTION_POLL_H

#include "human/config.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

/* Forward decl avoids pulling human/agent/reaction_handler.h into
 * callers (it transitively includes human/channels/reaction_event.h
 * which defines a hu_reaction_kind_t enum that collides with the
 * legacy hu_reaction_type_t in human/channel.h on the
 * HU_REACTION_QUESTION / HU_REACTION_CUSTOM_EMOJI tokens). Callers
 * that need to install the collector should use the wrapper below. */
struct hu_dpo_collector;

#ifdef __cplusplus
extern "C" {
#endif

void hu_daemon_reaction_wire_collector(struct hu_dpo_collector *collector);

/* Poll since_unix; feeds events into the reaction handler. */
hu_error_t hu_daemon_reaction_poll_tick(const hu_config_t *cfg,
                                        int64_t since_unix,
                                        size_t *out_ingested);

/* Interval-gated tick used by the daemon main loop (CF-3). */
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
