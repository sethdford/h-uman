#ifndef HU_AGENT_WORLD_MODEL_H
#define HU_AGENT_WORLD_MODEL_H

/* W9 — Per-contact unified world model.
 *
 * Replaces the four parallel calls (persona load, graph neighbors, emotional
 * state, contact get) with one: hu_world_model_load. Adds two cells the
 * planner has never had: theory-of-mind (what does the user believe about
 * me?) and negative memory (what should I never say or do here?).
 *
 * Layer 3 of the v2 stack (see docs/plans/2026-05-10-memory-v2-roadmap-
 * overview.md). Reads from layers 1-2 (W7 facade, W8 beliefs); written to by
 * layers 5-6 (W11 self-RAG, W14 scheduler).
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/belief.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "human/memory/personal_model.h"
#include "human/memory/write_trust.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* P1.1-P1.3 forward decls — kept opaque so the world-model header doesn't
 * pull in persona.h (which transitively drags in a large dependency graph).
 * Definitions live in `human/persona.h` and `human/persona/persona_deltas.h`. */
struct hu_persona;
struct hu_persona_delta;

/* An active goal the agent should keep in mind on every turn. */
typedef struct hu_active_goal {
    char text[160];
    float salience;          /* 0..1, how prominent right now */
    int64_t expressed_at;
    int64_t expires_at;      /* 0 = persistent */
} hu_active_goal_t;

/* "Don't say X around Y." First-class so retrieval and verification can
 * surface it explicitly rather than burying it in persona avoid_vocab. */
typedef struct hu_negative_memory {
    int64_t id;
    char text[200];
    char scope[64];          /* "topic", "contact", "channel", ... */
    char reason[120];
    hu_belief_t belief;      /* W8: how sure we are this is a hard rule */
    int64_t created_at;
} hu_negative_memory_t;

/* What the user appears to believe about us. Synthesized lazily from
 * persona expectations + recent persona deltas + emotional reactions. */
typedef struct hu_theory_of_mind {
    char user_thinks_we_are[160];
    char user_expects_we_can[200];
    char user_expects_we_cannot[200];
    /* P1.2 — channel-aware pragmatics digest from `hu_persona_overlay_t`
     * (formality, directness, face-saving, vulnerability tier, …). Distinct
     * from `style_summary` (which is the personal-model communication-style
     * string about the *user*). This is about how the user expects *us* to
     * interact on this channel. Empty when no persona overlay was merged. */
    char interaction_style[256];
    hu_belief_t confidence;
} hu_theory_of_mind_t;

/* Single struct the planner consults every turn. Built lazily, cached, and
 * invalidated on relevant writes (see hu_world_model_invalidate). */
typedef struct hu_world_model {
    char contact_id[64];

    /* Top-K entities relevant to this contact (mention_count desc). */
    hu_graph_entity_t *entities;
    size_t entities_count;

    /* Top-K relations for those entities (weight desc). */
    hu_graph_relation_t *relations;
    size_t relations_count;

    /* Latest emotional snapshot (placeholder until emotional state lands). */
    char dominant_emotion[32];
    float arousal;
    float valence;

    /* Active goals (top-K by salience). */
    hu_active_goal_t goals[8];
    size_t goals_count;

    /* Negative memory items in scope. */
    hu_negative_memory_t *negatives;
    size_t negatives_count;

    /* Theory-of-mind. */
    hu_theory_of_mind_t tom;

    /* Recent topics, last-N circular slot. */
    char recent_topics[10][64];
    size_t recent_topics_count;

    /* M2 ↔ W9 bridge: communication style summary from the personal model.
     * Populated by hu_world_model_merge_personal(); empty when no PM data. */
    char style_summary[256];

    /* Cache metadata. */
    int64_t built_at;
    int64_t valid_until;     /* unix ms; 0 = no TTL */
} hu_world_model_t;

/* Build a fresh world model for `contact_id`. Caller owns the returned
 * struct; free with hu_world_model_free. The function does not consult any
 * cache — for the cached path use hu_world_model_load. */
hu_error_t hu_world_model_build(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                 const char *contact_id, size_t cid_len,
                                 int64_t now_ms,
                                 hu_world_model_t **out);

/* Cached load. Behavior:
 *  - cache hit (entry not expired): returns a clone of the cached entry
 *  - cache miss / expired: builds, caches, returns clone
 * Caller still owns the returned pointer and must free with
 * hu_world_model_free. The TTL defaults to 60s; tune via the env var
 * HU_WORLD_MODEL_TTL_MS for tests.
 *
 * Equivalent to `hu_world_model_load_with_channel(..., NULL, 0, now_ms,
 * out)` — i.e. uses the empty-channel default cache key. Bridge / agent
 * paths that have a channel handle should call the `_with_channel`
 * variant so the per-channel persona overlay (P1.2) drives distinct
 * cached snapshots for the same contact across Slack / SMS / iMessage. */
hu_error_t hu_world_model_load(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                const char *contact_id, size_t cid_len,
                                int64_t now_ms,
                                hu_world_model_t **out);

/* P2.4 — channel-aware cached load. Cache key is `(contact_id, channel)`
 * so the same person on Slack vs SMS gets distinct snapshots, which is
 * required for the per-channel persona overlay (P1.2) to drive behavior.
 * Pass `channel == NULL || channel_len == 0` for the legacy single-key
 * lookup. `channel_len < 32` (cache slot capacity). */
hu_error_t hu_world_model_load_with_channel(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                            const char *contact_id, size_t cid_len,
                                            const char *channel, size_t channel_len,
                                            int64_t now_ms,
                                            hu_world_model_t **out);

/* Invalidate cached entries.
 *
 * `hu_world_model_invalidate(NULL, 0)` clears the entire LRU (graph
 * teardown / tests).
 *
 * `hu_world_model_invalidate(contact_id, len)` (non-NULL) clears ALL
 * channel-keyed entries for that contact — the right default because
 * most writes (graph upsert, negative memory, residue) are not channel-
 * scoped at the data layer.
 *
 * `hu_world_model_invalidate_channel(contact_id, len, channel, ch_len)`
 * clears only the (contact, channel) entry. Use when a write is
 * known-scoped to one channel (e.g., a channel-only ToM scenario). */
void hu_world_model_invalidate(const char *contact_id, size_t cid_len);
void hu_world_model_invalidate_channel(const char *contact_id, size_t cid_len,
                                       const char *channel, size_t channel_len);

/* P2.5 — observability: report cache slot capacity, total loads, hits,
 * and total evictions since process start (or last `_reset_for_tests`).
 * Hit rate = hits / loads. Any pointer can be NULL.
 *
 * Useful as `hu_observer_t` plumbing input — caller polls every N
 * seconds and emits a metric. Safe to call before the cache has been
 * touched (returns 0/0/0/configured-capacity). */
void hu_world_model_cache_stats(size_t *slots, uint64_t *loads, uint64_t *hits,
                                uint64_t *evictions);

/* Test-only: drop all cached entries AND zero the telemetry counters.
 * Production paths should use `hu_world_model_invalidate(NULL, 0)`,
 * which keeps counters intact. */
void hu_world_model_cache_reset_for_tests(void);

/* Free a world model returned by build/load. Safe with NULL. */
void hu_world_model_free(hu_allocator_t *alloc, hu_world_model_t *wm);

/* --- negative memory CRUD (used by the synthesizer + later workstreams) --- */

hu_error_t hu_negative_memory_add(struct hu_graph *g, const char *contact_id,
                                   size_t cid_len, const hu_negative_memory_t *nm,
                                   int64_t *out_id);

/* Same insert as `hu_negative_memory_add` but uses `hu_memory_facade_sqlite_db`
 * (W7-first; no graph handle). Prefer in bridge / agent paths that already
 * hold `hu_memory_facade_t *`. */
hu_error_t hu_negative_memory_add_facade(hu_memory_facade_t *m, const char *contact_id,
                                         size_t cid_len, const hu_negative_memory_t *nm,
                                         int64_t *out_id);

/* P3.1 — Adversarially-gated negative-memory insert. Routes the proposed
 * insert through W1 `hu_write_trust_score` before persisting. The W9 spec's
 * risk row says negatives must not be plantable by an attacker on an open
 * channel ("do not warn user about X" silences the agent).
 *
 * Outcomes:
 *   LIVE       — insert as-is, return HU_OK with `*out_id` set
 *   QUARANTINE — insert with `nm.belief.mean` clamped to ≤ 0.5 so the
 *                planner sees the rule as soft. Return HU_OK with `*out_id`.
 *   DROP       — refuse the insert. Return HU_ERR_PERMISSION_DENIED.
 *                `*out_id` left zero.
 *
 * `source` should reflect where the proposed negative came from
 * (HU_WRITE_SOURCE_USER for direct user statements, HU_WRITE_SOURCE_AGENT
 * for self-RAG abstention, HU_WRITE_SOURCE_CHANNEL_OPEN for unverified
 * webhook input, etc.). `now_ms == 0` uses OS clock for recency scoring. */
hu_error_t hu_negative_memory_add_facade_gated(hu_memory_facade_t *m, const char *contact_id,
                                                size_t cid_len, const hu_negative_memory_t *nm,
                                                hu_write_source_t source, int64_t now_ms,
                                                int64_t *out_id);

hu_error_t hu_negative_memory_list(struct hu_graph *g, hu_allocator_t *alloc,
                                    const char *contact_id, size_t cid_len,
                                    size_t limit, hu_negative_memory_t **out,
                                    size_t *out_count);

/* Same rows as `hu_negative_memory_list` but uses `hu_memory_facade_sqlite_db`
 * — prefer from `hu_world_model_build` and other W7-first paths. */
hu_error_t hu_negative_memory_list_facade(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                           const char *contact_id, size_t cid_len, size_t limit,
                                           hu_negative_memory_t **out, size_t *out_count);

void hu_negative_memory_free(hu_allocator_t *alloc, hu_negative_memory_t *nm,
                              size_t count);

/* M2 ↔ W9 bridge: merge personal model signal into an already-built world
 * model. Copies goals, topics, style summary, and dominant emotion from the
 * personal model when the world model lacks that data. Safe with NULL pm
 * (no-op). Idempotent: repeated calls overwrite the same fields. */
void hu_world_model_merge_personal(hu_world_model_t *wm,
                                   const hu_personal_model_t *pm);

/* P1.1 + P1.2 + P1.3 — Persona-grounded ToM synthesis.
 *
 * Closes the W9 spec's "deferred" item (file header of `world_model.c` and
 * spec line 127). Three things happen, in order:
 *
 *   1. `tom.user_thinks_we_are` is set from `persona->identity` (or
 *      `persona->name` as fallback). Replaces the previous wrong-by-design
 *      heuristic that put the most-mentioned entity name there.
 *   2. The per-channel `hu_persona_overlay_t` (looked up via
 *      `hu_persona_find_overlay(persona, channel, channel_len)`) is folded
 *      into `tom.user_expects_we_cannot` (face-saving / vulnerability
 *      tolerance / disagreement style) and `tom.interaction_style`
 *      (formality / directness / typing quirks digest).
 *   3. Recent `hu_persona_delta_t` rows for this contact (status APPLIED,
 *      confidence ≥ 0.6) are appended to the appropriate ToM string by
 *      kind: BOUNDARY / VOCAB_AVOID → `user_expects_we_cannot`;
 *      FORMALITY / TONE / LENGTH → `interaction_style`.
 *
 * Safe with NULL persona (no-op) and NULL/empty channel (skips overlay step).
 * Safe with NULL deltas / deltas_count == 0 (skips delta step). Idempotent:
 * repeated calls overwrite the same fields and may concatenate the same
 * delta text (callers should pass de-duplicated lists; the W5 evolver
 * already dedupes by kind+key).
 *
 * Confidence: each merged source bumps `tom.confidence.mean` by 0.05, capped
 * at 0.85 — we never claim certainty here. */
void hu_world_model_merge_persona(hu_world_model_t *wm,
                                  const struct hu_persona *persona,
                                  const char *channel, size_t channel_len,
                                  const struct hu_persona_delta *deltas,
                                  size_t deltas_count);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_WORLD_MODEL_H */
