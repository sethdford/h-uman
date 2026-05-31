# Phase E2 — Carve `service_lifecycle` Out of `daemon.c` (the unwritten "Phase 2b")

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax. Dispatch ONE file-editing chip at a time.

**Goal:** Shrink `src/daemon.c` from **14,723 LOC** to **< 800** by carving the
`service_lifecycle` giant — `hu_service_run` + `hu_service_run_proactive_checkins`
— and its helpers into single-responsibility translation units under
`src/daemon/`, driving the E0 file-size ratchet `MAX_BASELINE` down. **Behavior
must not change** — this is the highest-risk phase (splitting a multi-thousand-LOC
function), so it is front-loaded with a fresh inventory and a characterization
harness before any cut.

**Architecture:** Phase 2 (v1) extracted the 4 cohesive buckets into
`src/daemon/{director,identity,peripheral_gov,message_router,proactive_policy,reply_dedup}.c`
and exposed cross-bucket state via `daemon/common.h`. What remains in `daemon.c`
is the **service loop**: `hu_service_run` (the tick scheduler / inbound-outbound
dispatcher) and `hu_service_run_proactive_checkins`, plus ~8,500 LOC of helpers
the v1 plan under-counted. E2 decomposes the loop into:

| New unit | Responsibility |
|---|---|
| `src/daemon/service_loop.c` | the top-level `hu_service_run` tick scheduler (thin orchestrator) |
| `src/daemon/inbound_pump.c` | inbound message intake + routing-to-agent |
| `src/daemon/outbound_pump.c` | outbound flush / send-path dispatch |
| `src/daemon/proactive_checkins.c` | `hu_service_run_proactive_checkins` + its policy helpers |
| `src/daemon/tick_scheduler.c` | the per-subsystem tick cadence (cron, reflection, social, reaction-poll) |
| `daemon.c` (residual) | `main`-side daemon entrypoint + composition wiring only (< 800 LOC) |

**Hard constraint (the real difficulty):** `gov_budget` is mutated inside the
service loop but *owned* by the peripheral-gov bucket — direct field writes after
extraction invert the dependency. Resolve with an explicit
`hu_daemon_gov_*` **mutator API** on `peripheral_gov.h` (the loop calls the
mutator; it does not write the struct). Same resolution shape the v1 Phase-2 plan
used for `g_proactive_throttle` (keep the static, export the accessor).

**Tech Stack:** C11, CMake source lists, `HU_TEST_*` (88 `HU_IS_TEST` guards in
the loop must be preserved verbatim), ASan dev build.

---

## ⚠️ Stale-inventory warning (read first)

The v1 Phase-2 plan cited `hu_service_run` at "L2495, 3,759 LOC" and predicted
`daemon.c` would fall to ~13,900. **Measured 2026-05-31: it is 14,723** — the
line numbers have drifted and the file barely shrank. **Do NOT trust any line
number from the v1 plan.** Task 0 re-derives the inventory from the current file.
Per `~/.claude/rules/audit-verify-before-allege.md`, every "function X lives at
L_n" claim in this plan is a *placeholder to be filled by Task 0*, not a fact.

---

## Task 0: Fresh inventory + dependency map (orientation — no code change)

- [ ] **Step 1: Re-derive the function map of the current `daemon.c`.**

```bash
cd "$(git rev-parse --show-toplevel)"
# Top-level function definitions with their start lines:
grep -nE '^(static +)?(hu_error_t|void|int|bool|size_t|float|double|char|const) +[a-zA-Z_]+ *\(' src/daemon.c \
  | sed -E 's/\{.*//' > /tmp/daemon_fns.txt
wc -l /tmp/daemon_fns.txt; sed -n '1,60p' /tmp/daemon_fns.txt
```
Record, for `hu_service_run`, `hu_service_run_proactive_checkins`, and every
helper they call: name, start line, approximate LOC. This table replaces the
v1 "File Structure" table.

- [ ] **Step 2: Map the static globals each candidate unit touches.**

```bash
grep -nE '^static .*\b(g_[a-z_]+|gov_budget|g_proactive_throttle)\b' src/daemon.c
# For each static, which functions read/write it → determines move target vs daemon/common.h extern.
```
Produce the "shared-state resolution" table (which statics move with a unit, which
become `common.h` externs, which need a mutator API). `gov_budget` is the known
hard case → mutator API.

- [ ] **Step 3: Confirm threading model.** `grep -c pthread_create src/daemon.c`
(measured: **0** — the loop does not spawn threads itself; worker threads live in
`src/agent/`). This means E2 is function-decomposition, not concurrency surgery —
record it so reviewers don't fear data races from the move.

- [ ] **Step 4: Write the inventory as `docs/plans/2026-05-29-ddd-bounded-contexts/E2-inventory.md`** and commit it (`docs(ddd): E2 daemon service-loop inventory`). This is the contract the extraction chips execute against.

---

## Task 1: Characterization harness for the service loop (BEFORE any cut)

Splitting a multi-thousand-LOC function with no behavioral test is uninsurable.
Build the net first.

- [ ] **Step 1: Identify the loop's observable contract.** `hu_service_run` is a
tick scheduler — its observable effects per tick are: which subsystem ticks fire,
in what order, under what config gates (the 88 `HU_IS_TEST` guards already expose
many of these). Enumerate the observable side-effects (bus events published,
reply dispatches, checkin emissions) from the Task-0 inventory.

- [ ] **Step 2: Write `tests/test_daemon_service_loop_characterization.c`** that
drives ONE tick of `hu_service_run` (or a `HU_IS_TEST`-gated single-tick entry
point — add one if absent) against a fixture agent + mock bus, and asserts the
*current* observed sequence of side-effects. This is characterization (freeze
known-good behavior across the move), not bug-pinning
(`.claude/rules/tests-that-pin-bugs.md`): run once, observe, assert the observed.

- [ ] **Step 3: Run, set expected values, commit.** `./build/human_tests --filter=service_loop_characterization` green. Commit `test(ddd): characterize daemon service-loop tick before E2 carve`. **This test must stay green through every E2 chip — it is the behavior-preservation proof.**

---

## Task 2: Establish the gov-budget mutator API (unblocks the carve)

- [ ] **Step 1:** In `include/human/daemon/peripheral_gov.h`, add
`void hu_daemon_gov_budget_charge(...)` / `_reset(...)` / `bool hu_daemon_gov_budget_allow(...)`
covering every write the service loop currently makes to `gov_budget`. Implement
in `daemon/daemon_peripheral_gov.c` (it owns `gov_budget`).
- [ ] **Step 2:** Replace the loop's direct `gov_budget.field = …` writes with the
mutator calls — **in `daemon.c`, before extraction** (so this chip is a pure
in-place refactor verified by the full suite + characterization test).
- [ ] **Step 3:** Build prod + full suite + characterization green. Commit
`refactor(daemon): route service-loop gov-budget writes through mutator API`.

---

## Task 3–N: Extract one unit per chip (behavior-preserving)

For each unit in the architecture table (`proactive_checkins` first — it's the
most self-contained at ~1,496 LOC; then `tick_scheduler`, `inbound_pump`,
`outbound_pump`; `service_loop` orchestrator last), apply the chip:

- [ ] **Step 1: Header** — declare the unit's public functions (verbatim
signatures from the Task-0 inventory; promote needed statics to `hu_daemon_*`).
- [ ] **Step 2: Move** the functions + single-unit statics into
`src/daemon/<unit>.c`; cross-unit statics reach via `daemon/common.h`; `gov_budget`
via the Task-2 mutator. Add `#include "human/daemon/<unit>.h"` to both files.
- [ ] **Step 3: CMake** — register `src/daemon/<unit>.c`.
- [ ] **Step 4: Build prod (touch first) + FULL suite + characterization test.**
`Results: N/N`, 0 ASan, characterization green. Per
`ground-truth-over-proxy-signals.md`, trust the build, not clangd or any
sub-agent's report — re-run the suite yourself.
- [ ] **Step 5: Lower the E0 size ratchet.** After each extraction,
`wc -l src/daemon.c`; set `MAX_BASELINE` in `check-file-size-ceiling.sh` to the
new max (which stays `daemon.c` until it drops below the next-largest file).
- [ ] **Step 6: Commit** `refactor(daemon): extract <unit> from service loop (E2; daemon.c X→Y LOC)`.

**Sizing (`agent-task-sizing.md`):** ONE unit per chip. Never "extract the whole
service loop" in one dispatch — the v1 plan's failure mode was treating the loop
as one move; it is 5+ chips.

---

## Task FINAL: Verify the daemon shrank + lock the ratchet

- [ ] **Step 1:** `wc -l src/daemon.c` → **< 800**. The residual is the daemon
entrypoint + composition wiring only.
- [ ] **Step 2:** `wc -l src/daemon/*.c` — five new single-responsibility units,
none > 800 LOC (if any exceeds, it needs a further split — record as an E2
follow-on chip).
- [ ] **Step 3: Behavioral smoke test against the running daemon** — run the
daemon against a fixture inbound and confirm a reply still routes
(`scripts/agent-preflight.sh` / the daemon smoke harness). Evidence, not assertion
(`.claude/rules/quality-gates.md`).
- [ ] **Step 4:** Lower `MAX_BASELINE` to the new global max (now some *other*
file is largest — likely `agent/agent_turn.c` at 9,838, which becomes E4/future
work). Commit `ci(ddd): lower file-size ratchet after daemon carve (E2 complete)`.

---

## Self-Review

- **Spec coverage:** the service loop is decomposed into 5 units + a < 800-LOC
  residual; the gov-budget dependency inversion is resolved via a mutator API;
  characterization precedes every cut. ✓
- **No false line numbers:** the plan explicitly voids v1's stale line numbers and
  makes Task 0 (fresh inventory) a prerequisite — no extraction chip runs on
  unverified offsets (`audit-verify-before-allege.md`). ✓
- **Behavior preservation:** a characterization test built FIRST gates every chip;
  every chip ends in the full suite + prod build + that test green; the 88
  `HU_IS_TEST` guards move verbatim. ✓
- **Risk acknowledged:** flagged Med-High; front-loads the net; one unit per chip;
  threading confirmed out-of-scope (0 `pthread_create`). ✓
- **Ratchet-driven:** each extraction lowers `MAX_BASELINE`; the final chip hands
  the "largest file" crown to the next target. ✓
