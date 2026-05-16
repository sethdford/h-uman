# Sprint 6 Retrospective — Relational Authenticity

**Status:** CLOSED. Original auditor: `FAIL`. Re-audit after fix: `PASS_WITH_NOTES`. The FAIL was load-bearing and caught a real dead-code bug — exactly the value the scrum ceremony pays for.

**Stories delivered:** US-14 + US-17 + US-19 (3 of 3).

**Commits (6):**
- `78ba8409` US-14 — wire voice maturity directive into LLM prompt
- `e8cfa146` US-14 test — pin build_directive output per stage
- `96f3506c` US-17 — emotional contagion (partner mood modulates Seth)
- `202a2cfd` US-19 — post-gen case/punctuation mirroring (DEAD CODE — caught by audit)
- `22a5d0e6` US-19 audit-FAIL fix — wire the function + tighten proper-noun rule
- (pending commit) — suite-name normalization for verifier compatibility

**Final test count:** 10,350/10,350 passing. 0 ASan errors. Three new humanness behaviors active in production path.

## What worked

- **Deep research surfaced the real gaps.** Before this sprint I thought the timing layer was a stub; turns out it's WIRED with statistical learning from chat.db. The actual gaps were structural (voice_maturity computed but never used), feedback-loop (no emotional contagion), and enforcement-vs-advisory (style mirroring was LLM instructions, not post-gen pass). Research before planning saved a wasted sprint.
- **Auditor caught load-bearing dead code.** US-19's `hu_style_mirror_apply` was defined, unit-tested, header-included in agent_turn.c — and never called. The implementer's "tests pass" and the verifier's "function exists" both green-lit it. Only the auditor's adversarial re-read of "is the AC ACTUALLY delivered" caught the gap. This is now the third sprint where auditor-caught-something-everyone-else-missed; the role is paying for itself every time.
- **Three commits to fix one FAIL.** Tightly scoped audit-fix landed in one commit; everything else stayed clean. Per the scrum hard rule "audit FAIL re-opens stories," the workflow worked exactly as designed.

## What broke

- **Implementer wired-and-walked-away on US-19.** Built the function, wrote unit tests, included the header — but never called it from agent code. The new hookify rule (Sprint 4 US-10) checks production-reference in tests, but doesn't check call-site-presence for production functions. Worth a future hookify rule: any new production function in `src/persona/` or `src/agent/` should have ≥1 call site in `src/agent/` after merge.
- **Suite-name TitleCase recurrence.** Sprint 5 verifier caught this for `response_guard_retry`; here it recurred for 3 new suites (`StyleMirror`, `EmotionalContagion`, `VoiceMaturityDirective`). The implementer-prompt template should explicitly say "snake_case suite names matching the convention of existing suites." Worth a hookify rule too: any new `HU_TEST_SUITE(...)` string must be snake_case.
- **Original audit's "dead code" pattern.** The fact that this passes static analysis + unit tests + ASan but is functionally invisible at runtime — exactly the "test inlines production code" anti-pattern from Sprint 3, flipped: now it's "production code never gets called by tests OR by agent." The Sprint 3 hookify rule guards against tests inlining; we need its mirror: an audit-time rule that flags `src/` functions with no `src/agent/` call site as suspect.

## What changed mid-sprint

- Style mirror's proper-noun rule changed from "length heuristic (1-3 chars)" to "explicit COMMON_STARTERS allowlist." This was an auditor-flagged improvement, not a critic finding — auditor noticed the heuristic would silently lowercase "Al"/"Mo"/"Jo"/"Ben"/"Sam". Real correctness issue caught by adversarial reading.
- 3 new suites renamed TitleCase → snake_case at sprint close to match codebase convention.

## What's next (Sprint 7 candidate backlog — research-surfaced)

Highest-impact gaps from the deep research still open:
- **US-20: Per-contact personal model split** (`hu_personal_model_t` is unified). M.
- **US-21: Read-state detection on outbound messages** (can mark read, can't detect if THEY read). M.
- **US-22: Calendar-aware inbound pacing** (calendar exists but only feeds proactives). S.
- **US-23: Genuine boundaries → actual non-response** (currently just instructs LLM). M.
- **US-24: Relationship mode (DEEPENING/DRIFTING/REPAIR) in LLM prompt** (computed by governor but not surfaced). S.
- **US-25: In-joke / nickname lexicon per contact** (form-of-address adaptation is missing). L.
- **US-26: Hookify rule — new src/ function must have ≥1 src/agent/ call site** (catch dead code at PR time). XS.

## Agent tuning candidates

- **General-purpose implementer agent** — the wire-and-walk-away pattern on US-19 is the SAME shape as Sprint 4's "wave-2 implementer cut off before US-5 emission wiring." Pattern: implementer builds the API surface, writes unit tests, considers it done. Misses the integration step. Worth `/tune-agent` proposal: "When implementing a new function, verify ≥1 call site exists in production code before reporting DONE. Unit tests + header inclusion is not enough."
