# Sprint 6 Audit — Relational Authenticity

Branch: `sprint-6-relational-authenticity` · Base: `v-sprint-5-close` · Commits: 4

## Stories audited

| Story | AC count | Delivered | Partial | Missed | Drift | Ambiguous |
|---|---|---|---|---|---|---|
| US-14 | 4 | 4 | 0 | 0 | 0 | 0 |
| US-17 | 5 | 5 | 0 | 0 | 0 | 0 |
| US-19 | 4 | 1 | 0 | 1 | 2 | 0 |

## AC-by-AC findings

### US-14 — Voice maturity directive
- AC1: DELIVERED — `hu_voice_maturity_build_directive` defined at `src/persona/voice_maturity.c:125`; INTIMATE string contains "earned vulnerability" / "shared history" / "don't perform warmth" as specified.
- AC2: DELIVERED — Called at `src/agent/agent_turn.c:2880`, gated `#ifdef HU_ENABLE_PERSONA`, with Sprint 6 US-14 comment block at line 2872. Sits alongside mood/somatic context as required.
- AC3: DELIVERED — `VoiceMaturityDirective` suite: 4/4 PASS (`voice_build_directive_all_stages_distinct_nonempty`, `..._formal_differs_from_intimate`, null/zero-len guards).
- AC4: DELIVERED — Full suite 10,349/10,349 PASS, 0 failures, 0 ASan errors (`./build/human_tests`).

### US-17 — Emotional contagion
- AC1: DELIVERED — `hu_emotional_apply_contagion` at `src/cognition/emotional.c:261`; clamps to [-1, 1] (lines 307-310); mutates valence and arousal.
- AC2: DELIVERED — Called at `src/agent/agent_turn.c:1187`, AFTER `hu_emotional_cognition_perceive` (which consumes `stm_emo` from `hu_emotional_state_get_recent`-derived data) and BEFORE the prompt build path. Comment block at 1180 references "Sprint 6 US-17".
- AC3: DELIVERED — Default fraction 0.3 (line 269); 30% bound passed at call site (line 1188).
- AC4: DELIVERED — `tests/test_emotional_contagion.c:30` asserts sadness 0.8 × 0.3 × −0.7 = −0.168 within ±0.05; null-safe and neutral-no-change cases also asserted. `EmotionalContagion` suite: 7/7 PASS.
- AC5: DELIVERED — Full suite green.

### US-19 — Post-generation style mirroring
- AC1: DELIVERED — `hu_style_mirror_apply` in `src/persona/style_mirror.c:102` implements lowercase ≥70% rule, period-strip ≥70% rule, and a conservative 1-3-letter sentence-start word filter for proper-noun preservation. Question/exclamation marks preserved.
- AC2: **MISSED** — `hu_style_mirror_apply` is **NEVER CALLED** from any agent path. `grep -rn "hu_style_mirror_apply" src/agent/` returns zero hits. The header is `#include`d at `src/agent/agent_turn.c:23` but the function is never invoked. The DoD line "function is callable from the agent path and produces a different output than input when mirroring conditions are met" is **NOT** satisfied at runtime. This is the enforcement-layer story; without the call site, mirroring remains advisory — exactly the bug the story was filed to fix.
- AC3: DRIFT — Tests cover the three required cases plus extras, but they exercise the unit function in isolation. No integration test asserts that the agent pipeline actually rewrites an outbound buffer. `StyleMirror` suite: 12/12 PASS.
- AC4: DELIVERED — Full suite 10,349/10,349 green.

DRIFT note on AC1 proper-noun rule: the implementation uses a length heuristic (only lowercases sentence-start words of 1–3 chars). "Hi"/"It"/"OK" are lowercased; "Jordan" survives. This works for the named tests but will silently lowercase short proper nouns like "Al", "Mo", "Jo", "Ben", "Sam", "Tim", "Amy". The AC said "DO NOT modify proper nouns" — a length heuristic is not a proper-noun detector. Acceptable as a conservative stopgap, but the AC's spirit is partly violated.

## Scope creep

- `include/human/agent/prompt.h` and `src/agent/prompt.c` were modified (+5/+19 lines). Touches the prompt-context plumbing; plausibly required to thread the voice-maturity directive through, but not explicitly traced to a story in any commit message. Verify before merge.
- `src/cognition/emotional.c` grew by 207 lines (vs. ~50 for the contagion function alone). Some of this is trajectory-slope refactor visible at line 256. Not in any AC. NAME IT.

## DoD violations

- US-19 DoD: "Function is callable from the agent path and produces a different output than input when mirroring conditions are met." **Not satisfied** — function is defined and unit-tested but unreachable from the agent runtime. Story marked as if shipped; behavior absent in production path.
- No `/verify` runs in session log per the brief ("none ran formally"). Auditor-only verification accepted, but this is a process gap.

## Adversarial findings

- **US-19 is the load-bearing story of this sprint** (enforcement layer, the whole point of "not just LLM-instructed to do so"). Shipping the library function without the call site satisfies the letter of "code exists + tests pass" while violating the entire premise. Classic AC-satisfied-by-changing-the-test-surface pattern: the test asserts on `hu_style_mirror_apply` directly, never on agent output, so it can pass while the agent path is unchanged.
- US-19's proper-noun rule is a length heuristic, not a proper-noun detector. Adjacent inputs (short names) break. Narrow fix.

## Verdict

Sprint cannot be closed as shipped. US-19 fails its core promise (enforcement, not advice).

RESULT_sprint-auditor=FAIL
