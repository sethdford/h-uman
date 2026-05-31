---
title: "Blind-A/B Gate: ADVISORY → ENFORCING Runbook"
description: How to populate real Seth pairs to make the blind-A/B gate enforcing, and the gated path to flipping a capability LIVE.
---

# Blind-A/B gate: ADVISORY → ENFORCING (and the path to flipping a capability LIVE)

The blind-A/B measurement gate is wired and enforced in CI, but it ships
**ADVISORY** by design: it reports a verdict but does not block, because there
are not yet enough real Seth pairs to make the proxy measurement authoritative.
This is deliberate — a gate that blocked on synthetic data would be new theater.

## Why ADVISORY today

`scripts/blind_ab_gate.py:ENFORCE_MIN_PAIRS = 30`. The Tier-1 proxy
(`eval_blinded_ab.py --gate`) reports `proxy.mode = ENFORCING` only when it ran
against **≥ 30 real pairs**; below that it is `ADVISORY` and never fails CI.
The real-pairs corpus (`data/imessage/ground_truth.jsonl`) is currently far
below 30, so the gate is informational until it is populated.

The fail-closed CI check (`scripts/check_capability_gates.py`, run on every PR
via `ci.yml`) still has teeth **right now** for one thing: it will fail any PR
that flips a capability to `LIVE` in `docs/evaluation/capability_gates.json`
while the gate's `effective_verdict` is not `PASS`. Since nothing is LIVE and
the verdict is ADVISORY, it passes today and blocks premature activation later.

## The two tiers

1. **Proxy (automated, fast):** `scripts/eval_blinded_ab.py --gate` — Gemini
   LLM-judge picks "which reply is the real Seth"; writes the `proxy` half of
   `docs/evaluation/blind_ab_gate.json`. A *real* run needs `GEMINI_API_KEY`
   **and** a model-serving endpoint (`HU_GATEWAY_URL`, or `--mlx`/CLI). Without
   both it runs `--gate-dry-run` → ADVISORY (validates wiring, does not measure).
2. **Human (authoritative, cadence):** `scripts/blind_ab/` + `score.py
   --emit-gate` — real raters; writes the `human` half. A human `FAIL` is an
   absolute **veto** over a proxy `PASS` (`compute_effective_verdict`).

## Turning the gate from ADVISORY → ENFORCING

1. Populate ≥ 30 real pairs:
   `python3 scripts/extract_imessage_pairs.py` → `data/imessage/ground_truth.jsonl`.
2. Stand up a serving endpoint and run the real proxy:
   `HU_GATEWAY_URL=http://127.0.0.1:3002 GEMINI_API_KEY=... \
    python3 scripts/eval_blinded_ab.py --gate --gateway`
3. Confirm `docs/evaluation/blind_ab_gate.json` shows `proxy.mode == "ENFORCING"`.
   From here, a proxy `fool_rate` below `fail_under` (45%) — or a regression > 5
   points vs the recorded baseline — makes the proxy run exit nonzero.
4. Run ≥ 1 human cadence rating and emit the authoritative half:
   `python3 scripts/blind_ab/score.py <sheets...> --key <key.json> \
    --emit-gate docs/evaluation/blind_ab_gate.json`

## Flipping a capability LIVE (the gated action)

Only after the gate is ENFORCING **and** `effective_verdict == PASS` (proxy PASS
with no human veto) should you edit `docs/evaluation/capability_gates.json` to
set a capability's `state` to `LIVE`. The PR doing so is gated by the
`capability-gate-check` job in `ci.yml`, which fails closed unless the gate is
green.

Note: the env flags for `theory_of_mind` (`HU_TOM`) and `intent_response`
(`HU_INTENT_RESPONSE`) in the registry are **placeholders** — those capabilities
are not yet wired (verified: no `getenv` for them in `src/` as of this writing).
Confirm the real flag name when the capability lands before flipping it LIVE.
`graph_grounding` (`HU_GRAPH_GROUNDING`, src/agent/graph_grounding.c) and
`salience` (`HU_SALIENCE_SHADOW`, src/agent/agent_turn.c) are real, verified flags.
