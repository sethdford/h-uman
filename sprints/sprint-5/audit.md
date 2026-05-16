# Sprint 5 Audit — Validator-Chain Final Follow-up

## Stories audited

| Story | ACs | Delivered | Partial | Inconclusive |
|---|---|---|---|---|
| US-7  | 5 | 5 | 0 | 0 |
| US-8  | 6 | 4 | 1 | 1 |
| US-11 | 5 | 5 | 0 | 0 |
| US-12 | 4 | 4 | 0 | 0 |
| US-13 | 4 | 4 | 0 | 0 |

## AC-by-AC findings

### US-12 — smoke-test wrapper
- AC-12.1/12.2: DELIVERED — `check-test-references.sh` exits 1 for `bad.c`, 0 for `good.c`.
- AC-12.3: DELIVERED — `git status` post-run untouched.
- AC-12.4: DELIVERED — `./build/human_tests` 10,326/10,326.

### US-13 — outbound_chain doc
- AC-13.1/13.2: DELIVERED — `include/human/persona.h:450-456` states NULL = "chain build failed during persona load (e.g., out-of-memory)" and "An empty rule set still produces a non-NULL chain".
- AC-13.3: DELIVERED — grep "zero validator rules" empty.
- AC-13.4: DELIVERED — build clean.

### US-11 — daemon annotations
- AC-11.1: DELIVERED — All 5 sites carry the exact comment at daemon.c 1077, 1739, 9304, 10664, 11706 (small drift from spec's 1077/1738/9301/10659/11699; caused by earlier Sprint 5 commits, acknowledged in critic.md).
- AC-11.2: DELIVERED — `grep -c` == 5.
- AC-11.3: DELIVERED — `sprints/sprint-4/audit.md:65-91` has "AC-5.2 follow-up" with all 5 lines + rationale.
- AC-11.4 / 11.5: DELIVERED — build clean, suite passing.

### US-7 — Anti-CoT slim prompt
- AC-7.1: DELIVERED — `src/agent/response_guard_retry.c:45-56` has 3 explicit prohibitions (reasoning prose, third-person, AI-helper closers).
- AC-7.2: DELIVERED — `slim_prompt_contains_anti_cot_instructions` (test:297-321) injects a capture-provider, calls `hu_response_guard_retry_slim`, asserts 3 verbatim phrase substrings. Critic HIGH was fixed in 827bdeeb by replacing weak token checks with phrase-level verbatim asserts.
- AC-7.3: DELIVERED — stub vtable; no network.
- AC-7.4 / 7.5: DELIVERED — suite 4/4; full 10,326/10,326.

### US-8 — Fuzz harness
- AC-8.1: DELIVERED — harness has `LLVMFuzzerTestOneInput`, returns 0, no `main`.
- AC-8.2: INCONCLUSIVE — Configures + compiles `.c.o` cleanly under `cmake --preset fuzz`; link fails locally with `libclang_rt.fuzzer_osx.a not found`. Reproduced on `fuzz_base64` too — Apple-clang toolchain gap, NOT a sprint defect. Linux CI is the supported fuzz host.
- AC-8.3: PARTIAL — (a) Critic MED on single-shot was fixed in 827bdeeb: harness now calls `_execute` twice per invocation on the same chain. (b) The mandated `fuzz/corpus/output_validator_chain/` directory does NOT exist; the 5 named seeds (empty / single byte / valid JSON / oversized / null-containing) are absent in-tree. A run against a missing dir still mutates but skips the canonical seeds.
- AC-8.4: DELIVERED — `CMakeLists.txt:3575-3579`.
- AC-8.5: DELIVERED — `fuzz/CLAUDE.md:46` row present.
- AC-8.6: DELIVERED — full suite passes.

## Scope creep
- `scripts/check-test-references.sh` gained an `EXPLICIT_FILES` mode (+12 LOC) to bypass the path filter for explicit invocations. Traceable to US-12 AC-12.1 — not hidden creep.

## DoD violations
- US-8 DoD: "seed run exits clean" — seed corpus dir missing; AC-8.3 seeds not committed. Recommend follow-up: commit 5 seed files under `fuzz/corpus/output_validator_chain/` before fuzz CI relies on them.

## Adversarial findings
- US-7 test mocks the **downstream provider**, not the unit under test — real `hu_response_guard_retry_slim` runs and the real `repair_instruction` string is captured. Legitimate.
- US-8 harness rebuilds chain per-input; cross-iteration ownership state not stressed. The in-invocation double-execute mitigates partially.
- Commit count `v-sprint-4-close..HEAD` == 6. Matches review.

## Verdict
22/24 ACs delivered, 1 PARTIAL, 1 INCONCLUSIVE — both isolated to US-8 corpus/host-toolchain infra, not behavioral defects. Sprint is shippable; commit the 5 seed inputs before fuzz CI depends on them.

RESULT_sprint-auditor=PASS_WITH_NOTES
