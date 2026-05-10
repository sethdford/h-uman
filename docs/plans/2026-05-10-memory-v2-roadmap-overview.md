---
title: "Memory v2 Roadmap — Overview"
created: 2026-05-10
status: in_progress
parent: 2026-05-10-memory-roadmap-overview.md
related:
  - 2026-05-10-master-follow-through-program.md
  - 2026-05-10-memory-v2-execution-plan.md
  - 2026-05-10-w7-phase1-bypass-inventory.md
  - 2026-05-10-w1-bitemporal-foundation.md
  - 2026-05-10-w2-background-consolidation.md
  - 2026-05-10-w3-multi-graph-topology.md
  - 2026-05-10-w4-self-rag-provenance.md
  - 2026-05-10-w5-agent-writable-persona.md
  - 2026-05-10-w6-eval-memrl-redteam.md
---

# Memory v2 Roadmap — Overview

**Execution sequence and gates:** [`2026-05-10-memory-v2-execution-plan.md`](2026-05-10-memory-v2-execution-plan.md).

## Why this exists

v1 (W1–W6, merged May 2026) shipped *trustworthy structural memory*: bitemporal edges, write-time trust, idle consolidation, multi-graph cross-edges, response-path verification with provenance receipts, agent-writable persona deltas, and cascading erasure. The result is a memory subsystem that holds up under composed adversarial attack (W6's 8-scenario E2E proof passes on every build).

What v1 does **not** do — and where every other 2026 SOTA memory system has moved — is make the *model itself* smarter from your data, verify *during* generation rather than after, and present the agent with a single coherent picture of you instead of four parallel subgraphs. The honest gap report (`v2-gap-analysis` discussion, conversation thread May 2026) lists ten frontier capabilities, of which three (LoRA-loop closure, inline Self-RAG with abstention, unified user world model) are the primary leverage points.

This roadmap commits to **all ten**, sequenced by architectural dependency, decomposed in a way that prevents duplication and keeps each piece testable in isolation.

## North-star outcomes

After v2 lands, h-uman should be the only memory system in 2026 that simultaneously:

1. **Adapts the chat-time model's weights** to the user on-device (LoRA + DPO from W4 verifier flags + W5 persona deltas + W3 case outcomes).
2. **Verifies inline with abstention** — the model emits retrieve/critique/refuse tokens during generation, not after.
3. **Reasons over a unified world model** — one struct fuses entities, beliefs, persona, emotional state, goals, theory-of-mind.
4. **Carries beliefs, not floats** — every fact is a posterior with provenance, not a `confidence: float`.
5. **Reuses prior reasoning** — KV-cache and chain-of-thought are stored, retrieved, and replayed.
6. **Plans with goal-conditioned retrieval** — the planner's current goal shapes what gets retrieved.
7. **Rehearses counterfactually during sleep** — sleep-time compute scheduler runs LoRA, KV-warming, query precomputation, "what would I have said differently."
8. **Can cryptographically forget** — per-user envelope encryption; deletion = key destruction; DP-SGD on writes.
9. **Knows what it should not say** — negative memory is first-class, not a hedge.
10. **Proves itself on benchmarks** — LoCoMo / LongMemEval / DMR / MINJA / MemoryAgentBench in CI.

## The architecture

v2 is a 7-layer stack. Every workstream extends exactly one layer through exactly one vtable. No layer reaches around another. This is the "no duplication" guarantee.

```
   ┌──────────────────────────────────────────────────────────────────────┐
   │  Layer 7 — Evaluation             [W16]  hu_evaluation_t             │
   ├──────────────────────────────────────────────────────────────────────┤
   │  Layer 6 — Privacy & Governance   [W15]  hu_keystore_t, hu_audit_t   │
   ├──────────────────────────────────────────────────────────────────────┤
   │  Layer 5 — Response-path          [W11/W12] hu_self_rag_t,           │
   │            Reasoning                       hu_planner_t              │
   ├──────────────────────────────────────────────────────────────────────┤
   │  Layer 4 — Learning Loop          [W13/W14] hu_learner_t,            │
   │            (sleep + train)                  hu_scheduler_t           │
   ├──────────────────────────────────────────────────────────────────────┤
   │  Layer 3 — World Model            [W9]    hu_world_model_t           │
   ├──────────────────────────────────────────────────────────────────────┤
   │  Layer 2 — Belief Layer           [W8]    hu_belief_t                │
   ├──────────────────────────────────────────────────────────────────────┤
   │  Layer 1 — Memory Facade          [W7]    hu_memory_facade_t        │
   ├──────────────────────────────────────────────────────────────────────┤
   │  Layer 0 — v1 storage substrate                                      │
   │            graph + vector + persona + cross_edges + cases + deltas   │
   │            (W1–W6, already shipped)                                  │
   └──────────────────────────────────────────────────────────────────────┘
```

**Reading the stack:**

- A workstream may depend on its own layer or any layer below.
- A workstream may **never** depend on a layer above. (This is the dependency-direction rule from `docs/standards/engineering/principles.md`.)
- New code goes behind a vtable on the layer that owns it. No raw access from above.
- Existing v1 code is not rewritten — it is *adapted* into a layer-1 backend (`hu_memory_v1_backend`).

## Shared infrastructure (the "no duplication" contract)

Six concrete artifacts span Layers 1–3, 5, and 7; each is owned by one workstream and consumed above. They are written **once** and never duplicated:

| Artifact | Owner | Consumers | Purpose |
|----------|-------|-----------|---------|
| `hu_memory_facade_t` (W7 dispatch facade) | W7 | W11, W12, W13, W14, W16 | One read/write/erase surface over kinds (graph, cases, deltas, …). Legacy chat/vector store remains `hu_memory_t` in `human/memory.h` (Phase 0 rename; see type-collision cleanup doc). |
| `hu_belief_t` (posterior + provenance) | W8 | W7, W11, W14 | Replaces `float confidence` everywhere; carries (mean, variance, prov) |
| `hu_world_model_t` (per-contact snapshot) | W9 | W11, W12, W13, W14 | Single struct: entities + beliefs + persona + emotion + goals + ToM + negatives |
| `hu_planner_t` (goal-conditioned retrieval) | W12 | W11, agent turn | Heuristic + LLM-backed plans; executes retrieve/verify loops via the facade (`retrieval_planner.h`) |
| `hu_self_rag_t` (verifier vtable) | W11 | response-path, W14 | Heuristic backend (W4-existing) + inline-LLM backend + future model-native |
| `hu_evaluation_t` (benchmark vtable; see `include/human/evaluation/evaluation.h`) | W16 | CI, every prior W | LoCoMo / LongMemEval / DMR / MINJA / MemoryAgentBench / frontier compare |

Every other type already exists in v1 and is reused. Nothing in v2 introduces a second implementation of the **same** responsibility (e.g. no parallel memory facades).

## The 10 workstreams

| # | Branch | Layer | Spec | Depends on |
|---|--------|-------|------|------------|
| **W7** | `feat/memory-v2-w7-facade` | 1 | `2026-05-10-w7-memory-facade.md` | v1 |
| **W8** | `feat/memory-v2-w8-belief` | 2 | `2026-05-10-w8-belief-layer.md` | W7 |
| **W9** | `feat/memory-v2-w9-world-model` | 3 | `2026-05-10-w9-world-model.md` | W7, W8 |
| **W10** | `feat/memory-v2-w10-neural-memory` | 1+ | `2026-05-10-w10-neural-memory.md` | W7 |
| **W11** | `feat/memory-v2-w11-inline-self-rag` | 5 | `2026-05-10-w11-inline-self-rag.md` | W7, W8, W9 |
| **W12** | `feat/memory-v2-w12-goal-retrieval` | 5 | `2026-05-10-w12-goal-conditioned-retrieval.md` | W7, W9 |
| **W13** | `feat/memory-v2-w13-learning-loop` | 4 | `2026-05-10-w13-learning-loop.md` | W7, W11, W4/W5 (v1) |
| **W14** | `feat/memory-v2-w14-sleep-compute` | 4 | `2026-05-10-w14-sleep-compute.md` | W2 (v1), W13 |
| **W15** | `feat/memory-v2-w15-crypto-privacy` | 6 | `2026-05-10-w15-crypto-privacy.md` | W7 |
| **W16** | `feat/memory-v2-w16-eval-suite` | 7 | `2026-05-10-w16-eval-suite.md` | every prior W |

### What each workstream does (one paragraph)

**W7 — Memory facade.** Introduces `hu_memory_facade_t` (`hu_memory_facade_read` / `hu_memory_facade_write` / … in `human/memory/memory.h`) as the single entry point for structured memory kinds. Implements `hu_memory_v1_backend_t` that wraps existing graph, vector projections, persona deltas, cross_edges, cases, quarantine. Every existing call site (`src/agent/`, `src/persona/`, `src/feeds/`, channels) is migrated to go through the facade in phased passes (see Phase 1 inventory doc). The facade dispatches; backends do the work. This is the foundation that lets later workstreams swap memory backends (KV-cache, activations, etc.) without rewriting consumers.

**W8 — Belief layer.** Replaces the raw `float confidence` field on every memory entry with `hu_belief_t { float mean; float variance; hu_provenance_t prov; }`. Adds Bayesian update primitives (`hu_belief_update`, `hu_belief_combine`) and a fast-path LLM-judge-backed semantic-conflict detector (`hu_belief_semantic_conflict`) that supersedes the v1 deterministic key-match resolver when paraphrase detection is needed. Also adds **hyperedges**: `hu_belief_hyperedge_t` lets the graph store n-ary facts ("Alice met Bob at Acme on Friday about funding") as a single edge. v1's binary edges are a special case.

**W9 — World model.** A new struct `hu_world_model_t` per contact. Fields: entities snapshot, persona snapshot, emotional state, active goals, theory-of-mind sub-model (the user's beliefs about you), negative memory ("things I should not do or say"), recent topics. Built lazily, cached, invalidated on relevant writes. Replaces the four parallel calls (`hu_persona_load`, `hu_graph_neighbors`, `hu_emotion_state_load`, `hu_contact_get`) with one: `hu_world_model_load(graph, contact, &out)`. Every consumer (planner, prompt builder, verifier) reads from this single artifact going forward.

**W10 — Neural memory tier.** Extends the W7 facade (`hu_memory_facade_t`) with two new entry kinds: **KV-cache memory** (compressed activations for past prompts, retrievable by prompt-prefix hash) and **reasoning-trace memory** (chain-of-thought for past plans, retrievable by goal+anchor). Adds joined storage of (entity, embedding, KV-blob) co-resident in SQLite via a separate `neural_memory` table. Multimodal entries (image / voice / video bytes) get a generic `hu_memory_blob_t` cell. Cleanup falls under W14's sleep scheduler. KV-blobs are model-version-tagged so a model upgrade invalidates the right rows automatically.

**W11 — Inline Self-RAG with abstention.** The big one for trust. New `hu_self_rag_t` vtable replaces v1's heuristic verifier (which becomes the fallback backend). New backends: an **inline backend** that interleaves `<retrieve>`/`<critique>`/`<refuse>` control tokens during generation; an **atomic-claim decomposition backend** that splits a draft sentence into noun-phrase atomic claims and verifies each. Adds an explicit **refusal head**: when no claim crosses the threshold, the model returns `HU_VERIFY_ABSTAIN` and the channel layer renders "I don't have enough memory to say." There is **no** separate public `hu_provider_chat_with_self_rag` API in-tree today; Self-RAG uses `hu_self_rag_*` plus the existing provider chat surface. A dedicated provider entry point remains optional if a backend needs native control-token streaming.

**W12 — Goal-conditioned retrieval + multi-hop reasoning.** New `hu_planner_t` interface (vtable) that takes a goal + world-model snapshot and emits a retrieval plan. Implements **HippoRAG-style PageRank** over the entity graph for soft retrieval. Implements **3-hop traversal with verifier loops**: plan → retrieve → score → expand → re-verify, capped at 5 hops with a budget. Replaces ad-hoc memory queries scattered across `agent/context.c` with one planner call. Also unlocks complex queries like "when did Alice and Bob last collaborate on funding?" that v1 cannot answer in one round-trip.

**W13 — Learning loop (LoRA + DPO + RL).** Closes the long-standing M3 gap (per `CLAUDE.md` mission table). New `hu_learner_t` vtable with backends: `hu_learner_lora_mlx_t` (Apple Silicon), `hu_learner_lora_ggml_t` (Linux/CUDA). Reads training signal from existing v1 sources: W4 verifier flags (negative DPO pairs), W5 persona deltas (positive style adaptation), W3 case outcomes (reward signal). Wires the existing `lora-persona` CLI to actually fine-tune the **frontier-model adapter the chat path loads at inference time**, not the reference toy GPT. Adds checkpoint rotation and model-version tagging so KV-cache memory (W10) invalidates correctly.

**W14 — Sleep-time compute scheduler.** Extends v1's AutoDream from heuristic prune/summarize/decay into a full **idle-compute scheduler** (`hu_scheduler_t`). Hosts: W13 LoRA training jobs, KV-cache pre-warming for likely-asked queries, counterfactual rehearsal ("if I had known X earlier, what would I have said?"), W11 self-RAG re-verification of stale beliefs. Schedules around system load, battery state, and user-defined quiet hours. Single coordinator replaces multiple cron-shaped jobs in v1.

**W15 — Cryptographic privacy + DP.** Per-user envelope encryption: a master key derived from user passphrase / OS keychain wraps per-table data keys. Deleting the user's master key cryptographically revokes all data — even backups. Adds DP-SGD support to W13's training jobs (gradient clipping + Gaussian noise; `epsilon` budget tracked per epoch). Adds user-readable audit log (`human memory audit`) showing every read/write/erase with timestamps and source. Adds **GDPR Article 20 export** (`human memory export --json`) — the inverse of erasure.

**W16 — Continuous evaluation.** New `hu_evaluation_t` vtable (`evaluation.h`). JSON task harnesses remain `hu_eval_*` in `human/eval.h` — different surface. Backends: LoCoMo (long-conversation recall), LongMemEval (5 task categories), Deep Memory Retrieval, MINJA poisoning red-team, MemoryAgentBench (multi-agent coordination), Frontier-Compare (us vs frontier models with no memory on identical conversations). **CI:** nightly benchmark + regression gate lives in `.github/workflows/evaluation.yml`; PR-scoped eval JSON suites + offline red-team fleet live in `.github/workflows/eval.yml`; every PR still runs `human eval validate` / `human eval check-regression` from `.github/workflows/ci.yml`. Replaces specced-but-never-built v1 W6 LoCoMo skeleton.

## Sequencing

```
  W7  ── facade ──┐
                  ├── W8 ── belief ──┐
                  │                   ├── W9 ── world-model ──┐
                  │                   │                       ├── W11 ── inline self-RAG ──┐
                  │                   │                       │                            ├── W13 ── learning ──┐
                  │                   │                       ├── W12 ── goal retrieval ──┘                      │
                  │                   │                                                                          ├── W14 ── sleep
                  ├── W10 ── neural memory ─────────────────────────────────────────────────────────────────────┘
                  │
                  └── W15 ── crypto privacy (parallel after W7)

                  W16 ── eval suite (lands last, measures every prior W)
```

**Parallel-safe pairs:** {W8, W10}, {W11, W12}, {W13, W15}, {W14, W15}.

**Strict sequence:** W7 → W8 → W9 → W11 → W13 → W14. Anything else can run in any order so long as its `Depends on` column is satisfied.

## Cross-cutting principles

Inherited from v1's roadmap, with two additions:

- **One concern per branch.** No mixed feature + refactor + infra. (`AGENTS.md` §6)
- **Vtable discipline.** v2 introduces **six** planner-facing vtables: memory facade, belief-backed retrieval planner (`hu_planner_t` in `retrieval_planner.h`), learner, scheduler, self-RAG, and evaluation (`hu_evaluation_t`). JSON eval harness types (`hu_eval_suite_t`, etc. in `eval.h`) predate W16 and are not the W16 benchmark vtable. Every other addition extends an existing struct.
- **HU_IS_TEST guards on side effects.** Subagents must be scriptable in tests without spawning real processes.
- **Binary size budget.** v1 added ~430 KB (1.75 MB → 2.18 MB). v2 budget is +500 KB (target ≤ 2.7 MB MinSizeRel+LTO). W13 LoRA is gated behind the existing `HU_ENABLE_LEARNING` CMake option (see root `CMakeLists.txt`). W10 neural memory is gated behind `HU_ENABLE_NEURAL_MEMORY` (**defined**, default **OFF**; turn ON for W10 schema/tests per `w10-neural-memory` spec).
- **Zero ASan errors.** Every allocation freed; `SQLITE_STATIC` only.
- **Conventional commits.** Pre-commit hooks already enforce.
- **Test discipline.** Each workstream lands ≥1 boundary/failure-mode test per new public function. The W6-style E2E adversarial suite gets one new scenario per workstream (W17 of the existing eval suite, not a new file).
- **GDPR / EU AI Act.** W15 makes erasure cryptographic; W16 makes audits queryable. Aug 2026 EU AI Act applicability is a hard deadline.
- **(NEW) Layer-direction rule.** A workstream may never depend on a layer above its own. Violations are a build error (CMake topology check).
- **(NEW) Single-artifact rule.** The shared artifacts in the table above (plus `hu_planner_t` for W12) are each owned by one workstream and consumed via their public APIs everywhere else. PRs that introduce a parallel implementation are rejected.

## Schema migration policy

v2 introduces three schema changes:

- **W7** — adds a `memory_facade_routes` table that maps logical entry kinds to backend names. Pure metadata; old DBs auto-populated with `v1_backend` for every kind.
- **W8** — promotes every `confidence REAL` column to a paired `(confidence_mean REAL, confidence_variance REAL)`; old rows get variance = 0 (deterministic). Hyperedges live in a new `hyperedges` + `hyperedge_members` pair.
- **W10** — adds `neural_memory(blob, kind, model_version, prompt_hash, created_at)` and `kv_cache(prompt_hash, model_version, blob)` (per W10 spec). Both are gated behind `HU_ENABLE_NEURAL_MEMORY` in CMake (option exists; full schema coverage tracked in `test_w10_neural_memory.c` and the W10 spec).

All three follow v1's pattern: idempotent ALTERs, schema-version bump, refuse-to-open on mismatch, round-trip test per migration.

## Success metrics

| Workstream | Primary signal | Threshold |
|-----------|----------------|-----------|
| W7 | Lines deleted from direct-graph callers | > 80% of v1 direct calls migrated |
| W8 | Paraphrase-conflict recall on annotated suite | +30% vs v1 deterministic resolver |
| W9 | Single-load latency on planner path | < 5 ms p99 vs current 4 separate calls |
| W10 | KV-cache hit-rate on warm queries | > 60% on a 100-query benchmark |
| W11 | Hallucination rate on factual claims | −80% vs v1 SOFT verifier; ≥30% abstention rate on weak-evidence prompts |
| W12 | LoCoMo multi-hop subset | +10 pts vs v1 baseline |
| W13 | DPO-trained adapter measurable preference | blind A/B preference > 60% for adapted over base |
| W14 | Idle-CPU usage during scheduled hours | within 30% budget; LoRA training visible in logs |
| W15 | Cryptographic forgetting verifies | data unrecoverable post-key-deletion (formal test) |
| W16 | Frontier-compare on LongMemEval | h-uman ≥ GPT-5-no-memory on 4 of 5 categories |

## Out of scope (explicit non-goals)

- **Cross-user federated memory.** Each h-uman is local-first. Household-scope is W5's twin tier in v1; cloud-scope is not on the roadmap.
- **Replacing the C runtime.** No Rust rewrite. No Zig revival. C11 is the substrate.
- **Replacing SQLite.** SQLite stays the default storage; pgvector / qdrant / lance remain optional backends behind W7.
- **A new agent loop.** All v2 subagents (W14 scheduler, W11 self-RAG inline) plug into existing `src/agent/spawn.c` + `src/agent/dispatcher.c`.
- **A new GUI surface.** v1 specced a memory-view in `human-ui/`; v2 only adds two CLI subcommands (`audit`, `export`).
- **Multimodal output generation.** W10 stores multimodal *memory*; the agent does not learn to *generate* images/audio. That's a separate roadmap.

## Risks (rolled up)

| Risk | Mitigation |
|------|------------|
| W7 facade migration breaks 60+ call sites | Mechanical migration in a single PR with a CMake "no-direct-graph-call" lint rule; old API kept as deprecated thin wrappers for one release |
| W8 belief-type churn ripples through every test | Variance defaults to 0; v1 tests pass unchanged; `confidence_mean` is the migration target name so a one-shot sed pass is enough |
| W11 inline Self-RAG depends on provider control-token support | Backend-pluggable: heuristic backend always works; inline backend gracefully degrades when the active provider doesn't support it |
| W13 LoRA training requires Apple Silicon or CUDA | `hu_learner_t` is vtable; CPU fallback exists for tests; users without acceleration get the v1 system-prompt-only path |
| W14 sleep scheduler eats battery | Idle-only schedule + battery-state probe + per-job CPU budget + `human ml status` surfaces actual usage |
| W15 cryptographic forgetting must survive backup restore | Backups encrypted with the same per-user key; restore-after-deletion test in W15's suite |
| W16 eval API budget | LoCoMo/LongMemEval support both ADC-backed and offline-judge variants (frontier-compare does require live API) |
| Total binary growth exceeds budget | Each workstream lands a binary-size delta in its PR description; W10/W13 are gated; cmake `Release` size is checked in CI |

## Reference index

- `docs/plans/2026-05-10-memory-roadmap-overview.md` — v1 roadmap (predecessor)
- `docs/plans/2026-05-10-w1-bitemporal-foundation.md` … `w6-…` — v1 specs
- `include/human/memory/graph.h` — current graph API (entities, relations, communities, temporal, causal, conflict, retention)
- `src/memory/graph.c` — SQLite-backed implementation (~2460 lines after v1; run `wc -l` for current)
- `src/agent/response_verifier.c` — v1 heuristic verifier (becomes `hu_self_rag_t` heuristic backend)
- `src/agent/autodream.c` — v1 background consolidator (gets folded into W14 scheduler)
- `src/persona/persona_deltas.c` — v1 evolver (becomes a learning-signal source for W13)
- `src/ml/lora.c`, `src/ml/dpo.c`, `src/ml/train.c` — existing reference-GPT trainers (W13 wires these to the chat-time provider)
- `CLAUDE.md` Mission table M3 (Private Learning) — anchors W13's scope
- `docs/standards/engineering/principles.md` §Architecture Boundaries — anchors the layer-direction rule

## Approval gate

Once this overview is approved, each W-spec (W7 through W16) gets authored individually following the W1–W6 style: schema, public API, error model, test plan with adversarial scenarios, success metric, and binary-size delta budget. After each W-spec is approved (one approval per spec is fine), implementation proceeds in dependency order with the same `feat:` commit cadence as v1.

The end state is a memory subsystem that is genuinely **better than human** — not as a marketing claim, but as a measurable outcome on six published benchmarks against the three frontier models. v1 made the system trustworthy. v2 makes it brilliant.
