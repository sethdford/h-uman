# Plan: daemon.c Decomposition (12,262 LOC → cohesive modules)

**Status:** Plan (not a spec) — needs architectural buy-in before specs are authored
**Author:** 2026-05-16 audit follow-up
**Owner:** TBD (architect-level)
**Risk:** High — touches the runtime event loop that everything depends on
**Effort:** 3–4 weeks staged, do NOT attempt as a single PR

## Problem statement

`src/daemon.c` is the largest file in the codebase at **12,262 lines**. It
mixes responsibilities the audit identified as the classic daemon anti-pattern:

> Monolithic event loop + cross-channel routing + director AI + telemetry
> + photo-viewing delays (lines 3000-3008: `HU_PHOTO_VIEWING_DELAY_MIN_MS = 3000`).

The audit verdict was direct:

> daemon.c is a classic daemon anti-pattern. 8K-line agent_turn.c suggests
> the "planner DAG" is actually monolithic instruction dispatch.

Direct consequences:
- Untestable — daemon.c has zero dedicated test file.
- High blast radius — every code change risks the event loop.
- Cognitive load — a new contributor cannot fit it in their head.
- Slow build — touching daemon.c rebuilds a huge object.

## Goals

1. **No behavior change.** The decomposition is structural only.
2. **Each split module is ≤ 1,500 lines.**
3. **Each module has a dedicated test file.**
4. **Module dependency graph is a DAG** — verified by `scripts/gen-include-graph.sh`.
5. **Event loop is reviewable end-to-end in one screenful.**

Non-goals: rewriting the routing logic, changing the protocol, introducing
async/await semantics. Those are separate efforts.

## Proposed decomposition

```
src/daemon/
├── daemon.c              < 800 LOC — top-level init, event-loop skeleton, main()
├── routing.c             ≤ 1500 LOC — cross-channel message routing (currently lines ~2000-4000)
├── director.c            ≤ 1500 LOC — director AI (proactive outreach selection)
├── telemetry.c           ≤ 800 LOC — metrics, observability hooks
├── lifecycle.c           ≤ 600 LOC — signal handling, graceful shutdown, reload
├── ticks.c               ≤ 800 LOC — periodic tick scheduling (photo delays, reflection windows)
├── inbox.c               ≤ 1000 LOC — inbound message queue and dispatch
└── outbound.c            ≤ 1000 LOC — outbound queue, retry, send dispatch
```

Estimated split sizes from current daemon.c topology (verify before
implementation): 12,262 LOC → ~8,200 LOC across 7 files (with ~4,000 LOC of
deduplicated helpers extracted to shared modules).

## Staged execution

Each phase is ONE PR. Do not stack.

### Phase 0 — Test baseline (1 day)
Establish behavior baseline. Write `tests/test_daemon_baseline.c` that
exercises the event loop with N synthetic messages and asserts:
- All messages reach their target channel.
- Director fires once per tick window.
- Graceful shutdown drains the outbound queue.

These tests gate every later phase. They must pass identically after each split.

### Phase 1 — Extract `lifecycle.c` (1 day)
Signal handlers, shutdown coordination, config reload. The smallest, most
self-contained piece. Validates the extraction pattern.

### Phase 2 — Extract `telemetry.c` (1 day)
Metrics collection. Stateless w.r.t. the event loop. Low risk.

### Phase 3 — Extract `inbox.c` + `outbound.c` (3 days)
Message queues. Touch one direction at a time. Each gets a dedicated test file
that mocks the network side.

### Phase 4 — Extract `ticks.c` (2 days)
Periodic scheduling. Includes the `HU_PHOTO_VIEWING_DELAY_*` constants which
move to `include/human/daemon/ticks_constants.h` with comments explaining
why each value exists.

### Phase 5 — Extract `routing.c` (3 days)
Highest risk — cross-channel routing is the most-coupled code in daemon.c.
Use the Phase 0 baseline tests to gate.

### Phase 6 — Extract `director.c` (2 days)
Proactive outreach. Mostly self-contained but depends on persona + memory.

### Phase 7 — Cleanup (1 day)
- Delete dead code revealed by the split.
- Update `ARCHITECTURE.md` and `src/CLAUDE.md`.
- Add `daemon/CLAUDE.md` describing the new layout.

## Verification

Each phase must:
1. Compile under the `dev` preset with `-Werror`.
2. Pass `./build/human_tests` with no new failures.
3. Pass `scripts/gen-include-graph.sh` (no cycles introduced).
4. Pass `scripts/agent-preflight.sh`.
5. Be reviewed by a second engineer (high-blast-radius code).

## Out of scope

- Performance work. Decomposition is line-equivalent; perf optimization comes after.
- Async/event-driven refactor (e.g., libuv, io_uring). Separate proposal.
- daemon.c → daemon/ folder migration touches build files (CMakeLists.txt
  globbing). Plan for this in Phase 0.

## Audit evidence

- File line count: 12,262 (largest in codebase).
- Zero dedicated test file for daemon.c.
- Cross-concern grep: `routing`, `director`, `photo`, `telemetry` all
  collocated.
- Magic constants without justification (`HU_PHOTO_VIEWING_DELAY_MIN_MS = 3000`).

## Risks

- **Behavior drift.** *Mitigation:* Phase 0 baseline tests; never start a
  phase without them green.
- **Worktree merge conflicts.** *Mitigation:* freeze daemon.c for the
  duration of this work — no overlapping PRs.
- **Decomposition reveals a deeper architectural mismatch** (e.g., routing
  belongs in `src/channels/` not `src/daemon/`). *Mitigation:* if any phase
  reveals this, stop and re-plan. Don't carry the wrong decomposition.
