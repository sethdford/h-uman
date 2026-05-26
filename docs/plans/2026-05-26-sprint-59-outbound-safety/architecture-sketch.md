---
title: "Outbound Safety — Architecture Sketch (PRE-RESEARCH)"
created: 2026-05-26
status: draft / awaiting research findings
sprint: 59
---

# Outbound Safety Architecture — Sketch

**This document is a STARTING SKETCH** drafted before the deep-research
findings land. It establishes the frame for evaluating those findings.
The final architecture will be refined based on:

- SOTA primitives identified by the research agent
- Audit of h-uman's current outbound surface (how many paths, what
  validators each has today)
- Root cause of the F25 cross-contact bleed (what's the actual
  upstream populator doing)

The band-aid commits shipped 2026-05-26 (`4ba65b6b`, `b0941c94`,
`566faa82`) are HOLDING the line in production. Sprint 59's job is
to replace them with a clean, principled, tested design — same
behavior, better structure, no hardcoded blocklists.

## Why the band-aids must be replaced

Each band-aid violates a clean-code principle:

| Commit | Band-aid pattern | Why it's wrong |
|---|---|---|
| `4ba65b6b` | Replace flagged response with canned decline ("rather not get into that one") | Loses the context — sometimes the LLM said something fine that just contained a violence-related word. Proper fix: regenerate with sterner prompt. |
| `b0941c94` | Hardcoded list of directive-echo prefixes ("[SAFETY]", "reference something specific...") | Brittle — every new directive needs the list updated. Proper fix: semantic detection (edit-distance from prompt, classifier, n-gram overlap). |
| `566faa82` | Reject F25 if topic > 60 chars or has sentence punctuation | Symptom-not-cause. The TOPIC populator upstream is producing sentence-shaped content. Proper fix: scope the populator per-contact + validate topic shape at extraction time, not send time. |

## Architecture vision (subject to revision)

**A composable outbound-validation pipeline.** Every send from every path
flows through the SAME pipeline. The pipeline is a chain of stages, each
with a clear contract:

```
   outbound message
        ↓
   [hu_outbound_pipeline_run]
        ↓
   Stage 1: hu_outbound_strip       — character normalization
   Stage 2: hu_outbound_shape       — length + sentence structure validation
   Stage 3: hu_outbound_echo        — semantic check: did LLM echo its prompt?
   Stage 4: hu_outbound_crosstalk   — content from a different contact?
   Stage 5: hu_outbound_persona     — does this match Seth's voice?
   Stage 6: hu_outbound_moderation  — violence/hate/self-harm/PII
        ↓
   Verdict: SEND | REWRITE(new_text) | REJECT(reason) | REGENERATE(stricter_prompt)
        ↓
   if SEND: channel->vtable->send(...)
   if REWRITE: replace text, re-enter pipeline once
   if REJECT: drop, log, mark moment followed-up
   if REGENERATE: re-prompt LLM, re-enter pipeline once (then REJECT if loops)
```

### Per-path pipeline configs

Not all paths need all stages. A pipeline config selects stages per
caller:

| Path | Stages active |
|---|---|
| Reactive reply (response_guard already does most) | strip + crosstalk + moderation (light) |
| Proactive check-in | full pipeline |
| F25 emotional check-in | full pipeline + topic-shape pre-check at extract time |
| Temporal follow-up | strip + shape + moderation |
| Scheduled send | strip + crosstalk + moderation |
| Burst (sub-sends of a single reply) | inherit primary's verdict |

### Stage contract

```c
typedef enum hu_outbound_verdict_kind {
    HU_OUTBOUND_SEND = 0,
    HU_OUTBOUND_REWRITE,
    HU_OUTBOUND_REJECT,
    HU_OUTBOUND_REGENERATE,
} hu_outbound_verdict_kind_t;

typedef struct hu_outbound_verdict {
    hu_outbound_verdict_kind_t kind;
    const char *reason;            /* static, borrowed; e.g. "directive_echo" */
    const char *replacement;       /* heap, owned by verdict; NULL unless REWRITE */
    size_t      replacement_len;
    const char *regenerate_hint;   /* static, borrowed; system-prompt addition */
} hu_outbound_verdict_t;

typedef struct hu_outbound_stage {
    const char *name;              /* "strip" / "shape" / "echo" / etc. */
    hu_outbound_verdict_t (*run)(struct hu_outbound_stage *self,
                                 hu_outbound_message_t *msg,
                                 hu_outbound_context_t *ctx);
} hu_outbound_stage_t;
```

Each stage is a single file under `src/agent/outbound/`:
- `outbound/strip.c`
- `outbound/shape.c`
- `outbound/echo.c`
- `outbound/crosstalk.c`
- `outbound/persona.c`
- `outbound/moderation.c`

Each ships with its own unit test in `tests/test_outbound_<stage>.c`.

### Tested + composable + observable

- Every stage has a fixture test ("given message X, returns verdict Y")
- The pipeline runner emits structured logs at INFO for each stage's
  verdict so operators can grep `[outbound] stage=echo verdict=reject
  reason=directive_echo contact=+1234`
- A `/v1/outbound/stats` doctor check exposes per-stage rejection counts

## Open design questions (for user input after research lands)

1. **REGENERATE budget**: should the pipeline call an LLM to regenerate
   when a stage rejects? Cost vs UX tradeoff. Maybe: regenerate at most
   ONCE per outbound, then REJECT.
2. **Crosstalk detector**: per-contact recent-conversation lookup is
   expensive at send time. Should we cache per-contact n-grams in
   memory or do SQL on every send?
3. **Persona stage**: the eval_shape_classifier already scores
   persona-fidelity. Should the pipeline use it as a gate (with what
   threshold) or just as a logging signal?
4. **Moderation latency**: existing `hu_moderation_check` runs locally.
   Cost per turn?
5. **Cross-path consolidation**: should reactive replies ALSO be moved
   to this pipeline, displacing response_guard? Or two pipelines coexist?

## Migration plan (post-research)

Phase A: Build the pipeline framework + stages
Phase B: Wire one path through the pipeline (proactive check-in)
Phase C: Migrate F25 + temporal + scheduled
Phase D: Migrate reactive reply (displace response_guard if it makes sense)
Phase E: Delete the band-aid commits (revert by content, not by SHA — preserve git history)
