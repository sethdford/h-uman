#ifndef HU_DAEMON_REACTION_POLL_H
#define HU_DAEMON_REACTION_POLL_H

#include "human/config.h"
#include "human/core/error.h"
#include <stddef.h>

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

/* Install the daemon's DPO collector on the reaction handler.
 *
 * Production: call this once at daemon startup, after the agent is
 * constructed and its `sota.dpo_collector` is initialized but BEFORE
 * any channel poll can fire. Without this call, the production
 * reaction-poll tick silently no-ops (every event lands in a NULL
 * collector and is discarded).
 *
 * This is a thin wrapper around `hu_reaction_handler_set_collector`
 * that exists purely so `src/daemon.c` doesn't have to include
 * `human/agent/reaction_handler.h` (see the include-collision note
 * above). */
void hu_daemon_reaction_wire_collector(struct hu_dpo_collector *collector);

/* Production-callable tick.
 *
 * One call polls the configured iMessage chat.db for new tapback
 * reactions since `since_unix` and feeds each event to
 * `hu_reaction_handler_handle_event` so it lands in the daemon-owned
 * `hu_dpo_collector_t` (the same one DPO training reads from).
 *
 * Returns HU_OK on success (including the "feature disabled" and
 * "no env" cases — those exit early as a no-op). Returns the poll
 * function's error code unmodified if it fails. Writes the number
 * of events fed into the handler to `*out_ingested` if non-NULL.
 *
 * Pre-conditions:
 *   - cfg may be NULL (treated as feature-disabled, no-op)
 *   - the caller has already installed a collector via
 *     hu_reaction_handler_set_collector() — typically once at
 *     daemon startup after the agent is constructed
 *
 * Side effects:
 *   - reads HU_CHATDB env var for the chat.db path
 *   - calls hu_imessage_poll_reactions (SQLite read-only)
 *   - calls hu_reaction_handler_handle_event per event (records a
 *     hu_preference_pair_t row into the collector if the event
 *     resolves to a known assistant message)
 */
hu_error_t hu_daemon_reaction_poll_tick(const hu_config_t *cfg,
                                        int64_t since_unix,
                                        size_t *out_ingested);

#ifdef HU_IS_TEST
hu_error_t hu_daemon_tick_for_test(const hu_config_t *cfg);
void hu_daemon_set_poll_call_counter_for_test(int *counter);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_REACTION_POLL_H */
