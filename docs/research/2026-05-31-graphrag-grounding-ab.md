---
title: "GraphRAG Grounding A/B — does per-contact community grounding earn default-ON?"
created: 2026-05-31
status: final
measurement: graphrag_grounding_marginal_ab
verdict: NOT SUBSTANTIATED (ON-win-rate 43.3%, 95% CI [27.4, 60.8]) → default flipped ON→SHADOW
---

# GraphRAG Grounding A/B — does per-contact community grounding earn default-ON?

**Date:** 2026-05-31
**Branch:** `feat/graphrag-grounding-ab`
**Harness:** `scripts/grounding_ab.py` · **Gate:** `docs/evaluation/blind_ab_gate.json` (proxy half)

## TL;DR

A paired ON-vs-OFF A/B over **30 real iMessage pairs** measured GraphRAG
grounding's marginal effect at a **43.3% ON-win-rate** (ON 13 / OFF 17, 95%
Wilson CI **[27.4%, 60.8%]**). Grounding-ON did **not** beat OFF — it slightly
*lost*, and the CI crosses 50%. Per
`.claude/rules/feature-gate-requires-measurement.md` this is **NOT substantiated**,
so the source default is **flipped from ON to SHADOW** (loaded + logged, not
injected) until a measurement clears 50%. `HU_GRAPH_GROUNDING=on` re-enables
injection for anyone who wants it.

## Why this measurement

GraphRAG grounding (`src/agent/graph_grounding.c`) injects a contact's top-3
community summaries from `~/.human/graph.db` into the prompt under a
`## Relationship Context` header (`src/agent/prompt.c`). It shipped
**default-ON**. The project's activation contract
(`.claude/rules/feature-gate-requires-measurement.md`) says a behavior that
changes what gets *sent* must be substantiated by a measurement, not by a green
test suite.

`docs/evaluation/blind_ab_gate.json`'s `fool_rate` measures AI-vs-real-Seth in
**absolute** terms — it cannot isolate what grounding *marginally* contributes.
This A/B does: it holds everything constant except `HU_GRAPH_GROUNDING` and asks
a blinded judge which reply is closer to the real Seth reply.

## The blocker that had to be fixed first

Grounding only fires when `agent->memory_session_id` is set + non-empty
(`agent_turn.c`, `agent_stream.c`). Before this work, **no CLI or gateway path
set that field** — only the daemon's iMessage loop did (`daemon.c:1522`). So
grounding could not be exercised for measurement off the live daemon.

Fix: `--contact <id>` on the agent CLI (`src/agent/cli.c`) parses into
`hu_parsed_agent_args_t.contact_id` and, after the graph.db bind, assigns
`agent->memory_session_id` (+ `memory->current_session_id`), mirroring
`daemon.c:1522`. `memory_session_id` is a non-owning `const char*`; argv outlives
the turn. (This seam landed on main via #257; the measurement below ran against a
functionally identical implementation — both just bind `memory_session_id`, which
is the only thing grounding gates on.)

### Verification that grounding now fires (the causal control)

| Run | `--contact` | Grounding bytes loaded (shadow log) |
|-----|-------------|-------------------------------------|
| `HU_GRAPH_GROUNDING=shadow --contact +447914633409` | yes | **481 bytes** |
| `HU_GRAPH_GROUNDING=shadow` (no `--contact`)        | no  | **0 bytes**   |

Both runs reached the same `graph_grounding=ACTIVE` gate stage; grounding only
loaded summaries when `--contact` supplied `memory_session_id`. That isolates
`--contact` as the causal lever. (`+447914633409` has 24 community summaries.)

## Method

- **Pairs:** real `(incoming, real_seth_reply)` exported from the local iMessage
  DB (`scripts/blind_ab/export_seth_triples.py --keep-handles`), filtered to the
  four contacts that have community summaries in `graph.db`
  (`+447914633409`, `+12393005206`, `+18018285260`, `+14845661687`), pooled —
  each pair generated with **its own contact's** grounding.
- **Within-item paired A/B:** for each pair, generate two replies through the
  real agent turn — `HU_GRAPH_GROUNDING=on` vs `off` — via
  `./build/human agent --contact <id> -m <incoming>`. Each pair is its own
  matched control; grounding is the only variable.
- **Judge:** Gemini `gemini-3.1-pro-preview` (blinded, randomized A/B order),
  picks which candidate is closer to the real Seth reply / more human.
- **Exclusions (symmetric — cannot bias toward ON or OFF):**
  - byte-identical ON/OFF outputs → **ties** (grounding had no effect; excluded
    from the win-rate denominator).
  - degenerate prompt-leak replies (model echoing its persona constraints past
    `response_guard`) → excluded; per-condition counts reported.
- **Stat:** Wilson 95% CI on the ON-win-rate among decisions.
- **Model:** local `gemma-4-31b-it-8bit` + `seth-lora-v4-repair` on `:8741`.

## Results

| Metric | Value |
|--------|-------|
| Decisions (non-tie) | **30** |
| ON wins | 13 |
| OFF wins | 17 |
| **ON-win-rate** | **43.3%** |
| 95% Wilson CI | **[27.4%, 60.8%]** |
| Ties (grounding changed nothing) | 5 |
| Excluded (empty/leak/judge-error) | 7 |
| Degenerate prompt-leak: ON / OFF | **5 / 2** |
| Verdict | **NOT SUBSTANTIATED (CI crosses 50%)** |

Two things stand out:

1. **ON is below 50% — grounding slightly *hurt*.** The point estimate is 43.3%
   with the OFF condition winning more head-to-heads. Even setting the
   decision threshold aside, there is no positive signal here.
2. **The leak asymmetry points the same way.** Grounding-ON produced more
   degenerate prompt-leak replies than OFF (5 vs 2). Injecting ~480 bytes of
   relationship context into an already-large lean prompt appears to *mildly
   destabilize* the small local model's output, not stabilize it. (These were
   excluded symmetrically from the win-rate, so this is a separate observation,
   not a driver of the 43.3%.)

The run was stopped cleanly at the n=30 ENFORCING threshold (of a 71-pair pool);
generation is ~50–280s per reply, so each pair is two multi-minute model calls.
The verdict was stable and below 50% from n≈13 onward.

## Decision

Decision rule (`.claude/rules/feature-gate-requires-measurement.md`): if the
ON-win-rate is not meaningfully above 50% (CI crosses 50%, or point estimate
<55%), grounding is **not substantiated** and the default flips to **SHADOW**
until it is.

**ON-win-rate 43.3%, CI [27.4, 60.8] → flip default to SHADOW.**

Applied in this PR:
- `src/agent/graph_grounding.c`: unset/empty `HU_GRAPH_GROUNDING` now returns
  `HU_GRAPH_GROUNDING_SHADOW` (was `_ON`). `=on`/`=1` still force injection;
  `=off` still disables; unknown values still fail safe to OFF.
- `tests/test_graph_grounding.c`: `test_graph_grounding_mode_parse` updated to
  pin the new unset→SHADOW default.
- `docs/evaluation/blind_ab_gate.json`: proxy half written
  (`measurement=graphrag_grounding_marginal_ab`, `mode=ENFORCING`,
  `verdict=FAIL`, effective_verdict=FAIL), commit-stamped.

**Not touched:** the production launchd plist. If the live daemon sets
`HU_GRAPH_GROUNDING=on` explicitly, that remains the operator's choice; the
source-default flip only governs paths that don't opt in. Operators who want to
keep injecting set `HU_GRAPH_GROUNDING=on`.

### What would re-substantiate ON

The summaries themselves may be low-signal for *reply quality* even when
relationship-accurate. A future attempt should either improve what
`hu_graph_ground_load` selects/renders (e.g. only inject when the summary is
topically relevant to the incoming), or re-run this A/B against a stronger base
model where the extra context is less likely to destabilize output — then flip
back to ON only if the ON-win-rate clears 50% with the CI above it.

## Reproduce

```bash
cmake --build build --target human -j8
python3 scripts/blind_ab/export_seth_triples.py --keep-handles --max-per-contact 0 \
  --out /tmp/grounding_triples.json
python3 scripts/grounding_ab.py --triples /tmp/grounding_triples.json \
  --pairs 80 --timeout 300 --gate
```

The committed artifact is the aggregate proxy half of
`docs/evaluation/blind_ab_gate.json` (no message text). The per-trial JSON
(`data/grounding_ab.json`) contains real message text and is gitignored.
