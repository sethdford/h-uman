---
title: "Memory v2 — Evidence index (specs ↔ code ↔ tests ↔ CI)"
created: 2026-05-10
status: active
parent: 2026-05-10-memory-v2-roadmap-overview.md
related:
  - 2026-05-10-master-follow-through-program.md
  - 2026-05-10-memory-v2-execution-plan.md
  - adr/2026-05-10-w10-kv-replay-deferred.md
---

# Memory v2 — Evidence index

Single place to answer: **where is this workstream in the tree, how do we prove it, and what runs in CI?**

Update this file when suites, headers, or workflows change. Prefer **one row per workstream**; split rows only when multiple disjoint proof surfaces exist.

---

## CMake / compile gates (evidence for “gated” work)

| Flag | Default (dev preset) | Purpose |
|------|----------------------|---------|
| `HU_ENABLE_SQLITE` | ON (dev) | Graph, W7 facade, W10 neural tables, most memory tests |
| `HU_ENABLE_NEURAL_MEMORY` | OFF | W10 neural memory code paths; turn ON for W10-only matrix builds |
| `HU_ENABLE_LEARNING` / `HU_ENABLE_ML` | see `CMakeLists.txt` | W13 learner, LoRA runners |

---

## CI entrypoints (memory-adjacent)

| Check | Workflow | What it proves |
|-------|----------|----------------|
| Full C tests + count floor | `.github/workflows/ci.yml` | `./build/human_tests` (see `BASELINE` in same file) |
| JSON eval suites | `ci.yml` | `./build/human eval validate eval_suites/` |
| Eval regression | `ci.yml` | `./build/human eval check-regression eval_suites/` |
| Header dual-include | `ci.yml` | `scripts/check-memory-v2-header-collision.sh` |
| Nightly / dispatch benchmarks | `.github/workflows/evaluation.yml` | W16 bench harness (optional live frontier job) |
| PR-scoped eval + red-team | `.github/workflows/eval.yml` | Paths listed in workflow `on:` block |

---

## Workstream evidence table (W7–W16 + v2 E2E)

**How to run a suite:** `./build/human_tests --suite="<substring>"` (substring matches the `HU_TEST_SUITE` string).

| W | Layer | Spec | Primary public surface | Proof tests (representative) | Suite filter hint |
|---|-------|------|------------------------|------------------------------|-------------------|
| **W7** | 1 | [`2026-05-10-w7-memory-facade.md`](2026-05-10-w7-memory-facade.md) | `human/memory/memory.h` — `hu_memory_facade_*` | `tests/test_w7_memory_facade.c` | `W7 memory facade` |
| **W8** | 2 | [`2026-05-10-w8-belief-layer.md`](2026-05-10-w8-belief-layer.md) | `human/memory/belief.h` — `hu_belief_*`, graph variance columns | `tests/test_w8_belief_layer.c` | `W8 belief` |
| **W9** | 3 | [`2026-05-10-w9-world-model.md`](2026-05-10-w9-world-model.md) | `human/agent/world_model.h`, `world_model_bridge.h` | `tests/test_w9_world_model.c`, `tests/test_world_model_bridge.c` | `W9 world model`, `World-model bridge` |
| **W10** | 1+ | [`2026-05-10-w10-neural-memory.md`](2026-05-10-w10-neural-memory.md) | `human/memory/neural_memory.h` — KV / traces / blobs | `tests/test_w10_neural_memory.c` | `W10 neural memory` |
| **W11** | 5 | [`2026-05-10-w11-inline-self-rag.md`](2026-05-10-w11-inline-self-rag.md) | `human/agent/self_rag.h`, `self_rag_inline.c` | `tests/test_w11_self_rag.c` | `W11 inline self-RAG` |
| **W12** | 5 | [`2026-05-10-w12-goal-conditioned-retrieval.md`](2026-05-10-w12-goal-conditioned-retrieval.md) | `human/agent/retrieval_planner.h` — `hu_planner_*` | `tests/test_w12_planner.c`, `tests/test_w12_verifier_loop.c` | `W12 planner`, `W12 verifier loop` |
| **W13** | 4 | [`2026-05-10-w13-learning-loop.md`](2026-05-10-w13-learning-loop.md) | `human/ml/learner.h`, `learner_bridge.h` | `tests/test_w13_learner.c`, `tests/test_learner_bridge.c` | `W13 learner` |
| **W14** | 4 | [`2026-05-10-w14-sleep-compute.md`](2026-05-10-w14-sleep-compute.md) | `human/agent/scheduler.h` | `tests/test_w14_scheduler.c`, `tests/test_w14_runners.c` | `W14 scheduler`, `W14 runners` |
| **W15** | 6 | [`2026-05-10-w15-crypto-privacy.md`](2026-05-10-w15-crypto-privacy.md) | `human/security/keystore.h`, `memory/encrypted_store.h` | `tests/test_w15_keystore.c`, `tests/test_w15_backup_restore.c`, `tests/test_encrypted_store.c` | `W15`, `encrypted_store` |
| **W16** | 7 | [`2026-05-10-w16-evaluation-suite.md`](2026-05-10-w16-evaluation-suite.md) | `human/evaluation/evaluation.h`, `human/eval.h` (JSON harness) | `tests/test_w16_evaluation.c`, `tests/test_w16_eval_cli.c` | `W16 evaluation`, `w16_eval_cli` |
| **v2 E2E** | — | [`2026-05-10-memory-v2-execution-plan.md`](2026-05-10-memory-v2-execution-plan.md) | composed wiring | `tests/test_v2_e2e_adversarial.c`, `tests/test_v2_wiring_e2e.c` | `v2 E2E` |

---

## ADRs (decisions that constrain code)

| ADR | Decision |
|-----|----------|
| [`adr/2026-05-10-w10-kv-replay-deferred.md`](adr/2026-05-10-w10-kv-replay-deferred.md) | W10 KV **replay / provider skip** deferred; metadata probe + post-call upsert only |

---

## Appendix A — `hu_memory_query_t` surface area (Track B inventory)

Snapshot command (re-run when auditing):

```bash
rg -c 'hu_memory_query_t' src --glob '*.c' | sort -t: -k2 -n
```

**Interpretation:** every file in the result set should be reviewed so each stack `hu_memory_query_t` sets **`variant`** consistently with `kind` and the active union member. This index does **not** replace line-by-line review; it tells you **where** to look.

**Track B1 (2026-05-10):** Line-by-line pass completed for `src/**/*.c` call sites plus GDPR export generic loop in `src/memory/memory.c` (`HU_MEMORY_QUERY_WINDOW` for full-range window reads). Representative tests now set **`variant`** explicitly in `tests/test_w7_memory_facade.c` and `tests/test_w12_planner.c`. `src/persona/`, `src/feeds/`, and `src/channels/` contain **no** `hu_memory_query_t` references (B1.c N/A).

**W7 inventory (related):** `scripts/w7-phase1-graph-bypass-inventory.sh` counts **`hu_graph_<public-api>(`** call sites under `src/agent|persona|feeds` only — not typedefs like `hu_graph_entity_t`. Re-run after graph-touching migrations; see [`2026-05-10-w7-phase1-bypass-inventory.md`](2026-05-10-w7-phase1-bypass-inventory.md).

**Track B2:** `scripts/check-memory-query-variant.sh` (python3) enforces explicit `.variant =` after stack `memset` of `hu_memory_query_t` in `src/**` and `tests/**`. Wired into `scripts/verify-all.sh` and `scripts/agent-preflight.sh` (C path). Negative coverage for union aliasing remains `test_v2_e2e_facade_query_aliasing_is_safe` in `tests/test_v2_e2e_adversarial.c`.

---

## Appendix B — Maintenance

| When | Action |
|------|--------|
| New W-spec merged | Add/adjust row in workstream table + suite string |
| New CI gate | Add row under **CI entrypoints** |
| ADR accepted | Link from **ADRs** and from the master program status table |
