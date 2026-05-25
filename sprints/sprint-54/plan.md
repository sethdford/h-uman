---
sprint: 54
title: Sprint 54 Wave Plan — Tier 1/2/3 Cleanup + MLX Streaming
date: 2026-05-25
status: PLAN_READY
branch: sprint-54-tier-1-2-3-cleanup
working_directory: /Users/sethford/Projects/human-sprint-54
---

# Sprint 54 Wave Plan

## Sprint Metadata

| Field | Value |
|-------|-------|
| **Sprint** | 54 |
| **Title** | Tier 1/2/3 Cleanup: Provider Foundation + MLX Streaming |
| **Branch** | `sprint-54-tier-1-2-3-cleanup` (worktree-isolated) |
| **Working Directory** | `/Users/sethford/Projects/human-sprint-54` |
| **Stories File** | `sprints/sprint-54/stories.md` |
| **Designs** | 6 × DESIGN_READY (all stories have design review approval) |
| **DoD Owner** | Scrum Master (will enforce /verify + /aspect-panel for security-sensitive stories) |

## Sequencing & Dependency Graph

```
Wave 1 (Warmup, parallel-safe):
  ├─ US-CLEAN-1 [docs/plans/ only]

Wave 2 (Parallel, independent paths):
  ├─ US-C3.3    [src/doctor/check_provider.c + registry]
  └─ US-M3-B4   [src/providers/mlx.c only]
  
Wave 3 (Sequential, serialized on src/doctor.c):
  ├─ US-C3.9    [exit codes]
  └─ US-C3.7    [JSON output, depends on C3.9 landing first]
  
Wave 4 (Depends on Wave 2):
  └─ US-C2.3    [reuses US-C3.3 smoke check]
```

## Hard Constraints & Conflict Prevention

### 1. CMakeLists.txt + tests/test_main.c collision
- **Chokepoint:** These files are line-number-sensitive. Two agents editing simultaneously produce merge conflicts + hallucinated entries.
- **Prevention:** 
  - Wave 2 agents (US-C3.3 + US-M3-B4) may run in parallel, BUT **first agent to complete merges to sprint-54 branch, second agent rebases before their merge.**
  - Scrum Master will orchestrate: after US-C3.3 reports DONE, merge it immediately, then signal US-M3-B4 to rebase sprint-54 before merge.

### 2. src/doctor.c serialization
- **Chokepoint:** US-C3.9 and US-C3.7 both modify `src/doctor.c` (exit-code logic + JSON output respectively).
- **Prevention:** Wave 3 is purely sequential: C3.9 completes + merges → C3.7 starts after merge.

### 3. US-C2.3 depends on US-C3.3
- **Constraint:** US-C2.3 reuses the smoke-check function exported from US-C3.3's header.
- **Prevention:** US-C3.3 must merge to sprint-54 before US-C2.3 starts. Scrum Master will gate Wave 4 on this.

---

## Wave-by-Wave Dispatch Plan

### Wave 1: Warmup (Parallel, single story)

#### US-CLEAN-1 — Normalize plan-directory frontmatter
- **Implementer:** `general-purpose`
- **Isolation:** Branch sufficient (docs-only, no code)
- **Scope:** 26 files under `docs/plans/` receive consistent `status:` frontmatter
- **Duration:** ~1–2 hours (pure mechanical edits)
- **Merge Strategy:** Single commit to sprint-54 branch
- **Risk:** None (documentation-only)
- **Test Contract:** 
  - [ ] All 26 files have a canonical `status:` field in frontmatter
  - [ ] No content or comments altered; frontmatter changes only
  - [ ] Single commit titled "docs(plans): normalize 26 UNCERTAIN entries per STATUS.md schema"
  - [ ] `git log sprint-54 ^origin/main` shows one clean commit
- **Quality Gates:**
  - [ ] Manual review of diff: `git diff origin/main sprints/sprint-54/plan.md` + sample of modified files confirms intent
  - [ ] No TEST_FAILURE_evidence (this is docs, no test suite to run)

---

### Wave 2: Parallel Foundation (Two independent paths, coordinated merge)

#### US-C3.3 — Provider smoke-check doctor implementation
- **Implementer:** `general-purpose`
- **Isolation:** Worktree (tight integration with `src/doctor/` + CMakeLists.txt + tests)
- **Scope:** 
  - New `src/doctor/check_provider.c` (doctor check implementation)
  - New `tests/test_doctor_check_provider.c` (unit tests)
  - Register check in `src/doctor/registry.c`
  - Update `CMakeLists.txt` (register test source)
- **Duration:** ~3–4 hours (M estimate, credential handling complexity)
- **Test Contract:**
  - [ ] New doctor check passes PASS verdict when provider is reachable + credentials valid
  - [ ] Returns FAIL with one of 5 diagnostic modes: not_configured | credentials_missing | credentials_invalid_401 | rate_limited_429 | unreachable
  - [ ] Network I/O gated behind `#ifdef HU_IS_TEST` (no hanging tests in CI)
  - [ ] Mock provider with failure-injection: 6 test cases covering PASS + 5 FAIL modes + timeout
  - [ ] `./build/human_tests` full suite passes
  - [ ] No ASan errors
- **Quality Gates:**
  - [ ] `/verify` run on this story returns `RESULT_verifier=PASS`
  - [ ] `/aspect-panel` run on security-sensitive credential handling: returns CLEAN or PASS (no ESCALATE)
  - [ ] Critic review: no HIGH/CRITICAL findings
  - [ ] Log inspection: no raw API keys or credentials in stderr/logs
- **Merge Timing:** Merge immediately after DONE verification, before US-M3-B4 merge

---

#### US-M3-B4 — MLX streaming wire
- **Implementer:** `general-purpose`
- **Isolation:** Worktree (platform-gated code, affects MLX provider only)
- **Scope:**
  - New `mlx_stream_chat` function in `src/providers/mlx.c`
  - New `tests/test_mlx_stream_chat.c` (gated on `HU_ENABLE_MLX_PROVIDER` + macOS + ARM64)
  - Update `CMakeLists.txt` (register test with conditional gates)
- **Duration:** ~2–3 hours (M estimate, subprocess handling)
- **Test Contract:**
  - [ ] `mlx_stream_chat` wired into `hu_provider_mlx.supports_streaming = true`
  - [ ] Subprocess stdout read in non-blocking mode via `select()`
  - [ ] Each complete token emitted as `hu_stream_chunk_t`
  - [ ] SIGTERM cleanly terminates subprocess + consumer loop
  - [ ] No zombie processes post-test (verified via process accounting)
  - [ ] Tests gated: skip with SKIP verdict on non-macOS or non-ARM64 CI
  - [ ] Two test cases: `test_mlx_stream_chat_chunks_equal_batch` (parity check) + `test_mlx_stream_chat_cancellation_terminates_subprocess` (signal handling)
  - [ ] `./build/human_tests` full suite passes
  - [ ] No ASan errors
- **Quality Gates:**
  - [ ] `/verify` run returns `RESULT_verifier=PASS`
  - [ ] Critic review: no HIGH/CRITICAL findings
  - [ ] Process accounting verified: no zombie processes
- **Merge Timing:** Wait for US-C3.3 to merge, then rebase sprint-54 before merging C3.3's changes. After rebase, merge US-M3-B4 immediately.

**Coordination note:** After US-C3.3 merges, scrum-master sends signal to US-M3-B4 agent: "C3.3 merged; rebase sprint-54 now before your merge."

---

### Wave 3: Sequential Doctor Modifications (Strict sequence)

#### US-C3.9 — Doctor exit-code contract
- **Implementer:** `general-purpose`
- **Isolation:** Worktree (modifies core `src/doctor.c`, requires careful sequencing)
- **Scope:**
  - Modify `src/doctor.c` to emit specific exit codes (0 = all pass, 1 = user-action failures, 2 = bug-grade failures, 64 = doctor crash)
  - New `tests/test_doctor_exit_codes.c` (~200 LoC, mock checks)
  - New `docs/guides/doctor.md` (~80 lines, exit-code table + severity definitions)
  - New `scripts/check-doctor-exit-codes-in-sync.sh` (~40 lines, bash pre-commit check)
  - Wire pre-commit hook into `.githooks/pre-commit`
- **Duration:** ~2–3 hours (M estimate, includes pre-commit script)
- **Test Contract:**
  - [ ] Exit code 0 when all checks PASS
  - [ ] Exit code 1 when any check FAIL with "user-action-required" severity
  - [ ] Exit code 2 when any check FAIL with "bug-grade" severity
  - [ ] Exit code 64 when doctor crashes
  - [ ] Pre-commit script enforces sync between source codes and documented codes
  - [ ] `./build/human_tests` full suite passes
  - [ ] No ASan errors
- **Quality Gates:**
  - [ ] `/verify` run returns `RESULT_verifier=PASS`
  - [ ] Critic review: no HIGH/CRITICAL findings
  - [ ] Pre-commit script passes on all story-modified files
- **Merge Timing:** Merge immediately after DONE verification before Wave 3 sequence completes

---

#### US-C3.7 — Doctor `--json` output
- **Implementer:** `general-purpose`
- **Isolation:** Worktree (modifies `src/doctor.c` after C3.9; strict dependency)
- **Scope:**
  - Modify `src/doctor.c` to add `--json` flag parsing + output formatter
  - New `tests/test_doctor_json_output.c` (~250 LoC, schema + round-trip validation)
  - New fixture directories: `tests/fixtures/doctor_pass_all/` + `tests/fixtures/doctor_fail_provider/`
  - Update `CMakeLists.txt` (add test source + fixture paths)
- **Duration:** ~2–3 hours (M estimate, includes JSON serialization)
- **Test Contract:**
  - [ ] `--json` flag accepted by `human doctor` command
  - [ ] JSON schema v1 locked (version field, timestamp, checks array, aggregate verdict)
  - [ ] stdout emits JSON blob on PASS; stderr empty on PASS, may contain debug on FAIL
  - [ ] Fixtures cover two cases: all-pass + provider-failure
  - [ ] `./build/human_tests` full suite passes
  - [ ] No ASan errors
- **Quality Gates:**
  - [ ] `/verify` run returns `RESULT_verifier=PASS`
  - [ ] Critic review: no HIGH/CRITICAL findings (schema stability + JSON correctness)
- **Merge Timing:** Start AFTER US-C3.9 has merged. Rebase on sprint-54 to pick up C3.9's changes. Merge immediately after DONE verification.

**Wave 3 note:** C3.9 and C3.7 cannot run in parallel; C3.7 MUST start AFTER C3.9 merges to avoid merge conflicts on `src/doctor.c`.

---

### Wave 4: Consumer Layer (Depends on Wave 2)

#### US-C2.3 — Provider setup onboarding step
- **Implementer:** `general-purpose`
- **Isolation:** Worktree (new onboarding step, integrates with provider infrastructure)
- **Scope:**
  - New `src/onboard/step_provider.c` (user-facing provider selection + credential entry)
  - New `include/human/onboard/step_provider.h` (public API)
  - New `tests/test_onboard_step_provider.c` (~400 LoC, test injection via callback)
  - Modify `src/onboard/dispatcher.c` (register step in step_table)
  - Update `CMakeLists.txt` (register sources + tests)
- **Duration:** ~3–4 hours (L estimate, onboarding UX complexity)
- **Test Contract:**
  - [ ] Step plugs into `hu_onboard_dispatcher_t` at HU_ONBOARD_STEP_PROVIDER slot
  - [ ] Reuses `hu_doctor_check_provider` from US-C3.3 (no reimplementation)
  - [ ] User prompted to choose provider: Gemini, Claude, OpenAI, MLX
  - [ ] Prompts for API key + persists to `hu_onboard_state_t.provider` fields
  - [ ] Validates credentials via smoke check; re-prompts on failure with actionable message
  - [ ] Test injection via `user_data` callback matching `step_welcome.c` pattern
  - [ ] `./build/human_tests` full suite passes
  - [ ] No ASan errors
- **Quality Gates:**
  - [ ] `/verify` run returns `RESULT_verifier=PASS`
  - [ ] `/aspect-panel` run on credential handling at user touchpoint: returns CLEAN or PASS
  - [ ] Critic review: no HIGH/CRITICAL findings
  - [ ] Log inspection: no credential leakage in error messages
  - [ ] Verify US-C3.3 smoke check reuse (code review confirms no reimplementation)
- **Merge Timing:** Start ONLY AFTER US-C3.3 has merged and its header is available in sprint-54 branch. Rebase on sprint-54 to pick up C3.3. Merge immediately after DONE verification.

---

## Pre-Flight Checks Per Wave

### Before Wave 1 dispatch:
- [ ] Scrum-master verifies sprint-54 branch exists and is checked out
- [ ] Scrum-master verifies working directory is isolated (`pwd` shows `/Users/sethford/Projects/human-sprint-54`)
- [ ] CMakeLists.txt state captured (baseline for conflict detection)
- [ ] tests/test_main.c state captured (baseline for conflict detection)

### Before Wave 2 dispatch:
- [ ] Wave 1 (US-CLEAN-1) has been merged to sprint-54 branch
- [ ] Scrum-master confirms both C3.3 and M3-B4 agents understand: "First to complete merges to sprint-54; second rebases before merge"
- [ ] CMakeLists.txt state re-captured (post-Wave-1 baseline)
- [ ] tests/test_main.c state re-captured (post-Wave-1 baseline)

### Before Wave 3 dispatch:
- [ ] Wave 2 stories (US-C3.3 + US-M3-B4) have been merged to sprint-54
- [ ] src/doctor.c state captured (baseline)
- [ ] Scrum-master confirms C3.9 and C3.7 agents understand: "Strict sequence; C3.9 merges first, C3.7 starts after merge"

### Before Wave 4 dispatch:
- [ ] Wave 2 (US-C3.3) has been merged to sprint-54
- [ ] Wave 3 (US-C3.9 + US-C3.7) has been merged to sprint-54
- [ ] Scrum-master confirms C2.3 agent: "US-C3.3's provider smoke header is now in sprint-54; reuse it, don't reimplement"

---

## Agent Assignments & Isolation

| Story | Implementer | Isolation | Notes |
|-------|-------------|-----------|-------|
| US-CLEAN-1 | `general-purpose` | Branch | Docs-only; no code conflict risk |
| US-C3.3 | `general-purpose` | Worktree | Network-gated tests; credential handling (aspect-panel required) |
| US-M3-B4 | `general-purpose` | Worktree | Platform-gated tests; subprocess lifecycle |
| US-C3.9 | `general-purpose` | Worktree | Core file (`src/doctor.c`); pre-commit script |
| US-C3.7 | `general-purpose` | Worktree | Core file (`src/doctor.c`); JSON serialization; depends on C3.9 |
| US-C2.3 | `general-purpose` | Worktree | Reuses C3.3 smoke check; credential handling (aspect-panel required) |

---

## Quality Gates (Enforced by Scrum-Master)

Every story MUST satisfy all these gates before accepting a DONE report:

### Per-story gates (immediate post-implementation):

1. **Implementer commits to sprint-54 branch**
   - Verify: `git log sprint-54-tier-1-2-3-cleanup ^origin/main --oneline | grep -q "<expected-commit-pattern>"`
   - If commit not found: reject DONE report and re-dispatch

2. **Verifier runs the code**
   - Dispatch `/verify` agent with story AC inline
   - Verify: `RESULT_verifier=PASS` (not FAIL or INCONCLUSIVE)
   - If not PASS: story stays in flight; surface to implementer

3. **Critic adversarial review**
   - Dispatch `/critic` agent immediately after DONE report (not batched)
   - Verify: no HIGH or CRITICAL severity findings
   - If findings exist: task re-opens; new subtasks created

4. **Aspect-panel for security-sensitive stories**
   - US-C3.3 (credential validation): **required**, must return CLEAN or PASS
   - US-C2.3 (credential entry at user touchpoint): **required**, must return CLEAN or PASS
   - Other stories: optional (run if critic flags security concerns)

5. **Full test suite passes**
   - `./build/human_tests` must exit with code 0
   - No ASan errors (check stdout/stderr for "ERROR:" patterns)
   - Targeted tests for changed files must pass (via `scripts/what-to-test.sh`)

6. **Pre-commit checks pass**
   - Run `scripts/agent-preflight.sh` on changed files
   - US-C3.9: `scripts/check-doctor-exit-codes-in-sync.sh` must pass

### Fleet-wide gates (before sprint close):
- [ ] All 6 stories have `RESULT_verifier=PASS` evidence in their close
- [ ] All security-sensitive stories (US-C3.3, US-C2.3) have `RESULT_aspect_panel=CLEAN|PASS`
- [ ] No outstanding CRITICAL critic findings
- [ ] Full test suite passes once more on merged sprint-54 branch
- [ ] Sprint auditor produces no blocking findings

---

## Implementer Prompt Closure Contract

Every implementer agent receives this exact closure requirement in their story prompt:

> **CLOSURE REQUIREMENT (non-negotiable):**
> Your work is DONE only when:
> 1. You have committed your changes to `sprint-54-tier-1-2-3-cleanup` via `git add <paths> && git commit -m "<message>"` 
> 2. The commit appears in `git log sprint-54-tier-1-2-3-cleanup ^origin/main --oneline`
> 3. You report DONE in this exact format:
> ```
> RESULT_implementer=DONE
>   commits=<N>
>   build-exit=<exit-code from make>
>   test-exit=<exit-code from ./build/human_tests>
>   test-summary="<one-line summary of test output>"
>   git-log-evidence="<one commit line showing your work>"
>   asan=CLEAN|HAS_ERRORS
> ```
> 
> Working-tree-only changes (not committed) will be REJECTED. The scrum-master will re-dispatch.

---

## Merge Sequencing (Scrum-Master Orchestration)

After each story closes (DONE + VERIFY PASS + CRITIC CLEAN):

1. **US-CLEAN-1:** Merge to sprint-54 immediately after DONE verification
2. **US-C3.3:** Merge to sprint-54 immediately after DONE verification; signal US-M3-B4 agent
3. **US-M3-B4:** Rebase on sprint-54 (picks up US-C3.3); merge immediately after
4. **US-C3.9:** Merge to sprint-54 immediately after DONE verification
5. **US-C3.7:** Rebase on sprint-54 (picks up US-C3.9); merge immediately after
6. **US-C2.3:** Rebase on sprint-54 (picks up all prior merges); merge immediately after

Scrum-master MUST manually rebase agents' branches when dependencies land. This prevents agents from working against stale base.

---

## Evidence & Artifacts

After each story completes, scrum-master will:
1. Capture `git log sprint-54 ^origin/main --oneline` (the story's commits)
2. Capture `/verify` output (verifier PASS evidence)
3. Capture `/aspect-panel` output (for US-C3.3 and US-C2.3)
4. Capture critic findings (if any — becomes follow-up tasks)
5. Record evidence in `sprints/sprint-54/evidence/US-<N>/` directory

---

## Risk Mitigations

| Risk | Mitigation |
|------|-----------|
| **Network hangs in smoke test** | HU_IS_TEST gate + 10s hard timeout on provider.complete() call; CI uses mock provider only |
| **CMakeLists merge conflicts** | Strict sequencing: second agent rebases after first merge |
| **src/doctor.c conflicts** | Strict serialization: C3.7 starts only after C3.9 merges |
| **Credential leakage** | Aspect-panel enforces log sanitization; no raw API keys in stderr |
| **MLX tests skip silently** | Tests included in dev preset; skip verdict reported cleanly on CI; no build failure |
| **Zombie subprocess processes** | SIGTERM + WNOHANG waitpid + 2s timeout + SIGKILL fallback; post-test process accounting |

---

## Success Criteria (Sprint Closure)

Sprint 54 is COMPLETE when:

- [ ] All 6 stories merged to sprint-54-tier-1-2-3-cleanup
- [ ] All 6 stories have verifier PASS + critic CLEAN evidence
- [ ] US-C3.3 and US-C2.3 have aspect-panel CLEAN evidence
- [ ] Full test suite passes: `./build/human_tests` exit 0
- [ ] No ASan errors reported
- [ ] Sprint auditor produces no blocking findings (see Phase 5 protocol)
- [ ] `git log sprint-54-tier-1-2-3-cleanup ^origin/main` shows 6 commits (one per story, or CLEAN-1 may combine multiple edits)
- [ ] Close tag created: `git tag -a v-sprint-54-close -m "Sprint 54 close"` pinning the final merge commit

---

## Anti-Patterns (Do NOT)

- ❌ Run C3.9 and C3.7 in parallel (src/doctor.c conflict guaranteed)
- ❌ Run C2.3 before C3.3 merges (reuse of header not yet available)
- ❌ Accept DONE report without commit in sprint-54 branch (concurrent agent wipe risk)
- ❌ Skip aspect-panel for C3.3 or C2.3 (credential handling is security-sensitive)
- ❌ Batch critic review at sprint end (early finding = early fix)
- ❌ Merge second Wave-2 agent without rebasing first (stale CMakeLists.txt baseline)

---

**Plan Status:** PLAN_READY  
**Scrum-Master Sign-Off:** Ready to dispatch Wave 1.

