# Sprint 9 Plan — Distribution MVP

## Header

| Field | Value |
|---|---|
| Sprint | 9 |
| Goal | One-command install (Homebrew) + polished first-run experience; macOS user goes from zero to chatting via iMessage in under 10 minutes |
| Dates | 2026-05-17 — 2026-05-30 |
| Scrum-master | claude/interesting-engelbart-588b03 |
| Branch | `sprint-9-distribution-mvp` |
| Working directory | `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-9-distribution` |
| Base SHA | `ea02b08e` |
| Total estimate | 2×M + 4×S ≈ 8–10 engineer-days |

---

## §1 Sequencing

```
Wave 0 (parallel — no inter-story deps)
  US-9.1  Homebrew formula + CI smoke          [M, Low]
  US-9.2  Onboard post-success nextstep        [S, Low]
  US-9.3  iMessage non-allowlisted reply       [M, Medium]
  US-9.4  human doctor install-ready check     [S, Low]
  US-9.6  iMessage FDA/busy doctor messaging   [S, Low]

      ↓ ALL FIVE must be committed to sprint branch before Wave 1 opens

Wave 1 (sequential gate — depends on US-9.1)
  US-9.5  Website install one-liner + badge    [S, Low]
           Requires: real tap name and formula SHA256s confirmed by US-9.1
           Blocked until: US-9.1 commit on sprint branch AND tap org resolved
```

Rationale for wave boundary:
- US-9.5 embeds the canonical `brew tap humanlabs/human && brew install
  humanlabs/human/human` command verbatim (AC-9.5.1) and runs a
  drift-detection test (`check-install-matches-formula.mjs`) that parses
  `Formula/human.rb` for the tap name. If US-9.1 has not settled the tap
  org and the `# tap: humanlabs/human` comment in the formula, the
  drift test cannot pass. The implementer for US-9.5 must confirm the
  formula comment exists before beginning step 1.
- US-9.2 through US-9.4 and US-9.6 touch disjoint files
  (`src/onboard.c`, `src/channels/imessage.c`, `src/doctor.c`,
  `include/human/doctor.h`) and share no state with each other or US-9.1.
  They run fully in parallel.

---

## §2 Wave Assignments

### Wave 0

| Story | Title | Risk | Primary files | Implementer | Verifier scope | Critic scope | Aspect-panel | Worktree |
|---|---|---|---|---|---|---|---|---|
| US-9.1 | Homebrew formula + release CI | Low | `Formula/human.rb`, `scripts/update-formula-hashes.sh`, `scripts/check-formula-install.sh`, `.github/workflows/release.yml`, `tests/scripts/test_update_formula_hashes.sh` | general-purpose | Script unit tests exit 0; formula SHA256 fields are non-zero post-rc tag; `brew test` exits 0 on arm64 runner | Check that `update-formula-hashes.sh` cannot silently emit wrong architecture hashes; verify `brew-install-smoke` job is release-only (not in `ci.yml`); check for the `TODO(US-8.4)` codesign comment | NO — no security boundary, no C code | YES |
| US-9.2 | Onboard post-success nextstep | Low | `include/human/onboard.h`, `src/onboard.c`, `tests/test_onboard_nextstep.c`, `tests/CMakeLists.txt` | general-purpose | `./build/human_tests --filter=onboard_nextstep` passes; full suite 0 failures, 0 ASan; `hu_onboard_nextstep_format` truth-table all 5 rows exercised | Verify adversarial tests assert old generic message is ABSENT (not merely that new text is present); confirm `hu_config_load` result is freed (no ASan leak); confirm test file has no `@covers-none` bypass | NO | YES |
| US-9.3 | iMessage non-allowlisted sender reply | Medium | `src/channels/imessage.c`, `include/human/imessage.h` (or internal struct), `tests/test_imessage_non_allowlisted.c`, `tests/CMakeLists.txt` | general-purpose | All 18 tests in `tests/test_imessage_non_allowlisted.c` pass; full suite 0 failures, 0 ASan; dedup file does not exceed 256 lines; second injection within 24h produces no `last_courtesy_message` write | Verify the courtesy reply text cannot contain the operator's phone number or Apple ID handle; verify `mock_epoch_override` is inside `HU_IS_TEST` guard; verify the aggregate 50-reply-per-day cap is wired to the predicate; confirm the pending_courtesy ring cannot be drained by a non-allowlisted sender re-entering the agent | YES — outbound surface is spoofable; allowlist is security-adjacent | YES |
| US-9.4 | human doctor install-ready check | Low | `include/human/doctor.h`, `src/doctor.c`, `src/main.c`, `tests/test_doctor_install.c`, `CMakeLists.txt` | general-purpose | All 7 tests in `tests/test_doctor_install.c` pass; `human doctor --install` exits nonzero on any red check; `--install --json` emits valid JSON with `status` + `checks` array; full suite 0 failures, 0 ASan | Verify `--install` is positioned BEFORE `--fix` and AFTER subcommand tokens in the argv dispatch; confirm the exit-code adversarial test asserts `HU_ASSERT_NE(rc, HU_OK)` not equality to an integer; verify `doctor lies` risk is addressed — each sub-check reads primary evidence not cached state; confirm default `human doctor` exit code is UNCHANGED | NO | YES |
| US-9.6 | iMessage FDA/busy doctor messaging | Low | `src/doctor.c`, `include/human/doctor.h`, `tests/test_doctor_imessage_diagnose.c`, `tests/CMakeLists.txt` | general-purpose | All 7 fixture tests pass; AUTH output contains "Full Disk Access" AND "System Settings" AND does NOT contain "Messages.app may be syncing"; BUSY output severity is `HU_DIAG_WARN` (distinct from `HU_DIAG_ERR`); existing `tests/test_ported_modules.c` doctor-imessage block still passes | Verify each truth-table test includes at least one negative assertion per `.claude/rules/tests-that-pin-bugs.md`; confirm `hu_imessage_diag_from_poll_status` export is used in the test file (production symbol reference); confirm the BUSY/AUTH cross-contamination adversarial assertions are present | NO | YES |

### Wave 1

| Story | Title | Risk | Primary files | Implementer | Verifier scope | Critic scope | Aspect-panel | Worktree |
|---|---|---|---|---|---|---|---|---|
| US-9.5 | Website install one-liner + badge | Low | `website/src/data/install.json`, `website/src/components/InstallSection.astro`, `website/src/pages/index.astro`, `website/src/styles/global.css`, `website/scripts/check-install-matches-formula.mjs`, `website/tests/install-section.spec.ts`, `website/package.json`, `.github/workflows/ci.yml`, `.github/workflows/release.yml` | general-purpose | `pnpm --filter website build` exits 0; all 4 vitest tests pass including the drift-detector test (spawnSync status 0); axe job zero new violations; no raw hex / raw px in new CSS or component (grep gate) | Verify Test D (no hard-coded semver in component source) is a regex guard, not a truthy check; confirm the drift-detector spawns the check script with arg-array form (`spawnSync`) not a shell string; confirm `verified.macos_arm64.ok` is only written by release.yml not any human-editable path; verify `website/src/data/install.json` command field matches the formula literally | NO | YES |

---

## §3 Implementer Commit Discipline

Every implementer agent MUST commit work to `sprint-9-distribution-mvp` BEFORE
reporting DONE. The commit must be verifiable via:

```bash
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/sprint-9-distribution \
  log sprint-9-distribution-mvp ^ea02b08e --oneline | grep -q <expected-pattern>
```

Working-tree-only DONE reports are rejected. A story stays open until the
scrum-master can run the above command and see the commit. Sprint 1 was wiped
twice by concurrent agents running `git reset --hard HEAD` on a shared branch;
the commit-before-DONE gate is the only protection.

Commit message format (enforced by `.githooks/commit-msg`):

```
feat(onboard): emit whats-next block with imessage pairing step
```

Types: `feat fix refactor test docs chore perf ci build style`

For CI/script-only stories (US-9.1 scripts, US-9.5 website), use `ci` or `feat`
as appropriate. All commits must include the relevant test file in the same
commit as the implementation — not a follow-up.

---

## §4 Quality Gates

A story is closed only when ALL of the following hold. No exceptions.

### Per-story (checked before marking DONE)

- [ ] Commit exists on `sprint-9-distribution-mvp` branch (`git log` verifiable)
- [ ] All AC have evidence (test output showing pass count, verifier transcript, or
      smoke-test transcript for manual ACs)
- [ ] `/verify` returned `RESULT_verifier=PASS`
- [ ] Per-story critic ran immediately after DONE report and returned CLEAN
      (or only LOW/INFO findings)
- [ ] For US-9.3: `/aspect-panel` returned PASS or CLEAN (not ESCALATE)
- [ ] No `RESULT_critic=HAS_FINDINGS` of HIGH or CRITICAL severity outstanding
- [ ] Tests added for all AC (happy path AND error paths)
- [ ] Full suite green: `./build/human_tests` exits 0, 0 ASan errors
      (for C stories); `pnpm --filter website test` exits 0 (for US-9.5)
- [ ] No commented-out code, no TODO/FIXME without owner
- [ ] Test file references production symbols (`.claude/rules/test-references-production-symbol.md`)
- [ ] Adversarial tests assert dangerous outcomes are BLOCKED, not just that
      function returns OK (`.claude/rules/tests-that-pin-bugs.md`)

### Sprint close (checked before Phase 5 review)

- [ ] All 6 stories DONE per above gates
- [ ] Audit trail: `sprints/sprint-9/evidence/` has at least one file per story
- [ ] No open `RESULT_critic=HAS_FINDINGS` of any severity
- [ ] `sprint-auditor` invoked before retro

---

## §5 Cross-Sprint Coordination

### US-9.4 + Sprint-8 US-8.5 (`--privacy` flag) — dispatch precedence

`src/main.c::cmd_doctor` must follow this exact argv dispatch order:

```
1. Subcommand check: imessage | verifier | scheduler | responses  (already shipped)
2. else if (do_install)  ← US-9.4 adds this slot
3. else if (do_privacy)  ← Sprint-8 US-8.5 adds this slot (reserved, not blocked)
4. else if (do_fix)      (already shipped)
5. else                  (legacy default report)
```

A one-line comment at the top of `cmd_doctor` must document this order so
future flag additions don't accidentally introduce exclusivity. The US-9.4
implementer adds the comment; the sprint-8 US-8.5 implementer adds the
`do_privacy` branch in the reserved slot without altering the US-9.4 structure.

If sprint-8 lands its `--privacy` branch first with an exclusive `if/else if`
that returns early before the `--install` slot, the US-9.4 implementer must
flag a conflict to the scrum-master before committing. Do not silently adapt
around it.

### US-9.1 + Sprint-8 US-8.4 (signed binaries) — Homebrew bottle gate

The US-9.1 design explicitly defers real Homebrew bottles (`brew bottle`)
until US-8.4 (code signing + notarization) lands. The formula currently
ships a pre-built binary via raw URL + SHA256 (not a bottle). This is the
safe state: users get the binary without a compiler, and the formula's SHA
covers the exact artifact.

**Gate:** Do NOT add a `bottle do` block to `Formula/human.rb` in US-9.1.
The formula comment added in US-9.1 must include:

```ruby
# TODO(US-8.4): promote to brew bottle once codesign + notarization pipeline lands
```

If sprint-8 US-8.4 merges during this sprint, the US-9.1 implementer is
responsible for flagging the readiness to the scrum-master; a follow-up story
(`feat(formula): promote to brew bottle`) is created, not handled in-sprint
without a design review.

### US-9.5 hard gate on US-9.1

The Wave 1 gate is not a soft suggestion. The US-9.5 implementer may NOT begin
step 1 until:

1. US-9.1 is committed to `sprint-9-distribution-mvp` (verifiable via `git log`)
2. The `# tap: humanlabs/human` comment exists in `Formula/human.rb` (required by
   `check-install-matches-formula.mjs`)
3. The open question on tap org name (stories.md line 186) is resolved by the user

The scrum-master will check these three preconditions before dispatching the
Wave 1 agent.

---

## §6 Risks

### Risk 1: Tap org open question blocks Wave 0 completion

**Probability:** High (the question is unresolved at plan time).
**Impact:** Medium — US-9.1 cannot push a real tap; the brew-install-smoke CI
job can test `--build-from-source` but cannot test the pre-built binary path
(AC-9.1.2). The formula SHA256s can be computed and committed, but the tap
repo push step fails until the org exists.
**Mitigation:** The US-9.1 implementer raises the blocker immediately if the
tap repo (`humanlabs/homebrew-human`) does not exist. The scrum-master escalates
to the user (open question #1 in stories.md). Wave 1 is hard-gated on
resolution. In the interim, the smoke job is conditioned on
`if: env.HUMANLABS_TAP_PUSH_TOKEN != ''` so the release workflow does not fail
due to a missing secret — it warns instead.

### Risk 2: US-9.3 courtesy-reply spam vector escapes the per-handle cap

**Probability:** Low (aggregate cap is in the design at 50/day).
**Impact:** High — unbound outbound iMessages is an Apple TOS violation and
surfaces the operator's Apple ID.
**Mitigation:** The aspect-panel for US-9.3 must include a security verifier
that confirms: (a) `hu_imessage_should_courtesy_reply` is the ONLY code path
that queues a courtesy reply (no bypass via `pending_courtesy` direct write),
(b) the aggregate 50-reply/24h cap is wired to the predicate and has its own
test, (c) the reply text cannot contain `owner_handle` (only
`owner_display_name`). If the aspect-panel escalates, the story does not
close until a scrum-master review. The critic must also run before any
`RESULT_verifier=PASS` is accepted as evidence.

### Risk 3: US-9.4 doctor lies — reports READY on a broken install

**Probability:** Low (design specifies primary-evidence reads, not cached state).
**Impact:** High — a user trusts "install: READY" and wastes time trying to chat
with a broken setup. The entire sprint's goal (zero-to-chatting in 10 minutes)
is undermined.
**Mitigation:** The critic for US-9.4 must explicitly verify that each sub-check
reads primary evidence (file stat, real persona parse, non-empty channels array)
and has a test that passes "config says yes, reality says no" through the
predicate and confirms red output. The critic's review checklist item is:
"Does each sub-check have a test where the config is valid JSON but the
referenced file or resource is absent?" If any sub-check lacks this, the story
is rejected as DOD_BLOCKED.
