# Sprint 45 Backlog — "Native App Ship-It"

## Goal
Produce a notarized macOS DMG and a TestFlight-buildable iOS archive from
the existing `apps/` scaffold, closing the last distribution gap blocking
strategic mission M4 without requiring App Store submission.

---

## User Stories (in priority order)

### US-45.1 (P0): macOS Archive + Dev-Cert Codesign

**As a** CI engineer,
**I want** `apps/macos/project.yml` to produce a valid Xcode archive signed
with an Apple Development identity,
**so that** the macOS app can be built and codesigned in CI without a
manually maintained Xcode project file.

**Acceptance criteria:**

- AC-45.1.1: GIVEN the worktree contains no `.xcodeproj` for `apps/macos`,
  WHEN `xcodegen generate` is run from `apps/macos/`, THEN a `.xcodeproj`
  is produced and `.gitignore` excludes it (the generated file must not
  appear in `git status`).
- AC-45.1.2: GIVEN the `MACOS_CERT_P12` and `MACOS_CERT_PASSWORD` secrets
  are present, WHEN `scripts/ci/import-macos-cert.sh` is sourced, THEN it
  creates an ephemeral keychain, imports the cert, and an EXIT trap shreds
  that keychain; `set -euo pipefail` is the first non-comment line.
- AC-45.1.3: GIVEN secrets are present, WHEN the CI `macos-app-archive` job
  runs `xcodebuild archive` with `CODE_SIGN_IDENTITY="Apple Development"`,
  THEN the job exits 0 and a subsequent `codesign --verify --deep --strict`
  on the `.app` bundle exits 0; `--deep` is NOT passed to the `xcodebuild
  archive` invocation itself.
- AC-45.1.4: GIVEN secrets are absent (fork PR), WHEN the CI job runs,
  THEN it falls back to an unsigned `swift build -c release` smoke build
  that exits 0; the job does not fail or skip.
- AC-45.1.5: GIVEN the archive job passes, WHEN `native-apps-fleet.yml`
  runs the `fleet-sota-gate` aggregation job, THEN the gate treats
  `macos-app-archive` as a required check alongside the existing
  `macos-app-build` job.

**Estimate:** M
**Risk:** MEDIUM
**Dependencies:** none
**DoD:** archive build green in CI; `codesign --verify` exits 0; /verify PASS;
/aspect-panel CLEAN
**Test seam:** `macos-app-archive` CI job step `codesign --verify --deep --strict`
**Out of scope:** Hardened runtime, notarization, stapling (US-45.2)

---

### US-45.2 (P0): Notarized DMG

**As a** release engineer,
**I want** `scripts/notarize-mac.sh` to produce a stapled, notarized DMG
that passes `spctl --assess`,
**so that** macOS Gatekeeper allows users to open the app without a security
warning on first launch.

**Acceptance criteria:**

- AC-45.2.1: GIVEN `--dry-run` is passed, WHEN `scripts/notarize-mac.sh` is
  invoked, THEN it prints each intended step (create DMG, submit, wait,
  staple, spctl check) and exits 0 without contacting Apple; this path runs
  on every PR via a `notarize-dmg-dryrun-smoke` CI step.
- AC-45.2.2: GIVEN real-mode invocation with valid credentials, WHEN the
  script executes, THEN the Apple ID / password block is wrapped in
  `set +x` ... `set -x` brackets so credentials never appear in CI logs;
  `set -euo pipefail` is the first non-comment line; an EXIT trap removes
  any temp DMG staging directory regardless of exit code.
- AC-45.2.3: GIVEN the notarytool submit/wait cycle succeeds, WHEN
  `xcrun stapler staple` fails transiently, THEN the script retries exactly
  once; if the second attempt also fails the script exits with a distinct
  non-zero exit code (not 1) documented in a header comment.
- AC-45.2.4: GIVEN a notarized, stapled DMG, WHEN `spctl --assess --type execute`
  is run on the MOUNTED inner `.app` (not `--type open` on the outer DMG),
  THEN the command exits 0 and the script records this as its final step.
- AC-45.2.5: GIVEN hardened runtime enabled (`ENABLE_HARDENED_RUNTIME=YES`)
  in real-mode, WHEN `codesign -dv` is run on the `.app`, THEN the output
  contains `runtime` in the flags field; the CI `notarize-dmg` job is gated
  on a release tag and does NOT run on ordinary PRs.

**Estimate:** L
**Risk:** HIGH — signing credentials in CI, Apple notarization API latency,
staple transience; /aspect-panel mandatory before merge
**Dependencies:** US-45.1 (archive must exist before DMG creation),
US-45.6 (entitlements must be correct before hardened-runtime sign)
**DoD:** dry-run smoke green on every PR; real-mode exits 0 with stapled
artifact on tagged release; /verify PASS; /aspect-panel CLEAN
**Test seam:** `notarize-dmg-dryrun-smoke` CI step on all PRs; `spctl --assess`
on mounted `.app` in real-mode job
**Out of scope:** Sparkle auto-update, DMG visual background/layout,
distribution via Mac App Store

---

### US-45.3 (P0): iOS Archive + IPA Export

**As a** release engineer,
**I want** `scripts/ios-archive-export.sh` to produce a signed IPA from the
existing `apps/ios/` Xcode project,
**so that** the build artifact can be submitted to TestFlight by an operator
without manual Xcode GUI steps.

**Acceptance criteria:**

- AC-45.3.1: GIVEN `apps/ios/ExportOptions.plist` exists with `__TEAM_ID__`
  and `__PROVISIONING_PROFILE_UUID__` as literal placeholders, WHEN
  `scripts/ios-archive-export.sh` runs, THEN it substitutes both tokens via
  `sed -i bak` and an EXIT trap restores the original file; the restoration
  step must not suppress errors with `2>/dev/null`.
- AC-45.3.2: GIVEN the archive step completes, WHEN `plutil` asserts on the
  `.xcarchive`'s own `Info.plist`, THEN both
  `CFBundleIdentifier=ai.human.ios` and
  `CFBundleShortVersionString=1.1.0` are verified; these assertions fire even
  when `SKIP_EXPORT=1` is set so the archive itself is always validated.
- AC-45.3.3: GIVEN signing secrets are absent (fork PR), WHEN the CI
  `ios-archive` job runs with `SKIP_EXPORT=1`, THEN the archive step succeeds
  with `CODE_SIGNING_ALLOWED=NO` and the plist assertions in AC-45.3.2 still
  execute and pass.
- AC-45.3.4: GIVEN a completed IPA export, WHEN `scripts/check-no-provisioning-leak.sh`
  is run on the IPA contents, THEN it exits non-zero if any file contains a
  UUID matching both the dashed (`xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`) and
  non-dashed (`[0-9A-Fa-f]{32}`) UUID patterns injected from
  `__PROVISIONING_PROFILE_UUID__`; this script runs in `.githooks/pre-commit`
  AND in the CI job both before AND after archive.
- AC-45.3.5: GIVEN the CI `ios-archive` job, WHEN it runs on a PR touching
  `apps/ios/**` or `apps/shared/**`, THEN it completes within 20 minutes and
  the job is listed as a required check in `fleet-sota-gate`.

**Estimate:** L
**Risk:** HIGH — provisioning profile substitution, UUID leak risk,
platform-specific CI runners; /aspect-panel mandatory before merge
**Dependencies:** US-45.1 (cert import script reused by both platforms),
US-45.6 (bundle IDs must match entitlements)
**DoD:** archive + plist assertions green; provisioning-leak check green
pre- and post-archive; /verify PASS; /aspect-panel CLEAN
**Test seam:** `plutil` assertions on `.xcarchive/Info.plist`;
`check-no-provisioning-leak.sh` in pre-commit + CI
**Out of scope:** Actual TestFlight upload (operator action via `xcrun altool`),
Android archive, iPadOS-specific layout changes

---

### US-45.4 (P1): HumanKit @available + Swift 6 Concurrency

**As a** Swift developer consuming HumanKit,
**I want** all public types annotated with `@available(macOS 14.0, iOS 17.0, *)`
and the package built with `swiftLanguageMode: .v6`,
**so that** callers receive availability diagnostics at compile time and
data-race safety is enforced by the compiler rather than discovered at
runtime.

**Acceptance criteria:**

- AC-45.4.1: GIVEN `Package.swift` is updated to add `swiftLanguageMode: .v6`
  to every non-test target, WHEN `swift build` is run inside
  `apps/shared/HumanKit/`, THEN it exits 0 with zero warnings about
  `Sendable` conformances; `@unchecked Sendable` must not appear in any
  diff produced by this story.
- AC-45.4.2: GIVEN a top-level public type in any HumanKit target lacks an
  `@available` annotation, WHEN a consumer targets macOS 13 or iOS 16,
  THEN the compiler emits an availability error (not a warning); the CI
  `human-kit-swift` job must remain green after this change.
- AC-45.4.3: GIVEN a genuine concurrency race is surfaced by the Swift 6
  compiler, WHEN the engineer addresses it, THEN the fix uses structured
  concurrency (`async`/`await`, `actor`, `Sendable`) rather than
  `@unchecked Sendable`; any such fix must be accompanied by a new
  `XCTestCase` method that exercises the concurrent access path.
- AC-45.4.4: GIVEN `swiftLanguageMode: .v6` in `HumanKit/Package.swift`,
  WHEN HumanKit is consumed as a SwiftPM dependency from
  `apps/macos/Package.swift` or `apps/ios/project.yml`, THEN `swift build`
  in the consuming package exits 0 with no additional flags; specifically,
  `unsafeFlags(-strict-concurrency=complete)` must not appear in any
  target's `swiftSettings`.

**Estimate:** M
**Risk:** MEDIUM — Swift 6 strict concurrency may surface real races requiring
non-trivial refactors
**Dependencies:** none (pure Swift package change)
**DoD:** `swift build` + `swift test` green in `apps/shared/HumanKit/` and in
both consuming packages; zero `@unchecked Sendable`; /verify PASS
**Test seam:** CI `human-kit-swift` job (`swift test` in `apps/shared/HumanKit/`)
**Out of scope:** Objective-C interop annotations, watchOS/tvOS availability
macros, migration of `apps/macos` or `apps/ios` app-layer sources

---

### US-45.5 (P1): macOS First-Run OnboardingSheet

**As a** new macOS user launching h-uman for the first time,
**I want** an onboarding sheet to appear that guides me through initial setup
and cannot be dismissed accidentally,
**so that** I complete the minimum configuration before reaching the main
dashboard.

**Acceptance criteria:**

- AC-45.5.1: GIVEN `UserDefaults` key `humanOnboardingComplete` is absent or
  `false`, WHEN `HumanApp` launches, THEN an `OnboardingSheet` is presented
  with `.interactiveDismissDisabled(true)` so Escape and click-outside gestures
  do not dismiss it.
- AC-45.5.2: GIVEN the onboarding sheet is shown and the user activates the
  completion action, WHEN `NSWorkspace.shared.open(_:configuration:completionHandler:)`
  returns in its completion handler with a non-nil `NSRunningApplication`,
  THEN and only then is `UserDefaults.standard.set(true, forKey: "humanOnboardingComplete")`
  called; if the handler receives `nil` or an error the flag is not set and
  the sheet remains visible.
- AC-45.5.3: GIVEN `OnboardingURLs.swift` defines `static let` constants for
  all URLs opened during onboarding, WHEN a URL constant is changed, THEN no
  onboarding URL string literal exists anywhere else in
  `apps/macos/Sources/HumanApp/`; this is enforced by a `grep` assertion in
  the `macos-app-archive` CI step.
- AC-45.5.4: GIVEN a `#if DEBUG` launch argument `-uitestSkipOnboarding` is
  passed, WHEN the app starts in an XCUITest run, THEN the sheet is not
  presented and `humanOnboardingComplete` is not mutated; this guard must not
  compile into release builds (`#if DEBUG` not a runtime flag check).
- AC-45.5.5: GIVEN an XCUITest, WHEN the test pre-seeds
  `humanOnboardingComplete=true` before launch, THEN the sheet is absent from
  the accessibility hierarchy on first frame; a second XCUITest pre-seeds
  `false`, activates the skip action, relaunches, and asserts the sheet is
  absent — covering both the READ and WRITE sides of the completion flag.

**Estimate:** M
**Risk:** MEDIUM — async completion handler + flag write ordering is a common
source of race conditions in onboarding flows
**Dependencies:** US-45.1 (the `macos-app-archive` CI job that hosts the URL
grep assertion and XCUITest run)
**DoD:** XCUITest READ + WRITE cases green; no onboarding URL string literals
outside `OnboardingURLs.swift`; /verify PASS
**Test seam:** XCUITest cases in `apps/macos/` exercising both READ and WRITE
sides of `humanOnboardingComplete`
**Out of scope:** iOS onboarding surface, analytics events for onboarding
completion, A/B variant of onboarding copy

---

### US-45.6 (P0): Entitlements + Fastfile + Bundle ID

**As a** release engineer,
**I want** `apps/macos/Human.entitlements`, `apps/Fastfile`, and
`apps/fastlane/Appfile` to define the minimal correct entitlements and lane
definitions for both platforms,
**so that** CI and local release engineers can invoke a single lane command
to produce archive artifacts without manual Xcode configuration.

**Acceptance criteria:**

- AC-45.6.1: GIVEN `apps/macos/Human.entitlements`, WHEN
  `codesign -d --entitlements -` is run on the archived `.app`, THEN the
  output contains `com.apple.security.app-sandbox` and
  `com.apple.security.network.client` and does NOT contain any of:
  `com.apple.security.cs.disable-library-validation`,
  `com.apple.security.cs.allow-unsigned-executable-memory`,
  `com.apple.security.cs.disable-executable-page-protection`,
  `com.apple.security.cs.allow-dyld-environment-variables`,
  `com.apple.security.cs.allow-jit`,
  `com.apple.security.cs.debugger`, or any key matching
  `com.apple.security.temporary-exception.*`.
- AC-45.6.2: GIVEN the CI `entitlements-check` job, WHEN it runs a
  negative-grep over `Human.entitlements`, THEN it exits non-zero if any
  key from the block-list in AC-45.6.1 is present; this job runs on every
  PR touching `apps/macos/`.
- AC-45.6.3: GIVEN `apps/fastlane/Appfile`, WHEN parsed, THEN it contains
  exactly one `app_identifier` entry per platform section (`ai.human.mac`
  for macOS, `ai.human.ios` for iOS) as a string scalar, not as an array
  literal; `fastlane lanes` exits 0 without syntax errors.
- AC-45.6.4: GIVEN `apps/Fastfile` defines `mac_archive` and `ios_archive`
  lanes, WHEN `fastlane mac_archive` is invoked with `skip_codesign:true`,
  THEN it calls `xcodebuild archive` with the correct scheme and exits 0;
  when invoked without `skip_codesign:true` it calls the cert-import script
  before archiving.
- AC-45.6.5: GIVEN `apps/ios/project.yml` already declares
  `PRODUCT_BUNDLE_IDENTIFIER: ai.human.ios`, WHEN XcodeGen regenerates the
  project, THEN `grep -r 'ai\.human\.ios' apps/ios/HumaniOS.xcodeproj` finds
  the identifier and `grep -r 'ai\.human\.mac' apps/macos/` finds the macOS
  counterpart; no other bundle ID variants exist in either project tree.

**Estimate:** S
**Risk:** MEDIUM-HIGH — entitlement mistakes silently break hardened runtime
notarization; /aspect-panel mandatory before merge
**Dependencies:** US-45.1 (project.yml must exist before entitlements are
applied to the generated xcodeproj),
US-45.2 (entitlements are a prerequisite for hardened runtime in notarization)
**DoD:** `entitlements-check` CI job green; `fastlane lanes` exits 0;
/verify PASS; /aspect-panel CLEAN
**Test seam:** CI `entitlements-check` negative-grep job; `fastlane lanes`
smoke in CI
**Out of scope:** App Store Connect API key management, push notification
entitlements, iCloud entitlements, any `temporary-exception.*` key for any
reason

---

## Non-goals
- We will NOT submit to the Mac App Store or iOS App Store this sprint.
- We will NOT touch `src/*`, `Formula/*`, or `bindings/*` (other sprint scopes).
- We will NOT produce marketing screenshots, preview videos, or App Store copy.
- We will NOT change the daemon protocol or any C runtime code.
- We will NOT implement Android archive or Play Store distribution.

## Open questions for stakeholder
- None. All design decisions are specified in the task prompt with explicit
  defaults; no stakeholder input is required to begin sprint execution.

RESULT_product-owner=READY
