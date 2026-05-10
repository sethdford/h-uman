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
