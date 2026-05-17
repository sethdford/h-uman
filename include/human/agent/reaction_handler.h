/* include/human/agent/reaction_handler.h
 *
 * Phase 2 Task 13 (RL SOTA): map an inbound hu_reaction_event_t to a
 * hu_preference_pair_t row in the daemon-owned hu_dpo_collector_t.
 *
 * Wiring (production):
 *   daemon startup
 *     → hu_reaction_handler_set_collector(daemon's collector)
 *   channel poll / webhook fires
 *     → builds hu_reaction_event_t
 *     → hu_reaction_handler_handle_event(&evt)
 *     → resolves the (channel, thread, msg_ref) triple to the prompt
 *       + assistant response that produced it
 *     → strncpy's into hu_preference_pair_t (fixed-size char buffers,
 *       NOT pointers — see include/human/ml/dpo.h:15-26)
 *     → hu_dpo_record_pair(s_collector, &pair)
 *
 * Lookup CAVEAT: the resolver is an in-memory 256-entry circular store
 * populated by the test seam below. Phase 5 daemon integration will
 * replace it with the real assistant-message store; until then,
 * reactions on messages older than the most recent 256 sends silently
 * drop (R4 in the risk register documented in the plan).
 */
#ifndef HU_AGENT_REACTION_HANDLER_H
#define HU_AGENT_REACTION_HANDLER_H

#include "human/core/error.h"
#include "human/channels/reaction_event.h"
#include "human/ml/dpo.h"  /* hu_dpo_collector_t */

#ifdef __cplusplus
extern "C" {
#endif

/* The reaction handler needs a target hu_dpo_collector_t to write into.
 * The daemon owns the collector lifecycle (see src/daemon.c) and wires it
 * via hu_reaction_handler_set_collector at startup. Production code path
 * is therefore: daemon init → set_collector → channel poll/webhook fires
 * → handle_event writes to the daemon's collector. Tests use the same
 * setter with an in-memory SQLite collector. */
void hu_reaction_handler_set_collector(hu_dpo_collector_t *collector);

hu_error_t hu_reaction_handler_handle_event(const hu_reaction_event_t *event);

/* Per-turn signal flag — daemon's per-turn-cleanup block calls clear() at
 * the end of each turn (see src/daemon.c near label `skip_llm_this_batch:`)
 * and agent_turn.c queries was_called() during the substring-heuristic
 * branch (Task 14). The flag is NOT thread-safe — it relies on the daemon
 * being a single-threaded event loop with NO concurrent turn dispatch.
 * Verified in src/daemon.c: only `g_trust_mutex` exists (line 800, scoped
 * to trust management); there is NO per-channel turn lock. If the daemon
 * ever gains a thread pool or concurrent channel processing, this flag
 * MUST become per-agent-context (e.g. a field on hu_agent_t). */
void hu_reaction_handler_clear_turn(void);
int  hu_reaction_handler_was_called_this_turn(void);

/* Production + demo path: pre-register an assistant message before reactions
 * are replayed (Phase 5 H8 / Phase 6 demo). */
void hu_reaction_handler_register_assistant_message_for_production(
    const char *channel, const char *thread, const char *msg_ref,
    const char *prompt, const char *response);

#if HU_IS_TEST
/* Test seam: same lookup store as production registration. */
void hu_reaction_handler_register_assistant_message_for_test(
    const char *channel, const char *thread, const char *msg_ref,
    const char *prompt, const char *response);
void hu_reaction_handler_reset_for_test(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
