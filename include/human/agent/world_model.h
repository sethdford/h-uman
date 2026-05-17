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
#include "human/memory/hyperedge.h"
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

/* P3.2 — semantic origin tag for a negative-memory row. ORTHOGONAL to
 * `hu_write_source_t` (which is "where did the bytes enter the system";
 * P3.1 trust gate). This is "what KIND of negative is this", which the
 * planner / W11 verifier needs to differentiate handling:
 *
 *   USER_EXPLICIT     — user said "don't ever say X". Hard refusal in
 *                       responses; surface in transcript with citation.
 *   SELF_RAG_ABSTAIN  — W11 self-RAG abstained on a related claim.
 *                       Soft hedge in responses ("I'm not sure about X");
 *                       not a permanent refusal.
 *   AUTO_EXTRACT      — pulled from conversation by NLP heuristic.
 *                       Ask-to-confirm before treating as a hard rule.
 *   SYSTEM_POLICY     — built-in safety / compliance rule. Hard refusal +
 *                       audit-log every time it fires. Immutable from
 *                       agent-side writes (only privileged init paths
 *                       insert these). */
typedef enum hu_negative_source {
    HU_NEGATIVE_SOURCE_USER_EXPLICIT = 0,
    HU_NEGATIVE_SOURCE_SELF_RAG_ABSTAIN = 1,
    HU_NEGATIVE_SOURCE_AUTO_EXTRACT = 2,
    HU_NEGATIVE_SOURCE_SYSTEM_POLICY = 3,
} hu_negative_source_t;

/* P4.3 — Bitemporal change kind. Surfaced on the snapshot so the planner
 * can see "this fact was retracted yesterday" or "this relation replaces
 * the older one #42" without having to re-query W1 every turn.
 *
 *   SUPERSEDED — newer relation row points at an older one via
 *                supersedes_id; the older row's event_end was set when
 *                the new fact landed.
 *   RETRACTED  — relation's event_end > 0 but no successor was written
 *                (relationship explicitly ended; e.g. "we stopped
 *                meeting weekly"). */
typedef enum hu_world_change_kind {
    HU_WORLD_CHANGE_NONE = 0,
    HU_WORLD_CHANGE_SUPERSEDED = 1,
    HU_WORLD_CHANGE_RETRACTED = 2,
} hu_world_change_kind_t;

/* P4.3 — One bitemporal change in the snapshot's recent window.
 *
 * `at_ms` is the user-visible "when did this change happen":
 *   - SUPERSEDED → the new row's `last_seen` (when the new fact was
 *     first observed and the old row was retired)
 *   - RETRACTED  → the row's `event_end` (when the world stopped
 *     including this relation)
 *
 * `summary` is a short, human-readable digest the planner can lift
 * verbatim into chain-of-thought ("worked_at #4 → SUPERSEDED by #7"),
 * truncated to 120 chars. `prior_id` is 0 for RETRACTED. */
typedef struct hu_world_recent_change {
    int64_t relation_id;
    int64_t prior_id;
    hu_world_change_kind_t kind;
    int64_t at_ms;
    char summary[120];
} hu_world_recent_change_t;

/* "Don't say X around Y." First-class so retrieval and verification can
 * surface it explicitly rather than burying it in persona avoid_vocab. */
typedef struct hu_negative_memory {
    int64_t id;
    char text[200];
    char scope[64];          /* "topic", "contact", "channel", ... */
    char reason[120];
    hu_belief_t belief;      /* W8: how sure we are this is a hard rule */
    int64_t created_at;
    /* P3.2 — semantic origin (defaults to USER_EXPLICIT for back-compat
     * with rows inserted before the column existed; the schema migration
     * fills NULL with 0 = USER_EXPLICIT). */
    hu_negative_source_t source;
} hu_negative_memory_t;

/* P5.4 — trust gradient sparkline length. Sized at 16 to match the
 * roughly-one-conversation horizon (≈8 turns / hour over a typical
 * exchange) without making the snapshot unwieldy. The newest sample is
 * always at index `confidence_history_count - 1`; older samples scroll
 * left as new merges fire. */
#define HU_TOM_CONFIDENCE_HISTORY 16

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
    /* P5.4 — last N confidence.mean samples, oldest-first. Lets the
     * planner detect "trust dropped sharply over the last 3 turns" (a
     * de-escalation signal) without chasing every single dip the
     * way a single scalar forces it to. Populated by
     * `hu_world_model_merge_persona`: each call appends the new
     * post-merge mean and shifts older samples left when full. */
    float confidence_history[HU_TOM_CONFIDENCE_HISTORY];
    size_t confidence_history_count;
} hu_theory_of_mind_t;

/* P5.2 — Self-model alongside ToM. Distinct from `hu_theory_of_mind_t`
 * (which is what the user appears to believe about *us*); this struct
 * is what *we* currently believe about ourselves: who we are right now,
 * what we're optimizing for, and how our behavior has drifted recently.
 *
 * Unifies signals from `hu_personal_model_t`, the persona, the
 * persona-deltas log, and recent goal salience into one snapshot cell
 * so the planner doesn't have to look in four places. All fields are
 * inline strings (no owned pointers) so the snapshot stays POD-safe
 * at install/clone time.
 *
 *   name                  — the agent's working name (borrowed from
 *                           persona.name; truncated). Empty when no
 *                           persona was merged.
 *   focused_topics        — top-3 topics the agent is currently
 *                           tracking, ';'-joined. Empty when no topics
 *                           have been observed.
 *   recent_drift_kind     — most recent applied persona delta kind as
 *                           a short string ("BOUNDARY", "TONE", …) or
 *                           "" when no recent drift.
 *   recent_drift_value    — truncated value text from the same delta
 *                           (so the planner can echo "we agreed to be
 *                           shorter on Slack").
 *   confidence_in_self    — how sure we are about our own model
 *                           [0, 1]; computed as min of the personal-
 *                           model trust gradient and the persona's
 *                           identity completeness. Defaults 0.0 when
 *                           neither signal is present. */
typedef struct hu_self_model {
    char name[64];
    char focused_topics[200];
    char recent_drift_kind[32];
    char recent_drift_value[160];
    float confidence_in_self;
} hu_self_model_t;

/* P5.6 — multimodal context cells. Forward-looking seams that the
 * planner can already reason against ("user attached a screenshot last
 * turn — refer back to it") even before the channels populate them.
 * All fields are inline / nullable; default-zeroed by the build path.
 *
 *   contact_photo_path      — absolute path to the contact's avatar on
 *                             disk; empty when none. Populated by
 *                             channel handlers that ingest avatars.
 *   voice_fingerprint_hash  — 32-byte hex digest identifying the
 *                             user's last voice sample (cross-session).
 *                             Empty when voice channel is silent.
 *   last_image_caption      — captioned text of the most recent image
 *                             the user shared, truncated. Empty when
 *                             no image has been shared in the active
 *                             window.
 *   last_image_at_ms        — when that image was shared (unix ms);
 *                             0 when none. */
typedef struct hu_media_context {
    char contact_photo_path[256];
    char voice_fingerprint_hash[65];
    char last_image_caption[200];
    int64_t last_image_at_ms;
} hu_media_context_t;

/* P5.1 — Russell VAD + Mehrabian PAD-extended stance vector for the
 * latest emotional snapshot. Distinct from the legacy (valence, arousal,
 * dominant_emotion) triple which the planner has had since W9 day-0:
 *
 *   valence    — pleasure/displeasure axis [-1, 1]
 *   arousal    — calm/excited axis [0, 1]
 *   dominance  — submissive/dominant axis [-1, 1] (PAD; user's sense of
 *                control in the exchange)
 *   certainty  — wishy-washy/firm axis [0, 1] (Mehrabian extension; how
 *                resolute the user appears about their position)
 *
 * `dominance` and `certainty` are derived heuristically from the same
 * residue rows that drive (valence, arousal) — see the build path for
 * the exact mapping. NaN-safe: every component clamps to its valid
 * range; missing residue rows leave the vector at neutral defaults. */
typedef struct hu_stance_vector {
    float valence;
    float arousal;
    float dominance;
    float certainty;
} hu_stance_vector_t;

/* P5.3 — Conversational pressure cells. The planner has access to the
 * latest emotional snapshot via (valence, arousal, dominant_emotion),
 * but it has no view of the *trajectory* — "this is the third angry
 * message in a row" or "the user has been complaining for 47 minutes
 * straight". This struct surfaces the trajectory directly so the
 * planner can de-escalate / hand off / shorten responses without
 * re-querying the residue table.
 *
 *   recent_anger_count          — # negative-high-arousal residues in
 *                                 the last 60 minutes (cap 99). 0 when
 *                                 no recent anger.
 *   sustained_complaint_minutes — span (in minutes) over which negative
 *                                 residues have been continuously
 *                                 present without an interruption of
 *                                 ≥10 minutes. 0 when no complaint.
 *   urgency_score               — [0, 1] composite of recent anger,
 *                                 sustained complaint, and any explicit
 *                                 urgency cues in the latest residue
 *                                 reasons. 1.0 = "drop everything, this
 *                                 user needs a different response right
 *                                 now". */
typedef struct hu_conv_pressure {
    int recent_anger_count;
    int sustained_complaint_minutes;
    float urgency_score;
} hu_conv_pressure_t;

/* Single struct the planner consults every turn. Built lazily, cached, and
 * invalidated on relevant writes (see hu_world_model_invalidate). */
typedef struct hu_world_model {
    char contact_id[64];

    /* P1.4 — borrowed persona pointer (W9 spec line 65 "persona snapshot").
     *
     * LIFETIME CONTRACT: the world model does NOT own this pointer. The
     * persona must outlive every cached snapshot that may reference it
     * (the per-process LRU TTL is 60s; in practice the agent's persona
     * is loaded once at startup and lives for the process lifetime, so
     * this is a non-issue). Set lazily by `hu_world_model_merge_persona`;
     * NULL when no persona was merged.
     *
     * Stored as a borrowed pointer (not a deep copy) because
     * `hu_persona_t` carries hundreds of strings — copying it into
     * every cached snapshot for every contact would multiply RAM by
     * `cache_slots * |persona_strings|` (32-512 KB per slot at 32 slots
     * = 1-16 MB just for redundant persona). The borrowed pointer keeps
     * cache install at O(1) and consumers that mutate persona must
     * call `hu_world_model_invalidate(NULL, 0)` to flush every snapshot
     * before the borrowed memory is reused. */
    const struct hu_persona *persona;

    /* Top-K entities relevant to this contact (mention_count desc). */
    hu_graph_entity_t *entities;
    size_t entities_count;

    /* Top-K relations for those entities (weight desc).
     *
     * P4.1 — each row carries belief (mean = `confidence`,
     * variance = `confidence_variance`) and a deep-copied
     * `provenance` string so the planner can distinguish "0.8 from
     * channel-trusted source" from "0.8 from auto-extract". The
     * `context` field is still nulled (lean snapshot); callers that
     * need raw conversation context fetch via `hu_memory_facade_read`. */
    hu_graph_relation_t *relations;
    size_t relations_count;

    /* P4.3 — recent bitemporal changes derived from the relations
     * window above. Filtered to rows where `supersedes_id != 0` or
     * `event_end > 0`, sorted newest-first, capped at 8 to keep the
     * snapshot lean. NULL when no changes were observed. */
    hu_world_recent_change_t *recent_changes;
    size_t recent_changes_count;

    /* P4.2 — n-ary fact rows from W8. Queried per top-K entity and
     * deduped, capped at 16 to keep snapshot install at O(1). NULL
     * when no hyperedges touch any entity in this window. Owns deep
     * copies of `members` and `provenance`; freed by
     * `hu_world_model_free`. */
    hu_hyperedge_t *hyperedges;
    size_t hyperedges_count;

    /* Latest emotional snapshot. The legacy (valence, arousal,
     * dominant_emotion) triple stays in place for back-compat with
     * the prompt builder, the directive engine, and ~40 callers.
     * P5.1 layers a richer (V, A, D, C) stance vector on top via
     * `stance` below — every snapshot populates both. */
    char dominant_emotion[32];
    float arousal;
    float valence;
    /* P5.1 — Russell VAD + Mehrabian PAD-extended stance vector. */
    hu_stance_vector_t stance;
    /* P5.3 — conversational pressure derived from the residue
     * trajectory (NOT just the latest cell). */
    hu_conv_pressure_t pressure;

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

    /* P5.2 — agent's self-model (distinct from ToM). */
    hu_self_model_t self_model;

    /* P5.6 — multimodal context (photo / voice / image). */
    hu_media_context_t media;

    /* P6.2 — W10 (KV cache + reasoning trace) seams. NULL / 0 until
     * the W10 layer lands; carried on the snapshot now so consumers
     * can be wired before the implementation arrives.
     *
     * `kv_cache_handle` is an opaque pointer the caller MUST treat as
     * borrowed; the world-model never owns it. `last_reasoning_trace_id`
     * is a stable id (W10-assigned) of the most recent chain-of-thought
     * step the agent emitted for this contact. */
    void *kv_cache_handle;
    int64_t last_reasoning_trace_id;

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

/* P6.1 — Goal-conditioned re-ranking. HippoRAG-style PageRank seeded
 * from goal anchors: tokens shared between `goal_text` and an
 * entity's `name` (or a relation's endpoint names) bump that row's
 * effective relevance for this turn. Mutates the snapshot in-place
 * by reordering `wm->entities`, `wm->relations`, and `wm->recent_topics`
 * so that goal-aligned rows surface first.
 *
 * IMPORTANT: this is a snapshot-local re-rank, not a re-query — the
 * graph table is not consulted. Useful when the planner wants the
 * same snapshot framed for two different goals across one turn (e.g.,
 * draft + verify) without re-paying the build cost.
 *
 * `goal_text` is treated as a whitespace-separated bag of tokens;
 * comparison is case-insensitive. Tokens shorter than 3 chars are
 * ignored (filters out "a", "is", etc.). Empty or NULL `goal_text`
 * is a no-op (returns HU_OK). `alloc` is reserved for future
 * scoring buffers (currently unused; pass the same alloc that
 * built the snapshot).
 *
 * Idempotent: re-running with the same goal_text leaves the order
 * unchanged. Safe to call from any thread that owns the world-model
 * pointer (no shared cache state is touched). */
hu_error_t hu_world_model_rerank_for_goal(hu_world_model_t *wm,
                                           const char *goal_text,
                                           size_t goal_text_len,
                                           hu_allocator_t *alloc);


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
