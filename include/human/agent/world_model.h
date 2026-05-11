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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
 * HU_WORLD_MODEL_TTL_MS for tests. */
hu_error_t hu_world_model_load(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                const char *contact_id, size_t cid_len,
                                int64_t now_ms,
                                hu_world_model_t **out);

/* Invalidate the cached entry for `(contact_id, cid_len)`.
 * Special case: `(NULL, 0)` clears the entire LRU (graph teardown / tests).
 * An empty-string contact with `cid_len == 0` only invalidates that key,
 * not the whole cache. */
void hu_world_model_invalidate(const char *contact_id, size_t cid_len);

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

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_WORLD_MODEL_H */
