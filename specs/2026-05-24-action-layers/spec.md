# Self-Model + TOM Action Layers — Spec

Follow-up to Spec 3 (self-model scaffold) and Spec 4 (TOM activation),
which were both observe-only / descriptive-only. This spec adds the
minimum corrective behavior so each subsystem moves from "records signal"
to "uses signal."

Combined into one spec doc because the two subsystems share an
implementation pattern (inject a directive into the per-turn prompt)
and live in adjacent code paths.

## Goal

- **Self-model**: when drift detected past threshold, the agent gets a
  corrective directive in its next turn's system prompt nudging it back
  toward its calibrated baseline.
- **TOM**: when an unmet user expectation persists across ≥2 turns
  without being resolved, the agent gets a directive prompting it to
  ASK about that topic rather than confabulate.

Both stay strictly "prompt-side" — no agent-side action that BYPASSES
the LLM's reasoning. The LLM still decides; we just give it more
relevant context.

## User stories

- As a user, I want the agent to NOTICE when it's drifting from how it
  normally talks to me (e.g., gradually more terse), so style shifts
  are corrected before they accumulate invisibly.
- As a user, I want the agent to ASK when I've said something it
  doesn't have a recorded belief for, so it doesn't confidently
  confabulate.
- As a developer, I want the action layer to be a single directive
  hook each — not a behavioral overhaul — so the change is bounded,
  testable, and revertable.

## Acceptance criteria

- [ ] **AC-AL-1: Self-model drift directive.** When `agent_self_concerns`
  has an unresolved row in the last 7 days with `|magnitude_sigma| ≥ 2.0`
  (existing threshold), the world-model render produces a directive
  string like "Recent drift: response_length is +2.3σ above your
  baseline with Seth. Lean toward shorter replies in this turn."
  Pinned by a test that injects a concern row and asserts the rendered
  world model contains the directive substring.

- [ ] **AC-AL-2: TOM unmet-expectation clarification directive.** When
  `tom_user_expectations` has a row that is (a) unresolved
  (`resolved_ts_ms IS NULL`), (b) at least 2 turns old (created in a
  prior `session_key` OR ≥10 minutes ago), the TOM context block emits
  a directive like "User expects you know about <topic> (type=REMEMBERS),
  but you don't have a recorded belief about it. Consider asking
  briefly rather than guessing." Pinned by a test that records an
  expectation, advances time/session, and asserts the prompt directive.

- [ ] **AC-AL-3: Both directives are FEATURE-FLAG-GATED.** New build
  flag `HU_ENABLE_ACTION_LAYERS` defaults OFF. When OFF, the prompt
  output is identical to before (no directive injected). When ON, the
  directives appear when their conditions are met. Pinned by a build-
  variant test.

- [ ] **AC-AL-4: Both directives respect existing privacy rules.** The
  drift directive uses dimension names + magnitude (e.g.,
  "response_length is +2.3σ"), NEVER actual response content. The
  clarification directive uses the topic string already in the
  expectation row (already privacy-cleared) and the expected_knowledge_type
  enum, NEVER the user's prior messages. Pinned by the existing
  grep-based privacy test from Spec 3 (extended to cover the new
  emitters).

- [ ] **AC-AL-5: Self-model and TOM directives are independent.** A
  drift concern doesn't trigger a TOM directive, vice versa. Pinned by
  a test that injects ONE signal of each type and asserts only that
  type's directive appears.

## Non-goals

- Agent autonomously sending a "let me ask you about X" message
  unprompted. The directives only fire when the agent is ALREADY about
  to respond. No outbound-from-nothing.
- Multi-turn dialog state machine for clarification questions. Just one
  directive per turn; the LLM decides whether to ask.
- Drift correction outside the LLM's reasoning. We don't modify
  response post-hoc; we just inform the LLM.
- New baselines or drift thresholds. Reuse existing calibration values.
- TOM expectation pattern table improvements. Out of scope per Spec 4
  non-goals.

## Constraints

- C11 `-Wall -Wextra -Wpedantic -Werror`, ASan-clean.
- Both directives emitted inside existing functions:
  - `hu_world_model_merge_self_observations` (Spec 3, Phase D) — for AC-AL-1
  - `hu_tom_build_context_with_expectations` (Spec 4, Phase A) — for AC-AL-2
- New build flag `HU_ENABLE_ACTION_LAYERS` follows the existing
  HU_ENABLE_SELF_MODEL pattern from Spec 3.
- Directives are SHORT (≤ 200 chars each) to minimize prompt-size
  inflation — observed at 23KB it's already painful (see chat-lock fix).
- Privacy: dimension names + magnitudes + topic strings + enums only.
- Backwards compatible: with flag OFF, output is byte-identical.

## Design

### Components

- **Drift directive renderer** in `src/agent/world_model_bridge.c`,
  extending `hu_world_model_merge_self_observations`. Reads top concern
  from `agent_self_concerns` table; emits directive line into the
  rendered world model string.
- **Clarification directive renderer** in `src/agent/theory_of_mind.c`,
  extending `hu_tom_build_context_with_expectations`. Filters unmet
  expectations for the "stale enough" condition; emits directive line.
- **Build flag** `HU_ENABLE_ACTION_LAYERS` in CMakeLists.txt with
  propagation to all 4 targets (matches HU_ENABLE_SELF_MODEL pattern).
- **Tests** in `tests/test_action_layers.c` (new file) covering both ACs
  + the feature-flag-OFF byte-identical check.

### Data flow

```
[Turn arrives]
    │
    ▼
[Pre-turn assembly: pre-existing world-model merge + TOM context build]
    │
    ├──▶ AC-AL-1: if (action_layers_enabled) check agent_self_concerns
    │        if recent concern exists → append drift directive
    │
    └──▶ AC-AL-2: if (action_layers_enabled) check tom_user_expectations
             for each unresolved + ≥2-turn-old → append clarify directive
[Assembled prompt sent to LLM, which decides how to respond]
```

### Decisions

- **D-AL-1 (AC-AL-1, AC-AL-2): Inject as plain-text directive in
  existing context blocks, NOT a new top-level "## Action Directives"
  section.** Chose context-block injection because the LLM is more
  likely to attend to a directive that's contextually adjacent to its
  source (drift directive next to self-observations; clarify directive
  next to TOM expectations). A separate section invites the LLM to
  ignore it as boilerplate.

- **D-AL-2 (AC-AL-3): Feature-flag-gated, default OFF.** Chose default
  OFF because (a) action layers change observable LLM behavior — a
  user opting in is more honest than auto-on; (b) the drift directive
  could feel intrusive ("you're drifting!") to some users; (c) makes
  the change reversible by config.

- **D-AL-3 (AC-AL-1): Use ONE concern (the most recent) per turn,
  not all concerns.** Multiple directives compound; one focused signal
  is more actionable. The next turn picks up the new most-recent concern.

- **D-AL-4 (AC-AL-2): "Stale enough" = `created in prior session_key
  OR ≥10 min ago`.** Same-session expectations are still being clarified
  by the natural dialog; we wait until a session boundary OR a
  meaningful pause has passed before injecting the directive.

### Risks

- **R-AL-1**: LLMs may not faithfully follow the directive. Mitigation:
  start with a clear, imperative directive form. The LLM is the
  oracle — we accept some directive non-compliance.
- **R-AL-2**: Drift directive could lock the agent into baseline,
  preventing organic style evolution. Mitigation: the directive fires
  only at ≥2σ; transient drift won't trigger. And the user adjusting
  the calibrated baseline (a separate mechanism) shifts the goalposts.
- **R-AL-3**: Clarification directive could make the agent annoying
  ("wait, what did you tell me about X" repeated). Mitigation: the
  "≥2 turns old" filter prevents repeated firing within a single
  conversation flurry.

## Tasks

| # | Task | ACs |
|---|---|---|
| 1 | Add `HU_ENABLE_ACTION_LAYERS` CMake flag (default OFF) + propagate to human_core / human / human_core_test / human_tests | AC-AL-3 |
| 2 | Extend `hu_world_model_merge_self_observations` to emit drift directive when `agent_self_concerns` has recent unresolved row past threshold. Gated on flag. | AC-AL-1 |
| 3 | Extend `hu_tom_build_context_with_expectations` to emit clarification directive for stale-enough unmet expectations. Gated on flag. | AC-AL-2 |
| 4 | New test file `tests/test_action_layers.c` with 4 tests: drift directive fires, clarify directive fires, independence (one signal → only one directive), flag-off byte-identical output. | AC-AL-1, AC-AL-2, AC-AL-3, AC-AL-5 |
| 5 | Extend the privacy grep test from Spec 3 to cover the new emitters. | AC-AL-4 |
| 6 | Register the new test runner in `tests/test_main.c`. | (mechanics) |

Sequential: 1 → 2 → 3 → 4 → 5 → 6.
