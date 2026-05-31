---
title: "SOTA roadmap — six-month plan to lead on personalization, performance, and UX"
created: 2026-05-10
status: deferred
related:
  - ../../CLAUDE.md
  - ../../AGENTS.md
  - 2026-05-10-m3-frontier-model-bridge.md
  - 2026-05-10-behavior-v1-followups.md
  - 2026-05-10-master-follow-through-program.md
  - 2026-05-10-memory-v2-roadmap-overview.md
  - ../standards/ai/evaluation.md
  - ../standards/quality/governance.md
last_audit: 2026-05-25
---

# SOTA roadmap — six-month plan to lead on personalization, performance, and UX

This document is the **portfolio plan** for getting h-uman to state-of-the-art on the three dimensions that actually matter for the product: **personalization, on-device performance, and shipped UX**. It is deliberately scoped to six months and three concurrent tracks. It assumes a real engineering team with quality gates as the binding constraint.

It is NOT a plan to beat frontier models on generic benchmarks (we call the same models). It is a plan to be SOTA at the things our thesis says are our moat:

- **Persona as compiled architecture, not markdown templates.**
- **Privacy by architecture, not by settings.**
- **On-device personalization that measurably changes inference behavior.**

## North-star metrics (six-month exit)

These are the gates the whole program is steering toward. If we hit them, we are SOTA on our chosen dimensions.

| # | Dimension | Today | Six-month target |
|---|---|---|---|
| N1 | Persona eval — "feels like me" (50 scenarios, blind A/B vs Gemini PI / Claude Cowork) | not measured | ≥ 65% preference rate vs both |
| N2 | Personal-model retrieval relevance (held-out personal-fact eval) | heuristic ≈ unknown | ≥ 80% top-3 relevance |
| N3 | Tier-1 channel naturalness (50-scenario per-channel eval) | not measured per channel | ≥ 8/10 on all 4 Tier-1 channels |
| N4 | Conversational-tier decode TPS (E4B target) | n/a (no tier routing) | ≥ 100 tok/s |
| N5 | Analytical-tier decode TPS (31B + aligned draft via spec decode) | 20–21 tok/s | ≥ 35 tok/s |
| N6 | Cached TTFT (chat-length prompt, warm system prefix) | 670 ms | ≤ 200 ms |
| N7 | Adapter regression on base benchmarks (MT-Bench short) | not measured | ≤ 1% drop vs base |
| N8 | DAU / day-7 retention | 0 | 100 DAU / 30% retention |
| N9 | Binary size, RSS, ASan, test count | 1750 KB / 6 MB / 0 / 9500+ | hold or improve all four |

**Hard quality bar (binding across all tracks):** zero ASan errors, zero `-Werror` warnings, every new behavior has an eval gate in CI, no test count regressions, MinSizeRel binary stays under 2 MB.

## Program overview

| Track | Theme | Primary outcome | Owners |
|---|---|---|---|
| **A** | Personalization SOTA — on-device LoRA loop | LoRA that demonstrably changes inference; persona+memory eval ≥ N1, N2 | ML lead + 1 |
| **B** | Performance SOTA — multi-tier routing + spec decode | N4, N5, N6 hit; aligned draft adapter shared with Track A | Systems lead + 1 |
| **C** | UX SOTA — 4 Tier-1 channels shipped | N3, N8 hit; first-run onboarding to first message ≤ 5 min | Product lead + 1 |
| **E** | Eval / quality cross-cut | All gates land in CI; no track ships without its eval | QA lead (shared) |

Tracks A and B intersect at one shared milestone — the **persona-aligned E4B draft adapter** — which is the single thing that unlocks both real personalization (Track A) and real speculative decoding (Track B). If only one thing in this plan succeeds, it should be that.

## Suggested calendar

```text
Month 1   A1 (data pipeline)    + B1 (multi-tier routing)   + C1 (onboarding)        Track E: eval rig stands up
Month 2   A2 (frontier LoRA)    + B2 (spec decode prep)     + C2 (tier-1 hardening)  Track E: persona eval suite v0
Month 3   A3 (aligned draft) ───────────── unifies with B2 ── + C3 (naturalness)     Track E: per-channel eval suites
Month 4   A4 (personal model)   + B3 (TTFT, prompt cache++) + C4 (cross-channel)     Track E: regression gates in CI
Month 5   A5 (continuous loop)  + B4 (Apple-deep)           + C5 (retention infra)   Track E: longitudinal eval
Month 6   A6 (eval + proof)     + B5 (cross-platform)       + C6 (100 DAU launch)    Track E: external benchmark drop
```

**Global proof bar (every slice, every PR)**

| Gate | Command / artifact | Pass |
|---|---|---|
| G0 | `cmake --build build` (dev preset) | 0 errors, `-Werror` clean on touched TUs |
| G1 | `./build/human_tests` | 0 failures, 0 ASan leaks |
| G2 | `scripts/verify-all.sh` | All segments green before merging to main |
| G3 | Track-specific eval (see each track) | No regression > 5% on relevant eval; lifts ≥ stated target |
| G4 | Release slice: `cmake --preset release` + size note | MinSizeRel binary ≤ 2 MB; RSS ≤ 6 MB excluding model |

---

## Track A — Personalization SOTA

**Problem statement.** Today the personal model uses heuristic regex fact extraction and prompt-injection. The "private learning" mission (M3 in CLAUDE.md) trains a reference toy GPT, not the frontier model the user actually chats with. Persona depth is real in code (27 modules) but does not yet *change the model's outputs*, only its system prompt.

**Track outcome:** a continuous on-device personalization loop where conversation history → curated training data → LoRA adapter that measurably changes the frontier model's behavior → adapter is validated by eval before promotion.

### Phase A1 — Training-data pipeline (Month 1)

| Step | Work | Proof |
|---|---|---|
| A1.1 | Conversation-history → training-example extractor (LM SFT pairs, DPO preference pairs, persona examples by channel). There is a draft at `tests/test_training_data_extractor.c` — promote to `src/ml/` with full coverage. | `human ml prepare-conversations` ingests a 1000-message fixture; produces ≥ 800 valid SFT examples, ≥ 100 DPO pairs |
| A1.2 | PII redaction + quality filter (entropy, dedup, length, language) | New eval `tests/test_training_data_quality.c`: < 0.1% PII leak rate, < 1% near-duplicate rate |
| A1.3 | Persona example bank generation per channel (`hu_persona_example_bank_t`) | `human persona analyze --from-history` produces per-channel banks; existing `persona/examples.c` selector returns ≥ 5 high-quality examples for each Tier-1 channel |
| A1.4 | Tokenizer parity audit between training and inference | Smoke test: tokenize 100 random conversations through training path and inference path; mismatch rate = 0 |

**Phase exit (G3):** Reproducible pipeline that converts a user's chat history into training-ready artifacts with privacy and quality guarantees. Data never leaves the device.

### Phase A2 — Frontier-model LoRA bridge (Month 2)

This is the M3 mission delivery. Plan exists at [`2026-05-10-m3-frontier-model-bridge.md`](2026-05-10-m3-frontier-model-bridge.md); A2 executes it.

| Step | Work | Proof |
|---|---|---|
| A2.1 | `human ml lora-persona` trains against the actual served model (Gemma-4-31B via mlx_lm.lora) — not the toy HUML GPT | Training completes; adapter file produced; loads at inference |
| A2.2 | Fusion pipeline (`mlx_lm.fuse`) + checkpoint management | Fused checkpoint loads at inference; size delta < 5% |
| A2.3 | Quality preservation eval: MT-Bench-short, AlpacaEval-short delta | Δ score ≤ 1% vs base model |
| A2.4 | Persona-fidelity eval v0 (10 hand-curated scenarios) | Adapter scores ≥ 30% higher than base on persona traits |

**Phase exit (G3):** Single command produces a LoRA adapter that loads cleanly, preserves base capability within 1%, and demonstrably shifts persona-fidelity scores upward.

### Phase A3 — Persona-aligned draft adapter (Month 3) — SHARED WITH TRACK B

The single highest-leverage milestone in the plan. Performance and personalization converge here.

| Step | Work | Proof |
|---|---|---|
| A3.1 | Train an E4B LoRA on the same persona signal used in A2 | Adapter loads in mlx_lm 0.31.x; smoke generates coherent text |
| A3.2 | Verify spec-decode acceptance rate with aligned draft | Acceptance rate ≥ 50% on Tier-1 conversation set (vs ~0% with unaligned) |
| A3.3 | Bench end-to-end: 31B target + E4B aligned draft via speculative decoding | Decode TPS ≥ 32 (vs 20 baseline) on stream_long_reply scenario |
| A3.4 | Quality eval: same persona scenarios as A2.4 + naturalness | No degradation vs A2.4 |

**Phase exit (G3):** Speculative decode with persona-aligned draft delivers ≥ 1.5× decode TPS and equal-or-better persona scores. This phase is also the validation that the training loop actually works end-to-end.

### Phase A4 — Personal-model upgrade (Month 4)

Today: regex-based fact extraction, prompt-string summary. Target: structured, learned, retrievable.

| Step | Work | Proof |
|---|---|---|
| A4.1 | Replace heuristic fact extractor with structured NER + confidence + decay (`include/human/memory/personal_model.h` is currently 147 lines — expand `hu_personal_model_t` to track confidence, last-seen, source-turn) | Unit tests for confidence math, decay, retrieval |
| A4.2 | Personal-model facts indexed via embeddings (reuse `src/memory/` vector search) | Top-k retrieval relevance ≥ 80% on held-out fact eval |
| A4.3 | Personal model facts → training signal (not only prompt injection) — emit synthesized SFT examples reinforcing known facts | A2/A3 retraining incorporates personal-model signal; persona eval lifts |
| A4.4 | Privacy lifecycle: TTL, manual delete, export, secure-erase on uninstall | Tests for each lifecycle path; threat-model doc updated |

**Phase exit (G3):** Personal model retrieval ≥ 80% relevance; facts measurably feed the training loop.

### Phase A5 — Continuous learning loop (Month 5)

| Step | Work | Proof |
|---|---|---|
| A5.1 | Nightly opportunistic retraining job (`launchd` / systemd timer) that picks up new examples, retrains LoRA, writes candidate adapter | Candidate adapter produced from synthetic 24h data; logs deterministic |
| A5.2 | A/B harness: candidate adapter vs current adapter on persona eval | Auto-promote if Δ ≥ 1 stderr with n ≥ 10; auto-reject if regression |
| A5.3 | DPO pair extraction from conversation feedback signals (existing partial work in `src/ml/dpo.c`) | DPO training cycle completes; rejection samples logged |
| A5.4 | Rollback path: keep last-good adapter, restore on health failure | Forced regression test: candidate degrades → system rolls back automatically |

**Phase exit (G3):** Adapter quality improves automatically over time without human intervention; rollback proven safe.

### Phase A6 — Eval and proof (Month 6)

| Step | Work | Proof |
|---|---|---|
| A6.1 | "Feels like Seth" eval suite: 100 scenarios, judged blind by three human raters and a frontier model judge | Inter-rater agreement κ ≥ 0.6; suite checked in to `eval_suites/persona/` |
| A6.2 | Head-to-head: h-uman with persona adapter vs Gemini Personal Intelligence vs Claude Cowork on the same 100 scenarios | N1 metric: ≥ 65% preference rate vs both, with confidence intervals |
| A6.3 | Public benchmark drop: anonymized eval suite + methodology paper | Published artifact; reproducible on commodity M-series |

**Phase exit (G3):** N1 and N2 met. Eval suite shipped as a public benchmark.

---

## Track B — Performance SOTA

**Problem statement.** We are at ~78% of memory-bandwidth peak on 31B-4bit (20 tok/s of a 26 tok/s ceiling). All easy software levers are exhausted (proven by the n=3 bench: prompt cache + KV quantization + LoRA fuse net to ±2% at chat lengths). Real wins require **changing what model runs when** (multi-tier routing) and **getting the smaller model to do most of the work** (spec decode with an aligned draft).

**Track outcome:** Reflexive < 100 ms, Conversational > 100 tok/s on E4B, Analytical 1.5–2× on 31B via aligned-draft spec decode, TTFT < 200 ms cached.

### Phase B1 — Multi-tier model routing actually works (Month 1)

| Step | Work | Proof |
|---|---|---|
| B1.1 | Extend `src/agent/model_router.c` to route `HU_TIER_CONVERSATIONAL` to a local E4B model when available, falling back to cloud only when local unavailable. The existing test (`tests/test_model_router.c`) currently *enforces* on-device-only-for-REFLEXIVE — update both code and test contract; risk is high (CLAUDE.md flags this) | New router test asserts: REFLEXIVE→local-tiny, CONVERSATIONAL→local-E4B if loaded else cloud-flash, ANALYTICAL→local-31B else cloud-pro, DEEP→cloud-pro |
| B1.2 | mlx-server supports dual-model serving (E4B + 31B) or two ports + reverse routing | `human-serve.sh status` shows both models loaded; latency probe under 1 s for E4B path |
| B1.3 | Memory budget check: E4B + 31B both quantized must fit in 128 GB | RSS measurement persisted; alarm if > 80 GB total |

**Phase exit (G3):** Tier routing measurable per request; conversational responses < 1 s end-to-end on local E4B.

### Phase B2 — Spec decode prep (Month 2)

Build the infrastructure spec decode needs *before* the aligned draft from A3 arrives.

| Step | Work | Proof |
|---|---|---|
| B2.1 | The `speculative_generate_step` LanguageModelOutput unwrap patch (landed in mlx-server today) should be upstreamed to mlx-lm as a PR | PR opened against ml-explore/mlx-lm; or vendored cleanly in our repo |
| B2.2 | The `RotatingKVCache.to_quantized` patch (landed today) ditto | PR or vendored shim documented |
| B2.3 | Bench rig hardening: `scripts/bench-gemma-perf.py` with n ≥ 5 and confidence intervals; nightly CI bench on a self-hosted M-series runner | Bench artifacts persisted nightly; regression alerts wired to Slack/Linear |
| B2.4 | Profile decode bottleneck per layer (mlx tracing) to know where to cut next | Profile artifact in `docs/perf/`; identify top 3 layer types by ms |

**Phase exit (G3):** All infrastructure ready to consume A3's aligned draft. CI bench live with regression alerts.

### Phase B3 — TTFT reduction (Month 3)

Cached TTFT is 670 ms today. Target ≤ 200 ms for cached prompts.

| Step | Work | Proof |
|---|---|---|
| B3.1 | Prompt cache extension: cache persona-examples prefix, not only system message. Hash on `(persona_id, channel, examples_hash)` | Bench: TTFT for warm persona prompt < 250 ms |
| B3.2 | Pre-warm policy: keep model state hot between requests; idle eviction after N minutes | TTFT P50 < 200 ms on warm path; cold start < 1 s |
| B3.3 | Optional native-C HTTP front (only if profile shows Python overhead > 5%) | If shipped: minimum 5 ms TTFT win measurable; otherwise documented "not worth it" decision |
| B3.4 | Cache-aware request routing in gateway: avoid breaking cache continuity unnecessarily | Cache hit rate ≥ 70% on sequential same-persona conversation traces |

**Phase exit (G3):** N6 met (cached TTFT ≤ 200 ms).

### Phase B4 — Apple-deep optimizations (Month 4)

Only after Tracks A and B have delivered measurable value. This phase is the answer to "Apple proprietary GPU APIs in C/assembly" — pursued surgically where the bench identifies a real bottleneck.

| Step | Work | Proof |
|---|---|---|
| B4.1 | Custom `.metal` kernel for sliding-window KV pack at long context (≥ 2K tokens) — built only if profile shows KV ops are ≥ 20% of decode time at long context | Bench at 4K-token context shows ≥ 5% TPS improvement; quality unchanged |
| B4.2 | Investigate 3-bit weight quantization on 31B (QAT or post-training) with full quality eval | Bench TPS lift vs 4-bit; quality delta ≤ 1.5% on MT-Bench-short; otherwise documented "rejected" |
| B4.3 | ANE path for embedding model (small model, fits ANE; offloads vector search prep) — separate from chat model | Embedding throughput on ANE ≥ 5× CPU baseline; main chat tps unchanged |
| B4.4 | AMX inline-assembly spike for batched K/V packing — only if profile shows BNNS isn't already exploiting it | Either measurable win or written-up "Accelerate already wins" |

**Phase exit (G3):** At least one Apple-deep optimization delivers a measurable, non-regressive win OR all four are written up as "evaluated and rejected" with evidence. No yak-shaving without numbers.

### Phase B5 — Cross-platform performance parity (Month 5)

| Step | Work | Proof |
|---|---|---|
| B5.1 | Linux CPU path via llama.cpp backend in `src/providers/embedded.c` | Linux x86_64 build serves at ≥ 5 tok/s on Gemma-E4B-4bit on a representative cloud VM |
| B5.2 | Docker image: same binary serves model fetched at first run | `docker run` to first token < 60 s on a fresh host; image size < 100 MB without model |
| B5.3 | Raspberry Pi 5 (8 GB) smoke: load smallest supported model + serve | Pi 5 path documented in `docs/standards/engineering/cross-platform.md` with measurements |

**Phase exit (G3):** Three platforms have measured numbers; no platform regresses from documented baseline.

### Phase B6 — External benchmark publication (Month 6)

| Step | Work | Proof |
|---|---|---|
| B6.1 | Public bench artifact: h-uman vs Ollama vs llama.cpp vs raw mlx_lm on identical hardware | Reproducible; numbers published; methodology in `docs/perf/` |
| B6.2 | Quality + performance head-to-head with Gemini Personal Intelligence on local-equivalent tasks | Published artifact under `docs/perf/competitive/` |

**Phase exit (G3):** Public artifact. Numbers stand up to external reproduction.

---

## Track C — UX SOTA (4 Tier-1 channels)

**Problem statement.** `human onboard` exists (405 LOC, auto-suggested on first run) but defaults are weak, channel setup friction is high, and there is no shipped flow that takes a new user from install to "this AI actually knows me." Tier-1 naturalness eval doesn't exist yet.

**Track outcome:** A new user can install, configure persona, connect Telegram/Discord/iMessage/Slack, and have natural conversation within five minutes. 100 DAU at 30% day-7 retention by month six.

### Phase C1 — Onboarding gate (Month 1)

| Step | Work | Proof |
|---|---|---|
| C1.1 | Audit `src/onboard/onboard.c` against the actual first-run friction list. Add starter persona presets (e.g. "casual", "professional", "warm-and-direct") with channel overlays | New user smoke-test: install → first reply in < 5 min on at least one channel |
| C1.2 | Model wizard: detect local hardware, pick default tier defaults (E4B + 31B on M-series, llama.cpp on Linux), validate disk + RAM budgets up front | `human init` exits with clean status; no post-init re-config required |
| C1.3 | Persona creation wizard pulls in user-provided seed messages (3–10) and proposes a starter persona via the existing `src/persona/creator.c` | Onboarding eval: 5 test users produce non-default personas |
| C1.4 | Telemetry-free product analytics (local event log; user can inspect) | Local event log shows install → first message latency P50 < 5 min |

**Phase exit (G3):** Five test users complete the install-to-first-message path in < 5 min, three with non-default personas.

### Phase C2 — Tier-1 channel hardening (Month 2)

| Channel | Hardening targets | Proof |
|---|---|---|
| Telegram | Bot setup wizard, voice notes via existing voice pipeline, group-chat awareness | E2E test bot in CI; voice round-trip < 3 s |
| Discord | Slash-command surface, server-permission docs, voice-channel join (stretch) | 50-message e2e test against staging server |
| iMessage | macOS bridge stability (`src/channels/imessage.c`), group thread routing | 100-message e2e test; no message lost |
| Slack | Workspace install flow, app-mention vs DM routing, thread awareness | 50-message e2e test; thread continuity verified |

**Phase exit (G3):** All 4 Tier-1 channels pass their e2e test suite under CI.

### Phase C3 — Naturalness eval per channel (Month 3)

| Step | Work | Proof |
|---|---|---|
| C3.1 | 50-scenario naturalness eval per channel — covers tone, length, emoji norms, formality (matches `hu_persona_overlay_t` axes) | `eval_suites/channel/<channel>/` directories committed; CI runs them |
| C3.2 | Persona overlay validation: same persona, different channel, demonstrably different output | Stat test: overlay-aware output is preferred ≥ 70% over un-overlay output by frontier-judge model |
| C3.3 | Channel-specific cold-path latency (TTFT including channel adapter overhead) | TTFT under 1 s on each Tier-1 channel |

**Phase exit (G3):** N3 met — every Tier-1 channel ≥ 8/10 on its naturalness eval.

### Phase C4 — Cross-channel memory continuity (Month 4)

| Step | Work | Proof |
|---|---|---|
| C4.1 | User says X on Telegram → references X on Slack three days later → bot recalls correctly via personal-model retrieval (A4 integration) | Cross-channel-reference eval: ≥ 80% resolution rate on 30-scenario test set |
| C4.2 | Identity unification across channels: user-binding flow, secure pairing reused | Tests: unauthorized cross-channel access denied; authorized works |
| C4.3 | Privacy disclosure: clear UI on what the assistant remembers and how to delete | Disclosure flow tested; matches `docs/standards/security/data-privacy.md` |

**Phase exit (G3):** Cross-channel reference resolution ≥ 80%; privacy disclosure verified.

### Phase C5 — Retention infrastructure (Month 5)

| Step | Work | Proof |
|---|---|---|
| C5.1 | Daily-active-use signals (local-only): activity log, friction-event log, sentiment log | Schema + tests in `src/observability/`; user can inspect / delete |
| C5.2 | Health-check loop: model warm, channels live, personal-model populated; soft notification if degraded | Health check runs every N minutes; degradation triggers logged remediation |
| C5.3 | Test cohort onboarding: 20+ users for the closed beta | Cohort tracking sheet; consent process documented |

**Phase exit (G3):** Cohort active; signals visible.

### Phase C6 — 100 DAU launch (Month 6)

| Step | Work | Proof |
|---|---|---|
| C6.1 | Soft-launch to friends-and-family with the persona+memory+adapter loop fully wired | 50+ users invited; install → first-message conversion ≥ 60% |
| C6.2 | Public landing + onboarding flow on the existing Astro site | Landing page conversion (visit → install) ≥ 5% |
| C6.3 | Persona showcase demo: live persona-fidelity comparison against Gemini PI / Claude Cowork | Public-facing demo deployed |
| C6.4 | Retention measurement | N8 met: 100 DAU at 30% day-7 retention |

**Phase exit (G3):** N8 met.

---

## Track E — Eval and quality cross-cut

Eval is not optional — it is the **release gate** for every track. This track owns the eval rig and gates.

### Phase E1 — Eval rig stands up (Month 1)

| Step | Work | Proof |
|---|---|---|
| E1.1 | Unify eval interfaces: persona eval, channel eval, performance eval, regression eval all share a single CLI (`human eval --suite=<name>`) | All three tracks consume the same eval CLI; results in canonical JSON |
| E1.2 | Self-hosted M-series CI runner for bench + persona eval | Nightly CI green; artifacts persisted ≥ 30 days |
| E1.3 | Eval baseline JSON in `docs/evaluation/baseline.json` updated and locked | Baseline locked at start of plan; deltas tracked per PR |

### Phase E2 — Per-track eval suites (Months 2–3)

| Suite | Owner | Phase exit |
|---|---|---|
| `eval_suites/persona/` | Track A | Inter-rater κ ≥ 0.6; published baseline |
| `eval_suites/channel/<tier1>/` | Track C | 50 scenarios each, frontier-judge consistent |
| `eval_suites/perf/` | Track B | Nightly bench with stddev; regression alerts |
| `eval_suites/cross-channel/` | Track A + C | 30 scenarios; ≥ 80% resolution required |

### Phase E3 — Regression gates land in CI (Month 4)

| Step | Work | Proof |
|---|---|---|
| E3.1 | CI fails any PR that regresses persona eval > 5%, naturalness eval > 5%, perf bench > 5% | PRs blocked when regression confirmed; bypass requires "regression accepted" label |
| E3.2 | Adapter A/B harness gates promotion: candidate must beat current by ≥ 1 stderr | Promotion log auditable; rollback path tested |

### Phase E4 — Longitudinal eval (Month 5)

Repeat eval suites weekly; track drift over time as the continuous-learning loop runs.

### Phase E5 — External benchmark drop (Month 6)

Public release of anonymized persona eval suite, methodology, and head-to-head numbers vs. Gemini PI and Claude Cowork.

---

## Dependency graph

```text
Track A: A1 ──► A2 ──► A3 ──► A4 ──► A5 ──► A6
                       │
                       │  (shared: persona-aligned E4B draft)
                       │
Track B: B1 ──► B2 ────┴─► B3 ──► B4 ──► B5 ──► B6
                       │
                       │  (shared: eval rig)
                       │
Track E: E1 ──► E2 ────┴─► E3 ──► E4 ──► E5

Track C: C1 ──► C2 ──► C3 ──► C4 ──► C5 ──► C6
                                │
                       (consumes A4 personal-model upgrade and B1 routing)
```

**The critical-path lock-in:** A3 is the single point where personalization and performance meet. If A3 lands on time (end of month 3), the back half of both tracks accelerates. If A3 slips, B's spec-decode wins slip and the convergence story for the public benchmark in month 6 weakens.

## Risks and mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| LoRA training quality regression on 31B base | Medium | High | A2.3 quality gate before any A3 work; rollback adapter ready |
| Speculative decode acceptance < 50% even with aligned draft | Medium | Medium | Drop to E4B-only conversational tier for performance win; spec decode becomes nice-to-have |
| Tier-1 channel API breakage (vendor change) | Low–Medium | High per channel | Treat each channel as an isolated dependency; channel adapters are vtable-isolated already |
| Personal-model privacy incident (PII leak through model output) | Low | Catastrophic | A1.2 redaction + A6 audit; threat-model review at every major eval cycle |
| `model_router.c` test contract change destabilizes existing 9500+ tests | Medium | Medium | B1.1 risk-tier HIGH; phased migration with parallel test contracts before deletion |
| External eval benchmark contested | Medium | Medium | Inter-rater κ ≥ 0.6, frontier judge confirmation, public methodology |
| Six-month plan calendar slips | High | Low–Medium | Each phase has G3 exit; calendar is suggested, dependencies are real |

## What this plan explicitly excludes

- **Beating frontier on generic LLM benchmarks** (MMLU, HellaSwag, etc.) — we call the same models, won't win at their layer.
- **Toy GPT improvements in `src/ml/`** — Phase 0 work stays reference-only; A2 makes the frontier bridge real.
- **Web dashboard polish for end users** — Tier-1 channels are the product surface. The dashboard stays a dev/operator tool.
- **Voice-realtime SOTA** — separate workstream; on the critical path only insofar as Telegram/iMessage voice-notes are covered in C2.
- **Mobile-app deep work** — iOS/macOS/Android apps exist; deepening them is post-SOTA.
- **HuLa as platform** (M5 in CLAUDE.md) — explicitly out of scope for the six-month window; resume after we have users to test against.
- **Generic UI/design overhauls** — design system is mature; UX SOTA here means *conversation quality*, not visual polish.

## What lands if only Month 1 happens

If the plan is interrupted after month 1, we still ship measurable value:
- Track A1: a real training-data pipeline with privacy guarantees (foundation of the M3 thesis).
- Track B1: multi-tier routing fixed; conversational tier on local E4B.
- Track C1: onboarding gate verified with five test users.
- Track E1: eval rig in CI.

This is the **minimum viable improvement**. The personalization and performance plays are non-trivial without months 2–3, but every month exits with shippable value.

## Decision log entries the plan will produce

The plan should generate at least these ADRs (in `docs/plans/adr/`):

- `2026-MM-XX-model-router-tier-contract.md` — outcome of B1.1 contract change
- `2026-MM-XX-frontier-lora-vs-toy-gpt.md` — outcome of A2 (M3 bridge)
- `2026-MM-XX-spec-decode-draft-policy.md` — outcome of A3 (when to use spec decode)
- `2026-MM-XX-three-bit-weight-rejected-or-adopted.md` — outcome of B4.2
- `2026-MM-XX-persona-eval-methodology.md` — outcome of A6 / E5

## Open questions to resolve in week one

1. **Hardware for nightly CI bench**: which M-series machine becomes the canonical perf rig?
2. **Persona eval judge model**: do we use a frontier judge for the bench (Gemini 3.1 Pro / Claude Opus) and accept the cost, or build a local judge?
3. **Cohort recruitment for C5**: friends-and-family network, or a closed-beta signup form? Privacy posture matters.
4. **Failure mode for adapter rollback (A5.4)**: which health signal triggers rollback — eval score, perplexity drift, user feedback signal, or all three?
5. **Public benchmark licensing (A6 / B6 / E5)**: what license do the published eval suites carry, and do we need an external review for privacy claims?

Resolve these in the program kickoff. Plan execution starts after.
