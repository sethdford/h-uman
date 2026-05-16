#ifndef HU_DAEMON_REACTION_POLL_H
#define HU_DAEMON_REACTION_POLL_H

#include "human/config.h"
#include "human/core/error.h"

#ifdef HU_IS_TEST
hu_error_t hu_daemon_tick_for_test(const hu_config_t *cfg);
void hu_daemon_set_poll_call_counter_for_test(int *counter);
#endif

#endif /* HU_DAEMON_REACTION_POLL_H */
