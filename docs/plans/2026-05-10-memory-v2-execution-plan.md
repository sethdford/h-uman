---
title: "Memory v2 — Execution plan (plan → build → prove)"
created: 2026-05-10
status: active
parent: 2026-05-10-memory-v2-roadmap-overview.md
related:
  - 2026-05-10-w7-type-collision-cleanup.md
  - 2026-05-10-w7-phase1-bypass-inventory.md
  - 2026-05-10-w7-memory-facade.md
  - 2026-05-10-w8-belief-layer.md
  - 2026-05-10-w9-world-model.md
  - 2026-05-10-w10-neural-memory.md
  - 2026-05-10-w11-inline-self-rag.md
  - 2026-05-10-w12-goal-conditioned-retrieval.md
  - 2026-05-10-w13-learning-loop.md
  - 2026-05-10-w14-sleep-compute.md
  - 2026-05-10-w15-crypto-privacy.md
  - 2026-05-10-w16-evaluation-suite.md
---

# Memory v2 — Execution plan

This document turns [`2026-05-10-memory-v2-roadmap-overview.md`](2026-05-10-memory-v2-roadmap-overview.md) into **ordered work**, **gates**, and **proof obligations**. It does not replace the per-W specs; it sequences them.

## North-star proof bar

| Gate | Command / artifact | Pass criterion |
|------|-------------------|----------------|
| **G0 — compile** | `cmake --build build` (dev preset) | Zero errors, `-Werror` clean on touched TUs |
| **G1 — unit** | `./build/human_tests` | Zero failures, zero ASan errors |
| **G2 — collision** | `scripts/check-memory-v2-header-collision.sh` | No forbidden dual-include in `src/` (see script allowlist) |
| **G3 — release size** | `cmake --preset release && cmake --build --preset release` + benchmark workflow | Within +500 KB v2 budget when W10/W13 flags enabled |
| **G4 — product e2e** | Manual / optional `VERIFY_NATIVE=1`, live channels | Out of scope for CI; tracked per release |

## Phase 0 — Unblock the type system (hard prerequisite) — **DONE (2026-05-10)**

**Problem:** Legacy `human/memory.h` and W7 `human/memory/memory.h` both used the name `hu_memory_t` for different types.

**Resolution:** **Option A** — W7 surface is `hu_memory_facade_t` / `hu_memory_facade_*` / `hu_memory_facade_vtable_t` in [`2026-05-10-w7-type-collision-cleanup.md`](2026-05-10-w7-type-collision-cleanup.md). Legacy `hu_memory_t` is again unconditional in `human/memory.h`.

**Exit:** `human_tests` green for W7/W11/W14/v2 E2E suites + `scripts/check-memory-v2-header-collision.sh` unchanged in intent (dual-include still forbidden).

## Phase 1 — W7 facade completion

**Spec:** [`2026-05-10-w7-memory-facade.md`](2026-05-10-w7-memory-facade.md) (design text may still say `hu_memory_t`; **implemented API** is `hu_memory_facade_t` / `hu_memory_facade_*` after Phase 0.)

| Step | Work | Proof |
|------|------|--------|
| 1.1 | Inventory direct `hu_graph_*` / SQLite memory bypasses in `src/agent/`, `src/persona/`, `src/feeds/` | **Done:** [`2026-05-10-w7-phase1-bypass-inventory.md`](2026-05-10-w7-phase1-bypass-inventory.md) + `bash scripts/w7-phase1-graph-bypass-inventory.sh` |
| 1.2 | Migrate read/write hot paths to `hu_memory_facade_read` / `hu_memory_facade_write` | Per-subsystem tests + existing W7 suite |
| 1.3 | Trend script (optional CMake target `human_w7_phase1_inventory`) | Developers run target or script before/after migration slices; no default CI hard fail yet |
| 1.4 | Shared ctx lifetime + register semantics | `test_w7_*` + adversarial replace tests |

**Exit:** Roadmap metric “>80% lines deleted from direct-graph callers” *or* documented exception list with owner + removal date (inventory doc §Exception policy satisfies the latter until migrations land).

## Phase 2 — W8 belief layer

**Spec:** [`2026-05-10-w8-belief-layer.md`](2026-05-10-w8-belief-layer.md)

- Migrate `float confidence` → `hu_belief_t` on facade records and graph projections where specified.
- Hyperedge schema: follow W8 spec migrations; idempotent ALTER + round-trip tests.

**Exit:** W8 adversarial tests + no regression on W7 facade reads.

## Phase 3 — W9 world model

**Spec:** [`2026-05-10-w9-world-model.md`](2026-05-10-w9-world-model.md)

- Single-load path (`hu_world_model_load`) on planner + prompt + verifier entry points per spec.
- **Exit:** p99 latency gate from overview + `test_w9_*` / bridge tests.

## Phase 4 — W10 neural memory (gated)

**CMake:** `HU_ENABLE_NEURAL_MEMORY` (default **OFF** until schema lands).

**Spec:** [`2026-05-10-w10-neural-memory.md`](2026-05-10-w10-neural-memory.md)

- Tables + `hu_memory_kind_t` kinds for KV / trace / blob.
- **Exit:** `HU_ENABLE_NEURAL_MEMORY=ON` build + dedicated tests; default build unchanged.

## Phase 5 — W11 inline Self-RAG

**Spec:** [`2026-05-10-w11-inline-self-rag.md`](2026-05-10-w11-inline-self-rag.md)

- `hu_self_rag_t` vtable: heuristic + atomic + inline backends aligned with provider capability matrix.
- **Exit:** abstention / telemetry metrics from overview; no crash on unsupported providers.

## Phase 6 — W12 planner + retrieval

**Spec:** [`2026-05-10-w12-goal-conditioned-retrieval.md`](2026-05-10-w12-goal-conditioned-retrieval.md)

- Planner becomes default for goal-conditioned retrieval per spec.
- **Exit:** LoCoMo multi-hop subset gate (overview table).

## Phase 7 — W13 learning loop (M3 closure)

**Spec:** [`2026-05-10-w13-learning-loop.md`](2026-05-10-w13-learning-loop.md) + `CLAUDE.md` M3

- Bridge training artifacts to **chat-time** inference path (not reference GPT only).
- **Exit:** blind A/B metric from overview *or* honest “not yet” with measured gap.

## Phase 8 — W14 sleep compute

**Spec:** [`2026-05-10-w14-sleep-compute.md`](2026-05-10-w14-sleep-compute.md)

- Scheduler owns jobs listed in overview; AutoDream responsibilities migrated per spec.
- **Exit:** idle CPU budget tests + scheduler tick tests.

## Phase 9 — W15 crypto / privacy

**Spec:** [`2026-05-10-w15-crypto-privacy.md`](2026-05-10-w15-crypto-privacy.md)

- Envelope encryption, export, audit CLI per spec.
- **Exit:** “key destroyed → data unrecoverable” formal test.

## Phase 10 — W16 eval suite + CI

**Spec:** [`2026-05-10-w16-evaluation-suite.md`](2026-05-10-w16-evaluation-suite.md)

- `hu_eval_t` backends + nightly workflow (offline + optional live frontier).
- **Exit:** documented API budget; CI fails on regression where suites are non-flaky.

## Parallel tracks (safe)

- **{W8, W10}** after W7 per overview.
- **{W11, W12}** after W7+W8+W9.
- **{W13, W15}** in parallel once dependencies met.
- **W16** last (measures all prior).

## Immediate engineering fixes (this repo batch)

These are **already landed or landing with the plan PR** so “plan + fix” is not empty:

1. **W7 shared v1 ctx** — `hu_memory_register_backend` / `hu_memory_close` must not free the bundle while sibling slots reference it; bundle freed once via `v1_bundle_ctx`.
2. **`HU_ENABLE_NEURAL_MEMORY`** — CMake option reserved for W10 (default OFF).
3. **`scripts/check-memory-v2-header-collision.sh`** — fails CI if `src/**/*.c` includes **both** `human/memory.h` and `human/memory/memory.h` (allowlist for intentional bridge TUs).
4. **Include hygiene** — remove redundant W7 includes where headers already pull them.

## Handoff checklist (every phase PR)

1. What changed / what did not  
2. Gates run: G0–G2 minimum; G3 if release-affecting  
3. Risks + rollback  
4. Next phase entry criteria satisfied?
