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

#include "human/channels/reaction_event.h"
#include "human/core/error.h"
#include "human/ml/dpo.h" /* hu_dpo_collector_t */

/* Forward declaration — keeps the full memory subsystem header out of
 * any channel/agent TU that only needs the setter signature. */
struct hu_personal_model;
struct sqlite3;

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

/* Phase 1c of docs/plans/2026-05-18-imessage-sota.md: optional personal-
 * model sink. When non-NULL, iMessage reactions on registered assistant
 * messages are ingested as canonical English into the personal model
 * (separate concern from the DPO collector which exists for training
 * data). Wired by the daemon at init via
 * hu_daemon_reaction_wire_personal_model. Pass NULL at shutdown to clear. */
void hu_reaction_handler_set_personal_model(struct hu_personal_model *model);

/* Sprint A.7: optional identity-resolver wire. When non-NULL, reaction
 * sender_handle is canonicalized via hu_identity_lookup BEFORE the
 * personal-model ingest call. Effect: "Alice@imessage" + "Alice@slack"
 * reactions cluster under one canonical name in the persona, instead
 * of as two separate contacts.
 *
 * Conservative: only HIGH-confidence merges (canonical-phone or
 * canonical-email match) actually rewrite the handle. LOW/MEDIUM
 * confidence merges leave the raw handle untouched — the resolver's
 * own conservatism (don't merge strangers with the same first name)
 * already pinned at that level. */
#include "human/memory/identity_resolver.h"
void hu_reaction_handler_set_identity_graph(const hu_identity_graph_t *graph);

/* T8 (reflection retire-on-contradiction): optional reflection-DB wire.
 * When non-NULL, a NEGATIVE reaction (thumbs_down) retires every
 * reflection pattern surfaced in that event's channel within the last
 * HU_REFLECTION_CONTRADICTION_WINDOW_MS — the patterns that shaped the
 * thumbed-down turn are treated as contradicted. The daemon owns the
 * SQLite handle (same memory-backend db as reflection storage) and
 * wires it at init; pass NULL at shutdown to clear. */
void hu_reaction_handler_set_reflection_db(struct sqlite3 *db);

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
int hu_reaction_handler_was_called_this_turn(void);

/* Production + demo path: pre-register an assistant message before reactions
 * are replayed (Phase 5 H8 / Phase 6 demo). The alternative parameter (if non-NULL
 * and >= 4 bytes) is the rejected draft that becomes the "other side" of a complete
 * DPO pair on tapback. Pass NULL or empty string if no alternative is available. */
void hu_reaction_handler_register_assistant_message_for_production(
    const char *channel, const char *thread, const char *msg_ref, const char *prompt,
    const char *response, const char *alternative);

/* SOTA roadmap #13 (continuity context): most recent outbound response
 * registered for (channel, thread), regardless of msg_ref. Returns 1 on
 * hit (response copied into out, NUL-terminated, truncated to out_cap),
 * 0 on miss or NULL/degenerate args (out set to "" when writable). Feeds
 * the "Your last message to them" prompt section on the reactive path. */
int hu_reaction_lookup_last_response(const char *channel, const char *thread, char *out,
                                     size_t out_cap);

#if HU_IS_TEST
/* Test seam: same lookup store as production registration. */
void hu_reaction_handler_register_assistant_message_for_test(
    const char *channel, const char *thread, const char *msg_ref, const char *prompt,
    const char *response, const char *alternative);
void hu_reaction_handler_reset_for_test(void);
#ifdef HU_ENABLE_SQLITE
/* Test seam: run the production SQLite open+migrate at an arbitrary path.
 * Returns 1 when the store opened (fresh create, reopen of an already-
 * migrated store, or legacy pre-`alternative` migration); 0 on failure.
 * Pins the idempotent-migration contract that production rxn_db_open uses. */
int hu_reaction_handler_lookup_db_open_for_test(const char *path);
#endif
#endif

#ifdef __cplusplus
}
#endif
#endif
