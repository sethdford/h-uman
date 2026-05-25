---
title: "Prompt-Budget Compression — Requirements"
created: 2026-05-25
status: draft
owner: TBD
sprint: TBD
supersedes: (none — memory referenced this dir but it didn't exist until now)
---

# Prompt-Budget Compression — Requirements

**Motivation.** The 2026-05-25 director-compression audit
([audit-director-compression.md](../2026-05-25-initiative-layer/audit-director-compression.md))
established that the director does NOT compress the 25+ rich context
fields — it appends a 512-char directive to `conversation_context`.
The audit named three suspects for the "feels mechanical" symptom:

1. **Recency dominance** — the most informative fields sit early in
   the system prompt, then 16+ KB of structured context arrives, then
   `conversation_context` (the actual recent dialogue) lands LAST.
   Gemini 3.x's recency bias makes the early rich fields effectively
   invisible.
2. **Dead populators** — some context fields are wired-but-empty
   most of the time (e.g. `somatic_context`, `rupture_context`).
3. **Model can't use 18 KB for 80-token outputs** — the prompt:reply
   ratio is 200:1; the model's thinking phase processes the context
   but produces a generic short reply anyway.

This spec **does not solve all three at once.** It builds the
*infrastructure* — a measurable prompt-budget module — so subsequent
sprints can target each suspect with evidence rather than guesses.

The name "director compression" in memory was a misnomer. The real
work is a `prompt_budget` module that observes, classifies, and
optionally trims the rich-context input to `hu_prompt_build_system`.

## Goals

1. **Per-field byte accounting.** Every call to `hu_prompt_build_system`
   emits a structured count of (field_name, bytes_contributed) for
   every one of the 27 fields documented in the audit.
2. **Per-field dead-field detection.** Fields that contribute 0 bytes
   (or fewer than 16 bytes — likely a header-only stub) for ≥80% of
   recent invocations are tagged DEAD and surfaced via `human doctor`.
3. **Conservative trimming policy.** When `cfg->prompt_budget.enabled`
   is true, the builder drops DEAD-tagged fields from the system
   prompt entirely. The trim is structural (skip the appender call),
   not content-level (no LLM-driven summarization).
4. **JSON-tunable parameters.** Every threshold (% dead, min bytes,
   field allowlist/denylist) is configurable via `prompt_budget` in
   `config.json`. Defaults preserve current behavior (gate OFF).
5. **Observable via existing /v1/doctor JSON output.** The doctor
   adds a `prompt_budget` check that reports per-field statistics for
   the operator to inspect.

## Non-Goals

- **LLM-driven compression.** Calling a model to summarize context
  would add latency and cost without a measurement to prove it helps.
  Phase 1 ships measurement only; LLM-driven compression is a future
  Phase 2+ if data shows it's needed.
- **Recency rearrangement.** Reordering fields (e.g. putting
  conversation_context FIRST) is a separate experiment, not a
  budgeting one. Track in a sibling spec.
- **Director rewrite.** The director's 512-char meta-instruction is
  fine as-is. The audit confirmed director isn't the bottleneck.
- **Per-turn dynamic budgeting.** Static gate on/off + DEAD-field
  pruning is enough for Phase 1. Token-cost-aware dynamic budgeting
  is Phase 2+.

## Acceptance Criteria

**AC-1: Per-field byte instrumentation.**
Every call to `hu_prompt_build_system` populates a
`hu_prompt_field_stats_t[27]` array with `(name, bytes_contributed)`
entries. Callers can opt-out by passing `NULL` for the stats out-
param. Verified by a unit test that builds a system prompt with
known fixture context and asserts the per-field byte counts match a
golden table.

**AC-2: DEAD-field detection.**
A new `hu_prompt_budget_t` opaque state object accumulates per-field
byte counts across calls. After N calls (default N=100, configurable),
fields whose mean contribution is <16 bytes are tagged DEAD. Pure
function over the count history; testable without invoking the
builder.

**AC-3: Conservative trimming gate.**
When `cfg->prompt_budget.enabled=true`, `hu_prompt_build_system`
skips appender calls for DEAD-tagged fields entirely. The system
prompt comes out shorter; the LLM payload is smaller. Pinned by an
integration test that builds the prompt twice (gate off / on) and
asserts byte-count difference equals the DEAD-field sum.

**AC-4: JSON-tunable.**
`config.json::prompt_budget` accepts:
```json
{
  "enabled": false,
  "dead_field_min_bytes": 16,
  "dead_field_sample_count": 100,
  "field_allowlist": [],
  "field_denylist": []
}
```
All keys optional; missing keys use the documented defaults. Schema
parsed in `src/config_parse.c::parse_prompt_budget` (new function).

**AC-5: Doctor check exposes statistics.**
A new doctor check `prompt_budget` registered in
`src/doctor/registry.c` reports per-field statistics via
`detail_json`. The text output shows DEAD field count; `--json`
shows the full per-field table. Operator can run `human doctor` and
see which fields are wired-but-dead.

**AC-6: Silent-failure defense.**
Per `~/.claude/rules/silent-config-gated-subsystems.md`, when
`prompt_budget.enabled=false`, the builder emits one-shot info log
at first invocation naming the key to flip it on. (Pattern from
commit `48372778`.)

**AC-7: Zero behavioral change when disabled.**
With `prompt_budget.enabled=false` (the default), system prompts are
byte-for-byte IDENTICAL to today's output. Pinned by a golden-file
test that hashes the system prompt for a fixture turn.

## Risks

- **R-1: Per-field instrumentation overhead.** 27 string-copy + size
  measurements per turn. Mitigation: use `__builtin_strlen` or
  pre-known constants; measure overhead in a benchmark before
  shipping.
- **R-2: DEAD-field classification false-positives.** A field that
  fires only on relationship ruptures (low base rate) could be tagged
  DEAD even though it's load-bearing when it DOES fire. Mitigation:
  allowlist mechanism (AC-4); also surface `last_non_empty_at` per
  field so rare-but-real fields can be excluded by hand.
- **R-3: Premature optimization.** Compressing the prompt before
  proving the rich context helps is wasted work. Mitigation: Phase 1
  ONLY measures. Whether to act on the data is a separate sprint
  decision.

## Dependencies

- `src/agent/prompt.c::hu_prompt_build_system` — requires non-
  intrusive extension to populate the stats array.
- `src/config_parse.c` — new top-level config key.
- `src/doctor/registry.c` — new check registration.
- The 27 field-appender functions in agent_turn.c — only need to
  return their byte counts; no signature changes.

## Relationship to Initiative Layer

The initiative-layer spec
(`docs/plans/2026-05-25-initiative-layer/requirements.md`) depends
on rich context being EFFECTIVE for LONGER outputs. If this spec's
data shows the field population is healthy, that's evidence the
initiative layer should work well. If many fields are DEAD, the
initiative layer will be just as mechanical as reactive replies —
fix the populators first.

This spec is therefore a **diagnostic prerequisite**, not a blocker:
the initiative layer can ship in parallel, but its quality bar
depends on this spec's findings.
