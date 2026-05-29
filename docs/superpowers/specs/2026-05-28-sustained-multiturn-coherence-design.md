---
title: Rung 3 — Sustained Multi-Turn Coherence On-Device
created: 2026-05-28
status: design
---

# Rung 3 — Sustained Multi-Turn Coherence On-Device

**Date:** 2026-05-28
**Status:** Design (approved in brainstorming; feeds `/spec`)
**Author:** Seth + Claude

## Problem

Rungs 1–2 are shipped and verified: the local Gemma 4 31B + `seth-lora-v4-repair`
model gives reliable *short* replies and reliable *structured* replies on-device,
guarded by the mlx-server runaway/streaming protections (commits `689f05c`,
`9dc849c`). Those rungs prove single-turn behavior.

Rung 3 is the next claim: that the same local model **holds context, voice, and
latency across a long conversation** — not just one good reply, but twenty-five of
them in a row without forgetting what was said, drifting out of voice, or grinding
to a halt as history piles up.

Today that claim is a vibe. There is no measurement. `scripts/eval_multiturn.py`
exists but (a) runs only 3–4 turn exchanges, (b) targets a cloud model as the
thing-under-test rather than the local mlx-server, and (c) is not wired to a
pass/fail bar. This spec defines the measurement so "sustained multi-turn" becomes
a number, not an adjective.

## Goal

A **measurable bar** for sustained on-device multi-turn coherence, proven by a
nightly/manual eval tool that drives the **local** mlx-server and emits a
diagnosable verdict artifact. Where the first measurement shows breakage, spawn
evidence-driven remediation — but the deliverable of *this* spec is the harness +
the bar + the first verdict, not a pre-committed fix.

### Non-goals

- Not a per-PR CI gate (too slow/flaky given local generation + cloud judge).
- Not a context-path redesign up front. Remediation is contingent on measurement.
- Not a change to `eval_multiturn.py`'s existing short-scenario behavior.

## Decisions (locked in brainstorming)

| # | Decision | Choice |
|---|----------|--------|
| 1 | Spec scope | **Measure first, then fix what the measurement surfaces** |
| 2 | Turn depth | **20–30 user turns** (presses the 20 KB truncation, nears the 50-msg ring) |
| 3 | Judging | **LLM judge for retention + voice drift; latency wall-clock timed** |
| 4 | Latency bar | **Per-turn absolute ceiling AND bounded growth slope** |
| 5 | Scenarios | **Extend the existing 6, seeding recallable anchors** |
| 6 | Cadence | **Nightly/manual tool emitting verdict JSON** (like `eval_fidelity_nightly.py`) |
| 7 | Harness shape | **New isolated `eval_multiturn_local.py`** (option B), not an in-place extension |

## Architecture

New isolated nightly tool, modeled on `scripts/eval_fidelity_nightly.py`:

```
scripts/eval_multiturn_local.py
├── imports from eval_multiturn.py:   EVAL_DIMENSIONS, the judge call
├── imports from multiturn_scenarios_deep.py:  deep scenarios + anchors (data only)
├── LocalBackend:   POST /v1/chat/completions → local mlx-server (port 8741)
│                   sends FULL accumulated history each turn (mirrors compatible.c)
├── run_scenario(scenario, depth):   drives one extended conversation,
│                   times each turn (first-token + total), collects transcript
├── judge pass:     retention (per-anchor) + voice-drift via Gemini judge (ADC)
├── latency pass:   ceiling + slope from the timed series (no judge)
└── verdict:        writes results/<date>-multiturn-local-verdict.json
```

**Key properties:**

- **On-device under test; cloud only judges.** Generation happens on the local
  mlx-server (the artifact rung 3 must prove). The cloud Gemini judge never
  generates assistant turns — it only rates them. The thing-under-test is fully
  local.
- **Full-history-every-turn is reproduced deliberately.** That is the real
  `compatible.c` behavior (no server-side caching, full history resent each turn),
  and it is exactly what makes the latency slope meaningful.
- **Verdict JSON is the durable output** (per-axis pass/fail, per-turn latency
  series, per-turn judge verdicts), same philosophy as
  `us15-empirical-verdict.json`.

## Scenario extension & anchor seeding

Each of the 6 scenarios in `eval_multiturn.py` (`casual_catchup`,
`emotional_escalation`, `debate_opinions`, `banter_humor`, `news_reaction_chain`,
`advice_seeking`) grows from 3–4 turns to **20–30 user turns**, authored in a new
data-only module `scripts/multiturn_scenarios_deep.py`. The original short
scenarios in `eval_multiturn.py` are left untouched.

**Anchor seeding (makes retention countable).** Each deep scenario plants 3–5
**anchors** — concrete recallable facts introduced in early turns (~turns 1–6) —
each paired with a later **probe turn** that naturally invites recall:

```python
{
  "name": "casual_catchup",
  "anchors": [
    {"turn": 2,  "fact": "user's dog is named Biscuit",     "probe_turn": 24},
    {"turn": 5,  "fact": "user is flying to Denver Friday",  "probe_turn": 19},
    {"turn": 6,  "fact": "user hates cilantro",              "probe_turn": 27},
  ],
  "turns": [ ...20–30 scripted user messages... ]
}
```

At each probe turn the judge is handed the anchor fact + the assistant's
probe-turn response and asked a focused retention question: *did the reply at
turn N stay consistent with the fact established at turn M?* → per-anchor
`retained: true/false`.

**Depth padding without filler.** Added turns are in-character continuation of the
scenario's arc (a debate keeps escalating; banter keeps riffing) — never repeated
identical prompts — so voice-drift is measured over *real* sustained conversation.

## Scoring & the pass/fail bar

Three axes, each with an explicit gate. **All thresholds below are initial
calibration values** — the first run measures the actual distribution, then the
numbers are locked. Per the project threshold-provenance rule, no number is
borrowed from another classifier and assumed to transfer.

### Axis 1 — Context retention (judge + countable)

- Per-anchor `retained` boolean → `retention_rate = retained / total anchors`.
- **Bar:** `retention_rate ≥ 0.85` per scenario; no scenario below 0.70.

### Axis 2 — Voice drift (judge)

- Reuse `eval_multiturn.py`'s HUMAN / BORDERLINE / AI per-turn verdict plus the
  `personality_consistency` dimension.
- Measured as **degradation over distance**: mean verdict score of the *first
  third* of turns vs the *last third*.
- **Bar:** `last_third_mean ≥ first_third_mean − 0.10`; and no individual late
  turn flips to a hard `AI` verdict.

### Axis 3 — Latency (wall-clock, no judge)

- Per turn: first-token latency + total generation latency.
- **Ceiling:** every turn's first-token latency ≤ `C` ms (`C` seeded from the
  existing `first_token_budget_ms` = 500 as a starting point, recalibrated to
  on-device reality on first run).
- **Slope:** linear-fit slope of first-token latency vs turn index, normalized →
  late turns ≤ 20% slower than early turns.

### Overall verdict

- A **scenario PASSES** only if all three axes pass.
- The **run PASSES** if ≥ 5 of 6 scenarios pass.
- Verdict JSON records every per-axis number so a fail is diagnosable, not just red.

## Remediation path ("fix what breaks")

Contingent, not pre-committed. The spec defines decision rules keyed to which axis
fails, so remediation follows evidence:

| If this fails | Most likely cause (per the context-path map) | Remediation candidate |
|---|---|---|
| **Retention** drops at deep turns | 20 KB byte-truncation (`agent_turn.c:4823`) dropping early anchors before recall | Wire the existing `compaction.c` into the turn flow (exists but **not** auto-triggered) — summarize oldest turns instead of hard-truncating |
| **Latency slope** climbs | full-history-every-turn (`compatible.c`) resending a growing prompt with no server-side caching | Prompt-prefix reuse / KV-cache on the server, or compaction to cap prompt growth |
| **Voice drift** late | persona re-injection diluted as history grows | Persona-anchor reinforcement in deep-turn prompt assembly |

Each remediation is its own follow-up task spawned from what the first verdict
shows. This keeps rung 3 scoped to one concern: the harness, the bar, and the
first measured verdict.

## Error handling (harness robustness)

- **mlx-server unreachable** → fail fast with a clear message. Never silently fall
  back to a cloud model — that would invalidate the on-device claim.
- **ADC / judge unavailable** → run the deterministic latency axis anyway; mark
  retention + voice as `SKIPPED` (not PASS) in the verdict; exit non-zero.
- **A single turn errors mid-conversation** → record that turn as a failure,
  continue the scenario (don't abort the whole run), surface it in the verdict.
- **Secrets** → the harness never reads or logs `~/.human/config.json`; it only
  talks to the mlx-server HTTP endpoint.

## Testing the harness

Python tooling under `tests/`, using the existing importlib pattern, stdlib-only,
no live model and no real judge in the unit suite:

- **Anchor scoring** — `retention_rate` from a fixture of (anchor, judge-verdict)
  pairs: all-retained → 1.0, none → 0.0, partial → correct fraction.
- **Latency math** — synthetic per-turn series: assert ceiling-violation detection
  fires; slope normalization flags a climbing series and passes a flat one.
- **Voice-drift comparison** — fixture of per-turn scores: degradation gate
  triggers on a decaying series, passes a stable one.
- **Verdict JSON shape** — emitted artifact has every axis, per-scenario breakdown,
  and per-turn latency series (so a fail is always diagnosable).
- **Backend wiring** — mock the HTTP call; assert full accumulated history is sent
  each turn, and an unreachable server fails fast (no cloud fallback).
- **Judge/ADC-absent path** — retention + voice mark `SKIPPED` (not PASS); process
  exits non-zero.

No unit test invokes the real Gemini judge or the real local model — those are
integration concerns exercised by actually running the nightly tool.

## Deliverables

1. `scripts/multiturn_scenarios_deep.py` — 6 deep scenarios (20–30 turns) + anchors.
2. `scripts/eval_multiturn_local.py` — the nightly harness (local backend, timing,
   judge pass, latency pass, verdict emission).
3. `tests/test_eval_multiturn_local.py` — unit tests per the section above.
4. First verdict JSON under `results/` + calibrated thresholds locked into the
   harness.
5. Follow-up remediation tasks spawned from whatever the first verdict shows
   breaking.

## Findings (first runs, 2026-05-28/29)

### F1 — Empty-reply depth window (thinking starvation at turns ~11–15)

The first partial live run surfaced a reproducible coherence limitation: a
cluster of **empty assistant turns** (zero visible content after thinking-token
strip) at a conversation-depth window, recovering afterward.

**Evidence (cross-scenario depth alignment):**
- `casual_catchup` — empties at turns 13, 14, 15; coherent again from 16.
- `emotional_escalation` — empties at turns 11, 12, 13, 14; coherent again from 16.
- Two *different-content* scenarios go empty at the *same depth band*. Content is
  not the variable; **conversation depth (prompt size) is**.
- The empty turns are the **slowest** (44–52 s vs 31.8 s p50): long generation,
  zero output — the budget went somewhere invisible.

**Confirming run (judge-OFF spot-check, 2026-05-29):** an independent 14-turn
`casual_catchup` re-run with the judge disabled (isolating the serving path from
judge variance) reproduced the same signature: empties at turns **11 and 13**
(rate 0.143), and both empty turns were again among the slowest (**90.3 s** and
**68.6 s** first-token). Latency growth on this single scenario was 1.601 with a
178 s turn-9 outlier — a real at-depth climb, though one scenario alone does not
move the multi-scenario nightly verdict. Same depth band (~11–15), same
"empty turns are the slow turns" tell, with the judge entirely out of the loop —
which rules out the judge as the cause and pins it on prompt-size-vs-budget at
depth.

**Root cause (consistent with the M3 live-path fix, memory `m3_live_path_extractor_strip.md`):**
the `seth-lora-v4-repair` adapter emits ~150–200 thinking tokens regardless of
prompt. As history grows, the prompt consumes more of a fixed generation budget;
at the depth band the thought block eats the whole remaining budget, leaving an
empty visible reply after strip. KV/prompt-cache state shifts at turn 16 appear
to free budget, hence recovery.

**Why this is NOT a harness bug:** the harness faithfully records what the server
returned. The impact is already penalized by the retention gate (an empty
probe-turn fails its anchor) and the voice gate (an empty late-third turn tanks
the score). `count_empty_replies()` surfaces count/turns/rate into each scenario
verdict as a **diagnostic** so a nightly reviewer can see *why* a gate moved
without double-counting the failure.

**Remediation (server-side, NOT this repo):** the fix is a thinking-headroom
guarantee in the gemma-realtime serving path (`mlx-server.py`
`_thinking_headroom_tokens()` — already landed for the non-stream path in commit
`689f05c`, default 512 tunable via `GEMMA_THINKING_HEADROOM_TOKENS`). The deep
multi-turn case needs the same headroom applied as prompt size grows. Tracked as
a follow-up against the serving repo, not h-uman.

## Related

- `scripts/eval_fidelity_nightly.py` — the nightly-tool pattern this mirrors
  (produced the M3 fidelity proof, +27pp).
- `scripts/eval_multiturn.py` — source of `EVAL_DIMENSIONS`, the judge, and the
  6 base scenarios.
- `~/.claude/projects/.../memory/m3_mission_validated.md` — M3 empirical-validation
  precedent and verdict-JSON shape.
- `.claude/rules/classifier-score-plus-flag-gate.md` — threshold-provenance
  discipline (don't transfer a number between classifiers).
