---
title: "Sprint 59 — Outbound Safety: SOTA Design (synthesized)"
created: 2026-05-26
status: design (awaiting user approval)
sprint: 59
inputs:
  - architecture-sketch.md (pre-research frame)
  - incident-corpus.md (24 production messages, regression corpus)
  - research-agent-a (SOTA primitives, 11 patterns evaluated)
  - research-agent-b (outbound surface audit, 12 paths)
  - research-agent-c (F25 root cause, daemon_proactive.c:424)
---

# Sprint 59 — Outbound Safety: Synthesized Design

This document is the SOTA design proposal that replaces the band-aid
commits from 2026-05-26 (`4ba65b6b`, `b0941c94`, `566faa82`). It is
informed by three deep-research agents and graded against the
24-message production-incident corpus.

## TL;DR

Two-layer fix:

1. **Upstream root-cause fix (NEW)**: Scope `hu_feed_processor_get_all_recent`
   per-contact at `daemon_proactive.c:424`. This is the actual origin of
   the Annie/Mindy/Betty cross-contact bleed. Found by Agent C.

2. **Egress validator pipeline (NEW)**: A single funnel through which
   ALL outbound messages flow. Composable stages with a typed verdict.
   Replaces `hu_outbound_sanitize` (b0941c94) and the inline `[SAFETY]`
   handling in `agent_turn.c` (4ba65b6b). Each stage is unit-tested
   against the incident corpus.

Both ship together. The upstream fix prevents the bug. The egress
pipeline catches whatever bypasses it, plus future regressions.

## Part 1 — Root cause (Agent C's finding)

### What populates the bad topic

The Annie/Mindy/Betty incident is THREE rows in `emotional_moments`:

```
id 10 | contact_id +18018285260 (Mindy) | topic "but boy I am just more lonely..."
id 11 | contact_id +18018983303 (Betty) | topic "but boy I am just more lonely..."
id 12 | contact_id +13857220896 (Annie) | topic "but boy I am just more lonely..."
```

Same topic. Three contact_ids.

The smoking gun is `daemon_proactive.c:424`:

```c
if (hu_feed_processor_get_all_recent(alloc, fdb, since_feed, 32, &stored, &scount) == HU_OK ...)
```

This call retrieves feed items from EVERY contact — no contact filter.
The "FEED AWARENESS" context block built from those items gets
appended to the proactive prompt for whichever contact `cp` is. When
the prompt is then passed to `hu_agent_turn()` at `daemon.c:2090`, the
emotional-state recorder at `agent_turn.c:7807` extracts a topic FROM
THE PROMPT and writes it with `agent->memory_session_id`
(== `cp->contact_id` of the proactive target, not the original
speaker). Three target contacts → three identical-topic rows.

### The upstream fix

Replace the call site with a per-contact variant:

```c
hu_feed_processor_get_for_contact(alloc, fdb, cp->contact_id,
                                  strlen(cp->contact_id), 32, &stored, &scount)
```

`hu_feed_processor_get_for_contact` already exists (`include/human/feeds/processor.h:121`).

Then *also* add a positive contract test that pins per-contact scoping:

```
tests/test_daemon_proactive_feed_scope.c:
  test_feed_aware_ctx_does_not_include_other_contacts_topics
```

Test arranges three feed items (one per contact), builds the proactive
prompt for contact A, asserts the assembled `feed_aware_ctx` contains
A's items only.

### Why this isn't sufficient on its own

There are 11 OTHER outbound paths (per Agent B). Cross-contact bleed
could surface in any of them in the future. We need defense in depth.
That's the egress pipeline.

## Part 2 — Egress validator pipeline

### Architecture

Single funnel. Every outbound message flows through
`hu_outbound_pipeline_run` regardless of caller path. The pipeline is
a chain of stages with a typed verdict.

```
   outbound message
        ↓
   hu_outbound_pipeline_run(ctx, msg)
        ↓
   stage[0]: strip       — char normalization (U+FFFC, RTL overrides, ZWJ)
   stage[1]: shape       — length + sentence-structure validation
   stage[2]: echo        — directive-echo detection (semantic, not hardcoded list)
   stage[3]: crosstalk   — cross-contact bleed detection
   stage[4]: persona     — Seth-voice fidelity check
   stage[5]: moderation  — violence/hate/self-harm/PII
        ↓
   hu_outbound_verdict_t { kind, reason, replacement, regenerate_hint }
        ↓
   SEND       → channel->vtable->send
   REWRITE    → use replacement, re-enter pipeline ONCE
   REGENERATE → re-prompt LLM with stricter system prompt, re-enter ONCE
   REJECT     → drop, log structured event, mark moment followed-up
```

### Tier mapping (Agent A's 3-stage SOTA pattern)

Agent A's research recommends a 3-tier cascade by cost:

| Tier | Latency | Stages it maps to |
|---|---|---|
| 1: Regex/lookup | ~1ms | strip, shape, echo (token-overlap version), crosstalk (n-gram fingerprint) |
| 2: Local semantic | ~50-100ms | persona (eval_shape_classifier reuse), echo (edit-distance from prompt) |
| 3: LLM judge | ~400-500ms | moderation (existing `hu_moderation_check`), REGENERATE prompt |

Pipeline runs tier 1 unconditionally; tier 2 only if tier 1 passes;
tier 3 only if tier 2 passes AND moderation is configured.

### Stage contracts

```c
typedef enum hu_outbound_verdict_kind {
    HU_OUTBOUND_SEND = 0,        /* Pass-through */
    HU_OUTBOUND_REWRITE,          /* Replace text, re-enter ONCE */
    HU_OUTBOUND_REGENERATE,       /* Re-prompt LLM, re-enter ONCE */
    HU_OUTBOUND_REJECT,           /* Drop, log, mark followed-up */
} hu_outbound_verdict_kind_t;

typedef struct hu_outbound_verdict {
    hu_outbound_verdict_kind_t kind;
    const char *reason;             /* static string, e.g. "directive_echo" */
    char       *replacement;        /* heap, owned by verdict; NULL unless REWRITE */
    size_t      replacement_len;
    const char *regenerate_hint;    /* static, addition to system prompt */
} hu_outbound_verdict_t;

typedef struct hu_outbound_context {
    const char *recipient_contact_id;
    size_t      recipient_contact_id_len;
    hu_persona_t *persona;
    hu_memory_t  *memory;           /* for crosstalk lookup */
    const char  *channel_name;       /* "imessage", "slack", etc. */
    enum {
        HU_OUTBOUND_PATH_REACTIVE,
        HU_OUTBOUND_PATH_PROACTIVE,
        HU_OUTBOUND_PATH_F25,
        HU_OUTBOUND_PATH_TEMPORAL,
        HU_OUTBOUND_PATH_SCHEDULED,
        HU_OUTBOUND_PATH_BURST,
    } path;
    int regenerate_budget;           /* default 1; pipeline decrements */
} hu_outbound_context_t;

typedef struct hu_outbound_message {
    char  *content;                  /* mutable; stage may rewrite via verdict */
    size_t content_len;
    const char *prompt_used;         /* for echo detection — what we asked the LLM */
    size_t      prompt_used_len;
} hu_outbound_message_t;

typedef struct hu_outbound_stage {
    const char *name;                /* "strip" / "shape" / "echo" / ... */
    hu_outbound_verdict_t (*run)(struct hu_outbound_stage *self,
                                 hu_outbound_message_t *msg,
                                 hu_outbound_context_t *ctx);
} hu_outbound_stage_t;

hu_error_t hu_outbound_pipeline_run(hu_outbound_pipeline_t *pipeline,
                                    hu_outbound_message_t *msg,
                                    hu_outbound_context_t *ctx,
                                    hu_outbound_verdict_t *out_verdict);
```

### Per-path stage selection

Different paths get different stage configs (Agent B's audit informed
this — reactive already has response_guard, so it doesn't need full
treatment):

| Path | strip | shape | echo | crosstalk | persona | moderation |
|---|---|---|---|---|---|---|
| Reactive reply | ✓ | | | ✓ | | (light) |
| Proactive check-in | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| F25 emotional | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Temporal follow-up | ✓ | ✓ | | ✓ | ✓ | (light) |
| Scheduled send | ✓ | | | ✓ | | (light) |
| Burst (sub-send) | inherit primary's verdict | | | | | |

Configs live in `src/agent/outbound/pipeline_configs.c` as static
arrays of stage names. Adding a new path is one row in a table.

### Stage implementations

Each stage gets its own file + test:

| Stage | File | Catches (from corpus) | Algorithm |
|---|---|---|---|
| strip | `src/agent/outbound/strip.c` | (none currently — defensive) | UTF-8 walker; reject U+FFFC, U+202E, U+200D, U+200B |
| shape | `src/agent/outbound/shape.c` | #1, #2, #3, #6 | Length > 200 → REGENERATE; multi-sentence + > 60 chars → REGENERATE |
| echo | `src/agent/outbound/echo.c` | #5, #7, #8, #9, #10 | Token-overlap (≥40%) with `prompt_used` → REGENERATE; standalone noun ∈ known directive vocab → REJECT |
| crosstalk | `src/agent/outbound/crosstalk.c` | #1, #2, #3, #4 | Generate n-gram (5-gram char) fingerprint of message; query `messages` table WHERE contact_id != recipient AND ts > now-7d; if Jaccard > 0.4, REJECT with `crosstalk_<other_contact>` |
| persona | `src/agent/outbound/persona.c` | #6, #11-16 | Reuse `eval_shape_classifier` from `src/persona/shape_classifier.c`; gate at fidelity < 0.5 with REGENERATE (not REJECT — false-positives hurt UX) |
| moderation | `src/agent/outbound/moderation.c` | (none in corpus, but adversarial) | Wrap existing `hu_moderation_check`; on flagged → REGENERATE with `regenerate_hint = "Avoid endorsing harm. De-escalate."` |

### Verdict semantics (key decisions)

The four verdicts are NOT arbitrary; each has a precise behavior:

- **SEND**: stage saw nothing problematic. Pipeline moves to next
  stage. If LAST stage returns SEND, message is delivered.

- **REWRITE**: stage knows the fix. E.g., `strip` removes a U+FFFC
  character — the message body changes, but the *intent* is preserved.
  Pipeline restarts at stage[0] (the rewrite might violate an earlier
  stage's contract; check again). Limit: 1 rewrite per pipeline run.

- **REGENERATE**: stage saw garbage that re-prompting could fix. E.g.,
  `shape` saw a 200-char sentence-fragment topic and wants a fresh
  generation with `regenerate_hint = "Reply must be under 80 chars,
  single phrase."` Pipeline returns up to caller, which calls the LLM
  again with augmented system prompt. Budget: 1 regenerate per
  outbound (cost-controlled).

- **REJECT**: stage saw something that cannot be fixed by rewriting or
  regenerating. E.g., `crosstalk` saw a verbatim match of another
  contact's message — there's no rewrite that makes "but boy I am just
  more lonely" appropriate to send to Mindy. Message is dropped.
  Structured log line is emitted. Emotional moment (if any) is marked
  followed-up so it doesn't retry.

### Observability

Every stage emits a structured INFO log:

```
[outbound] stage=echo verdict=regenerate reason=directive_echo prompt_overlap=0.62 contact=+13857220896 path=proactive
```

A new doctor check `hu_doctor_check_outbound_stats` exposes
per-stage-per-verdict counts (already part of the `human doctor` shape).

## Part 3 — Migration plan

### Phase A — Pipeline framework (no behavior change)

- Create `include/human/agent/outbound_pipeline.h` with the contracts above
- Create `src/agent/outbound/pipeline.c` with `hu_outbound_pipeline_run`
- Create `src/agent/outbound/pipeline_configs.c` with the per-path table
- Create six empty stage stubs that all return SEND
- Add tests/test_outbound_pipeline.c — pipeline runner correctness
  (visits all stages in order, handles REWRITE / REGENERATE / REJECT,
  budget enforcement)
- Compile only; do NOT wire yet. Verify nothing regresses.

### Phase B — Stage implementations (still not wired)

For each stage in order strip → shape → echo → crosstalk → persona → moderation:

1. Implement the stage
2. Write `tests/test_outbound_<stage>.c` with at MINIMUM:
   - Every corpus case the stage is responsible for (from incident-corpus.md "Coverage" table)
   - Every corpus PASS case (the stage MUST NOT false-positive)
   - At least 3 adversarial-but-not-observed cases (e.g., RTL override, ZWJ, ZW spaces)
3. Run full suite. Stage must be green before moving to next.

### Phase C — Upstream fix at daemon_proactive.c:424

This is the actual root cause. Ship as its own commit:
- Replace `hu_feed_processor_get_all_recent` with `hu_feed_processor_get_for_contact`
- Add positive-contract test pinning per-contact scoping
- Verify all 3 corpus rows in `emotional_moments` would no longer be created

### Phase D — Wire pipeline into proactive check-in path

Wire `hu_outbound_pipeline_run` into `daemon.c` proactive send (line ~2122)
with `path = HU_OUTBOUND_PATH_PROACTIVE`. Run full corpus:

- Every REJECT case must be blocked
- Every PASS case must be delivered
- BORDERLINE cases (#17, #18) discussed with user — recommend REGENERATE-with-hint

### Phase E — Wire pipeline into remaining paths

- F25 (daemon.c:1208) — path=HU_OUTBOUND_PATH_F25
- Temporal follow-up — path=HU_OUTBOUND_PATH_TEMPORAL
- Scheduled send — path=HU_OUTBOUND_PATH_SCHEDULED
- Burst sub-sends inherit verdict

### Phase F — Delete band-aids (preserve git history)

Revert the *content* of the three band-aid commits, with descriptive
messages that record the migration:

- `4ba65b6b` — `[SAFETY]`-block replacement in `agent_turn.c` →
  superseded by `moderation` stage with REGENERATE
- `b0941c94` — `hu_outbound_sanitize` → superseded by `strip` + `echo`
  stages
- `566faa82` — F25 topic-shape gate → superseded by `shape` stage +
  Phase C upstream fix

`hu_outbound_sanitize` symbol stays in headers as a `__deprecated`
wrapper that calls the pipeline, for one release, then deleted in
Sprint 60.

## Part 4 — Test discipline

Tests are organized by stage and by corpus:

```
tests/test_outbound_pipeline.c           — pipeline runner (no stages)
tests/test_outbound_strip.c              — strip stage; covers U+FFFC, RTL, ZWJ
tests/test_outbound_shape.c              — shape stage; covers corpus #1,#2,#3,#6
tests/test_outbound_echo.c               — echo stage; covers corpus #5,#7,#8,#9,#10
tests/test_outbound_crosstalk.c          — crosstalk stage; covers corpus #1,#2,#3,#4
tests/test_outbound_persona.c            — persona stage; covers corpus #6,#11-16
tests/test_outbound_moderation.c         — moderation stage
tests/test_outbound_corpus_regression.c  — runs ALL 24 corpus rows through assembled pipeline
tests/test_daemon_proactive_feed_scope.c — Phase C upstream fix
```

The corpus-regression test is the END-TO-END gate: it instantiates a
proactive pipeline, walks all 24 corpus rows, asserts each one ends
in the documented verdict. If you change a stage, you find out
whether you broke anything by running this single test.

## Open design questions for user

Before writing any code, please confirm or revise:

### Q-1: REGENERATE budget

The pipeline regenerates at most ONCE before falling back to REJECT.
Cost: ~1 LLM call per blocked send. Alternative: 0 (everything that
fails goes straight to REJECT — cheaper, harsher UX). Recommendation:
**1** (matches industry SOTA per Agent A).

### Q-2: Crosstalk detector cost

Recommendation: **char-5-gram Jaccard over messages from last 7 days
WHERE contact_id != recipient**. This is ~10ms per outbound for a
typical message volume. Cached n-gram set per contact, invalidated on
new inbound. Alternative: full embedding similarity (much more
expensive, marginally more accurate).

### Q-3: Persona stage threshold

Recommendation: **fidelity < 0.5 → REGENERATE (not REJECT)**. False
positives in persona detection would block legitimate messages.
Better to ask the LLM to try again than drop. The eval harness
proved fidelity 0.586 → 0.856 with v4-repair, so 0.5 is a clear
"this doesn't match Seth at all" threshold.

### Q-4: Moderation latency budget

Existing `hu_moderation_check` runs locally (~10ms). Recommendation:
**always run for proactive/F25/temporal; skip for reactive** (where
response_guard already handles the reactive case). Phase A's
`pipeline_configs.c` table reflects this.

### Q-5: Cross-path consolidation

Should reactive replies be moved into this pipeline too, displacing
`response_guard.c`? Recommendation: **NO, not in Sprint 59**.
`response_guard` works and has its own well-tested invariants. Sprint
60 can revisit after we have 2 weeks of production data on the new
pipeline. Risk: divergence between reactive and unsolicited paths.
Mitigation: shared `strip` and `crosstalk` stages are used by both
(crosstalk stage is the only one wired into reactive in Sprint 59).

## Acceptance criteria (graded against corpus)

For each REJECT case (#1-16): outcome = REJECT or REGENERATE.
For each BORDERLINE case (#17-18): outcome = REGENERATE.
For each PASS case (#19-24): outcome = SEND.

All 24 cases are asserted in `tests/test_outbound_corpus_regression.c`.
The test name documents WHICH stage caught each case, so a future
maintainer sees the coverage table without reading the corpus doc.

## What this design explicitly does NOT do

- **No new LLM endpoint.** All stages use existing local primitives
  (eval_shape_classifier, hu_moderation_check, token-overlap regex).
- **No new dependency.** Pure C, in-tree, no curl, no library bindings.
- **No new persona JSON fields.** The pipeline reads existing persona +
  contact_profiles structures.
- **No silent disable.** Per `silent-config-gated-subsystems.md`,
  pipeline emits one-shot info log on startup with stage list and
  config status.
- **No retry loops.** REGENERATE budget is 1, hard cap.
- **No prod telemetry beyond stage logs.** Stats are exposed via
  doctor, not shipped offsite.

## Next step

Awaiting user approval on Q-1 through Q-5. After approval, Phase A
starts immediately (pipeline framework, no behavior change).
