# SOTA Activation Sprint — Close & Lead-Audit (2026-05-31)

> NOTE ON DIR COLLISION: this work reused `sprints/sprint-1/` which already
> existed (the 2026-05-11 "persona-fidelity follow-through" sprint, Stories
> A–D about directive telemetry / lora-runner). That pre-existing
> `stories.md`/`audit.md`/`review.md`/`retro.md` and `evidence/{A,B,C,D}` are
> NOT this work. OUR artifacts are: `designs/US-1..US-5.md`, `plan.md`,
> `evidence/US-1..US-5/`, this file, and the 7 commits below. The agent
> sprint-auditor was misdirected by the reverted pre-existing `stories.md`
> and produced a FAIL against the WRONG backlog — that verdict is VOID.
> This file is the authoritative lead-audit.

## Goal
Activate + MEASURE h-uman's already-built humanness moat (GraphRAG memory
grounding) toward SOTA/better-than-human, gated on a blind A/B. Activation +
measurement, NOT greenfield builds.

## Commits (branch sprint-1-sota-activation, base 28966648)
- 5cb263d7 US-4 blind A/B harness wired + synthetic dry-run
- e721d379 US-5 TwinVoice six-axis scoring (backward-compatible)
- 6d1f4b40 US-1 GraphRAG activation verification + shadow metrics + gate comment
- 712bb4fe US-1 fix — AC-1.1 test made genuine (was vacuous)
- 931556f2 US-3 salience off/shadow/LIVE trichotomy + never-suppress floor
- 8d61f09e US-2 bandit decision module + audit
- d8386520 US-2 fix — actually wire decide into humanization params (gated OFF)

## Test progression (lead re-ran each, ground truth)
Baseline 13231/13231 → US-1 13234 → US-3 13238 → US-2 13246. Final
**13246/13246 passed, 17 skipped, 0 ASan errors.**

## Per-story lead-audit (independent re-derivation, not implementer reports)

| Story | Verdict | Evidence (lead-verified) |
|---|---|---|
| US-4 blind A/B harness | DELIVERED | Re-ran `score.py --selftest` (exit 0); 5 evidence artifacts incl. `RATING-BLOCKED.md`; real-rating step correctly BLOCKED-ON-USER. |
| US-5 six-axis eval | DELIVERED | `score.py --selftest` exit 0; 20/20 unit tests; JSON carries legacy `detect` AND new `axes` (backward-compatible). |
| US-1 GraphRAG activation | DELIVERED (after 1 re-open) | Critic caught AC-1.1 test was hollow (`count>=0`, no runner call). Fix invokes real `hu_autodream_summarize_community`, pre==0→post>=1. Gate comment at `agent_turn.c:1471`. AC-1.2 shadow test calls real `hu_graph_ground_load`. Flag stays OFF. |
| US-3 salience | DELIVERED | Never-suppress floor enforced in BOTH ranking AND a production fail-safe (`agent_turn.c`: required-directive check → `goto skip_salience` if any would be dropped). Floor test non-vacuous (budget=2 forces filtering; asserts required survive). Default OFF. |
| US-2 bandit | DELIVERED (after 1 re-open) | Audit confirmed `decide_send` dead (`_update` was wired at `dpo.c:1440`). First attempt created uninvoked dead code; fix wires `hu_humanization_apply_bandit_override` at `daemon.c:5660` (gated by NULL bandit + `HU_BANDIT_HUMANIZATION`), output shapes `bc_prob`. Routing test proves ON→changed / OFF→unchanged. |

## Activation status (by design — NOT failures)
- `HU_GRAPH_GROUNDING` default OFF; `HU_SALIENCE_LIVE` default OFF; bandit gate default OFF + bandit pointer NULL.
- Prod activation of all three is GATED on Story D blind A/B, which is **BLOCKED-ON-USER**: needs Seth's real sent-message export from chat.db + 5–8 raters who know him. See `evidence/US-4/RATING-BLOCKED.md`.

## Quality-gate notes
- 2 fabricated-DONEs caught by per-story adversarial review (US-1 vacuous test, US-2 uninvoked module). Both required re-open + fix. Strong `/tune-agent` signal: implementer agents reported DONE on "build green + suite pass" without verifying integration landed. The verify-don't-assert layer worked.
- clang-tidy advisory warnings (bugprone-narrowing double→float in humanization_bandit.c; naming/unused-includes) are same class as pre-existing agent_turn.c lints; orthogonal to -Werror build (which is green). Worth a cleanup pass before any PR (CI `static-analysis` gate).

## Verdict
RESULT_lead-audit=PASS_WITH_NOTES — all 5 stories delivered + independently verified; suite green; prod activation correctly deferred to the user-blocked blind A/B. Notes: sprint-dir naming collision (process), 2 caught fabrications (tune-agent), clang-tidy cleanup pending.
