# Sprint 60 — Wave Plan

**Branch:** `sprint-60-sota-learning-loop`
**Working directory:** `/Users/sethford/Projects/human-sprint-60` (dedicated git worktree, base pinned to `5dca845e`)
**Goal:** Critical-path spine of the TikTok-style feels-alive learning loop, proven end-to-end.

## Environment constraint (HARD)

In this environment the `Agent` tool's `isolation: worktree` is **silently ignored** — file-editing
agents edit the shared working tree (documented twice in `~/.claude/rules/verify-worktree-isolation-before-fanout.md`).
Therefore **all implementers run SEQUENTIALLY**, each editing `/Users/sethford/Projects/human-sprint-60`
directly. After each implementer reports DONE we gate on: commit landed → `cmake --build` exit 0 →
`./build/human_tests` exit 0 → ASan CLEAN → verifier PASS → critic CLEAN → aspect-panel PASS, BEFORE
dispatching the next. No two file-editors run concurrently (they share `CMakeLists.txt` + `tests/test_main.c`).

## Authority of artifacts

`designs/SEAMS.md` is the **integration contract** and is authoritative on cross-story symbol
names/signatures (it was authored against the real code). Where a PO story name differs from a SEAMS
name, follow SEAMS and verify against existing code before authoring (audit-verify-before-allege).

## Dependency DAG → linear execution order

```
US-101 (reward model)  ── no deps
   └─> US-102 (outcomes table + DPO mining)
          ├─> US-103 (bandit)            [logically independent module; ordered for sequential safety]
          │      └─> US-104 (proactive outcome signals → bandit update)
          └─> US-105 (nightly LoRA retrain)
                 └─> US-106 (hot-swap adapter)
                        └─> US-107 (E2E loop integration test) ── deps ALL
```

### Wave 1 — ML core
- **US-101** Bradley-Terry reward model (HUML backbone, gradient-checkable). Design: `designs/US-101.md`.

### Wave 2 — Data + signals
- **US-102** `production_outcomes` table + `hu_dpo_mine_corrections` mining. Design: `designs/US-102.md`.

### Wave 3 — Bandit
- **US-103** Thompson-sampling contextual bandit (Marsaglia-Tsang Beta sampler, deterministic under seed). Design: `designs/US-103.md`.

### Wave 4 — Proactive signal wiring
- **US-104** `proactive_sends` table + async bandit update from implicit outcomes. Design: `designs/US-104.md`.

### Wave 5 — Online update
- **US-105** Nightly LoRA retrain (mining → mocked subprocess under HU_IS_TEST + cooldown + telemetry). Design: `designs/US-105.md`.
- **US-106** Hot-swap adapter integration (graceful fallback, no live MLX in tests). Design: `designs/US-106.md`.

### Wave 6 — Loop closure proof
- **US-107** End-to-end learning-loop integration test (deterministic, mock-backed per SEAMS Seam 10). Design: `designs/US-107.md`.

## Per-story Definition of Done (gate before advancing)
1. Implementer commit visible: `git -C /Users/sethford/Projects/human-sprint-60 log sprint-60-sota-learning-loop ^5dca845e --oneline` shows it.
2. Fresh `cmake --build build --target human human_tests` exit 0.
3. Fresh `./build/human_tests` exit 0, `Results: N/M passed` with 0 failures.
4. ASan CLEAN.
5. `/verify` → `RESULT_verifier=PASS`.
6. `critic` → CLEAN (HIGH/CRITICAL findings re-open the story).
7. `/aspect-panel` → PASS/CLEAN (ESCALATE → surface to user).

## Then
- Phase 3 Sprint Review (DoD checklist) → Phase 4 mandatory adversarial audit → Phase R red-team e2e proof → Retro → tag-sprint-close.
