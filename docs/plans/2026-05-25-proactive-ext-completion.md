# Plan — Complete the proactive_ext subsystem (F30 / F31 / F129)

**Drafted:** 2026-05-25 (post-audit followup)
**Status:** Deferred — needs design + storage layer, not a wire
**Discovered:** library-only-module audit, 2026-05-24 session

## What's actually missing

`src/agent/proactive_ext.c` ships a fully-tested compute layer for three
distinct subsystems. Two of the five sub-feature heuristics in the
module ARE wired in production; three are not, and the unwired three
share a common gap: **no data-collection layer feeds them.**

| Sub-feature | Function(s) | Wired? | Gap |
|-------------|-------------|--------|-----|
| F123 reciprocity throttling | `hu_reciprocity_budget_multiplier` | ✅ daemon.c:1073 | — |
| F124 busyness simulation | `hu_busyness_budget_multiplier` | ✅ daemon.c:1076 | — |
| F129 "did I tell you?" disclosure | `hu_disclosure_decide`, `hu_disclosure_build_prefix` | ❌ | No invocation policy decided |
| F30 spontaneous curiosity | `hu_curiosity_query_sql`, `hu_curiosity_score`, `hu_curiosity_topic_deinit` | ❌ | No `curiosity_topics` table created or populated |
| F31 callback opportunities | `hu_callback_query_sql`, `hu_callback_score`, `hu_callback_opportunity_deinit` | ❌ | No `callback_opportunities` table created or populated |

The orchestrator `hu_proactive_ext_build_prompt` ties F30 + F31 together
but only renders non-empty output when at least one curiosity topic or
callback opportunity has been ingested. With no ingest path, the
function silently returns an empty buffer if wired.

## Why a wire alone doesn't help

Shipping `if (curiosity_count > 0) PHASE6_APPEND(...)` in daemon.c looks
honest but is a no-op until someone:

1. Builds a curiosity-topic extractor (probably from chat history —
   what topics did the user mention that we haven't followed up on?)
2. Writes the SQL CREATE TABLE + INSERT path for `curiosity_topics`
3. Builds a callback-opportunity scanner (what statements deserve a
   follow-up? "you said you were nervous about the interview — how'd
   it go?")
4. Writes the SQL CREATE TABLE + INSERT path for `callback_opportunities`
5. Decides WHEN disclosure should fire (per-message? per-day?
   per-confidence-threshold?)

Items 1-4 are research + design + implementation work, not wiring.
Item 5 is a behavioral product decision (when do we volunteer info?).

## Recommended next step

Spawn this as a separate sprint design (Sprint 54 or later) with
sub-stories along the lines of:

- **F30.1**: curiosity-topic extractor from conversation history
  (probably an LLM-driven daily batch job, similar to the
  episodic-memory consolidation pass)
- **F30.2**: SQL schema + storage + a daemon tick that populates the
  table once per day from F30.1 output
- **F30.3**: prompt-injection wire (the easy ~30 LoC piece — write
  LAST when the data exists)
- **F31.1-F31.3**: same shape for callback opportunities
- **F129.1**: invocation policy spec — under what conditions does the
  agent volunteer unsolicited info? Confidence threshold? Time-since-
  last-disclosure? Per-channel rules? This is design, not code.
- **F129.2**: wire `hu_disclosure_decide` + `hu_disclosure_build_prefix`
  into outbound message construction once F129.1 is decided.

Estimated size: ~2000 LoC + a real design pass on F129.1. One full
multi-session sprint.

## Why this defer is honest

A wire that always emits empty output is worse than no wire — it
implies the feature exists when it doesn't. Per
`~/.claude/rules/silent-config-gated-subsystems.md`, the right answer
when a feature isn't really shipping is to NOT ship a sham — log the
gap, file the design work, and let the next sprint do it properly.

## Not in scope here

- **Wiring busy/reciprocity** — already done. Don't re-touch.
- **Wiring the build_prompt as a no-op** — explicitly avoided per the
  rationale above.
- **Building the data-collection layer** — that's the deferred sprint.

## Related

- `~/.claude/rules/audit-verify-before-allege.md` — the failure mode that
  produced this finding. The audit reported "proactive_ext is library-
  only"; verification confirmed 2 of 5 sub-features are wired and the
  remaining 3 need design + storage, not just wiring.
- `~/.claude/lessons.md` 2026-05-24 entry — the post-mortem from the
  cognitive-triad false-positive that motivated this verification pass.
