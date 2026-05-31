---
title: Reliable & accountable platform plan
description: Unify “better than an informal human helper” product goals with Human stack differentiators
status: deferred
related:
  - docs/plans/2026-03-08-better-than-human.md
last_audit: 2026-05-25
---

# Plan: Reliable, accountable assistant + sharpened Human stack

**North star:** Be **more reliable and accountable** than an informal human helper—not indistinguishable from a person. Reinforce that story with **measurable quality**, **small/fast runtime**, **fewer security sharp edges**, and **clear AI disclosure**.

**Relationship to other work:** Deep implementation spikes (STM, commitments, pattern radar, LLMCompiler, etc.) live in [`2026-03-08-better-than-human.md`](2026-03-08-better-than-human.md). This document is the **cross-cutting product and platform plan** that ties those (and existing subsystems) to the pillars below.

---

## Pillar A — Better than a person (assistant-style work)

### A1. Verifiable outputs

| | |
|--|--|
| **Outcome** | Answers are **grounded** (tools, citations, explicit uncertainty). The model does not invent file contents, command output, or live facts when it cannot have them. |
| **Anchors** | `docs/standards/ai/hallucination-prevention.md`, `docs/standards/ai/capability-claims.md`; eval: `eval_suites/capability_edges.json`, `tool_capability` / `capability_honesty` judge profiles; `scripts/adversarial-eval-harness.py` + `scripts/redteam-eval-fleet.sh`. |
| **Workstreams** | (1) Prompt + planner: enforce “state what you observed vs inferred” patterns in `src/agent/prompt.c` / persona rules. (2) Tools: ensure high-risk tools return structured, attributable results (`src/tools/`). (3) Eval: expand suite tasks for “no file path → no invented CSV” and similar; keep harness judge aligned with suite `expected`/`rubric`. |
| **Metrics** | Static suite pass rate on capability/tool suites; harness mean score + `judge_passed` count; regression budget: no sustained drop across two consecutive live fleet runs. |

### A2. Durable memory + policy

| | |
|--|--|
| **Outcome** | Same user, same **policy and preferences** across sessions; memory is **attributed** and **revocable** where privacy requires it. |
| **Anchors** | `src/memory/`, `hu_memory_t`; `src/security/policy`; config merge `src/config/config.c`; optional long-horizon work in [`2026-03-08-better-than-human.md`](2026-03-08-better-than-human.md) (promotion, consolidation). |
| **Workstreams** | (1) Document and test “memory read vs write” boundaries per backend (SQLite, markdown, etc.). (2) Ensure policy gates (autonomy, tool risk) apply consistently in agent + gateway paths. (3) Ship one “preference persistence” E2E path per primary surface (CLI + one channel) with tests. |
| **Metrics** | Tests for policy + memory integration; user-facing docs accurate; optional: session-resume correctness tests. |

### A3. Latency and availability

| | |
|--|--|
| **Outcome** | **Predictable** startup and memory; suitable for always-on and edge-adjacent deployment stories. |
| **Anchors** | `AGENTS.md` / `CLAUDE.md` performance baselines; `CMakePresets.json` release preset; CI `benchmark.yml` where applicable. |
| **Workstreams** | (1) Treat release binary size and RSS as **product metrics**—any major feature adds a budget note in PR. (2) Profile cold paths after large agent/memory changes. (3) Avoid silent degradation on constrained hosts (explicit errors vs hangs). |
| **Metrics** | Release build: binary size and startup within documented bands; benchmark workflow green; no new unbounded allocations on hot paths without justification. |

### A4. Safety and auditability

| | |
|--|--|
| **Outcome** | **Pairing, sandbox, and safe defaults**; logs and UI **never** leak secrets; failures are **explicit**. |
| **Anchors** | `docs/standards/security/`; `src/gateway/gateway.c`; `src/security/`; `src/runtime/`; `src/tools/`; `HU_IS_TEST` patterns in tests. |
| **Workstreams** | (1) Per-release pass: threat-relevant changes include failure-mode tests. (2) Gateway + tool allowlists reviewed when adding surface area. (3) Observability: structured logs without PII/secrets (`docs/standards/operations/`). |
| **Metrics** | Security-sensitive paths covered by tests; audit checklist in PR template for high-risk diffs; periodic `scripts/verify-all.sh` / doc fleet green. |

### A5. Product story (positioning)

| | |
|--|--|
| **Outcome** | External narrative matches internals: **consistent, grounded, available, auditable**—not “pretend human.” |
| **Anchors** | `docs/standards/brand/`; `docs/standards/ai/capability-claims.md`; website + dashboard copy (`website/`, `ui/`). |
| **Workstreams** | (1) Align marketing and in-app language with capability-claims standard. (2) Publish eval summaries under `docs/eval-runs/` after meaningful model or prompt changes. (3) Competitive claims cite benchmarks or evals, not adjectives. |

---

## Pillar B — Better than the current Human stack

### B1. Ship scores, not vibes

| | |
|--|--|
| **Outcome** | Quality is **versioned** with artifacts: static suites + live fleet + harness report. |
| **Anchors** | `human eval run`, `scripts/redteam-eval-fleet.sh`, `scripts/adversarial-eval-harness.py`, `docs/eval-runs/README.md`, `docs/templates/eval-release-notes.md`, `.github/workflows/eval.yml`. |
| **Workstreams** | (1) **Gate:** meaningful agent/prompt/gateway changes run at least targeted evals; releases run full fleet where feasible. (2) Store dated summaries + harness JSON pointers in `docs/eval-runs/`. (3) When adding suites, keep `expected`/`rubric` in sync for both C eval and Python harness. |
| **Metrics** | Harness `mean_score` + `judge_passed` + per-suite static scores recorded in release notes; regressions investigated before merge. |

### B2. Smaller, faster, cheaper to run

| | |
|--|--|
| **Outcome** | **MinSizeRel + LTO** remains a first-class product constraint. |
| **Anchors** | `cmake --preset release`; `AGENTS.md` size/RSS table; module boundaries in `ARCHITECTURE.md`. |
| **Workstreams** | (1) Feature flags: prefer compile-time or config toggles over always-on bloat. (2) Before large table/embeddings: cost/benefit in PR. (3) Re-run release preset after major merges. |
| **Metrics** | Binary size and RSS vs documented baseline; alert on CI benchmark regression. |

### B3. Fewer sharp edges

| | |
|--|--|
| **Outcome** | High-blast-radius code paths fail **closed**, with **tests for misuse**. |
| **Anchors** | Risk tiers in `AGENTS.md`; `src/gateway/`, `src/tools/`, `src/runtime/`, `src/security/`; `docs/standards/engineering/testing.md`. |
| **Workstreams** | (1) For each new tool/gateway method: input validation, error paths, `HU_IS_TEST` for side effects. (2) Periodic review of defaults (HTTPS-only, deny-by-default). (3) Fuzz targets where parsers accept untrusted input (`fuzz/`). |
| **Metrics** | New high-risk code ships with boundary tests; no skipped ASan failures. |

### B4. Clear “human vs machine” disclosure

| | |
|--|--|
| **Outcome** | Users always know they are talking to **software**; no romantic/deceptive anthropomorphism in product copy or default persona. |
| **Anchors** | `docs/standards/ai/` (conversation, disclosure, human-in-the-loop); persona system `src/persona/`; channel-specific UX `docs/standards/design/`. |
| **Workstreams** | (1) Audit default system strings and persona templates against disclosure standards. (2) UI/CLI: visible assistant identity where the standard requires it. (3) Eval or checklist item for “impersonation / false human-life claims” on shipped surfaces. |
| **Metrics** | Standards checklist signed off for major UI/persona releases; adversarial suite + human_likeness profiles stay green on disclosure-related probes. |

---

## Sequencing (suggested)

1. **Foundation (now):** B1 + B3 + B4 run in parallel with low code churn—process, docs, eval hygiene, disclosure audit.
2. **Trust layer:** A4 + A1 (security + grounding) before expanding memory/persona “cleverness.”
3. **Differentiation:** A2 + A3 + B2 as features land—each feature carries size/latency and eval evidence.
4. **Long arc:** Optional deep capabilities from [`2026-03-08-better-than-human.md`](2026-03-08-better-than-human.md) only where they **raise** A1/A2 metrics without breaking B2/B3.

---

## Definition of done (program-level)

- [ ] Live fleet + harness summary archived for the current release train (`docs/eval-runs/`).
- [ ] Release binary profile documented (size + startup + RSS) and within agreed tolerance.
- [ ] High-risk areas (gateway, tools, runtime, security) have explicit failure-mode tests for new behavior.
- [ ] Brand/capability copy reviewed against `capability-claims` and disclosure standards.
- [ ] Product narrative in README / website matches pillars A1–A5 (reliable, accountable—not “indistinguishable from a human”).

---

## Owners and cadence

| Cadence | Activity |
|---------|----------|
| **Every meaningful AI/gateway/tools change** | Targeted eval + `scripts/what-to-test.sh` / `agent-preflight.sh` |
| **Each release candidate** | Full `./build/human_tests` + release preset build + live fleet when API budget allows |
| **Monthly** | Review benchmark trends, eval-run folder, and security defaults checklist |
| **Quarterly** | Disclosure + capability-claims audit on user-facing surfaces |

This plan is intended to stay short; detailed task breakdowns belong in feature-specific plans or in [`2026-03-08-better-than-human.md`](2026-03-08-better-than-human.md).
