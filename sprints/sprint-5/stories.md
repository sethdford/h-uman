# Sprint 5 Backlog

## Goal

Close the five deferred or emergent validator-chain hardening items from Sprints 3 and 4: two documentation/comment fixes (wave 1, parallel), one audit annotation (wave 1, parallel), and two implementation stories — retry-slim anti-CoT hardening and a fuzz harness for the chain executor (wave 2, parallel).

---

## Delivery waves

- **Wave 1 (all parallel-safe):** US-11, US-12, US-13 — comments, fixture fix, audit annotation.
- **Wave 2 (parallel-safe, after wave 1 green):** US-7, US-8 — implementation.

---

## User Stories (in priority order)

---

### US-12 (P1, XS): As a CI engineer, I want the check-test-references smoke test to reliably exit 1 on bad fixtures, so that the gate self-validates.

**Context:** `tests/fixtures/check-test-refs/bad.c` is excluded by the script's default `tests/test_*.c` path filter when run without arguments, so the negative-case smoke test silently exits 0 instead of 1.

**Acceptance criteria:**
- AC-12.1: Running `scripts/check-test-references.sh tests/fixtures/check-test-refs/bad.c` exits with code 1.
- AC-12.2: Running `scripts/check-test-references.sh tests/fixtures/check-test-refs/good.c` exits with code 0.
- AC-12.3: Neither invocation touches the git index or staged files.
- AC-12.4: Full test suite still passes post-change (`./build/human_tests`).

**Estimate:** XS
**Dependencies:** none
**DoD:** tests pass, /verify pass, /aspect-panel CLEAN, smoke commands above produce documented exit codes

---

### US-13 (P1, XS): As a contributor reading persona.h, I want the `outbound_chain` doc comment to accurately describe NULL semantics, so that I do not misinterpret NULL as "no validator rules configured."

**Context:** Line 452 of `include/human/persona.h` reads "NULL only when persona has zero validator rules or _load_json() failed before completion." NULL actually indicates chain build failure (e.g. OOM during chain allocation) — not merely the absence of configured rules. Zero rules results in an empty (non-NULL) chain, not NULL.

**Acceptance criteria:**
- AC-13.1: The comment on the `outbound_chain` field no longer states that zero validator rules implies NULL.
- AC-13.2: The updated comment explicitly states that NULL indicates chain build failure (e.g. OOM), not an empty rule set.
- AC-13.3: `grep -n "zero validator rules" include/human/persona.h` returns no matches.
- AC-13.4: Build compiles clean under `cmake --preset dev` with no warnings.

**Estimate:** XS
**Dependencies:** none
**DoD:** tests pass, build clean, comment verified by grep

---

### US-11 (P1, S): As a future maintainer reviewing telemetry coverage, I want each un-instrumented chain-execute call site in daemon.c annotated with a canonical comment, so that the gap is discoverable without re-auditing the file from scratch.

**Context:** Sprint 4 AC-5.2 was partially closed. Five `hu_output_validator_chain_execute` call sites in `src/daemon.c` (approximately lines 1077, 1738, 9301, 10659, 11699) lack observer telemetry emission because no `hu_observer_t *` is in scope at those sites. Option (b) was selected: annotate rather than plumb.

**Acceptance criteria:**
- AC-11.1: Each of the five call sites carries exactly the comment `// telemetry: observer not in scope (architectural limit)` on the line immediately preceding or following the `hu_output_validator_chain_execute` call.
- AC-11.2: `grep -c "telemetry: observer not in scope" src/daemon.c` outputs `5`.
- AC-11.3: `sprints/sprint-4/audit.md` contains a new paragraph (headed "AC-5.2 follow-up") naming all five line numbers and explaining why plumbing an observer to these sites is out of scope for this sprint.
- AC-11.4: Build compiles clean under `cmake --preset dev`.
- AC-11.5: Full test suite passes.

**Estimate:** S
**Dependencies:** none
**DoD:** grep count confirmed, audit.md updated, build clean, tests pass

---

### US-7 (P2, S): As a user whose message triggered a chain REJECT, I want the retry-slim prompt to suppress reasoning artifacts, so that the repaired reply contains only natural human-visible text.

**Context:** `src/agent/response_guard_retry.c` `dispatch_slim_chat` uses a `repair_instruction` string (line 45). That instruction tells the model to omit "analysis, XML, markdown fences, or filler" but does not explicitly forbid: (a) emitting inline reasoning prose ("Let me think…", "Step 1:"), (b) referring to the persona in third person ("As Aria, I…"), or (c) appending AI-helper closers ("Is there anything else I can help you with?"). These anti-CoT instructions are missing.

**Acceptance criteria:**
- AC-7.1: The `repair_instruction` static string in `dispatch_slim_chat` contains explicit prohibitions against: reasoning prose (e.g. "Let me think"), third-person persona self-reference (e.g. "As [name], I"), and AI-helper closers (e.g. "Is there anything else").
- AC-7.2: A unit test in `tests/test_response_guard_retry.c` (new or extended) calls `hu_response_guard_retry_slim` with a fixture where the chain previously REJECTed and asserts the outgoing chat request's system message contains the string "reasoning" (or the specific anti-CoT token chosen in AC-7.1).
- AC-7.3: The test uses `HU_IS_TEST` guards — no real network calls.
- AC-7.4: Build compiles clean under `cmake --preset dev`.
- AC-7.5: Full test suite passes with zero new failures.

**Estimate:** S
**Dependencies:** none
**DoD:** AC-7.1 verified by grep on repair_instruction, AC-7.2 test visible in suite output, /verify PASS

---

### US-8 (P2, S): As a security engineer, I want a libFuzzer harness for `hu_output_validator_chain_execute`, so that ownership-transition bugs and memory errors are caught before they reach production.

**Context:** `fuzz/` has 21 harnesses (see `fuzz/CLAUDE.md`). `hu_output_validator_chain_execute` handles arbitrary model output and performs ownership transitions; it has no harness. New harnesses must follow the pattern in `fuzz/fuzz_base64.c` (minimal, one entry point, returns 0), be added to `CMakeLists.txt`, and be listed in `fuzz/CLAUDE.md`.

**Acceptance criteria:**
- AC-8.1: `fuzz/fuzz_output_validator_chain.c` exists and follows the libFuzzer harness convention (`LLVMFuzzerTestOneInput`, returns 0, no `main`).
- AC-8.2: `cmake --preset fuzz && cmake --build --preset fuzz` completes without error and produces the `fuzz_output_validator_chain` binary.
- AC-8.3: Running `./build-fuzz/fuzz_output_validator_chain fuzz/corpus/output_validator_chain -max_total_time=30` with at least 5 seed inputs (empty, single byte, valid chain JSON, oversized input, null-containing input) exits without ASan or UBSan errors.
- AC-8.4: The harness is registered in `CMakeLists.txt` under the fuzz target list.
- AC-8.5: `fuzz/CLAUDE.md` harness table is updated with a row for `fuzz_output_validator_chain`.
- AC-8.6: Full standard test suite (`cmake --preset dev && ./build/human_tests`) still passes.

**Estimate:** S
**Dependencies:** none
**DoD:** binary builds under fuzz preset, seed run exits clean, CMakeLists and CLAUDE.md updated, /verify PASS

---

## Non-goals

- We will NOT plumb `hu_observer_t *` into the five un-instrumented daemon.c call sites (deferred to a future architectural refactor).
- We will NOT extend the fuzz harness corpus with a CI-driven continuous fuzzing job (separate infra story).
- We will NOT change the retry-slim fallback provider selection logic or cloud model constant.
- We will NOT rename or relocate `tests/fixtures/check-test-refs/` — only the invocation pattern changes.
- We will NOT add new validator rules or chain configuration options.

---

## Open questions for stakeholder

- US-7 AC-7.2: should the test assert the full anti-CoT string verbatim, or just that key tokens ("reasoning", "third person", "Is there anything else") appear? A verbatim assert is brittle if the wording is tuned later; a token check is more stable. Defaulting to key-token check unless told otherwise.
- US-11: are all five daemon.c line numbers (1077, 1738, 9301, 10659, 11699) still accurate on the current branch, or did earlier Sprint 5 commits shift them? The implementer must verify with `grep -n` before placing comments.

---

RESULT_product-owner=READY
