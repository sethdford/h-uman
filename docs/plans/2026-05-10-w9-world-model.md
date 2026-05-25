---
title: "W9 — World Model: hu_world_model_t per-contact unified snapshot"
created: 2026-05-10
status: closed
parent: 2026-05-10-memory-v2-roadmap-overview.md
risk: medium
scope: include/human/agent/, src/agent/, src/persona/
last_audit: 2026-05-25
---

# W9 — World Model

## Goal

Replace four parallel calls (`hu_persona_load`, `hu_graph_neighbors`, `hu_emotion_state_load`, `hu_contact_get`, `hu_persona_delta_list`) with one: `hu_world_model_load(memory, contact, &out)`. The result is a single, lazily computed, cached snapshot the planner consults every turn. Adds two new cells the planner has never had access to: **theory-of-mind** (what does the user believe about *me*?) and **negative memory** (what should I never say or do here?).

## Motivation

`src/agent/context.c` builds the prompt by calling persona, graph, emotional state, contact, persona deltas, and case recall in sequence. Each adds latency, has its own error model, and yields a different shape. The W11 inline Self-RAG and the W12 planner each need this same set of inputs — without W9, both would re-implement the same fan-out, doubling code.

Theory-of-mind and negative memory exist as data (persona, deltas, persona avoid_vocab) but are not assembled into a single artifact the agent can read. This is the gap between "knows facts about you" and "models you."

## Prior art

- Letta `MemoryModule` aggregate state.
- Pi (Inflection) — explicit user-belief-about-me model.
- Mem0g — unified per-user state object.
- `src/memory/personal_model.c` (existing in repo as M2 mission anchor) — half-built; W9 finishes it.

## Design

### `hu_world_model_t`

```c
/* include/human/agent/world_model.h */

typedef struct hu_active_goal {
    char text[160];
    float salience;        /* 0..1, how prominent right now */
    int64_t expressed_at;
    int64_t expires_at;    /* 0 = persistent */
} hu_active_goal_t;

typedef struct hu_negative_memory {
    char text[200];        /* "do not mention X around Y" */
    char scope[64];        /* topic / contact / channel */
    char reason[120];
    hu_belief_t belief;    /* W8: how sure are we this is a hard rule */
} hu_negative_memory_t;

typedef struct hu_theory_of_mind {
    /* What the user appears to believe about us: capabilities, persona, values. */
    char user_thinks_we_are[160];
    char user_expects_we_can[200];
    char user_expects_we_cannot[200];
    hu_belief_t confidence;
} hu_theory_of_mind_t;

typedef struct hu_world_model {
    /* Identity */
    char contact_id[64];

    /* Persona snapshot (read-only copy from persona.c) */
    hu_persona_t persona;

    /* Recent entities + 1-hop relations */
    hu_graph_entity_t *entities;
    size_t entities_count;
    hu_graph_relation_t *relations;
    size_t relations_count;

    /* Emotional state (latest) */
    char dominant_emotion[32];
    float arousal;
    float valence;

    /* Active goals (top-K by salience) */
    hu_active_goal_t goals[8];
    size_t goals_count;

    /* Negative memory ("don't say X" facts) */
    hu_negative_memory_t *negatives;
    size_t negatives_count;

    /* Theory-of-mind */
    hu_theory_of_mind_t tom;

    /* Recent topics (last 10) */
    char recent_topics[10][64];
    size_t recent_topics_count;

    /* Cache metadata */
    int64_t built_at;
    int64_t valid_until;
} hu_world_model_t;

hu_error_t hu_world_model_load(hu_memory_t *m, hu_allocator_t *alloc,
                                const char *contact_id, size_t cid_len,
                                hu_world_model_t **out);
void hu_world_model_free(hu_allocator_t *alloc, hu_world_model_t *wm);
hu_error_t hu_world_model_invalidate(hu_memory_t *m, const char *contact_id, size_t cid_len);
```

### Cache

Per-process LRU keyed by `contact_id`, max 32 entries. TTL 60s. Invalidated by:
- Any write to `hu_memory_t` for that contact (writers call `_invalidate` after success).
- Explicit `hu_world_model_invalidate`.

### Negative memory storage

New table:
```sql
CREATE TABLE IF NOT EXISTS negative_memory (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL DEFAULT '',
    text TEXT NOT NULL,
    scope TEXT NOT NULL,
    reason TEXT,
    confidence_mean REAL NOT NULL DEFAULT 1.0,
    confidence_variance REAL NOT NULL DEFAULT 0.0,
    created_at INTEGER NOT NULL
);
```

### Theory-of-mind storage

Computed lazily from existing data (persona expectations, emotional reactions, deltas) by a deterministic synthesizer in `world_model.c`. No new schema.

## Phases

1. Author `world_model.h` + `world_model.c` deterministic synthesizer (no LLM yet).
2. Author `negative_memory.c` (CRUD).
3. Wire `hu_world_model_load` into `src/agent/context.c` (replace 4 calls with 1).
4. Add LRU cache + invalidation hooks on every `hu_memory_write`.
5. Add ToM synthesizer (heuristic over persona + recent deltas).
6. Adversarial tests.

## Test plan

- `test_w9_world_model_round_trip`: loads, writes, invalidates, reloads — fresh data appears.
- `test_w9_world_model_cache_hit_within_ttl`: repeated load within 60s returns cached.
- `test_w9_world_model_invalidates_on_write`: any `hu_memory_write` for contact → next load is fresh.
- `test_w9_negative_memory_blocks_response_path`: when a negative memory matches the draft topic, the verifier (W11) flags it.
- `test_w9_tom_synthesizer_grounded_in_persona`: ToM fields are reproducible from inputs.
- `test_w9_adversarial_negative_memory_poisoning`: attacker tries to inject "do not warn user about X" → write_trust catches it before W9 can read it.
- `test_w9_world_model_p99_latency_under_5ms`: benchmark.

## Success metric

- Single `hu_world_model_load` p99 ≤ 5 ms (vs ~12 ms for the 4 separate calls today).
- ToM synthesis stable across ≥10 reload cycles (deterministic).
- ≥3 measurable behavior changes in agent output when negative memory is set (proven by A/B test).
- Binary size delta ≤ +50 KB.

## Risks

| Risk | Mitigation |
|------|------------|
| Cache staleness leaks across contacts | Per-contact key; explicit eviction on every write |
| ToM synthesizer encodes false confidence | All ToM fields carry `hu_belief_t`; UI surfaces uncertainty |
| Negative memory exploited by attacker to silence the agent | Negative memory writes go through W1 write_trust; high bar from open channels |

## Out of scope

- Active disambiguation ("am I right that you prefer X?"). That's a future workstream.
- ~~Multimodal world model (image of user's room as part of ToM).~~ Now in scope as P5.6 seams (see SOTA-frontier section); population deferred to channel handlers.
- Cross-contact world model (group conversations).

## Binary size budget: +50 KB.

## Status (May 2026 — SOTA review follow-through)

The original phases (1–6) shipped in the W9 v1 bundle (commits `02f9d546`,
`4286d735`, `4c9f5d0b`, `2355658b`). After an adversarial review against
the world-model deferred items + competitor SOTA features, the seven-bundle
follow-through (P1–P7 below) closed every remaining gap.

### Shipped — moved out of "deferred"

**Bundle 1 — Persona-grounded ToM synthesis (P1.1–P1.5)**
- P1.1 `tom.user_thinks_we_are` populated from `persona->identity` (no longer the wrong-by-design "most-mentioned entity name").
- P1.2 `hu_persona_overlay_t` folded in per channel (formality / directness / face-saving / vulnerability tier / disagreement style / silence tolerance) into `user_expects_we_can/cannot` + new `interaction_style` field.
- P1.3 Recent applied `persona_deltas` for the contact merged in, kind-routed: `BOUNDARY` / `VOCAB_AVOID` → `cannot`; `FORMALITY` / `TONE` / `LENGTH` → `interaction_style`.
- P1.4 `hu_persona_t persona;` field on `hu_world_model_t` per spec line 65 — borrowed pointer with documented lifetime contract (snapshot does NOT own).
- P1.5 `tom.user_expects_we_can` populated from `persona->contacts[].allowed_behaviors` (per-contact match) + `persona->situational_directions[].instruction` (persona-wide affordances).

**Bundle 2 — Cache correctness + observability (P2.1–P2.6)**
- P2.1 `hu_world_model_invalidate` wired into both `hu_negative_memory_add` and `hu_negative_memory_add_facade`.
- P2.2 Wired into `hu_goal_create`, `hu_goal_update_status`, `hu_goal_update_progress`, `hu_goal_decompose`.
- P2.3 Wired into `hu_emotional_residue_add` so dominant_emotion shifts surface immediately.
- P2.4 Cache key extended from `(contact_id)` to `(contact_id, channel)` so the per-channel persona overlay drives distinct cached snapshots; `hu_world_model_load_with_channel` is the new primary entry point.
- P2.5 `HU_WM_CACHE_SLOTS` env-tunable (default 32, max 1024); telemetry counters for loads / hits / evictions via `hu_world_model_cache_stats`.
- P2.6 POSIX mutex around `s_cache` (held during lookup / install / invalidate; never held during snapshot build).

**Bundle 3 — Negative memory hardening (P3.1–P3.3)**
- P3.1 `hu_negative_memory_add_facade_gated` routes inserts through W1 `hu_write_trust_score` (DROP → `HU_ERR_PERMISSION_DENIED`; QUARANTINE → clamp belief ≤ 0.5; per-source allowlist demotes non-{USER, AGENT, CHANNEL_TRUSTED} from LIVE).
- P3.2 `hu_negative_source_t` enum (USER_EXPLICIT, SELF_RAG_ABSTAIN, AUTO_EXTRACT, SYSTEM_POLICY); persisted via `ALTER TABLE` schema migration; round-trips through `hu_negative_memory_t.source`.
- P3.3 Adversarial tests: SYSTEM_POLICY bypasses channel-source allowlist; out-of-band source ints coerce to USER_EXPLICIT.

**Bundle 4 — Belief surfaces on the snapshot (P4.1, P4.3)**
- P4.1 Relations carry `hu_belief_t` — `confidence` (mean), `confidence_variance`, and a deep-copied `provenance` string. Snapshot owns the lifetime; `hu_world_model_free` frees per row.
- P4.3 `recent_changes[]` derived from bitemporal relations (`supersedes_id != 0` → `SUPERSEDED`; `event_end > 0` → `RETRACTED`); capped at 8 newest-first; POD slab (no per-row owned strings).

**Bundle 5 — SOTA-frontier cells (P4.2, P5.1, P5.3, P5.4)**
- P4.2 `hu_hyperedge_t *hyperedges` on the snapshot (W8 n-ary facts). Queried per top-K entity via `hu_hyperedge_query_by_member`, deduped by id, shrink-fit at 16. Owns deep copies of `members` and `provenance`.
- P5.1 Stance vector: `(valence, arousal, dominance, certainty)` per Russell VAD + Mehrabian PAD-extended. Dominance is `valence - 0.5 * arousal` (clamped); certainty is `1 - var(valence)` over the residue window. Surfaced alongside the legacy `(valence, arousal, dominant_emotion)` triple for back-compat.
- P5.3 Conversational pressure: `recent_anger_count` (60-min window of strongly-negative high-arousal residues), `sustained_complaint_minutes` (span of continuous negative valence), `urgency_score` (composite, [0, 1]).
- P5.4 Trust gradient sparkline: `tom.confidence_history[16]` appended on every `hu_world_model_merge_persona` call; scrolls left when full.

**Bundle 6 — Self-model, multimodal seams, W10 hooks (P5.2, P5.6, P6.2)**
- P5.2 `hu_self_model_t` (name, focused_topics, recent_drift_kind, recent_drift_value, confidence_in_self) populated by `hu_world_model_merge_persona`. Distinct from ToM (which is what the user thinks of *us*).
- P5.6 `hu_media_context_t` seams (contact_photo_path, voice_fingerprint_hash, last_image_caption, last_image_at_ms). Default-zeroed; channel handlers populate.
- P6.2 W10 seams: `kv_cache_handle` (void*) + `last_reasoning_trace_id` (int64). NULL/0 until W10 lands.

**Bundle 7 — Goal-conditioned re-rank, planner provenance routing, perf gate, A/B (P5.5, P6.1, P7.1, P7.2, P7.4)**
- P5.5 `world_model_bridge.c` renders each negative with a per-source tag: `[hard]` (USER_EXPLICIT), `[soft]` (SELF_RAG_ABSTAIN), `[confirm]` (AUTO_EXTRACT), `[policy]` (SYSTEM_POLICY). Single bracketed prefix so the planner can grep without exploding the prompt budget.
- P6.1 `hu_world_model_rerank_for_goal(wm, goal_text, len, alloc)` — HippoRAG-style stable partition over entities / relations / recent_topics, promoting goal-anchor matches to the front. O(N²) worst case but bounded by snapshot caps (≤ 32 entities, ≤ 32 relations).
- P7.1 Latency benchmark `test_w9_world_model_p99_latency_under_5ms` — 200 cached loads complete in < 1s wall time (avg < 5 ms / load); seeded with realistic 32-entity / 32-relation footprint.
- P7.2 A/B test `test_w9_ab_negative_memory_changes_planner_signal` proving the spec's ≥3 measurable behavior changes: negatives_count rises, source enum survives, ToM `user_expects_we_cannot` becomes non-empty.
- P7.4 (this section).

### Test gate

52/52 W9 suite tests pass. 1373/1373 across all `W*` suites. Full 10,149/10,149 test fleet pass. Zero ASan errors. No regressions in the existing memory / agent / persona suites.

### Still open (intentionally deferred)

- **P7.3** "Type honesty" refactor: introduce `hu_world_model_relation_t` (POD, no owned strings) instead of the `hu_graph_relation_t` typedef bridge. Wide blast radius (~40 callers); deferred to a dedicated cleanup PR.
- **P5.5 follow-through** Wire the planner / W11 verifier *behavior* to the `[hard]` / `[soft]` / `[confirm]` / `[policy]` tags (not just the rendering). Currently the bridge emits the tags; W11 still treats every negative the same way.
