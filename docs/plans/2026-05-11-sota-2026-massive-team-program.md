---
title: "SOTA-2026 massive team program — 14-initiative parallel design fleet"
created: 2026-05-11
status: active
parent: 2026-05-10-sota-roadmap-6mo.md
related:
  - 2026-05-10-master-follow-through-program.md
  - 2026-05-10-sota-roadmap-6mo.md
  - 2026-05-10-m3-frontier-model-bridge.md
  - 2026-05-11-full-sota-rl-improvement-loop-design.md
  - 2026-05-11-rl-loop-phase-0-honesty.md
  - 2026-05-10-w13-learning-loop.md
  - 2026-05-10-w14-sleep-compute.md
  - 2026-05-10-w15-crypto-privacy.md
  - 2026-04-11-strategic-missions.md
  - ../../CLAUDE.md
  - ../../AGENTS.md
  - ../standards/security/threat-model.md
  - ../standards/ai/evaluation.md
---

# SOTA-2026 massive team program — 14-initiative parallel design fleet

This document is the **umbrella** for the next wave of SOTA work on top of the existing six-month roadmap and the master follow-through program. It coordinates **14 parallel design tracks**, each owned by a focused subagent, each producing **one** authoritative design doc in `docs/plans/2026-05-11-init-NN-*.md`.

This program is **larger than a single sprint** and intentionally written so that each initiative can be picked up, costed, sequenced, and either executed or descoped independently. Nothing here is implementation yet — every track exits with a design doc + proof bar + concrete C-API surface that downstream sprints can implement against.

**Why now.** The April–May 2026 arXiv literature has converged on a handful of techniques that *materially* widen our existing moats (persona-as-compiled-architecture, privacy-by-architecture, on-device personalization). Most of them slot cleanly into existing vtables we already shipped — `hu_provider_t`, `hu_memory_t`, `hu_learner_t`, `hu_rl_trainer_t`, `hu_scheduler_t`. The cost of *not* picking them up now is that competitors with similar moats (OpenClaw persona plugins, Google Personal Intelligence) close the gap.

**Honest scope.** Every initiative listed here is a **design + proof-of-feasibility** ticket, not a guaranteed ship commitment. The fleet output is a portfolio of 14 design docs. Sprint planning then picks the top 4–6 based on confidence, leverage, and binary-budget impact.

## How to use this document

1. **Each track has exactly one design subagent.** The agent reads the existing related plans, the arXiv references in this doc, the relevant `include/human/*.h` vtables, and produces a single `docs/plans/2026-05-11-init-NN-*.md`.
2. **Every design doc must contain a proof bar.** The proof bar is the gate the implementation sprint will satisfy. No design ships without one.
3. **Status table at the bottom of this file is the single source of truth.** When a design doc lands, flip its row to `design done`. When a sprint adopts it, flip to `sprint open`. When it ships, flip to `done`. When we descope, flip to `parked` with one-line rationale.

## Global proof bar (every initiative design doc)

| Gate | Requirement | Pass |
|------|-------------|------|
| D0 | Document exists at `docs/plans/2026-05-11-init-NN-<slug>.md` with YAML frontmatter (title, created, status, related) | File present, links resolve |
| D1 | Maps to one (or more) `include/human/*.h` vtable additions or extensions; new public functions named per `docs/standards/engineering/naming.md` | Names land in design doc; no surface changes that violate KISS/YAGNI |
| D2 | Lists every file to create / modify, with line-count estimate per file | Numbered file list present |
| D3 | Test plan: at least one unit test (deterministic), at least one integration test, optional fuzzer or red-team harness | Test names + suites listed |
| D4 | Risk register: top 3 risks (memory budget, security, ASan, binary size, model-quality regression) with mitigations | Table present |
| D5 | Cites at least 2 arXiv / industry references with arXiv ID or DOI | Reference list at bottom |
| D6 | Binary-budget delta: KB added in MinSizeRel + RSS at runtime | Number present, explicit ceiling |
| D7 | "Defer / descope" condition: what evidence would justify parking this initiative | One paragraph |

If a design doc fails D0–D7, it gets one revision round, then is parked.

---

## Locked conventions (cross-initiative)

The cross-initiative adversarial review (`adversarial-review-critic.md`, `adversarial-review-api-contracts.md`, `adversarial-review-security.md`, synthesized in `adversarial-review-synthesis.md`) identified two enums where independent design docs picked overlapping or inverted values. Both are now **locked** here as the single source of truth. Any initiative that consumes either enum must match exactly; deviations are a sprint-blocker.

### Trust-tier ordinal convention (init-09 is the owner)

Higher ordinal = more trust. Comparisons must use `>=` against a threshold, never `==` or `<` against a hardcoded numeric value.

| Ordinal | Name | Meaning |
|---------|------|---------|
| `4` | `HU_TRUST_USER_DIRECT` | User typed it into a 1:1 CLI / DM channel session |
| `3` | `HU_TRUST_PERSONA_DERIVED` | Computed from the user's own long-term outputs |
| `2` | `HU_TRUST_FIRST_PARTY` | User-installed tool, 1st-party data source (`human` itself, self-email, calendar) |
| `1` | `HU_TRUST_THIRD_PARTY` | Group chat, RSS, social feed, email from stranger |
| `0` | `HU_TRUST_UNTRUSTED` | Unknown origin or quarantine-flagged |

Initiatives consuming this enum: **#04** (gates LoRA training data), **#05** (TTT only on `>= USER_DIRECT`), **#08** (federated aggregation only on `>= FIRST_PARTY`), **#10** (consolidation only merges within same tier).

### `hu_job_kind_t` enum allocation (init-10 is the owner; init-05 / #08 / #14 extend)

The adversarial API review caught two independent designs (init-05 and init-10) each defining `HU_JOB_KIND_NREM = 1`. This allocation table prevents future collisions. New jobs claim the next free ordinal; deletions are forbidden (allocate a new ordinal instead).

| Ordinal | Constant | Owner initiative | Purpose |
|---------|----------|------------------|---------|
| `0` | `HU_JOB_KIND_NONE` | scheduler core | Sentinel / uninitialized |
| `1` | `HU_JOB_KIND_NREM` | **#10** | Episode consolidation (slow-wave summarization) |
| `2` | `HU_JOB_KIND_REM` | **#10** | Cross-episode connection-finding |
| `3` | `HU_JOB_KIND_TTT_STEP` | **#05** | One verifier-driven gradient step |
| `4` | `HU_JOB_KIND_TTT_REVERT` | **#05** | Roll back a TTT step on dissent |
| `5` | `HU_JOB_KIND_FED_ROUND` | **#08** | One federated aggregation round |
| `6` | `HU_JOB_KIND_FED_KEYGEN` | **#08** | Noise pairwise key derivation |
| `7` | `HU_JOB_KIND_BENCHMARK` | **#14** | Run a longitudinal eval batch |
| `8` | `HU_JOB_KIND_PRM_TRAIN` | **#07** | Process-reward-model training pass |
| `9` | `HU_JOB_KIND_KV_COMPACT` | **#13** | DeltaKV residual recoding |
| `10` | `HU_JOB_KIND_QUARANTINE_REVIEW` | **#09** | Pending-fact promotion / expiry sweep |

Ordinals `11–63` are reserved for future allocation; `64+` is forbidden (must extend `uint64_t` bitmask in `hu_scheduler_t`). Adding a new kind requires (a) appending here, (b) updating `include/human/scheduler.h`, (c) handling the kind in every scheduler backend that introspects `kind`.

### `hu_provider_t.load_adapter` surface (init-04 owner; init-02 / #05 / #08 consume)

`load_adapter` is the canonical entry point for swapping LoRA adapters at inference time. Initiatives #02 (MoLoRA experts), #05 (TTT step), and #08 (federated aggregation result) all need the *exact same* signature.

```c
hu_error_t (*load_adapter)(hu_provider_t *self,
                           const hu_lora_adapter_t *adapter,  /* NULL = unload */
                           hu_lora_apply_mode_t mode);        /* REPLACE / STACK */
```

Cloud providers (`openai`, `anthropic`, `gemini`, `vertex`, …) must implement this returning `HU_ERR_NOT_SUPPORTED` — the daemon's personalization auto-load falls through to base chat as proven by `tests/test_provider_all.c::test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` (commit 028f4544). This is a **safety contract**: a cloud provider returning anything other than `HU_OK` or `HU_ERR_NOT_SUPPORTED` is a sprint-blocker.

---

## Portfolio overview

| # | Track theme | Slug | Primary outcome | Vtable touched | Risk tier |
|---|-------------|------|-----------------|-----------------|------------|
| **01** | Activation steering / SAE persona control on cloud providers | `activation-steering` | Persona signal moves model outputs even on cloud calls, via prompt-side representation control + verifier-loop steering | `hu_provider_t`, `hu_persona_t` | Medium |
| **02** | MoLoRA per-channel persona routing | `molora-channels` | Per-channel LoRA expert selection at inference time (one base, N small experts) | `hu_provider_t.load_adapter`, `hu_persona_overlay_t` | Medium |
| **03** | Apple FoundationModels first-class provider | `apple-fm-provider` | Native macOS / iOS 19 on-device model exposed as `hu_provider_t`; ANE inference, zero network | `hu_provider_t` (new `apple_fm.c`) | High (security + entitlements) |
| **04** | MLX provider for Qwen3-4B + LoRA | `mlx-qwen3-provider` | First on-device frontier-class provider with **real** LoRA application path (closes M3 Bridge B) | `hu_provider_t`, `hu_learner_vtable_t` | High (binary size, ASan over Metal) |
| **05** | Verifier-driven test-time training (TTT) | `verifier-driven-ttt` | Per-conversation tiny gradient updates when verifier flags low-fidelity outputs; reverted on user dissent | `hu_learner_t`, `hu_scheduler_t` | High (drift, safety) |
| **06** | SimPO + ORPO + GRPO-2 RL trainer additions | `simpo-orpo-grpo2` | Reference-free + reference-policy trainers added to the `hu_rl_trainer_t` vtable; replaces DPO-only path | `hu_rl_trainer_t` | Medium |
| **07** | ThinkPRM trained verifier panel | `thinkprm-verifier` | Replace prompt-critic verifiers with a small trained process-reward model running on-device | `hu_eval_judge_external_t`, `hu_reward_model_t` | Medium |
| **08** | Federated LoRA across the user's own devices | `federated-lora` | User's phone + laptop + desktop share LoRA gradients privately over mDNS + Noise; never leaves the user's hardware fleet | `hu_learner_vtable_t`, new `hu_federation_t` | High (network + crypto) |
| **09** | Memory poisoning defenses (trust tiers) | `memory-trust-tiers` | Per-memory trust score; quarantine quasi-attack patterns (MINJA, MemoryGraft); verifier rejects facts from poisoned sources | `hu_memory_t`, `hu_personal_model_t` | High (security-critical) |
| **10** | MemMachine episode storage + SleepGate consolidation | `episode-storage-sleep-consolidation` | First-class episodes (not summaries) as ground truth; NREM/REM-style sleep consolidation under `hu_scheduler_t` idle jobs | `hu_memory_t`, `hu_scheduler_t` | Medium |
| **11** | PRISM proactivity gate + Stephanie2 typing simulation | `proactivity-typing` | Agent proactively pings only when expected-utility gate passes; outgoing messages simulate realistic typing/pause cadence | `hu_feeds_t`, `hu_channel_t.send` | Medium |
| **12** | MCP server mode (h-uman exposed to other agents) | `mcp-server-mode` | h-uman daemon exposes its own persona + memory as an MCP server other agents (Cursor, Claude Code, Copilot) can subscribe to | new `hu_mcp_server_t` | Medium |
| **13** | DeltaKV / SWAN KV-cache compression | `kv-compression` | 4–8× KV-cache compression at <1% quality loss; combines with W10 + Track B speculative-decode prep | `hu_neural_memory_t`, `hu_provider_t` (for llamacpp) | High (correctness) |
| **14** | Public benchmark suite expansion | `public-benchmarks` | LongMemEval, LoCoMo, KnowU-Bench, EMPA, ProAgentBench wired into `human_tests` + a separate `human_eval` harness; public numbers drop | `eval.c`, `tests/eval/` | Low |

**Tier mapping** (`docs/standards/quality/governance.md` review depth):
- Low risk: 14
- Medium risk: 01, 02, 06, 07, 10, 11, 12
- High risk: 03, 04, 05, 08, 09, 13

---

## Dependency graph

```text
                                       ┌──────────────────────────┐
                                       │ 09 memory trust tiers    │
                                       │ (security spine)         │
                                       └──────────┬───────────────┘
                                                  │
       ┌──────────────────────┐                   │
       │ 04 MLX Qwen3         │←──────────────────┘
       │ (M3 Bridge B close)  │
       └─────────┬────────────┘
                 │
        ┌────────┴────────┐
        ▼                 ▼
   ┌──────────┐      ┌──────────────┐    ┌──────────────────┐
   │ 02 MoLoRA│      │ 05 TTT       │    │ 06 SimPO/ORPO/  │
   │ channels │      │ verifier-led │←───│ GRPO-2 trainers  │
   └────┬─────┘      └──────┬───────┘    └────────┬─────────┘
        │                   │                     │
        └────┬──────────────┴─────────────────────┘
             ▼
   ┌──────────────────────┐
   │ 07 ThinkPRM verifier │
   └──────────┬───────────┘
              │
              ▼
   ┌──────────────────────┐    ┌────────────────────────┐
   │ 14 Public benchmarks │←───│ 10 Episode + SleepGate │
   │ (closes the loop)    │    └────────────────────────┘
   └──────────────────────┘

   Independent (no upstream blocker):
     01 activation steering, 03 Apple FM, 08 federation, 11 proactivity+typing,
     12 MCP server, 13 KV compression.
```

The hard critical path is **04 → 05 → 07 → 14**: real on-device frontier inference, then live personalization, then a real trained verifier, then a public benchmark drop. Everything else is laterally accelerative.

---

## Initiative summaries (full design specs live in per-initiative docs)

### 01 — Activation steering / SAE persona control on cloud providers
**One-line.** Even when we call a frontier cloud model, we can steer outputs toward the user's persona by activation-steering the *draft* model used in speculative decoding and by adversarially weighting prompt sections via SAE features.
**Why now.** April 2026 arXiv: trained SAEs reliably isolate "warmth", "formality", "humor density" features; persona steering via residual-stream addition is empirically robust at small (1B–4B) scale.
**Vtable.** Extend `hu_provider_t` with optional `apply_steering(const float *vec, size_t dim)`; cloud providers no-op, MLX/llama.cpp providers actually apply.
**Spec output.** `docs/plans/2026-05-11-init-01-activation-steering.md`

### 02 — MoLoRA per-channel persona routing
**One-line.** One base model, four to eight tiny LoRA experts (one per channel + one per persona macro-mode), gated by a learned router at inference.
**Why now.** Mixture-of-LoRA-Experts (MoLoRA, LD-MoLE, SAMoRA) is the current SOTA for handling N-way style overlays without N full fine-tunes.
**Vtable.** `hu_provider_load_adapter` already exists; new `hu_persona_overlay_t.expert_id` field; router lives in `src/persona/`.
**Spec output.** `docs/plans/2026-05-11-init-02-molora-channels.md`

### 03 — Apple FoundationModels first-class provider
**One-line.** macOS 26 / iOS 19 ship an OS-level on-device model; expose it as a `hu_provider_t` so iPhone / Mac users get a true zero-network path by default.
**Why now.** Apple's WWDC 2025 surface is the single largest distribution vector for "actually yours" privacy story.
**Vtable.** `src/providers/apple.c` exists (stub); turn it into the real provider via Swift bridge in `apps/HumanKit`.
**Spec output.** `docs/plans/2026-05-11-init-03-apple-fm-provider.md`

### 04 — MLX provider for Qwen3-4B + LoRA
**One-line.** Close M3 Bridge B: the user's *actual chat model* gets real LoRA adapters applied on-device on Apple Silicon.
**Why now.** Qwen3-4B-Instruct + MLX-LoRA + 4-bit quant = 35–60 tok/s on M3 Max, fits in 4 GB.
**Vtable.** Extend `hu_provider_t` with `load_adapter` / `unload_adapter`; new `src/providers/mlx_qwen3.c`; helper subprocess pattern (already proven in `embedded.c`).
**Spec output.** `docs/plans/2026-05-11-init-04-mlx-qwen3-provider.md`

### 05 — Verifier-driven test-time training (TTT)
**One-line.** When the verifier panel flags a low-fidelity response, perform a tiny (1–10 step) gradient update on the LoRA adapter, scoped to that conversation, with explicit rollback on user dissent.
**Why now.** Verifier-Driven TTT (April 2026) is the first formulation that doesn't catastrophically forget.
**Vtable.** Hooks `hu_learner_t.step()` from `hu_agent_turn` post-verify path; rollback hook on user negative feedback.
**Spec output.** `docs/plans/2026-05-11-init-05-verifier-driven-ttt.md`

### 06 — SimPO + ORPO + GRPO-2 RL trainer additions
**One-line.** Move beyond DPO-only. SimPO drops the reference policy (lighter); ORPO bakes preference + SFT into one pass; GRPO-2 is the cheap cousin of GRPO.
**Why now.** SimPO/ORPO are mature; GRPO-2 was published April 2026 and is the first GRPO variant whose cost we can actually afford on-device.
**Vtable.** `hu_rl_trainer_t` (defined in `2026-05-11-full-sota-rl-improvement-loop-design.md`); add three new factories.
**Spec output.** `docs/plans/2026-05-11-init-06-simpo-orpo-grpo2.md`

### 07 — ThinkPRM trained verifier panel
**One-line.** Replace the prompt-critic verifier panel with a small trained Process Reward Model that runs on-device and is calibrated on h-uman's own DPO data.
**Why now.** ThinkPRM + Athena-PRM show that even 0.5B–1B PRMs beat prompt critics on reasoning tasks; we can train ours from already-collected interaction logs.
**Vtable.** `hu_reward_model_t` (new); replaces prompt critics behind `HU_VERIFIER_TRAINED=1`.
**Spec output.** `docs/plans/2026-05-11-init-07-thinkprm-verifier.md`

### 08 — Federated LoRA across the user's own devices
**One-line.** Phone, laptop, and desktop share LoRA gradients over the local network with a Noise-encrypted protocol; never leaves the user's device fleet.
**Why now.** SDFLoRA / DP-FedLoRA give us formal privacy budgets; mDNS + Noise gives us a secure local protocol; this is the *only* way to scale personalization without buying GPU time.
**Vtable.** New `hu_federation_t`; per-device LoRA shard upload; secure aggregation.
**Spec output.** `docs/plans/2026-05-11-init-08-federated-lora.md`

### 09 — Memory poisoning defenses (trust tiers)
**One-line.** Tag every memory with a trust tier (user-direct, persona-derived, third-party, untrusted); quarantine MINJA/MemoryGraft-style injection patterns; the verifier refuses facts from low-trust sources.
**Why now.** MINJA (Sep 2024) and MemoryGraft (April 2026) demonstrated that personal-AI memory is *highly* attackable. Without trust tiers, an adversary's group-chat message can rewrite the user's persona.
**Vtable.** `hu_memory_t` adds `trust_tier` field; `hu_personal_model_t.fact_t` gains `provenance`.
**Spec output.** `docs/plans/2026-05-11-init-09-memory-trust-tiers.md`
**Note.** This is the **highest-priority security-critical** initiative. Design doc co-owned by `security-reviewer`.

### 10 — MemMachine episode storage + SleepGate consolidation
**One-line.** First-class episodes (verbatim conversation turns) become the ground truth; consolidation runs as a `hu_scheduler_t` idle job using a NREM/REM-style two-phase process.
**Why now.** Mem0/MemMachine results (March 2026) show that summary-first memory loses critical detail; episode-first + late consolidation beats summary-first on LongMemEval / LoCoMo.
**Vtable.** `hu_memory_t` extension; new `hu_consolidation_t`.
**Spec output.** `docs/plans/2026-05-11-init-10-episode-storage-sleep-consolidation.md`

### 11 — PRISM proactivity gate + Stephanie2 typing simulation
**One-line.** Don't message the user unless expected utility exceeds a learned threshold; when we do send, simulate realistic typing latency + pauses so it feels human-paced.
**Why now.** PRISM (March 2026) is the cleanest formulation of "when to interrupt"; Stephanie2 (April 2026) shows that typing simulation alone moves "feels like a real person" preference by 11 points.
**Vtable.** `hu_feeds_t` adds `proactivity_gate`; `hu_channel_t.send` gains optional `typing_profile_t`.
**Spec output.** `docs/plans/2026-05-11-init-11-proactivity-typing.md`

### 12 — MCP server mode (h-uman exposed to other agents)
**One-line.** h-uman daemon exposes its persona + (consented) memory as an MCP server that Cursor, Claude Code, Copilot, etc. can subscribe to. Reverses the integration polarity.
**Why now.** MCP is the de-facto standard; we already have an MCP *client* (Claude Code features). MCP-server-mode flips us from "another assistant" to "the persona layer for every assistant the user already runs".
**Vtable.** New `hu_mcp_server_t`; reuses existing transport from `src/gateway/`.
**Spec output.** `docs/plans/2026-05-11-init-12-mcp-server-mode.md`

### 13 — DeltaKV / SWAN KV-cache compression
**One-line.** 4–8× compression of the KV-cache via DeltaKV-style residual coding + SWAN window pruning; combines cleanly with W10 + the speculative-decode prep in Track B of the 6-mo roadmap.
**Why now.** DeltaKV / SWAN (April 2026) are the first KV compressors that ship with end-to-end ASan-clean reference implementations and < 1% quality loss.
**Vtable.** Extends `hu_neural_memory_t`; new compression backend negotiated via provider capability flag.
**Spec output.** `docs/plans/2026-05-11-init-13-kv-compression.md`

### 14 — Public benchmark suite expansion
**One-line.** Wire LongMemEval, LoCoMo, KnowU-Bench, EMPA, ProAgentBench into `human_tests` (regression) + `human_eval` (longitudinal); publish numbers + methodology.
**Why now.** None of our existing competitors publish numbers on persona-faithfulness benchmarks. Being the first to publish is itself a moat.
**Vtable.** `eval.c` adapter additions; `tests/eval/` directory; no public-surface changes.
**Spec output.** `docs/plans/2026-05-11-init-14-public-benchmarks.md`

---

## Team dispatch

The 14 design docs are produced **in parallel** by a fleet of code-architect / security-reviewer subagents, dispatched from the same kickoff invocation. Each subagent:

1. Reads the relevant existing plans listed in this doc's `related:` frontmatter.
2. Reads the touched vtable header(s) in `include/human/`.
3. Reads the standards in `docs/standards/engineering/principles.md` + `docs/standards/security/threat-model.md` (when applicable).
4. Produces **exactly one** Markdown file under `docs/plans/2026-05-11-init-NN-<slug>.md`.
5. Satisfies the D0–D7 proof bar at the top of this document.
6. Returns a one-paragraph summary + the file path + the top open question.

The expected wall-clock for the design pass is roughly 1 working day of agent-time (~15–25 minutes per agent in parallel). The expected output is **14 design docs** sitting next to this file, ready for sprint planning.

After the fleet returns, this document gets a synthesis section choosing the top 4–6 initiatives for **Sprint SOTA-2026-01** and parking the rest with explicit "defer to Sprint NN" notes.

## Status table (updated as the fleet returns)

| # | Slug | Subagent | Status | Spec file |
|---|------|----------|--------|-----------|
| 01 | activation-steering | code-architect | **design done** | `2026-05-11-init-01-activation-steering.md` |
| 02 | molora-channels | code-architect | **design done** | `2026-05-11-init-02-molora-channels.md` |
| 03 | apple-fm-provider | code-architect | **design done** | `2026-05-11-init-03-apple-fm-provider.md` |
| 04 | mlx-qwen3-provider | code-architect | **design done — S1 ADOPTED** | `2026-05-11-init-04-mlx-qwen3-provider.md` |
| 05 | verifier-driven-ttt | code-architect | **design done — DEFERRED (needs #09 + #07 land)** | `2026-05-11-init-05-verifier-driven-ttt.md` |
| 06 | simpo-orpo-grpo2 | code-architect | **design done** | `2026-05-11-init-06-simpo-orpo-grpo2.md` |
| 07 | thinkprm-verifier | code-architect | **design done** | `2026-05-11-init-07-thinkprm-verifier.md` |
| 08 | federated-lora | code-architect | **design done — DEFERRED (SECAGG protocol revision required)** | `2026-05-11-init-08-federated-lora.md` |
| 09 | memory-trust-tiers | security-reviewer | **design done — S1 ADOPTED (patched post-review)** | `2026-05-11-init-09-memory-trust-tiers.md` |
| 10 | episode-storage-sleep-consolidation | code-architect | **design done — DEFERRED (job_kind collision now resolved here)** | `2026-05-11-init-10-episode-storage-sleep-consolidation.md` |
| 11 | proactivity-typing | code-architect | **design done — S1 ADOPTED (typing half only)** | `2026-05-11-init-11-proactivity-typing.md` |
| 12 | mcp-server-mode | code-architect | **design done — DEFERRED (adapter-replacement attack)** | `2026-05-11-init-12-mcp-server-mode.md` |
| 13 | kv-compression | code-architect | **design done — DEFERRED (correctness budget)** | `2026-05-11-init-13-kv-compression.md` |
| 14 | public-benchmarks | code-architect | **design done — S1 ADOPTED** | `2026-05-11-init-14-public-benchmarks.md` |
| **W0a** | hu_episode_t ODR cleanup | code-simplifier | **done** (commit `1b9705ac`) | `2026-05-11-w0a-episode-rename-report.md` |
| **W0b** | hu_mcp_server_t → hu_mcp_client_t rename | code-simplifier | **done** (commit `1e6746c7`) | `2026-05-11-w0b-mcp-rename-report.md` |

Legend: `dispatched` → `design done` → `sprint open` → `done` (or `parked` / `DEFERRED`).

## Synthesis (post-adversarial-review)

All 14 design docs landed (status table above). The cross-initiative adversarial review then ran three parallel passes — `critic`, `api-contract-watcher`, `security-reviewer` — producing a combined ledger of:

- **4 BLOCKER findings** (B1–B4: type ODR violation, migration default upgrades poison, ingest call-site patching gap, MCP adapter-replacement attack)
- **6 ABI-BREAKING surface deltas** with **37 name-collision call sites** to patch in lockstep
- **4 CRITICAL security findings** (SECAGG over GF(2^8) is mathematically wrong, TTT poisonable without #09, MCP peer-driven adapter replacement, env-var override for code injection)
- **10 MINJA bypass categories** the 10-pattern detector missed

The full ledger is `adversarial-review-synthesis.md`, synthesizing `adversarial-review-critic.md`, `adversarial-review-api-contracts.md`, and `adversarial-review-security.md`.

### Preconditions (executed as W0a + W0b — both `done`)

| # | Slice | Purpose | Resolves | Status |
|---|-------|---------|----------|--------|
| **W0a** | `hu_episode_t` rename to `hu_session_episode_t` (agent/) + `hu_deep_episode_t` (deep_memory/) | Resolves B1 (triple-ODR violation); unblocks #10 episode storage | B1 | done — commit `1b9705ac` |
| **W0b** | `hu_mcp_server_t` → `hu_mcp_client_t`; `hu_mcp_host_t` → `hu_mcp_engine_t` (with 1-release deprecation shims) | Frees the `hu_mcp_server_t` slot for init #12 (MCP server mode) | API-3 (name collision) | done — commit `1e6746c7` |

### S1 selection — Sprint SOTA-2026-01

Five initiatives chosen for S1 based on (a) cleared adversarial gate after precondition fixes, (b) leverage on the M1–M3 missions, (c) no remaining cross-initiative conflicts after the locked conventions above.

| # | Initiative | Why S1 | Required design revisions baked in |
|---|-----------|--------|--------------------------------------|
| **#09** | Memory trust tiers | Security spine; precondition for #04, #05 personalization gating | B2 (migration default = THIRD_PARTY), B3/A1 (4-site `ingest` audit), MINJA broadening to ≥30 patterns + NFKC + leetspeak + locale-mismatch + pending-facts queue; ordinal convention locked |
| **#04** | MLX Qwen3-4B provider | Closes M3 Bridge B — first real on-device frontier model with live LoRA | Consumes locked `load_adapter` surface; cloud providers return `HU_ERR_NOT_SUPPORTED` (pinned by `test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat`) |
| **#14** | Public benchmark suite | Locks observable quality numbers before #04 / #05 ship; provides regression floor | No revisions — design clean |
| **#01** (prompt half only) | Activation-steering on cloud providers | Persona steering on cloud chat without waiting on MLX integration | Implementation limited to prompt-side / SAE-feature weighting; MLX residual-stream patching deferred to S2 |
| **#11** (typing half only) | Stephanie2 typing simulation | Highest user-perceptible quality gain at zero security risk | PRISM proactivity gate deferred to S2 (utility-model needs real training data) |

### Deferred initiatives (with unblock condition)

| # | Initiative | Why deferred | What would unblock |
|---|-----------|--------------|---------------------|
| **#05** | Verifier-driven TTT | Cannot land safely until #09 (trust gating) + #07 (trained verifier) are live | Both land in S1/S2 |
| **#08** | Federated LoRA | SECAGG-over-GF(2^8) is mathematically wrong (Shamir over a prime field is required); attacker can reconstruct other devices' gradients | Protocol revision to Shamir over `GF(p)` with `p > 2^31` |
| **#10** | Episode storage + sleep consolidation | `hu_job_kind_t` collision now resolved by locked enum table above; design still references old ordinals | Re-spec to consume `HU_JOB_KIND_NREM=1`, `HU_JOB_KIND_REM=2` from the locked allocation |
| **#12** | MCP server mode | Peer-driven adapter replacement attack surface (a peer MCP client could request the daemon load an adversary's LoRA); needs explicit policy gate | Add adapter-load policy (signed-only, user-confirmation-required, or disabled) and matching test |
| **#13** | KV compression | Correctness budget (< 1% quality loss) requires DeltaKV reference implementation; not yet in tree | Land reference implementation + bring-up tests; then re-evaluate |

### Sprint 2+ inputs

This synthesis updates `2026-05-10-sota-roadmap-6mo.md` Month-2 with the S1 plan above and slots #05/#10/#12 into Month-3 once the unblock conditions clear. Concrete S1 stories will be opened in `sprints/sprint-2/stories.md` after the planning gate commit lands.

## Adversarial review

The three reviewer passes have run. Their outputs:

- **critic** subagent → `adversarial-review-critic.md` (4 BLOCKERs B1–B4, plus half-fixes and cross-initiative regressions per design doc).
- **api-contract-watcher** → `adversarial-review-api-contracts.md` (6 ABI-breaking findings + 37 name-collision call sites, with the locked conventions above as the resolution).
- **security-reviewer** → `adversarial-review-security.md` (4 CRITICAL findings + 10 MINJA bypass categories; informs the #09 broadened detector and the #08 deferral).
- **synthesis** → `adversarial-review-synthesis.md` (go/no-go matrix; precondition fixes for S1; design revisions required for deferred initiatives; S1 implementer prompt checklist additions).

Before any S1 sprint closes, the **sprint-auditor** does an independent end-of-sprint pass: did we ship what the design doc claimed, or did we silently descope? That gate runs against this document and the corresponding `init-NN-*.md`s, not against the implementation alone.

## Anti-scope

Things explicitly **out of scope** for this 14-initiative program (so they don't quietly re-enter):

- Generic benchmark chasing (MMLU, MT-Bench at large scale) — we still call frontier models there.
- Replacing the existing 31 channels — `M6 channel focus` already covers Tier-1 prioritization.
- Web UI redesign — design tokens already cover SOTA aesthetics.
- Marketing / DAU growth — `M4 Ship to Users` owns it.

If a subagent's design doc starts drifting into one of these, fail the D7 ("defer / descope condition") gate.

---

**End of master coordinator. The fleet ships next.**
