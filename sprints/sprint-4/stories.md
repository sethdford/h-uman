# Sprint 4 Backlog — Validator Chain Hardening Follow-up

## Goal

Eliminate per-message validator chain allocations, add observable telemetry for REJECT/REWRITE decisions, close the auditor-flagged Site A production-path gap, resolve the Sprint 3 DoD literalism note, and hookify the "test inlines production code" anti-pattern so it cannot recur silently.

## Scoping Decision

**Option (b) accepted: US-4, US-5, US-6, US-9, US-10.**

US-7 (retry-slim prompt) and US-8 (fuzz harness) deferred to Sprint 5.

Rationale: Sprint 3 hit the implementer tool-use ceiling at 3 stories (M+M+S). Sprint 4 carries two M and one S as the execution load; US-9 and US-10 are XS and can be parallelized with the S wave without adding meaningful dispatch cost. US-7 and US-8 are P2 with no dependencies on Sprint 4 work — deferring them creates no downstream blockers.

---

## User Stories (in priority order)

---

### US-6 (P1): As a validator-chain maintainer, I want an end-to-end daemon integration test that injects a known-bad model response and asserts the transport layer receives REJECT/cleaned content, so that the Site A production path in `daemon.c` is covered by tests that would actually fail if the chain call were deleted.

**Acceptance criteria:**
- AC-6.1: A test in `tests/test_daemon_e2e_validator.c` (or a new suite file following existing fuzz/mock-provider conventions) spawns a daemon-like state under the test harness using a mock provider that returns `JORDAN_LEAK_F1` verbatim as the model response; it does NOT inline-reconstruct the validator chain.
- AC-6.2: The mock iMessage transport captures the outbound content; the test asserts the captured content does NOT contain the `JORDAN_LEAK_F1` payload (i.e., content was either rejected or stripped before reaching the transport).
- AC-6.3: Deleting or commenting out the `hu_output_validator_chain_execute` call at `daemon.c:2095-2122` causes AC-6.2 to fail — confirmed by the test author running the suite with that line removed and observing the failure, with the result documented in the PR description.
- AC-6.4: `./build/human_tests --suite=daemon_e2e_validator` (or equivalent suite name) passes 0 failures on the dev preset (ASan on). Full suite remains 0 failures.

**Estimate:** M
**Dependencies:** US-1 (Sprint 3, done), US-2 (Sprint 3, done)
**DoD:** AC-6.3 deletion-check documented in PR. Full suite passes. ASan clean. `/verify` returns PASS.

---

### US-4 (P1): As a developer integrating h-uman's agent loop, I want the validator chain cached on `hu_persona_t` at persona load time, so that per-message heap allocations for chain construction are eliminated and the allocator is not invoked on every agent turn.

**Acceptance criteria:**
- AC-4.1: `hu_persona_t` (in `include/human/persona.h`) has a field `hu_output_validator_chain_t *outbound_chain`; it is NULL before persona load and non-NULL after a successful load for any persona with at least one validator rule.
- AC-4.2: `hu_persona_load` (or its designated init path) populates `outbound_chain` exactly once per persona lifetime; a test asserts the pointer is stable across two consecutive calls that would previously have triggered chain construction.
- AC-4.3: All call sites in the five daemon paths plus `agent_turn.c` and `agent_stream.c` that previously constructed the chain inline are migrated to read `persona->outbound_chain`; `grep -rn "hu_validators_build_default_outbound_chain" src/` returns 0 hits outside `src/persona/`.
- AC-4.4: `hu_persona_unload` (or equivalent teardown) calls `hu_output_validator_chain_destroy` on `outbound_chain`; a test exercises the full load-use-unload cycle with ASan enabled and reports 0 leaks.
- AC-4.5: `./build/human_tests` passes 0 failures on dev preset after migration. No regression in `output_validator`, `validators_builtin`, `validators_persona_safety` suites.

**Estimate:** M
**Dependencies:** US-1 (Sprint 3, done)
**DoD:** AC-4.3 grep-check passes. Full suite passes. ASan clean. `/verify` returns PASS.

---

### US-5 (P1): As an operator monitoring h-uman in production, I want structured telemetry events emitted whenever the validator chain issues a REJECT or REWRITE decision, so that I can observe the rate and context of safety interventions without grepping logs.

**Acceptance criteria:**
- AC-5.1: A new observer event type (e.g., `HU_OBS_VALIDATOR_DECISION`) is declared in `include/human/observer.h` with payload fields: `validator_name` (const char *), `decision` (enum: PASS/REJECT/REWRITE), `channel_id` (const char *), `persona_name` (const char *), `response_len` (size_t), `bytes_stripped` (size_t, 0 on REJECT).
- AC-5.2: `hu_output_validator_chain_execute` emits this event via `hu_observer_notify` for every REJECT or REWRITE outcome; PASS outcomes are not emitted (to avoid per-token noise).
- AC-5.3: A test registers a capturing observer, runs the chain against a known-REJECT input, and asserts the captured event has `decision == REJECT`, correct `validator_name`, and `response_len` matching the rejected string's length.
- AC-5.4: A test for REWRITE asserts `bytes_stripped > 0` and `decision == REWRITE`.
- AC-5.5: When no observer is registered, `hu_output_validator_chain_execute` completes without crash or memory error on both REJECT and REWRITE paths.

**Estimate:** S
**Dependencies:** none (observer.h and chain API are both stable post-Sprint-3)
**DoD:** Full suite passes. ASan clean. `/verify` returns PASS.

---

### US-9 (P0-admin): As the sprint auditor, I want the Sprint 3 Pattern C DoD literalism resolved with an explicit written disposition, so that the open audit note is closed and the DoD accurately describes what the code does.

**Acceptance criteria:**
- AC-9.1: The two remaining `hu_conversation_strip_channel_tags` calls in `daemon.c` (lines 2127 and 2137 as of Sprint 3 close) are either (a) removed with the fallback replaced by a no-op safe default, OR (b) explicitly documented in `src/daemon.c` with a comment block explaining they are intentional defense-in-depth fallbacks that fire only when chain construction fails; one of the two options is chosen and implemented.
- AC-9.2: The Sprint 3 `stories.md` DoD line for US-2 is updated to reflect the actual post-implementation invariant: "the primary AGENT_STREAM_TEXT path uses `hu_output_validator_chain_execute`; legacy strip calls survive only in chain-build-failure fallback arms" — or the equivalent text if option (a) was chosen.
- AC-9.3: `./build/human_tests --suite=pattern_c_paths` passes 5/5 after the change.

**Estimate:** XS
**Dependencies:** US-2 (Sprint 3, done)
**DoD:** AC-9.1 code change committed. AC-9.2 DoD text updated. Suite passes.

---

### US-10 (P1-process): As a future implementer writing tests for h-uman, I want a pre-commit hook or CI check that flags test files which do not reference at least one symbol from the production source file they claim to cover, so that the "test inlines production code" anti-pattern that recurred twice in Sprint 3 is caught before merge rather than at audit time.

**Acceptance criteria:**
- AC-10.1: A hookify rule file is added at `.claude/rules/test-references-production-symbol.md` that specifies: any new file matching `tests/test_*.c` must contain at least one `grep`-detectable reference to a non-`static` function or macro from the production `.c` file it names (e.g., `tests/test_daemon_e2e_validator.c` must reference `hu_output_validator_chain_execute` or another symbol exported from `daemon.c`).
- AC-10.2: A pre-commit check script at `scripts/check-test-references.sh` is added; it accepts a list of staged `tests/test_*.c` files, extracts the implied production module name, and exits non-zero if no production symbol from that module appears in the test file. The script's own `--help` output describes the rule.
- AC-10.3: The script is invoked from `.githooks/pre-commit` (or the project's existing hook entry point) so it runs on every `git commit`.
- AC-10.4: Running the script against `tests/test_pattern_c_paths.c` (known-good) exits 0. Running it against a synthetic test file that contains no production symbol exits 1 with a human-readable error message identifying the missing reference.

**Estimate:** XS
**Dependencies:** none
**DoD:** AC-10.2 script executable, AC-10.3 wired into pre-commit, AC-10.4 spot-checked manually and result documented in PR. No test suite regression.

---

## Non-goals

- We will NOT implement the retry-slim prompt fix (US-7) — deferred to Sprint 5 as P2.
- We will NOT add the fuzz harness for `hu_output_validator_chain_execute` (US-8) — deferred to Sprint 5 as P2.
- We will NOT touch `agent_stream.c:1317` structured-output gap (auditor finding #2 from Sprint 3) — minor gap, not blocking safety intent, backlog for Sprint 5.
- We will NOT address clang-tidy diagnostic noise on pre-existing code — separate hygiene task, not a validator concern.
- We will NOT add language bindings or public SDK documentation for the observer event type added in US-5.

---

## Dispatch guidance for scrum-master

Wave 1 (parallel): US-9 + US-10. Both are XS with no code dependencies on wave 2. Land them first to clear the audit note and hook before implementers begin wave 2.

Wave 2 (parallel): US-5. S-sized, no dependency on US-4 or US-6.

Wave 3 (sequential): US-4 then US-6. US-6's E2E test exercises the cached chain (US-4), so US-4 should land and be verified before US-6 dispatches. If US-4 runs long, US-6 can begin against the pre-cache chain and be rebased; flag this to lead if it happens.

Tool-use budget: US-4 is the heaviest story (7 call sites to migrate). Dispatch it as a single-concern implementer. Do not batch US-4 with US-6 in one agent.

---

## Open questions for stakeholder

- US-9 option choice: does Seth prefer (a) remove the two fallback `strip_channel_tags` calls entirely (slightly smaller blast radius, loses defense-in-depth on chain-build failure) or (b) keep them with explicit comment documentation? Both satisfy the AC. Recommend (b) — defense-in-depth is worth the two lines — but this is a judgment call.
- US-5 PASS emission: the current AC suppresses PASS events to avoid per-token noise. If production monitoring needs a throughput baseline (e.g., "N messages validated per minute"), a sampled PASS emission may be desirable. Confirm suppression is correct before implementation begins.

Last line: RESULT_product-owner=READY
