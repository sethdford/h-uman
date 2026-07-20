---
title: SOTA Program Design — Containment, Measurement, Ship
description: Program design to make h-uman honestly competitive with 2026 personal-AI / Claw SOTA, sequenced from security honesty through measured bars to shipping.
date: 2026-07-19
status: approved
---

# SOTA Program Design — Containment → Measurement → Ship

**Date:** 2026-07-19  
**Status:** Approved (decision delegated to agent; approach D)  
**HEAD baseline:** `ce10fd87` (+ subsequent wave commits)  
**Inputs:** deep audit canvas, SOTA comparison audit canvas, PRODUCT.md M1–M6

## Problem

h-uman has deep compiled persona and preference infrastructure, but:

1. Tool security is **not one envelope** — dispatcher has five layers; DAG / HuLa / stream bypass most of them.
2. Docs **overclaim** SOTA (memory, on-device ML, channel/binary counts, Gemini 2.5 refs).
3. Memory quality is **unproven** against LongMemEval / LoCoMo (baseline LoCoMo P@1 ≈ 0.058).
4. Humanness gate is **theater** — blind A/B human verdict ABSENT (n=0).
5. Distribution is **zero DAU** while OpenClaw / Apple / Gemini own defaults and mindshare.

“Get to SOTA and built” without sequencing produces thrash. This program sequences hard gates.

## Decision

**Approach D — full stack in waves:**

| Wave | Name | Exit criteria |
|------|------|---------------|
| **A** | Containment + honesty | One secured pre-execute primitive on dispatcher, stream, DAG, HuLa; SOTA_BENCHMARK demoted to measured language; Turing HTTP auth; no Gemini 2.x refs |
| **B** | Measured SOTA | Reproducible LongMemEval + LoCoMo numbers published; contact isolation regression tests; blind A/B human n≥30 or explicit FAIL gate in CI |
| **C** | Ship path | Default onboard = local (Ollama/MLX); Tier-1 iMessage/Telegram depth; productized DPO/KTO reaction path; M4 first-user loop |

We do **not** chase SWE-bench as a primary moat. Coding-agent SOTA is table stakes via the underlying model, not our differentiator.

## Non-goals (this program)

- Matching Gemini Gmail/Photos corpus breadth
- Publishing PyPI/npm HuLa packages (M5 — Wave C+ optional)
- Rewriting OpenClaw feature-for-feature
- Claiming “SOTA memory” before LongMemEval is measured

## Architecture (Wave A)

### Canonical tool gate

Add `hu_agent_internal_pre_execute_checks()` in `src/agent/agent.c` / `agent_internal.h`:

```
permission → pre-hook → ESCALATE → policy/arg-inspect
```

Returns allow / deny (with `hu_tool_result_t` populated) / need-approval flag.

Extend `hu_agent_internal_dispatch_with_hooks` into (or alongside) a secured dispatch that runs the full gate before `vtable->execute`, and always fires post-hook.

### Call-site rewiring

| Path | File | Change |
|------|------|--------|
| Stream | `agent_stream.c` | Call pre_execute_checks before execute; keep post_hook |
| DAG parallel | `agent_turn.c` `dag_parallel_worker` | Same |
| DAG sequential | `agent_turn.c` batch loop | Same |
| HuLa CALL | `hula.c` `exec_call` | Optional borrowed `hu_agent_t *` via `hu_hula_exec_set_security_agent`; run checks when set |
| Dispatcher batch | already partially gated | Prefer filtering denied calls before `hu_dispatcher_dispatch` (fix `dispatch_allowed` unused bug if still present) |

### Honesty

- Rewrite `docs/SOTA_BENCHMARK.md` ratings to COMPETITIVE/PARTIAL where unmeasured
- Sync binary/test/channel counts to measured values
- Remove Gemini 2.5 / Claude 3.7 as SOTA references

### Gateway

- Require `v1_auth_ok` on `/api/turing/*` when `auth_token` configured

## Wave B (summary)

- Eval harness publishes LongMemEval + LoCoMo to `docs/evaluation/` with method notes
- Vector/hybrid retrieval requires contact/session namespace (fail closed in tests)
- Blind A/B: enforce existing `2026-05-31-blind-ab-measurement-gate-design.md`

## Wave C (summary)

- `human init` / `onboard` local-first (already partially done) + iMessage what’s-next
- One-command preference train path from reactions
- LLM Wiki–style visible compounding (index/log) on personal model — thin slice

## Success metrics (program-level)

| Metric | Target |
|--------|--------|
| Tool path parity | All four paths call pre_execute_checks; adversarial tests deny DANGER tool via DAG/HuLa/stream |
| Doc honesty | Zero “SOTA” labels without a cited measured bar |
| Memory | LongMemEval score published; LoCoMo P@1 improved vs 0.058 baseline with method disclosed |
| Humanness | Blind A/B human n≥30 or CI fails closed when gate file says ABSENT and feature flips LIVE |
| Ship | At least one real daily user loop (operator) with local provider default |

## Risks

- HuLa without security_agent still weak — agent_turn call sites must always set it
- Parallel DAG + hooks may not be thread-safe — serialize hooks or document THREAD_SAFE tools only after gate
- Memory SOTA work may require retrieval redesign — keep Wave B scoped to measure + isolation first

## Approval

Delegated by user 2026-07-19 (“you make the decision and push forward”). Implementation proceeds via the companion plan.
