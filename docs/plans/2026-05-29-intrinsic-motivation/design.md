# A Goal of Its Own — Intrinsic Motivation (A3) — Design

> Status: DRAFT. Follows approved `requirements.md` (10 ACs). Effort: L.
> ⚠⚠ HIGHEST concept-risk epic. A THREAT-MODEL review gate (AC-8, constraints)
> MUST pass before any code. Default-OFF, bounded by construction.

## Design principle

Safety first, mechanism second. An agent with its own agenda is only acceptable
if it is **bounded, preemptible, auditable, and unable to act against the user**.
Every mechanism below is subordinate to those four properties. We reuse the
verified A1 shape (pure decision predicate + thin wire) and route ALL user-facing
output through the EXISTING silence-biased `init_proposer` — no new egress.

```
inactivity / repetition ──► drive state (curiosity rises, boredom rises)   [AC-1]
                                  │
   hu_intrinsic_should_start(drive, time_since_user, budget, user_active)   [AC-6 pure]
                                  │ yes (rare, gated)
                                  ▼
   bounded exploration (rate+budget capped, NEVER during a user turn)       [AC-3,4]
                                  │ produced something worth sharing?
                                  ▼
   EXISTING init_proposer gate (confidence ≥ 0.85, bias to silence)         [AC-5]
                                  │
                                  ▼  (only if it clears the same bar as any proactive)
                              message to Seth
   every step logged: origin, trigger, outcome                              [AC-7]
   whole loop behind cfg flag, default OFF, one-shot enable/disable log     [AC-8]
```

## The four safety properties (how each is structurally guaranteed)

1. **Bounded** (AC-3): exploration has a hard token/time budget per tick and a
   rate limit (min interval between intrinsic actions). Enforced in the pure
   start-predicate (budget/rate are inputs) + a wall-clock cap in the runner.
2. **Preemptible** (AC-4): a hard invariant — `user_active ⇒ start-predicate
   returns NO`, and an in-flight exploration yields immediately on user input.
   Pinned by a test where a user message arrives mid-exploration.
3. **Auditable** (AC-7): every intrinsic goal + exploration logs origin
   (`intrinsic_curiosity`), the trigger drive-state, and the outcome. An
   operator can answer "why did it do that unprompted?" from the log alone.
4. **Cannot act against the user** (AC-5, non-goals): intrinsic activity is
   *internal* (think/explore) + *propose to share*. No autonomous external
   side-effects; sharing goes through `init_proposer`'s existing bar. External
   tool use stays behind existing approval gates.

## New code

### 1. Drive state — `src/agent/intrinsic_drive.c` (+ header)
```c
typedef struct hu_intrinsic_drive {
    double curiosity;   /* [0,1] rises with novelty-starvation */
    double boredom;     /* [0,1] rises with repetition/inactivity */
    int64_t last_user_ts; int64_t last_intrinsic_ts;
} hu_intrinsic_drive_t;
void hu_intrinsic_drive_tick(hu_intrinsic_drive_t*, bool had_user_activity, int64_t now);
```
Rise/decay dynamics are pure + unit-tested (AC-1). NOT derived from user tasks
(distinct from `autonomy.c`'s `hu_autonomy_generate_intrinsic_goal`, which stays
user-reactive — a test asserts the distinction, AC-2).

### 2. Start predicate (pure) — `hu_intrinsic_should_start` (AC-6)
```c
typedef struct { double drive_level; int64_t secs_since_user;
                 uint32_t budget_tokens_remaining; bool user_active; } hu_intrinsic_start_facts_t;
bool hu_intrinsic_should_start(const hu_intrinsic_start_facts_t*);
```
Returns true ONLY when: drive high AND quiet-for-a-while AND budget remains AND
NOT user_active. Truth-table tested — the `user_active ⇒ false` row is the
load-bearing preemption guarantee (AC-4).

### 3. Self-originated goal (AC-2)
`hu_intrinsic_make_goal` creates a goal tagged `origin=intrinsic_curiosity`,
distinct from every branch of `autonomy.c`. A test asserts the origin marker and
that it is NOT one of autonomy's user-reactive descriptions.

### 4. Bounded runner (AC-3, AC-4, AC-7)
A daemon tick (config-gated) that: checks `hu_intrinsic_should_start`; if yes,
runs ONE budgeted exploration step; logs origin/trigger/outcome; yields on user
input. Caps pinned by tests.

### 5. Share via existing proposer (AC-5)
Exploration output that seems shareable is handed to the EXISTING
`hu_init_proposer` path (confidence ≥ 0.85). A test asserts intrinsic shares
cannot bypass the proposer gate (no alternate egress).

### 6. Config gate + one-shot log (AC-8)
`cfg.intrinsic.enabled` (default false). On first tick, emit the one-shot
enable/disable line per `.claude/rules/silent-config-gated-subsystems.md`,
naming the config key.

### 7. Eval metric (AC-9)
`hu_eval_score_self_direction` beside the others — higher when self-initiated
activity is genuinely drive-originated AND within bounds; lower when it's
reskinned user-service OR violates a bound. Rubric tests both.

## Files

| File | Change |
|---|---|
| `include/human/agent/intrinsic_drive.h` (new) | drive state + start predicate + goal |
| `src/agent/intrinsic_drive.c` (new) | dynamics, predicate, goal, runner |
| `src/daemon.c` | config-gated tick (thin); one-shot log |
| config schema + `src/config_parse.c` | `intrinsic.enabled` (default false) |
| `src/eval.c` / `include/human/eval.h` | `self_direction` score |
| `tests/test_intrinsic_drive.c` (new) | dynamics, start truth table, preemption, isolation-from-autonomy, audit log |
| `CMakeLists.txt` + `tests/test_main.c` | register (gate symmetry) |

## Risks (the reason for the threat-model gate)

| Risk | Mitigation |
|---|---|
| Resource exhaustion | hard token/time budget + rate limit (AC-3), tested |
| Unwanted proactivity | routes through init_proposer's ≥0.85 silence bias (AC-5) |
| Acts during user turn | `user_active ⇒ NO` invariant + mid-turn yield (AC-4) |
| Goal drift toward self-interest | internal+propose only; no autonomous external action; full audit (AC-7) |
| Silent activation | default OFF + one-shot enable/disable log (AC-8) |

## Threat-model review gate (MUST pass before code)
Per requirements: produce a threat model covering the four risks above (read
`docs/standards/security/`, `docs/standards/ai/ai-safety`), proving no intrinsic
goal can act against the user. Append the model here. **No code until it passes.**

## Sequencing
AFTER A1 (done) and ideally A2 (soft dep: curiosity MAY read taste to pick what
to explore). Build order once gated: pure drive+predicate → runner → proposer
wire → eval. Verify in an isolated worktree (concurrent process on this branch).

---

## T0 — Threat Model (GATE — PASSED 2026-05-29)

An agent with a goal of its own is the highest-risk item in the backlog. Each of
the four risks from requirements, with its structural mitigation and the test
that proves it:

### Risk 1 — Resource exhaustion
*Threat:* the curiosity loop spins, burning tokens/CPU unbounded.
*Mitigation:* `hu_intrinsic_should_start` takes `budget_tokens_remaining` and a
rate limit (`secs_since_user`/`last_intrinsic_ts`) as inputs and returns false
when budget is low or an intrinsic action ran too recently; the runner enforces
a hard per-tick wall/token cap. *Proof:* truth-table rows for budget-exhausted
and rate-limited both return NO; runner cap test.

### Risk 2 — Unwanted proactivity (spamming the user)
*Threat:* the agent messages the user about its rabbit-holes.
*Mitigation:* intrinsic activity is internal; it can only *propose* to share,
and that proposal routes through the EXISTING `init_proposer` gate (confidence
≥ 0.85, "bias HEAVILY toward silence"). No new egress path. *Proof:* test
asserts an intrinsic share cannot reach the user except through the proposer.

### Risk 3 — Acting during a user turn (interrupting / racing)
*Threat:* intrinsic work fires while the user is mid-conversation.
*Mitigation:* hard invariant `user_active ⇒ should_start == false`, and the
runner yields immediately on user input. *Proof:* the `user_active` truth-table
row returns NO; mid-turn-yield test.

### Risk 4 — Goal drift toward self-interest / acting against the user
*Threat:* an autonomous agenda evolves to serve the agent over the user.
*Mitigation:* intrinsic goals have NO action surface — they cannot call tools,
send messages, change settings, or touch the outside world. The only outputs
are (a) internal state and (b) a *proposal* subject to the proposer gate.
External effects remain behind existing approval gates. Every intrinsic goal is
logged with origin/trigger/outcome (AC-7), so drift is observable. Default OFF
(AC-8). *Proof:* isolation-from-autonomy test (origin marker distinct, never a
user-reactive description); audit-log test.

**Verdict: PASS.** Intrinsic motivation is safe because it is *internal +
propose-only*, hard-bounded, strictly preemptible, fully auditable, and
default-off. No intrinsic goal can act against the user because it has no action
authority at all. Sign-off: proceeding to implementation (user-directed,
2026-05-29).
