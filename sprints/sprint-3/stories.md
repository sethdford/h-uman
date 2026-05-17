# Sprint 3 — Validator Chain Hardening

## Sprint Goal

Close the three confirmed gaps that leave the PR #81 validator chain incomplete in production: wire the dead structured-output layer, cover the two legacy-stripper escape paths (daemon bus broadcast + format.c channel path), and add telemetry + an end-to-end integration test so future regressions are detected before they ship. Allocation overhead and retry-slim prompt alignment are addressed as secondary concerns.

---

## Stories

### US-1: Wire structured-output (JSON schema) per persona opt-in

**Priority:** P0
**Estimate:** M
**Depends on:** none

**As** Seth
**I want** `hu_validators_build_default_outbound_chain` (or its callers in `agent_turn.c` / `agent_stream.c`) to set `req.response_format = "json_schema"` and `req.response_schema` when a persona opts into structured output
**So that** the Gemini `responseSchema` layer (Layer 1 of the three-layer defense) is actually active in production, not dead code

**Acceptance criteria**
- AC1: A new field `bool structured_output_enabled` (or equivalent) is readable from `hu_persona_t`; at minimum one test persona fixture has it set to true.
- AC2: When `structured_output_enabled` is true, the outbound `hu_chat_request_t` constructed in `agent_turn.c` has `response_format == "json_schema"` and a non-NULL `response_schema` before the provider `chat` vtable is called; confirmed by a unit test that captures the request struct.
- AC3: When `structured_output_enabled` is false (or persona is NULL), `response_format` remains NULL; the existing test suite passes without regression.
- AC4: `tests/test_structured_output.c` contains at least one test exercising the wiring path end-to-end through a mock provider that asserts `response_format`.

**Definition of Done**
- `cmake --preset dev && cmake --build --preset dev` exits 0 with no `-Werror` violations.
- `./build/human_tests --suite=structured_output` passes (0 failures).
- Full suite (`./build/human_tests`) passes.

---

### US-2: Cover Pattern C escape paths — daemon bus broadcast and format.c channel path

**Priority:** P0
**Estimate:** M
**Depends on:** none

**As** Seth
**I want** the daemon bus broadcast path (`src/daemon.c` ~line 2089, `HU_AGENT_STREAM_TEXT` branch) and the `format.c` channel-format path (~line 585) to run output through `hu_output_validator_chain_execute` instead of calling legacy strippers directly
**So that** F2-shaped leaks (assistant-closer tokens, persona-narrator frames) cannot escape via these two paths even if `agent_turn.c` is bypassed

**Acceptance criteria**
- AC1: `src/daemon.c` `HU_AGENT_STREAM_TEXT` branch calls `hu_output_validator_chain_execute`; no direct call to `hu_conversation_strip_channel_tags` or legacy strippers remains in that branch.
- AC2: `src/channels/format.c` channel path calls `hu_output_validator_chain_execute` (or a helper that wraps it) instead of bare `hu_channel_strip_markdown` + `hu_channel_strip_ai_phrases` sequence.
- AC3: A test in `tests/test_validators_builtin.c` (or a new `test_pattern_c_paths.c`) injects a string containing an assistant-closer token into each converted path and asserts the returned text does not contain the token.
- AC4: Full suite passes; no ASan errors.

**Definition of Done**
- The primary `HU_AGENT_STREAM_TEXT` path uses `hu_output_validator_chain_execute`; legacy strip calls (`hu_conversation_strip_channel_tags`) survive only in chain-build-failure and no-allocator fallback arms, which are explicitly documented in source (Sprint 4 US-9 annotation, see `sprints/sprint-4/notes-from-sprint-3.md`).
- `./build/human_tests` passes (0 failures, 0 ASan errors reported in build log).

---

### US-3: Remove or wire the dead `channel` parameter in `hu_stop_sequence_registry_lookup`

**Priority:** P0
**Estimate:** XS
**Depends on:** none

**As** Seth
**I want** the `channel` + `channel_len` parameters of `hu_stop_sequence_registry_lookup` to either be removed from the signature or wired to per-channel stop-sequence lookup logic
**So that** the API surface is not a lie — callers cannot rely on per-channel differentiation that is silently discarded

**Acceptance criteria**
- AC1: If removed: the public header `include/human/agent/stop_sequence_registry.h` no longer declares `channel`/`channel_len` parameters, all callers compile cleanly, and a comment documents why channel differentiation was deferred.
- AC2: If wired: at least one test in `tests/test_stop_sequences.c` passes distinct channel values and asserts distinct stop-sequence lists are returned.
- AC3: No `(void)channel;` / `(void)channel_len;` suppression remains in `src/agent/stop_sequence_registry.c`.
- AC4: Full suite passes.

**Definition of Done**
- `grep "(void)channel" src/agent/stop_sequence_registry.c` returns no output.
- `./build/human_tests --suite=stop_sequences` passes.

---

### US-4: Cache the validator chain on `hu_persona_t` (eliminate per-message allocation)

**Priority:** P1
**Estimate:** M
**Depends on:** US-1 (structured-output opt-in field sits on persona; chain composition may depend on it)

**As** Seth
**I want** `hu_validators_build_default_outbound_chain` to be called once when a persona is loaded and the resulting chain stored on `hu_persona_t` (or an equivalent long-lived owner)
**So that** burst sends do not allocate and destroy 8 validators + chain struct + entries array per message

**Acceptance criteria**
- AC1: `hu_persona_t` (or its extended variant) has a `hu_output_validator_chain_t *outbound_chain` field that is populated on `hu_persona_load` / `hu_persona_create` and destroyed on `hu_persona_destroy`.
- AC2: All five daemon call sites (`daemon.c` lines ~1071, ~1733, ~9239, ~10596, ~11636) use `persona->outbound_chain` instead of calling `hu_validators_build_default_outbound_chain` inline.
- AC3: A test verifies that the chain pointer is non-NULL after persona load and NULL after persona destroy (no double-free, no leak detected by ASan).
- AC4: Full suite passes.

**Definition of Done**
- `grep "hu_validators_build_default_outbound_chain" src/daemon.c` returns zero hits.
- ASan build (`cmake --preset dev`) shows no leak or double-free in test run.

---

### US-5: Add telemetry hook for validator chain REJECT events

**Priority:** P1
**Estimate:** S
**Depends on:** none

**As** Seth
**I want** every `HU_VALIDATOR_REJECT` result from `hu_output_validator_chain_execute` to emit a structured log entry that includes the deciding validator name, the channel, and a truncated (first 120 chars) copy of the rejected text
**So that** I can measure F1/F2/F3 firing rates in production and distinguish which failure mode is occurring

**Acceptance criteria**
- AC1: A new helper `hu_validator_chain_log_reject` (or inline in each call site) emits a `HU_LOG_WARN` entry with fields: `deciding_validator`, `channel`, `text_preview` (truncated to 120 chars, no newlines).
- AC2: The log entry is emitted at all five daemon REJECT branches and the `agent_turn.c` REJECT branch.
- AC3: A test asserts that after a chain REJECT, the log buffer (captured via a test observer or mock logger) contains `deciding_validator_name` and a substring of the rejected text.
- AC4: No secret / PII leaks: the `text_preview` is truncated and the test uses synthetic non-PII text.

**Definition of Done**
- `./build/human_tests --suite=output_validator` passes, including the new log-capture test.
- Code review confirms truncation logic caps at 120 chars before any format call.

---

### US-6: End-to-end integration test — inject Jordan leak, assert wire content is clean

**Priority:** P1
**Estimate:** M
**Depends on:** US-2 (Pattern C paths must be covered before E2E is meaningful)

**As** Seth
**I want** a single integration test in `tests/test_validator_chain_e2e.c` that instantiates the daemon's send path with a mock provider returning a Jordan-style F2 leak (assistant-closer string), runs it through the full chain, and asserts the string that would reach the wire does not contain the leak token
**So that** any future refactor that breaks the chain for the Jordan persona is caught before merge, not after

**Acceptance criteria**
- AC1: `tests/test_validator_chain_e2e.c` exists, is compiled into `human_tests`, and contains at least three test cases: one for F1 (prose-CoT), one for F2 (assistant-closer), one for F3 (role-collapse).
- AC2: Each test case: (a) configures a Jordan-equivalent persona, (b) passes a synthetic model response containing the leak token through `hu_output_validator_chain_execute`, (c) asserts `final_decision == HU_VALIDATOR_REJECT` or the output text does not contain the leak token.
- AC3: Tests use `HU_IS_TEST` guard; no real network calls; deterministic.
- AC4: `./build/human_tests --suite=validator_chain_e2e` passes.

**Definition of Done**
- `./build/human_tests --filter=validator_chain_e2e` reports 3+ tests, 0 failures.
- Full suite green.

---

### US-7: Align retry-slim prompt with validator-chain REJECT reason

**Priority:** P2
**Estimate:** S
**Depends on:** US-5 (telemetry provides the deciding_validator_name; US-6 confirms retry behavior is observable)

**As** Seth
**I want** `hu_response_guard_retry_slim`'s regeneration prompt to include the `deciding_validator_name` when called after a validator-chain REJECT (not a guard REJECT)
**So that** the model is told which constraint it violated and is less likely to regenerate the same CoT leak

**Acceptance criteria**
- AC1: `hu_response_guard_retry_slim` accepts (or derives from context) a `const char *reject_reason` parameter; when non-NULL, the slim prompt appends a sentence of the form: "The previous response was rejected because: <reject_reason>. Do not repeat that pattern."
- AC2: The existing guard-REJECT call site passes NULL for `reject_reason` (no behavior change for guard path).
- AC3: The validator-chain REJECT call site in `agent_turn.c` passes `chain_result.deciding_validator_name`.
- AC4: A test asserts that when `reject_reason` is non-NULL, the prompt string passed to the provider contains the reason substring.

**Definition of Done**
- `./build/human_tests --suite=response_guard` passes including new prompt-content test.
- No regression in existing retry tests.

---

### US-8: Add fuzz harness for `hu_output_validator_chain_execute`

**Priority:** P2
**Estimate:** S
**Depends on:** none

**As** Seth
**I want** a libFuzzer harness `fuzz/fuzz_output_validator_chain.c` that feeds arbitrary byte sequences as the `text` input to `hu_output_validator_chain_execute` with the default outbound chain
**So that** ownership transitions across REWRITE/REJECT — the exact memory-safety surface the chain introduces — are exercised by automated fuzzing in CI

**Acceptance criteria**
- AC1: `fuzz/fuzz_output_validator_chain.c` exists, compiles under `cmake --preset fuzz`, and links with `HU_IS_TEST=1`.
- AC2: The harness runs 10,000 iterations without ASan/UBSan crash under `cmake --preset fuzz` locally (evidence: run log showing iteration count and "Done" exit).
- AC3: `CMakeLists.txt` includes the new harness in the fuzz targets list.
- AC4: Zero UBSan errors at any input boundary (null, zero-length, 1-byte, max-size).

**Definition of Done**
- `cmake --preset fuzz && cmake --build --preset fuzz` exits 0.
- Run log from `./build/fuzz_output_validator_chain -runs=10000` shows no crash / ASan finding.

---

## Non-goals

- We will NOT implement Anthropic tool-use proxy or OpenAI json_schema structured output (SOTA stretch; Gemini wiring in US-1 is the only provider targeted this sprint).
- We will NOT drop or audit vestigial Claude-2 stop sequences (`"\n\nHuman:"`); that is a separate cleanup with no safety impact.
- We will NOT address the gateway architectural limit (gateway path has no persona context; flagged as a known architectural constraint to document, not fix, this sprint).
- We will NOT change the public `hu_output_validator_t` vtable interface; all stories work within the existing vtable contract.
- We will NOT migrate the remaining daemon call sites that already use the chain (`~1071`, `~1733`, etc.) — only the two uncovered Pattern C paths (US-2) are in scope.

---

## Open questions for stakeholder

- US-3 (stop-sequence `channel` param): prefer remove-and-document or wire? Wiring adds ~S of work; removing is XS but is a public API break if any external consumer exists. Clarify before US-3 is picked up.
- US-4 (chain caching): `hu_persona_t` is the proposed owner, but if personas are shared across threads the chain must be either read-only (all validators are stateless) or protected. Confirm validators are stateless before implementation begins — the plan doc states this but the implementer should re-verify with `grep -n "ctx" src/agent/validators/*.c`.
