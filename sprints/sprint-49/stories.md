# Sprint 49 Backlog — C1: Installable Artifacts for macOS

**Goal:** Decompose C1 (macOS .pkg installer) into 4-5 parallel user stories. A user can download a signed .pkg, install it to `/Applications/Human.app`, and run `human --version` without Xcode, CMake, or brew dependencies.

**Release strategy:** No tags. Ship to main continuously. Definition of Done per story: full test suite green, daemon still starts cleanly, `/verify` passes, new artifacts produced.

---

## User Stories (in dependency/priority order)

### US-C1.1 (P0): As a developer, I want to create the Human.app macOS bundle skeleton, so that the build system can package a properly signed application.

**As a** developer, **I want to** create the app bundle directory structure with a minimal Info.plist and bundle layout, **so that** cmake can output a `/Applications/Human.app` target instead of loose binaries.

**Acceptance criteria:**
- AC-C1.1.1: Directory tree exists at `apps/macOS/Human.app/Contents/{MacOS,Resources,_CodeSignature}` with Info.plist containing version, bundle ID, executable name, and required keys per macOS app spec
- AC-C1.1.2: CMakeLists.txt has a new `build_app_bundle` target that copies `build/human` (daemon binary) and `build/human_cli` (CLI binary) into `Contents/MacOS/`
- AC-C1.1.3: `cmake --preset dev && cmake --build` produces `/Applications/Human.app` with the daemon and CLI executable and readable perms
- AC-C1.1.4: `ldd` or `otool -L` on the bundled binary shows only system libc dependencies (no rpath to build/); readiness for signing
- AC-C1.1.5: Test: `test_app_bundle_structure.c` verifies the bundle directory exists, Info.plist is valid XML, and executable files are present and executable

**Estimate:** S (250-350 LoC: Info.plist ~40 lines, CMakeLists.txt additions ~80 lines, test ~120 lines)

**Risk:** macOS bundle metadata (LSMinimumSystemVersion, NSPrincipalClass) can vary; test must allow for future customization without breaking the layout.

**Dependencies:** none (foundational)

**DoD:** test passes, daemon still starts, no ASan errors, `cmake --preset test` green

---

### US-C1.2 (P0): As a packager, I want to script the .pkg installer creation, so that a release build outputs a distributable macOS package.

**As a** packager, **I want to** create `scripts/release/build-pkg.sh` that converts the Human.app bundle into a signed .pkg, **so that** `./scripts/release/build-pkg.sh --output myrelease.pkg` produces an installable artifact.

**Acceptance criteria:**
- AC-C1.2.1: Script accepts `--output <path>` and `--app-path <path>` flags; defaults to `build/Release/Human.app` and `./human-release.pkg`
- AC-C1.2.2: Script uses `pkgbuild` to create a component package that installs to `/Applications/Human.app` with proper ownership (root:wheel) and perms (755 on /Applications/Human.app, 755 on Contents/MacOS/*, 644 on Contents/Info.plist)
- AC-C1.2.3: Script creates a distribution.xml for productbuild that sets InstallationCheck to verify macOS 10.15+ and at least 500 MB free space
- AC-C1.2.4: `.pkg` file is at least 8 MB (binary included); `file ./human-release.pkg` returns `xar archive`
- AC-C1.2.5: Test: `test_pkg_builder.c` mocks pkgbuild invocation and verifies the command line includes correct paths and flags; integration test runs on macOS CI and verifies pkgbuild succeeds

**Estimate:** S (200-300 LoC: script ~180 lines, test ~100 lines)

**Risk:** pkgbuild only exists on macOS; test must use HU_IS_TEST to skip network/spawn invocations on non-macOS CI, or mock the tool.

**Dependencies:** US-C1.1 (needs the .app bundle structure)

**DoD:** script runs without error, produces valid .pkg, test mocks or skips on non-macOS, no ASan leaks

---

### US-C1.3 (P0): As a packager, I want to implement code-signing and notarization, so that downloaded packages pass macOS Gatekeeper without scary warnings.

**As a** packager, **I want to** create `scripts/release/sign-and-notarize.sh` that signs the Human.app with a Developer ID certificate and submits to Apple for notarization, **so that** a user can safely install the .pkg without overriding security warnings.

**Acceptance criteria:**
- AC-C1.3.1: Script accepts `--pkg <path> --cert-id <id> --apple-id <email> --app-password <pass>` flags; cert-id, apple-id, and app-password are read from env vars (`APPLE_DEV_ID`, `APPLE_ID_EMAIL`, `APPLE_APP_PASSWORD`) if not provided
- AC-C1.3.2: Script uses `codesign` to sign the Human.app bundle with the provided cert before packaging (or re-signs the .pkg); entitlements file references no sensitive entitlements (no keychain, no hardware)
- AC-C1.3.3: Script uses `xcrun notarytool submit` to upload the .pkg and polls with `xcrun notarytool info --id <req-id>` until notarization completes; timeout is 15 minutes
- AC-C1.3.4: On notarization success, script uses `xcrun stapler staple` to attach the notarization ticket to the .pkg
- AC-C1.3.5: On notarization failure, script emits the Apple rejection log translated by `scripts/release/diagnose-notary.sh` (see US-C1.3a). Failure messages are human-readable: "Code signature invalid: X" or "Malware detected: Y" mapped to actionable fixes
- AC-C1.3.6: Test: `test_sign_and_notarize.c` mocks codesign/xcrun and verifies the command sequence is correct; integration test on macOS CI attempts signing (with a placeholder cert env var set to "skip") and verifies no crash

**Estimate:** M (300-450 LoC: script ~250 lines, notary diagnose ~150 lines, test ~120 lines)

**Risk:** Apple's notarization API can reject for transient reasons (quota, slow service); script must retry with exponential backoff. Placeholder cert on non-macOS CI must gracefully exit with a message ("Signing requires a Developer ID certificate").

**Dependencies:** US-C1.2 (needs the .pkg to sign)

**DoD:** script handles missing env vars gracefully, test mocks all Apple calls, CI doesn't attempt real notarization without the secret, no ASan errors

---

### US-C1.3a (P1, sub-story of US-C1.3): As an operator, I want to understand notarization failures in human terms, so I can debug why Apple rejected the package.

**As an** operator, **I want to** run `scripts/release/diagnose-notary.sh --log <path>` and get actionable text, **so that** "rejected for code signature issues" becomes "codesign was missing --timestamp; re-run with --timestamp and resubmit."

**Acceptance criteria:**
- AC-C1.3a.1: Script accepts `--log <path>` (Apple's notarization rejection XML) and parses it for common failures: code signature missing, timestamp server unreachable, malware detection flags, architecture mismatch
- AC-C1.3a.2: For each failure, script outputs a 1-3 sentence explanation + the exact command to fix it (e.g., "codesign -s <cert> --timestamp --options runtime app.bundle")
- AC-C1.3a.3: Test: `test_diagnose_notary.c` feeds 5 representative Apple rejection logs and verifies output contains actionable fix text; no test touches the network

**Estimate:** XS (120-180 LoC: script ~100 lines, test ~80 lines)

**Risk:** Apple's rejection XML format changes infrequently; document the expected schema in a comment.

**Dependencies:** US-C1.3 (same script family)

**DoD:** test passes, script produces human-readable output for all 5 test cases, no cryptic "code 1234" messages

---

### US-C1.4 (P1): As a user, I want to install human via Homebrew, so that `brew install human` works as an alternative to downloading the .pkg.

**As a** user, **I want to** have a `Formula/human.rb` Homebrew formula that installs the daemon, CLI, and launchd plist, **so that** `brew install human` produces a working installation with the daemon auto-starting on login.

**Acceptance criteria:**
- AC-C1.4.1: Formula file `Formula/human.rb` is valid Ruby and passes `brew audit --strict`; uses stable release (committed artifact or GitHub release tarball URL)
- AC-C1.4.2: Formula installs binaries to `$(brew --prefix)/bin/human` and `$(brew --prefix)/libexec/human_daemon`; launchd plist goes to `~/Library/LaunchAgents/com.human.daemon.plist`
- AC-C1.4.3: Formula includes a post-install step that enables the launchd plist via `launchctl load ~/Library/LaunchAgents/com.human.daemon.plist`; verifies daemon starts with `human --version` returning a version string
- AC-C1.4.4: `brew test human` runs a smoke test (daemon starts, version is readable); test must be able to run without network
- AC-C1.4.5: Test: `test_homebrew_formula.c` (or a shell script test) parses the formula, checks it references valid binaries from the release tarball, and verifies the launchd plist is valid XML with correct paths

**Estimate:** S (150-250 LoC: formula ~80 lines, test ~100 lines, launchd template ~30 lines)

**Risk:** Homebrew formula depends on a published release tarball; test must mock the tarball fetch or use a fixture.

**Dependencies:** US-C1.2 (needs the release package to reference)

**DoD:** `brew audit --strict Formula/human.rb` passes, test passes, launchd plist is valid and can be parsed

---

### US-C1.5 (P1): As a maintainer, I want a GitHub Actions workflow that builds, signs, and publishes macOS release artifacts, so that every main push produces an installable .pkg and notarized binary.

**As a** maintainer, **I want to** wire up `.github/workflows/release-macos.yml` that builds the release binary, creates the .pkg, signs it, and uploads to GitHub Releases, **so that** users can download the latest `.pkg` from the releases page without manual build steps.

**Acceptance criteria:**
- AC-C1.5.1: Workflow triggers on push to main (can be gated to a specific event or manual dispatch during sprint development)
- AC-C1.5.2: Workflow builds `cmake --preset release` in a macOS runner (os: macos-latest); produces `build/Release/Human.app`
- AC-C1.5.3: Workflow runs `scripts/release/build-pkg.sh --output human-macos-latest.pkg`; workflow skips signing if `APPLE_DEV_ID` secret is missing, and prints a clear message ("Signing skipped; set APPLE_DEV_ID, APPLE_ID_EMAIL, APPLE_APP_PASSWORD secrets to enable")
- AC-C1.5.4: Workflow uploads the .pkg as a GitHub Release asset with a description linking to the installation guide; release is marked "pre-release" if not tagged (continuous main push mode)
- AC-C1.5.5: Workflow includes a step that runs `./scripts/agent-preflight.sh` to verify no regression in core tests; if preflight fails, workflow fails and blocks the release
- AC-C1.5.6: Test: `test_release_workflow.sh` (shell script test) verifies the workflow YAML is valid GitHub Actions syntax (can parse with `yq` or a YAML linter); no network calls

**Estimate:** S (200-280 LoC: workflow YAML ~180 lines, test ~100 lines)

**Risk:** GitHub Actions has secrets management; placeholder env vars must never be leaked into logs. Test must verify no echo of secret vars.

**Dependencies:** US-C1.2, US-C1.3 (needs the build and sign scripts)

**DoD:** workflow YAML passes GitHub Actions validation, test passes, preflight runs and passes, no secrets in any log output

---

### US-C1.6 (P1): As a user, I want clear installation instructions, so that I can understand how to download and install the .pkg or use Homebrew.

**As a** user, **I want to** find `docs/guides/installation.md` that explains how to download, verify, install, and uninstall, **so that** I can get from "I clicked a GitHub release link" to "the daemon is running and I can chat with it" in under 5 minutes.

**Acceptance criteria:**
- AC-C1.6.1: Document covers three install paths: (1) download .pkg from releases + double-click, (2) `brew install human`, (3) build from source (for developers)
- AC-C1.6.2: For each path, document explains: system requirements, what gets installed, where the config lives, how to verify success (`human --version` or `human doctor`), how to uninstall
- AC-C1.6.3: Document includes a "Gatekeeper" section explaining what the security warning is and why it's safe (signed + notarized by Anthropic); link to Apple's Gatekeeper docs
- AC-C1.6.4: Document includes a "Troubleshooting" section linking to `human doctor` and `human help troubleshoot`
- AC-C1.6.5: Document is discoverable from README.md (linked from the top-level "Quick start" section)
- AC-C1.6.6: Test: `test_installation_guide.c` verifies the markdown file exists, contains all required sections (Gatekeeper, uninstall, doctor, troubleshoot), and has no broken links (can check against doc structure)

**Estimate:** XS (200-300 lines of markdown; ~100 LoC for test)

**Risk:** Screenshots of the installer UI will quickly become outdated; store them as separate files (`docs/guides/img/install-step1.png`) with a note "update when UI changes."

**Dependencies:** US-C1.2, US-C1.4 (needs the .pkg and brew formula to describe)

**DoD:** markdown exists, README.md links to it, test passes, no broken internal links

---

## Sequencing & Parallelization

```
US-C1.1 (P0) [foundational]
   ↓
US-C1.2 (P0) [build .pkg] ← also enables US-C1.4, US-C1.5
   ↓
┌─ US-C1.3 (P0) [sign + notarize] ← depends on US-C1.2
│  └─ US-C1.3a (P1) [notary diagnostics] ← sub-story of US-C1.3
│
├─ US-C1.4 (P1) [brew formula] ← depends on US-C1.2
│
├─ US-C1.5 (P1) [CI workflow] ← depends on US-C1.2, US-C1.3
│
└─ US-C1.6 (P1) [docs] ← depends on US-C1.2, US-C1.4
```

**Wave 1 (serial, blocking):**
- US-C1.1 → US-C1.2 (foundational; outputs the .pkg)

**Wave 2 (parallel, after Wave 1):**
- US-C1.3 + US-C1.3a (signing path; ~10 days)
- US-C1.4 (brew formula; ~5 days)
- US-C1.5 (CI wiring; ~5 days)
- US-C1.6 (docs; ~3 days)

**Calendar estimate:** ~3 weeks total (1 week for Wave 1, 2 weeks for Wave 2 in parallel).

---

## Non-Goals

- **Auto-update:** .pkg installs a static binary; updates require re-download and re-install.
- **Sandboxing:** Sandbox entitlements add complexity; Sprint C ships unsandboxed.
- **Multi-user:** Single-user install to /Applications assumed; no per-user daemon variants.
- **Telemetry from installer:** Telemetry (collection, upload consent) is C4 scope; installer is silent.
- **GUI installer:** CLI scripts only; UI elegance comes from the onboarding wizard (C2).

---

## Definition of Done (per story)

For each story:
- [ ] Code compiles with `-Wall -Wextra -Wpedantic -Werror`
- [ ] All acceptance criteria testable (AC test files exist and pass)
- [ ] Full test suite (`./build/human_tests`) green; no new failures
- [ ] `scripts/agent-preflight.sh` passes for changed files
- [ ] `scripts/eval-prompt-blocks.sh` still passes (persona blocks unwired)
- [ ] `/verify` agent confirms behavior
- [ ] Daemon still starts cleanly; no ASan/ubsan errors
- [ ] No commented-out code or TODO without tracking
- [ ] Commit message explains WHY (e.g., "macOS .app bundle for notarization" not "added Info.plist")

## Final Sprint Definition of Done

After all stories merge to main:
- [ ] Full test suite green (11,727 tests)
- [ ] `cmake --preset release && cmake --build` produces `/Applications/Human.app`
- [ ] `scripts/release/build-pkg.sh --output test.pkg` produces a valid .pkg file
- [ ] `.github/workflows/release-macos.yml` runs successfully (mocked notarization, or skips with clear message)
- [ ] `docs/guides/installation.md` is discoverable and complete
- [ ] Manual smoke test: download .pkg, install to /Applications, run `human --version` returns version
- [ ] Daemon starts on login via launchd (or via brew post-install)
- [ ] `human doctor` can be run post-install without errors
- [ ] No new ASan leaks; memory footprint unchanged (~6 MB resident)

---

## Open Questions for Stakeholder

1. **Apple Developer ID Certificate:** Who owns the cert? Is it stored as a GitHub secret, or will CI be run from a macOS machine with local cert access? This shapes the workflow error-handling in US-C1.5.
2. **Release Tarball Location:** Homebrew formula needs a stable URL for the release tarball. Should it point to GitHub Releases, or a separate distribution server (e.g., S3)? For Sprint C, GitHub Releases is acceptable; future sprints may have different strategy.
3. **Notarization Polling Timeout:** 15 minutes is a safe default; acceptable, or would you prefer longer?
4. **Gatekeeper Warning Content:** The installation guide should explain what the user sees. Have you verified what Gatekeeper says for unsigned-then-notarized apps? (Typically "verify…from Apple", which is safe.)

---

## Related

- `docs/plans/2026-05-25-sprint-c-backlog.md` — C1's original ~2500-word spec
- `CLAUDE.md` (M4 mission) — "Ship to Users: 100 DAU"
- `.claude/rules/test-source-gate-symmetry.md` — ensures all tests are gated correctly
- `.claude/rules/agent-task-sizing.md` — 4-5 stories is optimal for parallel dispatch

RESULT_product-owner=READY n_stories=6
