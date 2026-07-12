---
title: Egress single-funnel unification + humanness-block de-blinding
status: active
created: 2026-07-12
last_audit: 2026-07-12
owner: agent/outbound + daemon
related:
  - docs/plans/2026-05-26-sprint-59-outbound-safety/design.md
---

# Egress single-funnel unification

> One outbound funnel for every send path, and zero send-mutating code the
> test suite cannot see. This finishes Sprint 59's unfinished goal.

## Why (verified 2026-07-12 egress audit)

Outbound text flows through **three uncoordinated pipelines**:

| Path | Chain | Governor / pipeline? |
|------|-------|----------------------|
| **Reactive** (every inbound→reply, hot path) | `validator_chain` + inline humanness block + splitter | NO pipeline. Governor added in-place 2026-07-12 (commit 6c691536). |
| Proactive / F25 / scheduled | `outbound_sanitize` → `hu_outbound_pipeline_run` | Full pipeline. |
| Burst fragments | `burst_egress` → pipeline | Full pipeline (Sprint 60). |

Two structural defects fall out of this fork:

1. **The reactive humanness block runs under `#ifndef HU_IS_TEST`.** The
   13,499-test suite never executes the production shaping of the
   highest-traffic path. Every 2026-07-12 incident (disfluency "wait no",
   the deliberation echoes) originated in code no test could see.
2. **Three chains drift.** The governor had to be wired twice (pipeline
   stage + reactive in-place). The next shaping feature will face the same
   double-wiring, and the two can silently diverge.

## The entanglement (why this is NOT a one-shot refactor)

Read of `src/daemon.c` ~12356–12500 found the humanness block is not a
clean unit:

- **Typo-correction state leak.** `hu_conversation_apply_typos` writes
  `original_response`, `original_len`, `typo_seed` — consumed *later*,
  outside the block, to send a follow-up "correction" bubble. Extracting
  the block must preserve or return this state.
- **Inline-reply needs history.** The F40 quoted-reply prefix consults
  `history_entries` / `combined`. Not a pure text→text transform.
- **Buffer-ownership interleave.** `fillers` and `disfluency` each
  `realloc` `response` against `response_alloc_len` in place. Any
  extraction must thread `response_alloc_len` as in/out and keep the ASan
  clean.

These are exactly the cross-dependencies that make a "just move it" edit
dangerous on the path that texts real contacts. Hence: phased, each phase
independently shippable + verified.

## Phases (each: TDD → full suite 0-fail 0-ASan → prod probe → deploy)

### Phase 1 — Extract the *pure* shapers into a testable orchestrator
Pull the order-dependent, side-effect-free shapers (`typing_quirks`,
`vary_complexity`, `fillers`, `disfluency`, `style_governor`) into
`hu_daemon_shape_text_inplace(ctx, char **buf, size_t *len, size_t *cap,
uint32_t seed)` in a **testable** TU (`src/daemon/daemon_shape.c`, compiled
in all builds). Leave `typos` (correction-state) and F40 (history) in the
daemon for now. The daemon block becomes: grow buffer → one call.
**Gain:** the shaping ORDER + gates become unit-testable. Add a golden test
that would have caught the disfluency incident (all-default gates + fixed
seed → asserted output).
**Risk:** buffer ownership. **Mitigation:** verbatim move; full suite + ASan
+ a prod probe comparing byte-identical output pre/post on 50 fixtures.

### Phase 2 — Gate the last ungated mutators
`vary_complexity` and `fillers` (fillers chip already in flight) get the
`off|shadow|live` env gate, default OFF, per
feature-gate-requires-measurement. `typing_quirks`/`typos` are already
config-gated. After this, NO send-mutating code runs un-gated + unmeasured.

### Phase 3 — De-blind the residue
Move `typos` (threading its correction-state through the orchestrator's
out-params) and F40 inline-reply (passing history in) into the tested
orchestrator. Now the ENTIRE reactive shaping chain is under test.
Delete the `#ifndef HU_IS_TEST` wrapper.

### Phase 4 — One funnel
Route reactive through `hu_outbound_pipeline_run` (or make the orchestrator
the single implementation both the pipeline stage and the reactive path
call). The pipeline's REGENERATE stages (persona/shape) need an LLM-callback
shim on the reactive path — design that interface first. Retire the
duplicate wiring. Splitter (`choreography`/`split_response`) tuned to the
measured 43% multi-bubble burst rate and gated on a blind-A/B verdict.

## Verification gate (every phase)
1. `./build/human_tests` → `N/N passed`, 0 ASan.
2. Prod build links (`Linking C executable human` + `Signing`).
3. A probe binary against `libhuman_core.a` shows the intended shaping on
   real Seth-style fixtures (the pattern used for the governor 2026-07-12).
4. Deploy via `scripts/install-human-daemon.sh`; `doctor` 5-ok.
5. Governor/disfluency gates stay at their deployed values (shadow/off).

## Non-goals
- No behavior change to what SHIPS until a blind-A/B verdict promotes a
  gate to live. Every phase is refactor-or-gate, not a live shaping change.
- Not touching the model, adapter, or memory layers — pure egress.
