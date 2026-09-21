#ifndef HU_DAEMON_DIRECTOR_H
#define HU_DAEMON_DIRECTOR_H

#include "human/agent.h"
#include "human/channel.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Director / response-classification bucket extracted from daemon.c.
 * Owns g_classify_provider (the Flash-Lite meta-behavior classifier). */

/* Director action type */
typedef enum { DIR_TEXT = 0, DIR_TAPBACK, DIR_SILENCE } hu_director_action_t;

/* Director result structure */
typedef struct {
    hu_director_action_t action;
    uint32_t delay_s;
    hu_reaction_type_t reaction;
    bool burst;
    char direction[512];
} hu_director_result_t;

/* Real-time emotion detection: test builds use heuristic-only (no LLM), production uses hybrid
 * routing via g_classify_provider when available. */
hu_emotional_state_t hu_daemon_detect_emotion(hu_allocator_t *alloc, hu_agent_t *agent,
                                              const hu_channel_history_entry_t *entries,
                                              size_t count);

/* Parse director result from raw string output */
void hu_daemon_parse_director_result(const char *raw, size_t len, hu_director_result_t *out);

/* Real-time scene director: Flash Lite call that returns structured meta-behavior.
 * Decides action (text/tapback/silence), delay, reaction type, burst mode, and
 * performance direction. Only runs when llm_decides && g_classify_provider_ok.
 * Returns true if result is valid. Caller uses result to route behavior. */
bool hu_daemon_director_call(hu_allocator_t *alloc, const char *combined, size_t combined_len,
                             const hu_channel_history_entry_t *entries, size_t entry_count,
                             hu_director_result_t *result);

/* ── G6 director-echo guard wiring (Sprint 34/37/40, wired 2026-09-21) ──
 *
 * The director's `direction` string is injected into the model's prompt
 * ("--- Scene Direction (this message only) ---"), so the model can quote
 * it back verbatim. The response_guard's G6 check exists to reject exactly
 * that. Until this seam was wired, G6 read `agent->scene_direction_text`
 * and `agent->director_history[]`, which nothing in src/ ever wrote — so
 * G6 was inert in production while looking wired at all three guard call
 * sites (agent_stream.c:1735/2711, agent_turn.c:7312).
 *
 * Ownership (see src/agent/agent_internal.h:152): the DAEMON owns the
 * `hu_director_result_t`; the agent only borrows a const pointer into
 * `result->direction`. Hence the strict pairing below.
 *
 * Call order, once per batch iteration:
 *   1. hu_daemon_director_contact_boundary(agent, batch_key, key_len)
 *        — before anything else, so a previous contact's director ring
 *          cannot gate this contact's reply (post-mortem rowid 56355).
 *   2. hu_daemon_director_arm_guard(...)  — before the agent turn.
 *   3. hu_daemon_director_end_turn(agent) — after the LAST retry of the
 *        turn, and MUST run before `result` leaves scope, or G6 reads
 *        freed stack memory on the next turn.
 */

/* Append the scene direction to *convo_ctx and arm G6 for this turn.
 * No-op unless `result->direction` is non-empty. Reallocates *convo_ctx
 * on `alloc` (freeing the old buffer) and republishes it onto
 * `agent->conversation_context`. On allocation failure the context is
 * left untouched and the guard is NOT armed — the direction never
 * reached the prompt, so there is nothing for G6 to catch.
 *
 * `result` must outlive the turn and the matching end_turn call. */
void hu_daemon_director_arm_guard(hu_allocator_t *alloc, hu_agent_t *agent,
                                  const hu_director_result_t *result, char **convo_ctx,
                                  size_t *convo_ctx_len);

/* End-of-turn: push the going-stale director into the agent's heap-owned
 * history ring (so G6 still catches it on the NEXT turn) and drop the
 * borrowed pointer into the daemon's stack buffer. Idempotent. */
void hu_daemon_director_end_turn(hu_agent_t *agent);

/* Drop all borrowed/retained director state when the daemon switches to a
 * different contact. Same-contact consecutive batches keep their history.
 * `key`/`key_len` is the batch session key. */
void hu_daemon_director_contact_boundary(hu_agent_t *agent, const char *key, size_t key_len);

/* F27: Classify our response type for comfort pattern learning.
 * Heuristic: haha/lol/joke -> distraction; sorry/i understand/that sucks -> empathy;
 * very short (<20 chars) -> space; you should/try this/maybe -> advice; default empathy. */
void hu_daemon_classify_comfort_response_type(const char *response, size_t response_len,
                                              char *out_type, size_t out_cap);

/* F23: Extract significant topic keywords from user text and record baselines.
 * Skips stopwords, records each significant word (3–32 chars) via topic_baselines. */
void hu_daemon_record_topic_baselines_from_text(hu_memory_t *memory, const char *contact_id,
                                                size_t contact_id_len, const char *text,
                                                size_t text_len);

/* F27: Score engagement from their reply. reply_len>20 + positive words -> 0.8;
 * brief thanks -> 0.4; very short -> 0.2. */
float hu_daemon_score_comfort_engagement(const char *reply, size_t reply_len);

/* Store a conversation summary as long-term memory.
 * Concatenates the user message and agent response, runs deep-extract
 * on the full exchange, and stores extracted facts scoped to the contact.
 * When graph is non-NULL, also upserts facts and relations into the GraphRAG knowledge graph.
 * agent may be NULL; when non-NULL and bth_metrics set, increments facts_extracted. */
void hu_daemon_store_conversation_summary(hu_allocator_t *alloc, hu_memory_t *memory,
                                          hu_graph_t *graph, hu_agent_t *agent,
                                          const char *session_id, size_t session_id_len,
                                          const char *user_msg, size_t user_msg_len,
                                          const char *response, size_t response_len);

#endif /* HU_DAEMON_DIRECTOR_H */
