# Sprint 5 Retrospective — Validator Chain Hardening Final Followup

**Status:** CLOSED. Sprint-auditor: `PASS_WITH_NOTES`. 22 of 24 ACs DELIVERED; 1 PARTIAL (US-8 corpus seeds); 1 INCONCLUSIVE (US-8 host toolchain can't link libfuzzer).

**Stories delivered:** US-7, US-8, US-11, US-12, US-13 (5 of 5).

**Commits (6):**
- `7df97a91` US-12 — smoke-test wrapper for check-test-references negative case
- `59586f06` US-13 — outbound_chain doc semantics fix
- `1db73992` US-11 — annotate 5 daemon.c chain-execute sites without telemetry
- `eb6ce987` US-7 — anti-CoT instructions in retry-slim prompt
- `90997b13` US-8 — fuzz harness for chain execute
- `827bdeeb` critic+verifier fixes — verbatim assertions, suite-name rename, double-execute fuzz

**Final test count:** 10,326/10,326 passing. ASan clean. Anti-CoT suite 4/4 under correct filter.

## What worked

- **Five-story sprint with one implementer dispatch.** Wave-1 (US-12/13/11) and wave-2 (US-7/US-8) executed in a single subagent invocation. The XS+S sizing was honest enough that no descope was needed.
- **Critic + verifier surfaced complementary findings.** Verifier caught the suite-name mismatch (filter command produced 0/0 passed). Critic caught that even when the filter worked, the assertions were loose-substring. Both fixed in one follow-up commit.
- **Auditor flagged the fuzz INCONCLUSIVE up front.** Host toolchain (`libclang_rt.fuzzer_osx.a` missing) can't link libfuzzer harnesses — affects all 22 fuzz targets equally, not just ours. Auditor classified it as INCONCLUSIVE rather than FAIL — correct call.
- **The full sprint-stack pattern is now repeatable.** PR #81 → #82 → #84 → (Sprint 5 PR), each based on the prior sprint's close tag. Branch tagging discipline kept the audit trail clean.

## What broke

- **Subagent truncation continued.** Verifier, critic, and auditor all got cut off mid-investigation and required a follow-up `SendMessage` to deliver their reports. Pattern is now established: complex multi-file investigation tasks hit tool-use ceilings around 24-46 tool calls. Worth adding to `/tune-agent` candidates.
- **AC verifier-command literalism.** US-7's AC said `--suite=response_guard_retry` but the implementer registered the suite with title-case spaces. Same anti-pattern as Sprint 3's DoD literalism — the contract said one thing, the code did another, and only the verifier caught the mismatch. Fix landed but worth adding a hookify rule: any new HU_TEST_SUITE must match the snake_case naming of existing suites.
- **US-8 fuzz harness compiles but can't run.** Host toolchain limit. The harness is structurally correct (verified by `clang -c` succeeding), but a real fuzz run would need a Linux Clang with libfuzzer runtime. Acceptable for this sprint; document in retro as a CI gap.

## What changed mid-sprint

- US-7 test assertions tightened from generic substrings to verbatim prohibitions (critic HIGH).
- US-8 fuzz harness changed from single-shot to double-execute on same chain (critic MED — ownership transitions only exercised within one invocation).
- Suite registration renamed `"Response Guard Retry"` → `"response_guard_retry"` (verifier-AC alignment).

## What's next

Sprint 5 was scoped as final cleanup. The validator-chain hardening initiative is now complete from a backlog perspective.

Outstanding for future sprints (not blocking ship):
- **US-8 corpus seeds (PARTIAL):** auditor noted no `fuzz/corpus/output_validator_chain/` seed inputs were committed. Add a starter set for CI fuzz job.
- **US-8 toolchain fix (INCONCLUSIVE):** install libfuzzer runtime on the build host or document the OS-specific build path.
- **Critic LOW findings:** hardcoded "Seth" persona in fuzz, line-shift documentation in Sprint 4 audit. Cosmetic.
- **Sprint 4 retry-slim deeper investigation:** the new anti-CoT instructions are forward-looking. Whether they actually prevent CoT regeneration in production needs observation in real traffic (US-5 telemetry from Sprint 4 will measure this).

## Agent tuning candidates

**General-purpose agent** — repeated truncation pattern across Sprint 3, Sprint 4, and Sprint 5 verifier+critic+auditor dispatches. Recommend `/tune-agent` proposal:
- "If you're approaching the tool-use ceiling, STOP reading and WRITE your findings file now. A partial report file is better than no file. The controller can SendMessage you to fill gaps; they cannot magically retrieve unwritten work."

This is the third sprint exhibiting the pattern. Two failures of the same shape qualify per the global rule.

## Sprint 5 itself

Smallest sprint in the initiative (1.5 days of work across 5 stories). Most efficient ceremony: 1 PO + 1 implementer + 1 finisher + 1 verifier + 1 critic + 1 auditor = 6 agent dispatches for 5 stories. Sprint 3 by contrast was 10+ dispatches for 3 stories. The XS-heavy mix paid off.
