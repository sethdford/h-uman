# Sprint 49 Plan — C1: macOS Installable Artifacts

**Sprint name:** C1: macOS Installable Artifacts  
**Repo:** h-uman  
**Worktree:** `/Users/sethford/Projects/human-sprint-49`  
**Branch:** `sprint-49-distribution`  
**Base commit:** `36187a1a` (on main, 2026-05-24)  
**Stories:** 7 (US-C1.1 through US-C1.6)  
**Target close:** ~3 weeks (per PO estimate)

---

## Wave Sequencing

**Wave 1 (serial, blocking):** US-C1.1 → US-C1.2  
**Wave 2 (parallel, after Wave 1):** US-C1.3, US-C1.3a, US-C1.4, US-C1.5, US-C1.6

### Dependency Justification

- **US-C1.1** (foundational) → **US-C1.2** (consumes Human.app bundle)
- **US-C1.2** → **US-C1.3, US-C1.4, US-C1.5, US-C1.6** (Wave 2 all depend on .pkg / scripts / bundle)
- **US-C1.3 and US-C1.3a:** US-C1.3a is a sub-story of US-C1.3; can parallelize once US-C1.3 produces sign-and-notarize.sh stub
- **US-C1.4, US-C1.5, US-C1.6:** Orthogonal to signing; can run concurrently with Wave 2's signing work

Wave 2 can start in parallel because:
- US-C1.4 (brew formula) needs only the .pkg script, not the signing implementation
- US-C1.5 (CI workflow) needs stub scripts; actual signing impl (US-C1.3) can land in stubs
- US-C1.6 (docs) references concepts from Wave 1; no code dependency blocks it

---

## Wave 1 Assignments

### US-C1.1: macOS App Bundle Skeleton

**Implementer:** `general-purpose`, `isolation: worktree`

**Input from design:**
- Creates `apps/macOS/Human.app/Contents/{MacOS,Resources,_CodeSignature}` directory structure
- Creates Info.plist with bundle ID, version, executable name per macOS app spec
- CMakeLists.txt target `build_app_bundle` copies daemon/CLI binaries into bundle

**Files to create/modify:**
- `apps/macOS/Human.app/Contents/Info.plist` (+40 LoC)
- `CMakeLists.txt` (+120 LoC, new target)
- `tests/test_app_bundle_structure.c` (+140 LoC)
- Directory structure: `apps/macOS/Human.app/Contents/{MacOS,Resources,_CodeSignature}`

**Acceptance criteria (from stories.md):**
- AC-C1.1.1: Directory tree exists with Info.plist containing version, bundle ID, executable name
- AC-C1.1.2: CMakeLists.txt has `build_app_bundle` target copying binaries to Contents/MacOS/
- AC-C1.1.3: `cmake --preset dev && cmake --build` produces `/Applications/Human.app` with executable perms
- AC-C1.1.4: `otool -L` on bundled binary shows only system libc (no rpath)
- AC-C1.1.5: Test `test_app_bundle_structure.c` verifies bundle layout, Info.plist validity, executables present

**Definition of Done:**
- All AC verified via `/verify` PASS
- `test_app_bundle_structure.c` passes (unit tests)
- Full test suite green (`./build/human_tests` — 11,727 tests)
- Daemon still starts cleanly (smoke test: `build/human service-loop --help` returns usage)
- No ASan errors
- **CRITICAL:** Commit work to `sprint-49-distribution` with `git add <paths> && git commit -m "feat(macos,app-bundle): C1.1 Human.app skeleton"` BEFORE reporting DONE. Working-tree-only DONE reports will be rejected.

**Critic gate:** After implementer reports DONE, `/aspect-panel` run immediately. Any HIGH or CRITICAL finding re-opens the story. Only LOW/INFO allowed to close.

---

### US-C1.2: macOS .pkg Installer Build Script

**Implementer:** `general-purpose`, `isolation: worktree`

**Input from design:**
- Creates `scripts/release/build-pkg.sh` script using pkgbuild + productbuild
- Creates `scripts/release/com.h-uman.human.plist` (launchd template)
- Creates `tests/fixtures/distribution.xml.template` for Distribution policy

**Files to create/modify:**
- `scripts/release/build-pkg.sh` (+180 LoC)
- `scripts/release/com.h-uman.human.plist` (+35 LoC)
- `tests/test_pkg_builder.c` (+100 LoC)
- `tests/fixtures/distribution.xml.template` (+25 LoC)
- `CMakeLists.txt` (+12 LoC, pkgbuild test gate)

**Acceptance criteria:**
- AC-C1.2.1: Script accepts `--output`, `--app-path` flags with sensible defaults
- AC-C1.2.2: Uses `pkgbuild` to create component package installing to `/Applications/Human.app` with proper ownership (root:wheel) and perms
- AC-C1.2.3: Creates Distribution.xml with InstallationCheck for macOS 10.15+ + 500 MB free space
- AC-C1.2.4: .pkg file is ≥8 MB; `file ./human-release.pkg` returns `xar archive`
- AC-C1.2.5: Test mocks pkgbuild invocation, verifies command-line flags; integration test on macOS CI verifies pkgbuild succeeds

**Definition of Done:**
- All AC verified via `/verify` PASS
- `test_pkg_builder.c` passes (unit tests on all platforms; integration test runs on macOS CI only with HU_IS_TEST gating)
- Full test suite green (11,727 tests)
- Daemon still starts cleanly
- No ASan leaks
- Script actually produces a valid .pkg when run manually on macOS (smoke test in integration gate)
- **CRITICAL:** Commit work to `sprint-49-distribution` with `git add scripts/ tests/ && git commit -m "feat(macos,release): C1.2 build-pkg.sh script"` BEFORE reporting DONE.

**Critic gate:** `/aspect-panel` run immediately after DONE report. Cross-check with Wave 2 stories that will consume this script (US-C1.3 signing, US-C1.5 CI workflow).

---

## Wave 2 Assignments (Parallel after Wave 1)

### US-C1.3: Code-Signing and Notarization

**Implementer:** `general-purpose`, `isolation: worktree`

**Input from design:**
- Creates `scripts/release/sign-and-notarize.sh` script orchestrating codesign → productsign → xcrun notarytool submit/wait → xcrun stapler
- Creates `scripts/release/entitlements.plist` (minimal, no keychain/hardware)
- Handles env vars (APPLE_DEV_ID, NOTARY_PROFILE) gracefully; skips signing if missing

**Files to create/modify:**
- `scripts/release/sign-and-notarize.sh` (+250 LoC)
- `scripts/release/entitlements.plist` (+20 LoC)
- `tests/test_sign_and_notarize.c` (+120 LoC)
- `CMakeLists.txt` (+5 LoC, entitlements.plist install)

**Acceptance criteria:**
- AC-C1.3.1: Script accepts env vars (APPLE_DEV_ID, NOTARY_PROFILE); reads from env if flags not provided
- AC-C1.3.2: Uses `codesign` with --options runtime, --timestamp, --entitlements (minimal); no sensitive entitlements
- AC-C1.3.3: Uses `xcrun notarytool submit` + --wait polling (timeout 15 minutes)
- AC-C1.3.4: Uses `xcrun stapler staple` on notarization success
- AC-C1.3.5: On notarization failure, captures submission log via `xcrun notarytool log`, passes to diagnose-notary.sh (US-C1.3a)
- AC-C1.3.6: Test mocks codesign/xcrun calls; integration test on macOS CI attempts signing with --dry-run flag

**Definition of Done:**
- All AC verified via `/verify` PASS
- `test_sign_and_notarize.c` passes (mocked on all platforms; --dry-run integration test on macOS CI)
- Full test suite green (11,727 tests)
- Daemon still starts cleanly
- No ASan errors
- Entitlements.plist is valid XML
- **CRITICAL:** Commit work to `sprint-49-distribution` with `git add scripts/ tests/ include/ && git commit -m "feat(macos,signing): C1.3 sign-and-notarize.sh and entitlements"` BEFORE reporting DONE.

**Critic gate:** `/aspect-panel` run immediately. Cross-check: script gracefully handles missing secrets (CI should not fail if APPLE_DEV_ID not set).

---

### US-C1.3a: Notarization Failure Translator

**Implementer:** `general-purpose`, `isolation: worktree`

**Input from design:**
- Creates `scripts/release/diagnose-notary.sh` shell wrapper
- Creates `src/tools/notary_translate.c` with pure function translating Apple rejection JSON
- Implements translation table mapping 8 common rejection patterns to human-readable fixes

**Files to create/modify:**
- `scripts/release/diagnose-notary.sh` (+100 LoC)
- `src/tools/notary_translate.c` (+180 LoC)
- `include/human/tools/notary_translate.h` (+50 LoC)
- `tests/test_notary_translate.c` (+90 LoC)
- `tests/fixtures/notary/*.json` (5 test fixtures, ~200 LoC total)

**Acceptance criteria:**
- AC-C1.3a.1: Script accepts `--log <path>` (Apple JSON); parses for code signature, timestamp, malware, arch mismatch issues
- AC-C1.3a.2: For each failure, outputs explanation + exact fix command (e.g., "codesign -s <cert> --timestamp")
- AC-C1.3a.3: Test feeds 5 representative rejection logs; verifies actionable fix text in output

**Definition of Done:**
- All AC verified via `/verify` PASS
- `test_notary_translate.c` passes (all 5 fixtures produce correct output)
- Full test suite green (11,727 tests)
- Daemon still starts cleanly
- No ASan errors
- Fixtures are valid JSON
- **CRITICAL:** Commit work to `sprint-49-distribution` with `git add scripts/ src/tools/ include/human/tools/ tests/ && git commit -m "feat(macos,notary): C1.3a diagnose-notary.sh translator"` BEFORE reporting DONE.

**Critic gate:** `/aspect-panel` run immediately. Cross-check: test fixtures represent realistic Apple rejection log structures; no hallucinated JSON.

---

### US-C1.4: Homebrew Formula

**Implementer:** `general-purpose`, `isolation: worktree`

**Input from design:**
- Finalizes existing `Formula/human.rb` (already ~85% complete)
- Creates `scripts/install/human-daemon.plist.template` with `${HOME}`, `${BREW_PREFIX}` substitutions
- Creates `.github/workflows/test-homebrew-formula.yml` CI workflow
- Creates `tests/test_homebrew_formula.sh` bash test script

**Files to create/modify:**
- `Formula/human.rb` (+40 LoC finalization)
- `scripts/install/human-daemon.plist.template` (+30 LoC)
- `tests/test_homebrew_formula.sh` (+80 LoC)
- `.github/workflows/test-homebrew-formula.yml` (+40 LoC)

**Acceptance criteria:**
- AC-C1.4.1: Formula passes `brew audit --strict`; uses stable release tarball URL
- AC-C1.4.2: Formula installs binaries to `$(brew --prefix)/bin/human`, launchd plist to `~/Library/LaunchAgents/com.human.daemon.plist`
- AC-C1.4.3: Post-install hook runs `launchctl load`; verifies daemon starts with `human --version`
- AC-C1.4.4: `brew test human` runs smoke test (daemon starts, version readable); no network
- AC-C1.4.5: Test parses formula, checks binaries from release tarball, validates launchd plist XML

**Definition of Done:**
- All AC verified via `/verify` PASS
- `test_homebrew_formula.sh` passes
- `.github/workflows/test-homebrew-formula.yml` passes on macOS CI
- Full test suite green (11,727 tests)
- Daemon still starts cleanly
- No ASan errors
- **CRITICAL:** Commit work to `sprint-49-distribution` with `git add Formula/ scripts/install/ tests/ .github/workflows/ && git commit -m "feat(homebrew,install): C1.4 brew formula and launchd plist"` BEFORE reporting DONE.

**Critic gate:** `/aspect-panel` run immediately. Cross-check: formula correctly references US-C1.2 .pkg artifacts; no hardcoded version numbers that will break on next release.

---

### US-C1.5: GitHub Actions Release Workflow

**Implementer:** `general-purpose`, `isolation: worktree`

**Input from design:**
- Creates `.github/workflows/release-macos.yml` with build → package → (conditionally) sign → release pipeline
- Splits signing: main push = unsigned artifact, v* tag push = full signed release
- Uses macos-14-arm64 runner; documents fallback to macos-13-arm64
- Includes preflight check via `scripts/agent-preflight.sh` before signing

**Files to create/modify:**
- `.github/workflows/release-macos.yml` (+180 LoC)
- `tests/test_release_workflow.sh` (+100 LoC)
- `scripts/release/sign-and-notarize.sh` stub (stub only; real impl in US-C1.3)
- `scripts/release/diagnose-notary.sh` stub (stub only; real impl in US-C1.3a)

**Acceptance criteria:**
- AC-C1.5.1: Workflow triggers on push to main, push to v* tags, manual dispatch
- AC-C1.5.2: Builds `cmake --preset release` on macos-14-arm64; produces `build/Release/Human.app`
- AC-C1.5.3: Runs `scripts/release/build-pkg.sh`; skips signing if secret missing with clear message
- AC-C1.5.4: Uploads .pkg to GitHub Release asset; marks pre-release if not tagged
- AC-C1.5.5: Runs `scripts/agent-preflight.sh` before packaging; fails if preflight fails
- AC-C1.5.6: Test validates workflow YAML syntax; no secrets echoed

**Definition of Done:**
- All AC verified via `/verify` PASS
- `test_release_workflow.sh` passes (YAML linting, no secrets in steps)
- Full test suite green (11,727 tests)
- Daemon still starts cleanly
- No ASan errors
- Workflow YAML is valid GitHub Actions syntax
- Manual test: push to main → verify unsigned artifact uploads as pre-release
- **CRITICAL:** Commit work to `sprint-49-distribution` with `git add .github/workflows/ tests/ && git commit -m "feat(ci,release): C1.5 macOS release workflow"` BEFORE reporting DONE.

**Critic gate:** `/aspect-panel` run immediately. Cross-check: no secrets hardcoded; ARM runner fallback is documented in comments; preflight runs BEFORE signing (not after).

---

### US-C1.6: Installation Guide & README

**Implementer:** `general-purpose`, `isolation: worktree`

**Input from design:**
- Creates `docs/guides/installation.md` (300+ lines) covering three install paths: .pkg, brew, source
- Updates `README.md` to link installation guide above build instructions
- Creates `tests/test_installation_guide.c` to verify markdown structure and links
- Creates `.github/workflows/docs-lint.yml` for markdownlint + lychee link-check

**Files to create/modify:**
- `docs/guides/installation.md` (+300 LoC)
- `README.md` (+20 LoC changes)
- `tests/test_installation_guide.c` (+100 LoC)
- `.github/workflows/docs-lint.yml` (+30 LoC)

**Acceptance criteria:**
- AC-C1.6.1: Document covers .pkg download, brew install, build-from-source paths
- AC-C1.6.2: Each path explains system requirements, what gets installed, verification, uninstall
- AC-C1.6.3: Dedicated "Gatekeeper" section explaining security warning, why it's safe, Apple docs link
- AC-C1.6.4: "Troubleshooting" section with top 5 issues + link to `human doctor` (forward-ref to US-C3)
- AC-C1.6.5: README.md links to installation.md in top-level "Install" section
- AC-C1.6.6: Test verifies markdown exists, contains 8 required sections, no broken internal links

**Definition of Done:**
- All AC verified via `/verify` PASS
- `test_installation_guide.c` passes (markdown structure, required sections)
- Docs-lint workflow passes (`markdownlint`, `lychee` link-check)
- Full test suite green (11,727 tests)
- Daemon still starts cleanly
- No ASan errors
- README.md reorder complete; "Install" section before "Build"
- **CRITICAL:** Commit work to `sprint-49-distribution` with `git add docs/ README.md tests/ .github/workflows/ && git commit -m "docs(install): C1.6 installation guide and README reorder"` BEFORE reporting DONE.

**Critic gate:** `/aspect-panel` run immediately. Cross-check: no broken links in docs; forward-refs to US-C3 `human doctor` are clearly marked as "coming soon"; Gatekeeper explanation is accurate.

---

## Quality Gates (Enforced per story)

### Per-Story Definition of Done Checklist

After each implementer reports DONE:

1. **Commit exists on `sprint-49-distribution`:**
   ```bash
   git log sprint-49-distribution ^36187a1a --oneline | grep -E "C1\.[1-6]"
   ```
   If no commit found, reject DONE. Working-tree-only claims are not acceptable.

2. **Verifier passes:**
   - Dispatch `/verify` agent with the story's AC inline
   - Verify returns `RESULT_verifier=PASS`
   - FAIL or INCONCLUSIVE → story re-opens

3. **Critic review (immediate, per-story, NOT batched at sprint end):**
   - Dispatch critic agent after verifier PASS
   - Critic examines: edge cases, cross-agent regressions (with Wave 2 parallel stories), silent failures, test coverage gaps
   - Any HIGH or CRITICAL finding → story re-opens
   - LOW/INFO findings are logged but don't block closure

4. **Aspect panel passes:**
   - Run `/aspect-panel` after critic clean
   - Must return PASS or CLEAN (no ESCALATE)
   - ESCALATE → escalate to lead for triage

5. **Full test suite green:**
   - `./build/human_tests` must pass all 11,727 tests
   - No new failures vs. baseline
   - No ASan errors, no ubsan errors

6. **No regression in eval or preflight:**
   - `scripts/eval-prompt-blocks.sh` still passes (persona blocks unwired)
   - `scripts/agent-preflight.sh` on changed files: no lint errors, no formatting issues

7. **Daemon smoke test:**
   - Launch daemon: `build/human service-loop --help` returns usage without crash
   - Verify startup banner is present in logs
   - Kill cleanly with SIGTERM

### Wave-Level Gate (before moving to next wave)

Wave 1 → Wave 2 transition:
- [ ] US-C1.1 DONE (all 7 checks above pass)
- [ ] US-C1.2 DONE (all 7 checks above pass)
- [ ] Both stories' commits exist on `sprint-49-distribution`
- [ ] `cmake --preset dev && cmake --build` produces `/Applications/Human.app`
- [ ] `scripts/release/build-pkg.sh --output test.pkg` produces a valid .pkg file

Wave 2 closure:
- [ ] All 5 stories (US-C1.3, US-C1.3a, US-C1.4, US-C1.5, US-C1.6) DONE
- [ ] All 35 checks (7 per story) pass
- [ ] Full test suite green (11,727 tests)
- [ ] Daemon starts cleanly on all paths: from .pkg, from brew, from source

---

## Risk Register

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| **pkgbuild unavailable on non-macOS CI** | HIGH | MEDIUM | Test gated by CMake check; returns exit 79 (unsupported) if missing. CI skips. |
| **Apple notary service slow/unavailable** | MEDIUM | MEDIUM | 15-minute timeout with exponential backoff. US-C1.3a translates rejections. Retry is manual. |
| **ARM runner (macos-14-arm64) oversubscribed** | LOW | MEDIUM | Document fallback to macos-13-arm64 in workflow comments. Do NOT auto-fallback; maintainer chooses. |
| **Secrets leaked in GitHub Actions logs** | MEDIUM | LARGE | Use `add-mask` for all secrets. Test in `test_release_workflow.sh` verifies no echo of secret vars. |
| **Entitlements.plist syntax error blocks notarization** | LOW | MEDIUM | US-C1.3 starts with minimal entitlements (empty dict). US-C1.3a diagnoses Apple rejections. |
| **Gatekeeper prompt wording changes across macOS versions** | LOW | SMALL | Document general shape ("May see a prompt") not exact wording. Link to Apple docs as source of truth. |
| **`human doctor` command doesn't exist yet (C3 scope)** | MEDIUM | MEDIUM | US-C1.6 uses forward-ref with TODO. Test checks forward-ref comment exists. |
| **Daemon won't start after .pkg install (permissions)** | MEDIUM | MEDIUM | Installation guide documents Full Disk Access + Accessibility setup. `human doctor` (C3) validates. |
| **Brew formula sha256 hashes stale** | MEDIUM | MEDIUM | Compute sha256 of release binaries before commit. Include in commit message. Validate before each release. |

---

## Anti-Patterns to Refuse

1. **Implementer reports DONE without commit on `sprint-49-distribution`.** Check `git log sprint-49-distribution ^36187a1a` for the commit. If absent, reject and ask for proper git add + commit.

2. **Skipping `/verify` or `/aspect-panel`.** Both are mandatory per-story gates. Do not accept "looks good" or "I tested it locally" without evidence.

3. **Closing stories without critic review.** Critic runs immediately after verifier PASS, not batched at sprint end. (Sprint 1 shipped a regex bug because critic review was batched.)

4. **Parallel stories that share state.** Wave 2 was vetted by PO for orthogonality. If implementers report cross-contamination, halt and sequence.

5. **Moving to Wave 2 before Wave 1 is fully closed.** Both US-C1.1 and US-C1.2 must be DONE with all gates passing before any Wave 2 story starts.

---

## Branch and Worktree

- **Worktree path:** `/Users/sethford/Projects/human-sprint-49`
- **Sprint branch:** `sprint-49-distribution` (isolated; implementers must work here, not on shared branches)
- **Base:** `36187a1a` (on main, 2026-05-24)

All implementers will work in the human-sprint-49 worktree with the sprint-49-distribution branch. No switching to other branches without scrum-master approval.

---

## What Happens After Sprint Close

1. **Merge:** All commits from `sprint-49-distribution` are cherry-picked or merged into main. Tests must be green post-merge.
2. **Audit:** Sprint auditor reviews all 7 stories' evidence (verifier outputs, critic reports, aspect-panel results).
3. **Retro:** Team reflects on Wave 1 serial work vs Wave 2 parallel work. Did parallelization help or hurt?
4. **Tag:** Close commit is tagged immutably as `v-sprint-49-close` to pin the exact state.

---

## Expected Timeline

- **Wave 1:** ~1 week (US-C1.1 → US-C1.2 serial, blocking)
- **Wave 2:** ~2 weeks (5 stories in parallel: signing, brew, CI, docs)
- **Total:** ~3 weeks (per PO estimate)

Each implementer can start their Wave 2 story as soon as Wave 1 is closed, allowing them to work in parallel without blocking each other.

---

**RESULT_scrum-master=PLAN_READY waves=2 stories=7**
