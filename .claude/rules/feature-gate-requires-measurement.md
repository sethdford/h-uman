# New Behavior Subsystems Ship OFF→SHADOW→LIVE, Gated on a Measurement

Every new humanness/behavior subsystem in h-uman that changes what gets SENT
(GraphRAG grounding, salience filtering, bandit-chosen humanization params, future
ones) ships **gated OFF by default** and is only promoted toward production by a
**measurement**, not by a green test suite. The activation has three states, and a
gate comment that names the measurement required to advance.

## Why

A persona daemon that texts AS Seth cannot have unproven behavior changes flipped
on by "tests pass." Tests prove the code does what the code says; only a blind A/B
(or equivalent human-judged measure) proves the new behavior reads as *more* human,
not less. Three subsystems built 2026-05-31 all converged on this shape
independently — it's the project's activation contract, so make it explicit.

## The contract

1. **Three states, env-gated, default OFF:**
   ```
   OFF    (default): subsystem does no work; zero behavior change, zero perf cost
   SHADOW: subsystem runs, LOGS what it WOULD do; emitted output unchanged
   LIVE:   subsystem actually shapes the sent output
   ```
   Parse precedence LIVE > SHADOW > OFF from env (e.g. `HU_GRAPH_GROUNDING`,
   `HU_SALIENCE_LIVE`/`HU_SALIENCE_SHADOW`, `HU_BANDIT_HUMANIZATION`). Default
   (all unset) MUST be OFF.

2. **A gate comment at the activation site names the measurement:**
   ```c
   /* <FEATURE> activation gated on Story D blind A/B: do not flip to default-ON
    * without a measurement showing the change is judged superior by real humans. */
   ```
   Precedent: `agent_turn.c:1471` (GraphRAG), the salience trichotomy block, the
   bandit gate at `daemon.c`.

3. **A safety floor for anything that can SUPPRESS content.** If LIVE mode can drop
   a directive, required/safety/crisis/grief directives must pass through
   unconditionally — enforced in production code (not just the test), with a
   fail-safe that reverts to OFF behavior if the invariant would be violated.
   Precedent: salience `goto skip_salience` on a required-directive miss.

4. **SHADOW must capture a metric** (bytes, suppressed-vs-kept counts, decision
   distribution) to `sprints/.../evidence/` via an `HU_IS_TEST`-guarded path —
   NEVER by running the live daemon / sending real messages.

## What this is NOT

- Not a reason to leave a feature unwired. The wiring must be REAL and reachable
  (see `~/.claude/rules/integration-done-contract.md`): when the gate is ON, a test
  must prove the decision shapes the output; OFF, it's unchanged. A gate routing to
  nothing fails this contract.
- Not a substitute for the measurement. "Gated OFF, tests green" is the START
  state, not done. Flipping to LIVE requires the named measurement to PASS.

## When it applies / does not

- APPLIES: any subsystem that alters the outbound message (content, timing,
  humanization params, directive selection, model routing).
- DOES NOT apply: pure read/inference that never reaches the send path; offline
  eval/training tooling; bug fixes to already-LIVE behavior.

## Suggested enforcement (follow-up)

A `scripts/check-feature-gates.sh` pre-commit check could assert that any new
`getenv("HU_*")`-gated behavior block in `src/agent/`/`src/daemon.c` has (a) a
default-OFF path and (b) a gate comment containing "gated on" + a measurement
noun. Until that lands, this rule is the reviewer's checklist.

## Related

- `~/.claude/rules/integration-done-contract.md` — the gate must be REAL (a caller
  exists, assertions non-vacuous); this rule is the activation-state shape.
- `~/.claude/rules/audit-verify-before-allege.md` — verify the gate's default
  state by reading it, don't assume.
- `project_h-uman_voiceai_gap_REALITY` memory — why activation+measurement (not
  build-from-scratch) is h-uman's real work.
