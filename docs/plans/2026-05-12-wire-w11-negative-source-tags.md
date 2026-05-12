---
title: "Wire W11 verifier to honor negative source tags"
created: 2026-05-12
status: draft
sprint: 2c
story: A
owner: agent
worktree: /Users/sethford/Documents/human-story-2c-A
branch: story-2c-A-w11-negative-tags
---

# Wire W11 verifier to honor negative source tags

## Problem statement

The W9 world model carries four kinds of negative memory, each tagged with
`hu_negative_source_t`:

```
USER_EXPLICIT    → "[hard]"    → "I asked you not to mention this"
SYSTEM_POLICY    → "[policy]"  → "Built-in safety/compliance rule"
SELF_RAG_ABSTAIN → "[soft]"    → "I tried to verify and couldn't — soft block"
AUTO_EXTRACT     → "[confirm]" → "I heuristically extracted this — confirm?"
```

The bridge (`world_model_bridge.c`) renders the tag prefix into the prompt's
"Avoid:" section. **But that's all.** The W4 response verifier
(`src/agent/response_verifier.c`) and the W11 inline verifier
(`src/agent/self_rag_atomic.c`, `src/agent/self_rag_inline.c`) never consult
`wm->negatives` at all. Their entire job is checking claims against
*supporting* evidence in the graph; *forbidden* topics flow through the
prompt only.

This is the gap: data on the snapshot, no behavior change downstream.

## Goal

Wire the verifier so that a draft containing a claim that *hits* a negative
memory item produces an outcome shaped by the negative's `source` tag, not
just text in the prompt that the LLM may or may not respect.

## Non-goals

- Stance/pressure/recent-changes/hyperedge/self-model wiring (Stories B–F).
- LLM-side prompt-engineering changes (the prompt block already tags the
  negatives; this story is about deterministic verifier-side enforcement).
- Removing or rewriting the existing W4 supporting-evidence pass.
- Multi-language tokenization (current claim extractor is ASCII-only; we
  inherit that limitation).
- Semantic disambiguation between "medical advice" and "legal advice" when
  both share the word "advice". Documented FP class.

## Design

### Public API delta

Two new symbols in `include/human/agent/response_verifier.h`:

```c
struct hu_world_model; /* forward */

/* Entry point: verify against W7 facade AND a loaded world model. */
hu_error_t hu_response_verify_against_world_model(
    hu_allocator_t *alloc, hu_memory_facade_t *memory,
    const struct hu_world_model *wm,
    const char *contact_id, size_t contact_id_len,
    const char *draft, size_t draft_len,
    const hu_verifier_config_t *cfg, hu_verifier_report_t *out_report);

/* Public primitive: scan one claim against wm->negatives. Returns the
 * strictest implied outcome, optionally rendering the matching negative's
 * refusal/hedge text. Used by both response_verifier.c and
 * self_rag_atomic.c so both share the same matcher and source-tag mapping. */
hu_verifier_outcome_t hu_negatives_scan_claim(
    const struct hu_world_model *wm,
    const char *claim,
    char *out_refusal, size_t refusal_cap,
    char *out_hedge, size_t hedge_cap,
    bool *out_policy_hit);
```

`hu_response_verify()` becomes a thin wrapper around
`hu_response_verify_against_world_model(..., wm=NULL, ...)` so existing
callers don't move.

### Matcher

```
tokenize_lower(negative.text, len, out, cap)
   → up to `cap` lowercase tokens of ≥5 chars, non-stopword.

negative_hit_score(negative_tokens, nt, claim_text)
   → hits = count of negative tokens that appear (substring) in lowercased claim
   → score = hits / nt
   → returns score in [0, 1]

hit ⇔ score ≥ HU_NEG_MATCH_THRESHOLD          (0.3)
HU_NEG_MIN_TOKEN_LEN = 5
```

The ≥5-char minimum filters generic 4-char verbs ("give", "card", "deal")
while keeping topic words ("merger", "advice", "credit", "therapy",
"medical", "close", "timing").

0.3 catches the 1-in-3 case so a draft mentioning *one* topic word from a
three-token negative ("never discuss the merger" hit by "The merger talks
are going well") trips, while a benign claim sharing zero topic words stays
below threshold.

Stopwords (inherited from the existing W4 tokenizer):
`is, was, were, will, the, and, this, that, with, have, has, had, for,
from, your, you, they, them, i'm, i've, i'll, i'd`.

### Outcome aggregation across hits

```
worst = SUPPORTED
for each negative N in wm:
    if claim hits N:
        match = outcome_for_source(N.source)
        worst = stricter_of(worst, match)
        if worst == ABSTAIN: break    # short-circuit
return worst
```

Strictness lattice: `ABSTAIN > HEDGED > SUPPORTED`.

### Refusal & hedge templates

`static const` strings in `response_verifier.c`:

```c
#define HU_NEG_REFUSAL_HARD   "I can't help with that — you've asked me not to discuss it."
#define HU_NEG_REFUSAL_POLICY "I can't help with that — it would violate a safety policy."
#define HU_NEG_HEDGE_SOFT     "I'm not confident enough to commit to that — let me double-check first."
#define HU_NEG_HEDGE_CONFIRM  "I think we agreed not to bring this up — is that still right?"
```

When `nm->reason` is non-empty, the HARD refusal appends
" — you said: '<reason>'", width-bound to fit `refusal_text[256]`.

### Audit log for [policy]

`SYSTEM_POLICY` hits emit a single `hu_log_warn` line so security tooling
can grep for them. No allocations on the hot path; no structured-log
dependency. The full audit-log hook (`hu_w7_audit_log_*`) is out of scope
for Story A — best-effort log only.

### W11 backends

Three TUs to wire:

1. `src/agent/self_rag.c::heuristic_verify_impl` — calls
   `hu_response_verify` today. Change to
   `hu_response_verify_against_world_model(..., req->wm, ...)`. One-line
   patch.

2. `src/agent/self_rag_atomic.c::atomic_verify` — doesn't route through
   `hu_response_verify` at the top level (decomposes the draft itself and
   scores per-claim). Add a negative-scan pass after the decompose step,
   using `hu_negatives_scan_claim`. ABSTAIN-class hits short-circuit
   immediately; HEDGE-class hits mark the matching claim as fabricated so
   the existing rebuild path threads the hedge text.

3. `src/agent/self_rag_inline.c` — control-token-driven. `req->wm` already
   passed into `inline_score_critique`. Out of scope for Story A; the
   inline backend is reserved for streaming providers that emit control
   tokens, where negative-source enforcement is the model's job. Story G
   (later) covers inline integration.

### Failure / OOM modes

- `wm == NULL` or `wm->negatives_count == 0` → instant SUPPORTED return,
  zero behavior change.
- All matcher buffers are stack-allocated (`16 × 32` tokens, 256-byte
  refusal, 160-byte hedge). No heap, no OOM path.
- Refusal-text buffer overflow → snprintf width-bound to truncate cleanly.

## Risk register

| Risk | Mitigation |
| --- | --- |
| Concurrent agent on the shared sprint-2c-followups branch wipes work | Worktree isolation per F4 rule + commit early & push |
| False-positive matches (e.g. "medical advice" trips "legal advice") | Documented above. Conservative for safety, acceptable for Story A. Tunable later via telemetry. |
| Wide blast radius across self-RAG backends | Only two backends wired: heuristic (one-liner) and atomic. Inline deferred to Story G. |
| Test idiom drift (other agents adding W11 tests in parallel) | Tests use distinct names (`verifier_*` prefix) to avoid collision |

## Test inventory

7 tests, all in `tests/test_w11_self_rag.c`:

| Test | Inputs | Expected |
| --- | --- | --- |
| `verifier_hard_tag_forces_abstain` | USER_EXPLICIT, text="never discuss the merger", reason="ongoing negotiation", draft="The merger talks are going well." | outcome=ABSTAIN, refusal contains "you've asked me not to" and "ongoing negotiation" |
| `verifier_policy_tag_forces_abstain_with_safety_text` | SYSTEM_POLICY, text="give medical advice", draft="You should give medical advice about ibuprofen dosage." | outcome=ABSTAIN, refusal contains "safety policy" |
| `verifier_soft_tag_emits_low_confidence_hedge` | SELF_RAG_ABSTAIN, text="specific deal close timing", draft="The specific deal close timing is Friday." | outcome=HEDGED, modified_draft contains "not confident enough" |
| `verifier_confirm_tag_emits_ask_to_confirm_hedge` | AUTO_EXTRACT, text="bring up therapy sessions", draft="Have you been to your therapy sessions lately?" | outcome=HEDGED, modified_draft contains "is that still right" |
| `verifier_strictest_outcome_wins_when_multiple_match` | 2 negatives: SOFT + HARD, draft hits both | outcome=ABSTAIN |
| `verifier_no_negatives_in_wm_behavior_unchanged` | empty wm, draft + matching graph facts | outcome equal to `hu_response_verify` baseline |
| `verifier_w11_path_honors_hard_tag` | end-to-end through `hu_w11_self_rag_verify`, [hard] negative seeded via `hu_negative_memory_add_facade` | out_outcome=HU_W11_OUTCOME_ABSTAINED |

## Delivery checklist

- [ ] Plan reviewed (this doc).
- [ ] Worktree active (`pwd` ends in `human-story-2c-A`).
- [ ] Sprint-2c stories.md drafted.
- [ ] API delta lands in `response_verifier.h`.
- [ ] Matcher + scanner implemented (`response_verifier.c`).
- [ ] Heuristic backend threads `req->wm` (`self_rag.c`).
- [ ] Atomic backend consults `req->wm` (`self_rag_atomic.c`).
- [ ] Audit-log best-effort wired for `[policy]` hits.
- [ ] 7 tests written + registered.
- [ ] 7 tests pass.
- [ ] Full fleet stays green.
- [ ] AC checkboxes flipped in `sprints/sprint-2c/stories.md`.
- [ ] Branch pushed to remote (durable).
- [ ] Optional: PR opened.

## Open questions

- Q1: Should `[confirm]` (AUTO_EXTRACT) escalate to ABSTAIN if the same
  contact has hit the same auto-extracted negative N times without
  confirming? **Deferred:** Story F.
- Q2: `cfg->negative_match_threshold` knob? **Decision:** hardcode 0.3 for
  Story A; add knob when telemetry warrants.
- Q3: Full audit-log hook for `[policy]` hits? **Deferred:** Story A ships
  `hu_log_warn` only; structured audit log is Story H.
