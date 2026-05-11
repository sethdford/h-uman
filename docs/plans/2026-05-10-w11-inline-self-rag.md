---
title: "W11 — Inline Self-RAG with Abstention: hu_self_rag_t vtable, control tokens, refusal head"
created: 2026-05-10
status: proposed
parent: 2026-05-10-memory-v2-roadmap-overview.md
risk: high
scope: include/human/agent/, src/agent/, src/providers/
---

# W11 — Inline Self-RAG with Abstention

## Goal

Move verification *into* generation. The agent emits `<retrieve>`, `<critique>`, `<refuse>` control tokens during the chat call, decomposes claims into atomic noun-phrase units, and chooses to **abstain** rather than hedge when no evidence supports a claim. v1's heuristic verifier becomes one fallback backend behind a vtable; the new inline backend is the default when the active provider supports control tokens.

## Motivation

v1 W4 verifies *after* the draft is generated. Hedging "I'm not 100% sure but…" is honest but weak. The frontier — Self-RAG (Asai et al. 2023, refined 2024-25), Atlas, Self-CRITIC — runs verification interleaved with generation, decomposes claims atomically, and produces an explicit refusal/abstention path.

Today's verifier treats whole sentences as atomic. "Alice works at Acme since 2024 in NYC" is three claims; we verify it as one. SOTA decomposes into noun-phrase atomic claims and verifies each.

Today's verifier never says "I don't know." It always emits *something*. Honesty calibration research (TruthfulQA, Selective QA) shows that a refusal mechanism is the single biggest hallucination reducer.

## Prior art

- Self-RAG (arxiv 2310.11511) — control tokens during generation.
- Self-CRITIC, Atlas — atomic claim decomposition.
- TruthfulQA, Selective QA — abstention as honesty.
- v1's `hu_response_verify` — heuristic backend that becomes the fallback layer.

## Design

### Vtable

```c
/* include/human/agent/self_rag.h */

typedef enum hu_self_rag_outcome {
    HU_SELF_RAG_SUPPORTED = 0,
    HU_SELF_RAG_HEDGED = 1,
    HU_SELF_RAG_REWRITTEN = 2,
    HU_SELF_RAG_ABSTAINED = 3,
} hu_self_rag_outcome_t;

typedef struct hu_atomic_claim {
    char text[160];               /* noun-phrase claim */
    char span_start[32];          /* byte offset in draft */
    char span_end[32];
    hu_belief_t support;          /* W8: how supported */
    hu_provenance_atom_t prov;
    bool fabricated;              /* high-variance + no provenance */
} hu_atomic_claim_t;

typedef struct hu_self_rag_request {
    hu_world_model_t *wm;         /* W9: the agent's view of the user */
    const char *draft;
    size_t draft_len;
    hu_verify_mode_t mode;        /* OFF/SOFT/STRICT/INLINE */
    float abstain_threshold;      /* default 0.3 — below this, refuse */
} hu_self_rag_request_t;

typedef struct hu_self_rag_response {
    hu_self_rag_outcome_t outcome;
    char modified_draft[2048];
    bool draft_modified;
    hu_atomic_claim_t claims[32];
    size_t claims_count;
    char refusal_text[256];       /* populated when ABSTAINED */
} hu_self_rag_response_t;

typedef struct hu_self_rag_vtable {
    const char *name;
    hu_error_t (*verify)(void *ctx, const hu_self_rag_request_t *req,
                         hu_self_rag_response_t *resp);
    void (*deinit)(void *ctx);
} hu_self_rag_vtable_t;

typedef struct hu_self_rag {
    hu_self_rag_vtable_t *vt;
    void *ctx;
} hu_self_rag_t;

/* Three backends: */
hu_error_t hu_self_rag_heuristic(hu_memory_t *m, hu_self_rag_t *out);  /* v1 path */
hu_error_t hu_self_rag_atomic(hu_memory_t *m, hu_provider_t *embedder,
                               hu_self_rag_t *out);                    /* claim decomposition */
hu_error_t hu_self_rag_inline(hu_memory_t *m, hu_provider_t *chat,
                               hu_self_rag_t *out);                    /* control tokens */
```

### Inline backend protocol

When the provider advertises `supports_control_tokens = true`:

1. The chat request carries a system message extension: "You may emit `<retrieve>QUERY</retrieve>` to fetch memory; `<critique>CLAIM</critique>` to mark a claim for verification; `<refuse>REASON</refuse>` to abstain."
2. The response stream is parsed by `hu_self_rag_inline.verify`. Retrieve tags trigger `hu_memory_read` mid-stream. Critique tags trigger atomic verification. Refuse tags terminate the stream and populate `refusal_text`.
3. Output to the user is the post-processed stream (control tokens stripped; receipts inserted; refusal rendered).

### Atomic backend (provider-agnostic)

For providers without inline support, a post-hoc decomposer:
1. Splits draft into noun-phrase atomic claims via embedder + light parser.
2. Each claim verified against `hu_world_model_t` and `hu_memory_t`.
3. Sub-threshold claims either hedged (SOFT) or removed (STRICT).
4. If ≥50% of claims are sub-threshold, returns `HU_SELF_RAG_ABSTAINED`.

### Refusal text generation

Deterministic templates per refusal category:
- `unknown_fact`: "I don't have memory backing this. Want to tell me?"
- `policy`: "This is something I shouldn't say without more confidence."
- `negative_memory_match`: "Based on what I know, I'd rather not weigh in here."

## Phases

1. Move v1 `response_verifier.c` content behind `hu_self_rag_heuristic` backend (no functional change; just relocation). Vtable type defined.
2. Author atomic backend + tests.
3. Author inline backend + provider integration (one provider initially: anthropic; gemini and openai follow).
4. Wire `hu_self_rag_t` into the response path, replacing direct calls to `hu_response_verify`.
5. Add abstention rendering in channel layer.
6. Adversarial tests.

## Test plan

- `test_w11_heuristic_backend_matches_v1_behavior`: parity test.
- `test_w11_atomic_backend_decomposes_compound_claim`: "Alice works at Acme since 2024 in NYC" → 3 claims.
- `test_w11_atomic_backend_abstains_when_majority_unsupported`.
- `test_w11_inline_backend_strips_control_tokens_from_output`.
- `test_w11_inline_backend_triggers_mid_stream_retrieve`.
- `test_w11_refusal_renders_template`.
- `test_w11_adversarial_prompt_injection_to_avoid_refusal`: malicious system override fails.
- `test_w11_adversarial_atomic_claim_attack`: claim crafted to look supported via paraphrase → W8 semantic judge catches.
- `test_w11_e2e_inline_with_world_model_and_memory`.

## Success metric

- Hallucination rate on factual claims: −80% vs v1 SOFT verifier on annotated 200-prompt suite.
- Abstention rate ≥ 30% on weak-evidence prompts (vs always-emit baseline).
- Inline backend latency overhead ≤ 150 ms on warm cache.
- Binary size delta ≤ +90 KB.

## Risks

| Risk | Mitigation |
|------|------------|
| Inline control tokens depend on provider | Backend is pluggable; heuristic always works; gracefully degrades |
| Abstention frequency annoys users | `abstain_threshold` tunable; defaults conservative; A/B before raising |
| Atomic decomposition fails on poetic / metaphorical text | Skip decomposition when claim density is low; fall back to sentence-level |

## Out of scope

- Training a custom Self-RAG model. (We use existing providers.)
- Multimodal verification (verify a generated image against memory).

## Binary size budget: +90 KB.

---

## P1 — Agent-level apply seam (landed 2026-05-10)

The atomic + heuristic + inline backends and the bridge-level `hu_w11_self_rag_verify_with_provider` were already wired into `agent_turn`. P1 promotes the swap-and-bump logic into a single shared seam:

- `hu_agent_self_rag_apply(agent, draft, len, mode, *swapped, *swapped_len)` — the canonical entrypoint. Runs the verifier, transfers ownership of the swapped buffer through `*swapped` when SOFT/STRICT actually rewrote the draft, and bumps `agent->self_rag_runs / claims_total / claims_flagged / abstentions / refusals_rendered` consistently. Returns `HU_OK` with `*swapped == NULL` when no swap occurred; returns `HU_ERR_INVALID_ARGUMENT` when the agent is not bound to a W7 facade + memory session.
- `hu_agent_self_rag_telemetry(agent, *runs, *abstentions, *refusals_rendered, *claims_total, *claims_flagged)` — read-only snapshot. Tolerant of `NULL` agent (zeros every non-NULL out) so the daemon `/status` JSON can query before the agent is bound.

`agent_turn.c` and `agent_stream.c` both call the same code path now:

- `agent_turn` calls `hu_agent_self_rag_apply` and adopts the returned buffer when non-NULL.
- `agent_stream`'s streaming `<refuse>` path swaps to the deterministic `HU_REFUSAL_POLICY` template AND bumps `self_rag_refusals_rendered` (previously only `self_rag_abstentions` was incremented, silently undercounting streaming refusals).

Tests (`tests/test_w11_self_rag.c::W11 inline self-RAG with abstention`):

- `test_w11_agent_self_rag_apply_swaps_under_soft_and_bumps_counters` — empty graph + fact-shaped draft → ABSTAINED → swapped buffer matches `HU_REFUSAL_UNKNOWN_FACT`; `runs == abstentions == refusals_rendered == 1`.
- `test_w11_agent_self_rag_apply_telemetry_does_not_swap` — TELEMETRY mode increments `abstentions` but never `refusals_rendered`.
- `test_w11_agent_self_rag_apply_off_short_circuits` — OFF mode never runs the verifier.
- `test_w11_agent_self_rag_apply_rejects_unbound_agent` — guards the precondition contract.
- `test_w11_agent_self_rag_telemetry_handles_null_agent` — zero-fills every out under NULL agent.

### Remaining scope

- Calibrate `abstain_threshold` against the 200-prompt annotated suite so the success metric ("≥30 % abstention on weak-evidence prompts") is measurable — bridge already wires `claims_total/flagged` so a smoke harness can read them via `hu_agent_self_rag_telemetry`.
- Inline backend control-token wiring for additional providers (currently only the placeholder protocol parser is exercised end-to-end).

---

## P2 — Inline backend memory-backed scoring (landed 2026-05-10)

The inline backend previously initialized every claim's `support` belief to a hardcoded `hu_belief_init(0.0f, kind, now)`, which meant the abstention path (and any consumer that read `claim.support.mean`) was acting on a value that always lied about what memory said. P2 replaces that placeholder with real per-claim scoring:

- **`<critique>` claims** flow through `hu_response_verify` configured with `max_claims=1`, mirroring the atomic backend's `score_atomic_claim`. The token-overlap score against the contact's relations becomes `support.mean`; sub-floor scores set `claim.fabricated`.
- **`<retrieve>` claims** issue a single `hu_memory_facade_read` for the contact's relations and use record count (saturated at 5) as the support proxy. A future revision can grade each record against the query string with `hu_crag_grade_document`.
- **W9 single-load lift**: when the request supplies a `hu_world_model_t` snapshot AND its loaded entities cover the claim text (case-insensitive substring), weak SQL scores are lifted to a 0.5 floor. This is the W9 promise threaded through W11 — the verifier consumes the unified world-model artifact rather than always re-querying for entity discovery. The lift is monotone: it never lowers a strong SQL score.
- **STRICT-mode score-based abstention**: when `req->mode == HU_VERIFY_STRICT` and `abstain_threshold > 0`, the inline backend now abstains with the deterministic `HU_REFUSAL_LOW_CONFIDENCE` template once the fabricated-claim ratio crosses the threshold. Existing INLINE/SOFT callers fall through to the prior tag-stripping outcome — no behavioral change for the existing test surface.
- **Provenance preserved**: `claim.prov.source` still carries the **tag kind** (`"critique"` / `"retrieve"`) so downstream routing keeps working; the receipt source is appended as a secondary provenance atom on the belief itself, and `claim.prov.weight` now equals `claim.support.mean` instead of always being 0.

Tests added in `tests/test_w11_self_rag.c`:

- `test_w11_inline_critique_supported_when_memory_matches` — seeded relation, claim aligns → `support.mean ≥ 0.6`, not fabricated, prov.weight ≥ 0.6.
- `test_w11_inline_critique_fabricated_when_memory_empty` — empty memory → fabricated, but INLINE-mode does NOT auto-abstain (preserves prior contract).
- `test_w11_inline_retrieve_score_reflects_grade_relevance` — seeded memory + matching query → score > 0, belief carries `inline-probe-graded` source (was previously `inline-probe` when scoring was count-only).
- `test_w11_inline_retrieve_irrelevant_query_scores_low` — seeded memory + non-matching query → score < 0.2, fabricated, belief carries `inline-probe` source. Demonstrates the grader distinguishes "memory is empty" from "memory has stuff but none of it matches."
- `test_w11_inline_strict_abstains_on_score` — STRICT + 0 evidence → ABSTAINED + LOW_CONFIDENCE template.
- `test_w11_inline_strict_supported_when_evidence_present` — STRICT + seeded memory → no abstention.
- `test_w11_inline_wm_entity_match_lifts_weak_score` — entity present in WM but no relation → SQL score below floor → WM lift bumps to 0.5 with `inline-wm-lift` provenance source.
- `test_w11_inline_wm_no_match_leaves_score_unchanged` — claim mentions an entity NOT in WM → no lift → primary source remains `inline-graph`.

### Remaining scope (after P2)

- True streaming control-token integration with each frontier provider (Anthropic, Gemini, OpenAI). The deterministic protocol parser still drives the in-process tests; live provider wiring is independent of the scoring path landed in P2.
- Calibration of `abstain_threshold` against the 200-prompt annotated suite (carried over from P1).

---

## P3 — Grade-aware retrieve scoring (landed 2026-05-10)

`inline_score_retrieve` previously scored on record count alone (saturated at 5 → 1.0), so a contact with 16 unrelated relations would have read as "fully supported" for any retrieve query. P3 replaces that with per-record grading via `hu_crag_grade_document` (the same token-overlap grader the atomic backend's STRICT-mode corrective-RAG path already uses):

- Each loaded relation's `context` text is graded against the retrieve query.
- `HU_RAG_RELEVANT` records contribute their full `score` to the support sum.
- `HU_RAG_AMBIGUOUS` records contribute half.
- `HU_RAG_IRRELEVANT` records contribute zero.
- Sum saturates at 1.0; `c->fabricated` flips when the final score is below 0.2 (the same threshold the grader uses to separate AMBIGUOUS from IRRELEVANT).
- When the query is empty (e.g. `<retrieve></retrieve>`), the prior count-only fallback still applies so empty-tag retrieves don't auto-fabricate.
- Belief source flips to `inline-probe-graded` when at least one record graded RELEVANT, otherwise stays `inline-probe`. Consumers can introspect to tell apart "graded matches" vs "presence-only fallback."

Cost: O(N) grading calls for N ≤ 16, each is bounded token-overlap scoring (no SQL, no allocation per grade). Empirically ~50–200 µs for the full sweep on typical contacts.
