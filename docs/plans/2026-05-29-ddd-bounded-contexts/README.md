# DDD Bounded-Context Rationalization — Master Roadmap (Program v2)

> **Status:** Active. **Reconciled 2026-05-31** against measured codebase state.
> This Program v2 *supersedes and absorbs* the original 2026-05-29 five-phase
> plan (Phases 0–4). It is the single canonical source of truth for the
> bounded-context refactor. Each `phase-E*-*.md` is an independently-executable
> implementation plan in `superpowers:writing-plans` format.
>
> **What changed from v1:** the original 5 phases are reconciled against what
> actually landed — Phases 0, 1, and 2-as-scoped are **done**; the SOTA-critical
> memory thread (T1) is carried forward unchanged; and three genuinely-missing
> plans are added: the four **enforcement gates** (E0), the **101-loose-root-file**
> relocation (E1), and the **unwritten daemon "Phase 2b"** service-lifecycle carve
> (E2). Sequencing now leads with the gates ("stop the bleeding"), then root
> sprawl, per the program owner's 2026-05-31 direction.

**Goal:** Convert h-uman's 42 feature-folders into ~10 well-defined bounded
contexts with **enforced** boundaries — no loose root files, no god-files, no
silent duplication, no coverage regressions — fixing the clean-architecture
violations that block the product's "runs anywhere / privacy-by-architecture"
moat.

---

## Operating contract (every phase, every chip)

Set by the program owner, 2026-05-31:

1. **Coexist — never block a feature PR.** Every change lands as a small,
   independently-mergeable **chip**. Feature work (RL/GraphRAG, PRs #198+)
   continues in parallel.
2. **Pure behavior-preserving.** Every chip is a provable no-op: a
   **characterization test** pins current behavior *first*, then the move; logic
   edits are out of scope (`.claude/rules/tests-that-pin-bugs.md` — pin intent,
   not bugs). `/verify` (the verifier agent) gates each chip.
3. **Chips + ratchets.** Mechanical scope is bounded per
   `~/.claude/rules/agent-task-sizing.md` (N≤8 sites/chip or split). Each chip
   lowers a numeric **ratchet** wired into `.githooks/pre-commit`.
4. **Done is measured, not asserted.** "Wire/extract X" is not done until a
   ratchet moved and the **full** `./build/human_tests` suite is green against
   the **production binary** (`touch` source first per
   `.claude/rules/cmake-build-stale-binary.md`; trust the build you run, not
   reports or clangd per `.claude/rules/ground-truth-over-proxy-signals.md`).
5. **Dispatch one file-editing chip at a time.** In this environment
   `isolation: "worktree"` has silently failed (shared tree → unbuildable pile);
   per `~/.claude/rules/verify-worktree-isolation-before-fanout.md`, sequence
   file-editing chips, or prove isolation with a `git status` canary before fanning out.

---

## State reconciliation — v1 plan vs measured reality (2026-05-31)

| v1 Phase | Intent | Measured 2026-05-31 | Status |
|---|---|---|---|
| 0 — erosion guards + renames | path-scoped ratchets, ubiquitous-language, Modeled-Person doc | ~35 `check-*.sh` guards live; `bounded-contexts.md` standard written | ✅ **Done** |
| 1 — channel `behavior_class` | delete hardcoded `memcmp` in agent core | `MEMCMP_BASELINE=0` in `check-agent-core-boundary.sh` | ✅ **Done** |
| 2 — daemon split | extract the 4 cohesive buckets (~840 LOC) | `daemon/{director,identity,peripheral_gov,message_router,proactive_policy,reply_dedup}.c` all present | ✅ **Done as scoped** |
| 2b — daemon `service_lifecycle` | (named but **never written**) | `daemon.c` still **14,723 LOC**; `hu_service_run` 3,759 + checkins 1,496 remain | 🔴 **→ E2 (new)** |
| 3 — memory query interface (T1) | repository pattern, migrate raw-SQL | **114** `sqlite3.h` includers (was 113); raw `get_db` across **41 files** / ~146 sites (was 33) — **eroded** | 🔴 **→ E3 (carried)** |
| 4 — repackaging | `agent/` sub-packages + config facade | `agent/` = **157 flat files**, 2 subdirs; **14 `config_*.c` at root**; `FACTORY_BASELINE=4` | 🔴 **→ E4 (carried)** |
| — | (the four enforcement gates) | size-ceiling / no-root-files / clone guards: **none exist** | 🔴 **→ E0 (new)** |

**Headline finding:** v1 named T1 (ambient SQLite) the moat-critical thread, yet
between authoring and today its blast radius **grew** (113→114 includers, 33→41
caller files). Erosion outran the migration chips because the ratchet fenced only
`sqlite3.h` *includers*, not the *growth of `get_db` callers*. E0 closes that gap
before E3 resumes the migration.

---

## The three systemic findings (verified — the diseases, not the symptoms)

Directory sprawl is a symptom. These are the diseases:

| # | Thread | Diagnosis | Blast radius |
|---|---|---|---|
| **T1** | SQLite is ambient, not abstracted | `hu_memory_t` vtable is a read/write dispatcher; domain code reaches past it to raw SQL | **114 files** `#include <sqlite3.h>`; raw `get_db` in **41 files** / ~146 sites |
| **T2** | Orchestration is a god-context | `daemon.c` = 14,723 LOC (`service_lifecycle` ~13K remains); `agent/` = 157 flat files / ~8 sub-domains | ~330 files |
| **T3** | Boundaries leak in practice | (largely closed by Phase 1/2) residual: agent core instantiates providers via factory include | `FACTORY_BASELINE=4` sites |

### What is NOT broken — do NOT "fix" (verified clean)

- `eval/` (runtime task-runner, 256 includers, live in agent loop) vs
  `evaluation/` (W16 offline benchmark, 15 includers, CI-only) are **legitimately
  distinct generations**, bridged via `evaluation_legacy_bridge.c`. Only the
  *naming* is a hazard (a future runtime→`scoring/` rename is tracked, not urgent).
- `provider/` (shared schema) vs `providers/` (vtable contracts) is an
  **intentional** split.
- `hu_config_t` is a **DTO** (correct dependency direction), not a domain leak —
  but its 73 includers are a coupling risk addressed by the E4 facade.
- The 8-dir "humanness" cluster has **near-zero cross-coupling** — over-partitioned,
  not tangled. Two real aggregate roots: `hu_persona_t`, `hu_personal_model_t`
  (the Modeled-Person context; layering enforced by
  `scripts/check-modeled-person-layering.sh`).

---

## Target bounded-context map (~10 contexts from 42 folders)

| Target context | Absorbs (current) | Aggregate root | Phase |
|---|---|---|---|
| Conversation Orchestration | `agent/`, `daemon*.c`, routing | turn lifecycle / `hu_agent_t` | E2, E4 |
| Modeled Person | `persona/` + `cognition/` + `behavior/` | `hu_persona_t` | ✅ Phase 0 doc + guard |
| Learning Loop | `intelligence/` + `reflection/` | closed loop | E4 |
| Recall (Memory) | `memory/` | `hu_memory_t` + `hu_personal_model_t` | E3 |
| Knowledge Resources | `data/` | dataset/blob loaders | rename (tracked) |
| Evaluation (runtime) | `eval/` + `eval.c` | `hu_eval_suite_t` | rename (tracked) |
| Benchmarking (offline) | `evaluation/` + `cli_evaluation.c` | `hu_evaluation_t` | keep |
| Calibration | `calibration/` | `hu_calibration_*` | keep |
| Configuration (supporting) | `config_*.c` | `hu_config_t` | E1, E4 |
| Model Access (edge ACL) | `providers/` `channels/` `tools/` | the 3 vtables | ✅ Phase 0 guard |

---

## The unified sequence (execution order)

Ordered so each phase makes the next **safer**: stop erosion, then the cheap
high-leverage structural moves, then the high-blast-radius memory re-layer last.

| Phase | Plan | Risk | Effort | Ratchet target | One-line |
|---|---|---|---|---|---|
| **E0** | [phase-E0-erosion-brake-gates.md](phase-E0-erosion-brake-gates.md) | Low | Days | install 4 gates @ baseline | Land the four enforcement ratchets so nothing re-erodes while E1–E4 run |
| **E1** | [phase-E1-root-sprawl-to-modules.md](phase-E1-root-sprawl-to-modules.md) | Low | 2–4 wk | root `.c` **101 → 0** | Relocate the 101 loose `src/*.c` into bounded contexts; lock root with E0 gate |
| **E2** | [phase-E2-daemon-service-lifecycle.md](phase-E2-daemon-service-lifecycle.md) | Med-High | 3–6 wk | `daemon.c` **14,723 → <800** | Carve `hu_service_run`/checkins out of the god-file (the unwritten "Phase 2b") |
| **E3** | [phase-E3-memory-query-interface.md](phase-E3-memory-query-interface.md) | High | 4–8 wk | `get_db` callers **41 → 0** | Repository pattern; migrate raw-SQL to `memory/repos/` — **the moat** |
| **E4** | [phase-E4-repackaging.md](phase-E4-repackaging.md) | Low-Med | Ongoing | `FACTORY_BASELINE 4 → 0`; `agent/` flat **157 → <40** | Config facade + `agent/` sub-packages + provider injection |

**Completed (v1, for the record):** Phase 0 (erosion guards + standards),
Phase 1 (channel `behavior_class`), Phase 2 (4-bucket daemon extraction). Their
detailed plans live in git history at commit `723791cc`; their *outcomes* are the
live `scripts/check-*.sh` ratchets and the `src/daemon/` files. They are folded
into this ledger rather than kept as standalone open plans.

---

## The four enforcement gates (E0 — the "done bar" made executable)

Every gate is a **ratchet** (baseline today's number, fail on regression),
copying the proven `scripts/check-agent-core-boundary.sh` idiom and wired into
`.githooks/pre-commit`. They make "perfect" *measurable and permanent* rather
than aspirational.

| Gate | Script (new) | Baseline 2026-05-31 | Target | Enforces |
|---|---|---|---|---|
| **File-size ceiling** | `check-file-size-ceiling.sh` | max = 14,723 (`daemon.c`) | no `.c` > **800 LOC** | Single-purpose files; ratchet the max down per phase |
| **No new root files** | `check-no-new-root-files.sh` | 101 loose `src/*.c` | **0** | Enforced module boundaries; bans new root files immediately |
| **Zero-duplication** | `check-clone-ratchet.sh` | (measure on first run) | no growth | Collapses copy-paste (PMD-CPD / token-hash for C) |
| **Coverage-preserving** | extend `check-untested.sh` | current | no extracted unit loses its test | The 824 test files stay the safety net |

E0 installs all four at their *current* baselines so the build stays green today;
each later phase drives its ratchet down and, when it hits the floor, locks it.

---

## Why this is the SOTA path

The product thesis stakes the moat on *privacy-by-architecture* and *runs
anywhere* (M2 personal model, M3 on-device learning). **T1 structurally
contradicts that thesis**: the personal model — the moat itself — is welded to
SQLite via 114 files. A Redis-only or pure-on-device target cannot run without
E3. E0–E2 exist to make E3 *survivable*: E0 stops the erosion that already cost
v1 ground on T1; E1 clears the root so contexts are findable; E2 shrinks the
daemon so the memory re-layer is tractable. **The bounded-context work *is* the
moat work.**

---

## Execution

Each phase plan uses the `superpowers:writing-plans` format (TDD, bite-sized
steps, exact paths). Recommended: execute via
`superpowers:subagent-driven-development` (fresh subagent per chip, review
between), one file-editing chip at a time (rule §5 above). Build/verify against
the **production binary** (`touch` first) and run the **full** `./build/human_tests`
before each commit (`.claude/rules/quality-gates.md`).
