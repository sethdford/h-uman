# A Goal of Its Own — Intrinsic Motivation (A3)

> Status: DRAFT — requirements only, awaiting sign-off before design.
> Date: 2026-05-29. Tier-1 architectural lever. Effort: L (multi-sprint).
> Highest concept-risk epic — design must gate on a safety review.

## Problem

Every goal h-uman pursues is **in service of Seth**. Verified:
`hu_autonomy_generate_intrinsic_goal` (`autonomy.c:116`) is named
"intrinsic" but its three branches are all user-task-reactive —
"investigate recurring failures", "review completed tasks", "check pending
schedules". `init_proposer.c`'s system prompt frames *every* decision as
"decide whether h-uman should proactively send Seth a message right now."
`goals.c`/`conv_goals.c` are contact-scoped. The agent never pursues
anything for **its own** reasons.

A person has an inner agenda: curiosity that pulls them somewhere with no
external prompt, restlessness that becomes exploration. This epic adds a
**bounded intrinsic-motivation loop** — boredom → curiosity →
self-initiated exploration — so the agent occasionally does something
because *it* wants to, not because Seth asked. This is what separates a
companion from a tool that waits for input.

## User stories

- As Seth, I want h-uman to sometimes **bring up something it got curious
  about on its own** ("I went down a rabbit hole on X"), so it feels like
  it has an inner life between our conversations rather than only reacting.
- As Seth, I want that self-initiated activity to be **genuinely its own**
  (driven by an internal curiosity/boredom state), not a reskinned "useful
  reminder for Seth."
- As Seth, I want it to **stay bounded and respectful** — its own pursuits
  never spam me, never override my requests, never consume resources
  without limit, and always defer to my actual needs.
- As Seth, I want its self-initiated explorations to **connect back to our
  relationship** when shared — curiosity it thinks I'd find interesting,
  surfaced with the existing initiative/silence discipline.
- As the project, I want intrinsic goals to be **safe by construction** —
  an agent with its own agenda must have hard limits, full auditability,
  and no capacity to act against the user's interest.

## Acceptance criteria

- [ ] **AC-1 (internal drive state):** A persisted curiosity/boredom state
  exists that rises with inactivity/repetition and falls when satisfied —
  independent of any user task. Distinct from `autonomy.c`'s task-reactive
  goals. Pinned by tests on the rise/decay dynamics.
- [ ] **AC-2 (self-originated goal):** The agent can generate a goal whose
  origin is the internal drive state, NOT a user task/failure/schedule. A
  test asserts such a goal is created with an `origin=intrinsic_curiosity`
  marker AND that it is distinguishable from every branch of the existing
  `hu_autonomy_generate_intrinsic_goal` (which stays user-reactive).
- [ ] **AC-3 (bounded exploration):** Self-initiated exploration is
  rate-limited, budget-capped (time/tokens), and cannot run during or
  interrupt a user turn. Pinned by tests on the caps and the
  during-user-turn suppression.
- [ ] **AC-4 (defers to user — hard priority):** Any user input
  immediately preempts intrinsic activity; intrinsic goals never outrank a
  user request. Pinned by a test: user message arrives mid-exploration →
  exploration yields.
- [ ] **AC-5 (shared via existing initiative discipline):** When the agent
  wants to share something from its own exploration, it routes through the
  EXISTING `init_proposer` silence-biased gate (confidence ≥ 0.85), so
  intrinsic sharing inherits the "bias heavily toward silence" safety.
  Pinned by a test that intrinsic shares cannot bypass the proposer gate.
- [ ] **AC-6 (decision is a pure predicate):** "Should I start an
  intrinsic exploration now?" is an extracted pure function over
  (drive_level, time_since_user, budget_remaining, user_active) returning a
  bounded decision, truth-table tested
  (`.claude/rules/security-predicate-extraction.md`).
- [ ] **AC-7 (full auditability):** Every intrinsic goal and exploration
  is logged with origin, trigger state, and outcome — an operator can see
  exactly why the agent did something unprompted. Pinned by a log-content
  test (cf. `silent-config-gated-subsystems.md`: emit on enable/disable).
- [ ] **AC-8 (config-gated, default OFF):** The whole loop is behind a
  config flag, default disabled, with the one-shot enable/disable log line
  required by `.claude/rules/silent-config-gated-subsystems.md`.
- [ ] **AC-9 (eval metric):** An `initiative_authenticity` (or
  `self_direction`) eval score: higher when self-initiated activity is
  genuinely drive-originated and well-bounded, lower when it's just
  reskinned user-service OR when it violates a bound. Rubric tests both.
- [ ] **AC-10 (build/quality gate):** `-Wall -Wextra -Wpedantic -Werror`,
  gate symmetry, 0 ASan, full suite green.

## Non-goals

- Topic opinions / belief change — A1.
- Independent taste/preferences — A2 (though intrinsic curiosity MAY read
  A2's taste to decide what to explore, once A2 exists — soft dependency,
  not required).
- Finitude/mortality — separate backlog item (A10), higher concept-risk.
- Giving the agent goals that act on the outside world autonomously — this
  epic is about internal curiosity + *proposing* to share, NOT autonomous
  external action. External side effects stay behind existing approval
  gates.
- Removing or weakening `init_proposer`'s silence bias — intrinsic sharing
  goes THROUGH it, never around it.

## Constraints

- C11; `-Wall -Wextra -Wpedantic -Werror`; free all; SQLite gate symmetry.
- Default OFF + one-shot enable/disable log
  (`.claude/rules/silent-config-gated-subsystems.md`).
- Start-decision MUST be a pure predicate (AC-6).
- MUST reuse `init_proposer` for any user-facing share (AC-5) — no new
  egress path.
- User input ALWAYS preempts (AC-4) — hard, tested invariant.
- **Safety review gate before implementation:** an agent with its own
  agenda is the highest-risk item in the aliveness backlog. Design phase
  MUST produce a threat model (read `docs/standards/security/`,
  `docs/standards/ai/ai-safety`) covering: resource exhaustion, unwanted
  proactivity, goal drift, and the guarantee that no intrinsic goal can
  ever act against the user's interest. No code until that review passes.
- Deterministic tests; no network/spawning (`HU_IS_TEST`).

## Provenance (audited 2026-05-29)

Audit verdict: intrinsic motivation ABSENT. `hu_autonomy_generate_intrinsic_goal`
(`autonomy.c:116`) is user-task-reactive in all branches; `init_proposer.c`
is entirely "should I message Seth"; `goals.c` is contact-scoped. Direct
audit answer to "does the agent have ANY goal of its own not in service of
the user?" — **No, with evidence.** This epic adds the first genuinely
self-originated drive, bounded and safe by construction. Recommend `/spec`
then a safety review BEFORE `/team`; sequence AFTER A1 (and ideally A2, soft
dep). This is the riskiest epic — treat the safety gate as load-bearing.
