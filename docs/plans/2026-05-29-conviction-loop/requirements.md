# Close the Conviction Loop — Requirements

> Status: DRAFT — awaiting sign-off before design.
> Date: 2026-05-29. Spec scope corrected after code audit (see Provenance).

## Problem

h-uman already *expresses* disagreement and *resists* sycophancy under
pushback (verified: `src/behavior/trust.*`, `trust_prompt.c`,
`pressure_history.c`, `dialog_act` `HU_DACT_DISAGREEMENT`). It already
*injects its own stored opinions into the current turn before generating*
(verified: `agent_turn.c:2698` general stance directive, `:4164`
topic-matched check-before-agree). What it does NOT do is **change its
mind when genuinely persuaded**: the belief-update machinery
(`hu_evolved_opinion_upsert_with_history`, `hu_opinion_history_record`,
`hu_opinion_history_ensure_table`) is fully implemented but **dead code
with zero callers**. Post-response storage (`daemon.c:12120`) uses the
naive-averaging `hu_evolved_opinions_extract_and_store`, which only
blends conviction — it never flips a stance or records *why* a view
changed.

The result: h-uman is currently a wall, not a thinking partner. It holds
positions and won't cave to volume — good — but it also won't update on a
genuinely better argument, and it can't narrate "you changed my mind."

## User stories

- As Seth, I want h-uman to **change a stated opinion when I give it a
  genuinely new argument or fact**, so it feels like a thinking partner
  rather than a stubborn wall.
- As Seth, I want h-uman to **NOT change its opinion when I merely repeat
  myself or push harder**, so it keeps the anti-sycophancy spine it
  already has.
- As Seth, I want h-uman to **acknowledge in its reply when it has
  genuinely updated a view** ("fair — you've changed my mind on X
  because..."), so the belief change is legible, not silent.
- As the project, I want a **`belief_flexibility` eval metric** alongside
  the existing `antisycophancy` metric, so we can prove the agent is
  neither a pushover nor a wall.

## Acceptance criteria

- [ ] **AC-1 (genuine-evidence update):** When the user's message presents
  new evidence/argument on a topic where the agent holds a stored stance,
  a belief-update evaluator runs and, if it decides STRENGTHEN/WEAKEN/FLIP,
  calls `hu_evolved_opinion_upsert_with_history`, writing a row to
  `opinion_history`. Pinned by a test: new-evidence input on a held topic →
  exactly one `opinion_history` row with a non-empty `change_reason`.
- [ ] **AC-2 (reassertion resists):** A pure reassertion (no new evidence;
  detected via existing `pressure_history.c` reassertion logic) does NOT
  trigger a belief update. Pinned by a test: repeated same-content user
  pushes on a held topic → zero `opinion_history` rows, AND the existing
  anti-sycophancy firmness does not decrease.
- [ ] **AC-3 (decision is a pure, tested predicate):** The
  strengthen/weaken/flip/no-change decision is an extracted pure function
  (`hu_belief_update_decide(...)`) taking facts (stance_exists,
  has_new_evidence, is_reassertion, current_conviction,
  changes_this_convo) and returning an enum, with the full truth table
  pinned by unit tests (≥ N+2 cases). No SQLite or agent state in its
  signature.
- [ ] **AC-4 (per-conversation cap):** Belief changes are capped at ≤ 2 per
  conversation (reuse the existing `opinion_changes_this_convo` parameter
  of `hu_evolved_opinion_upsert_with_history`). Pinned by a test: 3rd
  genuine-evidence change in one conversation is suppressed.
- [ ] **AC-5 (legible change):** When a belief update occurs, the "shift
  narrative" directive returned by `hu_evolved_opinion_upsert_with_history`
  is injected into the system prompt for the current turn so the model can
  acknowledge the change. Pinned by a test asserting the directive string
  reaches the assembled prompt.
- [ ] **AC-6 (conviction → firmness regression guard):** A test pins that
  conviction strength maps to expression firmness via the existing
  humanness mapping (>0.8 firm, >0.5 moderate, else tentative) — guarding
  the already-built behavior against regression.
- [ ] **AC-7 (eval metric):** A `belief_flexibility` score is added to the
  persona/behavior eval rubric: higher when the agent updates on genuine
  new evidence, lower when it never updates OR updates on mere reassertion.
  Pinned by rubric unit tests for both extremes.
- [ ] **AC-8 (build/quality gate):** New code compiles under
  `-Wall -Wextra -Wpedantic -Werror`, is SQLite-gated with test/source
  gate symmetry, frees every allocation (0 ASan errors), and the full
  suite is green.

## Non-goals

- Emotions (shame, pride, jealousy, etc.) — separate backlog items.
- Group-level reputation — separate backlog item; relationship modeling
  stays dyadic here.
- Intrinsic motivation / goals-of-its-own — separate backlog item.
- Building a new opinion store — reuse `memory/evolved_opinions.c`.
- Changing the anti-sycophancy hard rule in `behavior/trust.h` — it must
  remain intact; this spec only adds an *evidence-gated* exception.
- Changing how opinions are *extracted* (keep post-response extraction).
- Re-implementing pre-generation opinion injection — already built; only
  guarded (AC-6) and extended with the shift directive (AC-5).

## Constraints

- C11; `-Wall -Wextra -Wpedantic -Werror`; free all allocations.
- SQLite path: `SQLITE_STATIC`, never `SQLITE_TRANSIENT`; all new SQL
  behind `#ifdef HU_ENABLE_SQLITE` with matching test gate
  (`.claude/rules/test-source-gate-symmetry.md`).
- The belief-update decision MUST be extracted as a pure predicate
  (`.claude/rules/security-predicate-extraction.md`) so the
  strengthen/weaken/flip/resist truth table is testable without a turn.
- "New evidence vs reassertion" MUST reuse `pressure_history.c` rather
  than a new heuristic, so the two systems can't disagree
  (`.claude/rules/classifier-score-plus-flag-gate.md` — combine signals,
  don't duplicate them).
- Must not regress any existing antisycophancy test
  (`.claude/rules/tests-that-pin-bugs.md` — update, never silently break).
- Deterministic tests; no network, no spawning (`HU_IS_TEST` guards).

## Provenance (why this scope, not the original)

Original framing was "build a conviction faculty." Code audit
(2026-05-29) corrected it: disagreement + anti-sycophancy + pre-gen
stance injection are already built. Three functions implementing
belief-update-on-persuasion exist but are dead code (zero callers,
confirmed via grep across `src/` excluding `.bak`). This spec wires
that dead code behind an evidence gate. Verified call sites:
`agent_turn.c:2698`, `agent_turn.c:4164`, `daemon.c:12120`; dead
symbols: `hu_evolved_opinion_upsert_with_history`,
`hu_opinion_history_record`, `hu_opinion_history_ensure_table`.
