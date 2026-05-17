# Sprint 14 Backlog — Native App Ship-It

## Goal
Deliver a notarized macOS DMG and an archivable iOS IPA artifact, establishing the first non-CLI distribution path toward M4 (100 DAU, 30% day-7 retention).

## Sprint Metadata

| Field | Value |
|---|---|
| Sprint | 14 |
| Branch | `sprint-14-native-apps-ship` |
| Working dir | `apps/`, `scripts/`, `.github/workflows/native-apps-fleet.yml` |
| Base SHA | `34b34cb5` |
| Scrum-master | TBD |
| Dates | 2026-05-17 → 2026-05-28 |
| Budget cap | $20 |
| Total estimate | M + S + M + XS + M + S = ~4.5 sprint-days |
| P0 count | 2 |
| HIGH-risk count | 2 (US-14.2, US-14.3) |
| Wave structure | Wave 1: US-14.1, US-14.4 (parallel, no deps) → Wave 2: US-14.2, US-14.3 (depend on US-14.1) → Wave 3: US-14.5, US-14.6 (parallel, no blocking deps) |

---

## User Stories (priority order)

---

### US-14.1 (P0): macOS app — Xcode-project archivable and signable

**As a** release engineer,
**I want** the macOS app to produce a valid `.xcarchive` via `xcodebuild archive` with a development code-signing identity,
**so that** the notarization pipeline in US-14.2 has a signed artifact to submit.

**Acceptance criteria:**

- AC-14.1.1: GIVEN the repo is checked out on a macOS runner with Xcode 16 and a "Apple Development" cert in the keychain, WHEN `xcodebuild archive -scheme Human -configuration Release -archivePath build/Human.xcarchive` runs against `apps/macos/`, THEN the command exits 0 and `build/Human.xcarchive/Products/Applications/Human.app` exists.
- AC-14.1.2: GIVEN the archive from AC-14.1.1, WHEN `codesign --verify --deep --strict build/Human.xcarchive/Products/Applications/Human.app` runs, THEN exit code is 0 and output contains "valid on disk".
- AC-14.1.3: GIVEN the macOS `Package.swift` and the `apps/macos/` source tree, WHEN `swift build -c release` runs (existing CI step), THEN it continues to exit 0 (no regression in the release-build job).
- AC-14.1.4: GIVEN a CI run on `native-apps-fleet.yml`, WHEN the new `macos-app-archive` job runs, THEN it reports success in the `fleet-sota-gate` summary table alongside the existing `macos-app-build` row.

**Estimate:** M
**Dependencies:** none
**Risk:** MEDIUM — requires a signing identity; CI uses a development cert stored in a GitHub Actions secret; no notarization required at this story level.
**Test seam:** `.github/workflows/native-apps-fleet.yml` job `macos-app-archive`; local: `xcodebuild archive ...` + `codesign --verify`.
**DoD:** archive CI job green; `codesign --verify` exits 0; existing `macos-app-build` job unaffected; `/verify` PASS; `/aspect-panel` CLEAN (touches signing config).
**Out of scope:** notarization, DMG packaging, Hardened Runtime entitlements (those are US-14.2).

---

### US-14.2 (P0): macOS app — notarized DMG produced by release pipeline

**As a** user downloading h-uman for the first time,
**I want** a DMG that macOS Gatekeeper accepts without a "damaged or can't be opened" warning,
**so that** I can install the app without disabling security settings.

**Acceptance criteria:**

- AC-14.2.1: GIVEN `scripts/notarize-mac.sh --dry-run --app build/Human.xcarchive/Products/Applications/Human.app --bundle-id ai.human.mac`, WHEN run locally without Apple ID credentials, THEN the script exits 0, prints "DRY RUN: would call xcrun notarytool submit", and creates `build/Human.dmg` with a placeholder staple marker file `build/Human.dmg.dryrun`.
- AC-14.2.2: GIVEN a CI environment with `APPLE_ID`, `APPLE_TEAM_ID`, and `NOTARYTOOL_APP_PASSWORD` secrets set, WHEN `scripts/notarize-mac.sh` runs (no `--dry-run`), THEN it calls `xcrun notarytool submit`, polls until status is `Accepted`, calls `xcrun stapler staple`, and exits 0.
- AC-14.2.3: GIVEN the stapled `Human.dmg` from AC-14.2.2, WHEN `spctl --assess --type open --context context:primary-signature --verbose Human.dmg` runs, THEN exit code is 0 and output contains "accepted".
- AC-14.2.4: GIVEN `scripts/notarize-mac.sh --help`, WHEN run without arguments, THEN usage including `--dry-run`, `--app`, `--bundle-id`, and `--output-dmg` flags is printed and exit code is 0.
- AC-14.2.5: GIVEN the script with Hardened Runtime enabled (`--enable-hardened-runtime`), WHEN `codesign --display --verbose=4 Human.app` runs on the re-signed app, THEN output contains `flags=0x10000(runtime)`.

**Estimate:** M
**Dependencies:** US-14.1 (archive must exist before notarization)
**Risk:** HIGH — touches notarization credentials, Apple ID secrets, and Gatekeeper policy. Requires `/aspect-panel` review before merge.
**Test seam:** `scripts/notarize-mac.sh --dry-run` (local, no secrets); CI gate: `.github/workflows/release.yml` (new `notarize-mac` job, secrets-gated, runs on tag push only).
**DoD:** dry-run exits 0 locally; `spctl` accepts the stapled DMG in CI on a tag; no Apple credentials appear in CI logs; `/verify` PASS; `/aspect-panel` CLEAN.
**Out of scope:** Mac App Store submission, auto-update via Sparkle, universal binary (x86_64 + arm64 fat binary).

---

### US-14.3 (P1): iOS app — TestFlight-buildable IPA artifact

**As a** beta tester,
**I want** the iOS app to produce a valid `.ipa` from the CI pipeline,
**so that** an operator can upload it to TestFlight without any local Xcode setup.

**Acceptance criteria:**

- AC-14.3.1: GIVEN `apps/ios/` with XcodeGen 2.x installed and a "Apple Distribution" provisioning profile for `ai.human.ios` in the CI keychain, WHEN `xcodebuild archive -scheme HumaniOS -configuration Release -destination "generic/platform=iOS" -archivePath build/HumaniOS.xcarchive` runs, THEN exit code is 0 and `build/HumaniOS.xcarchive/Products/Applications/HumaniOS.app` exists.
- AC-14.3.2: GIVEN the archive from AC-14.3.1 and an `ExportOptions.plist` declaring `method=app-store`, WHEN `xcodebuild -exportArchive -archivePath build/HumaniOS.xcarchive -exportPath build/ipa -exportOptionsPlist apps/ios/ExportOptions.plist` runs, THEN exit code is 0 and `build/ipa/HumaniOS.ipa` exists and is >100 KB.
- AC-14.3.3: GIVEN `build/ipa/HumaniOS.ipa`, WHEN `unzip -p build/ipa/HumaniOS.ipa Payload/HumaniOS.app/Info.plist | plutil -p -` runs, THEN output contains `CFBundleIdentifier = "ai.human.ios"` and `CFBundleShortVersionString = "1.1.0"`.
- AC-14.3.4: GIVEN the `apps/ios/project.yml`, WHEN `xcodegen generate` runs, THEN the generated `HumaniOS.xcodeproj/project.pbxproj` contains `CODE_SIGN_STYLE = Automatic` for the `HumaniOS` target (no hardcoded signing team ID in the project file).
- AC-14.3.5: GIVEN a CI run on `native-apps-fleet.yml` after this story, WHEN the `ios-ipa-archive` job runs, THEN it uploads `HumaniOS.ipa` as a workflow artifact with 7-day retention and the `fleet-sota-gate` summary table includes an `iOS IPA archive` row.

**Estimate:** M
**Dependencies:** none (parallel to US-14.1; both depend on XcodeGen, which is already in CI)
**Risk:** HIGH — provisioning profiles, distribution certs, and `ExportOptions.plist` are signing-sensitive. Requires `/aspect-panel` review.
**Test seam:** `.github/workflows/native-apps-fleet.yml` job `ios-ipa-archive`; local smoke: `xcodebuild archive ... -destination "generic/platform=iOS"` (fails gracefully without a cert, which is expected and documented).
**DoD:** IPA artifact uploaded in CI; `plutil` confirms bundle ID and version; no provisioning profile UUIDs hardcoded in project file; existing `ios-simulator-fleet` job unaffected; `/verify` PASS; `/aspect-panel` CLEAN.
**Out of scope:** actual TestFlight upload (operator action), App Store submission, push notification entitlements, on-device device testing.

---

### US-14.4 (P1): HumanKit — public API stabilization with availability annotations

**As a** Swift developer embedding HumanKit,
**I want** every public symbol in `HumanProtocol`, `HumanClient`, and `HumanChatUI` to carry `@available(macOS 14, iOS 17, *)` annotations and a doc comment,
**so that** the compiler tells me at import time which OS versions are required and what each type/function does.

**Acceptance criteria:**

- AC-14.4.1: GIVEN `apps/shared/HumanKit/Sources/`, WHEN `swift build` runs with `-strict-concurrency=complete` added to `swiftSettings` in `Package.swift`, THEN exit code is 0 with zero new warnings about missing availability or concurrency annotations on public symbols.
- AC-14.4.2: GIVEN any public `struct`, `class`, `enum`, or `func` in `HumanProtocol`, `HumanClient`, or `HumanChatUI`, WHEN `swift-doc` or `swift package generate-documentation` runs, THEN every public symbol has a non-empty doc comment (zero "undocumented symbol" warnings).
- AC-14.4.3: GIVEN the `Methods` enum in `HumanProtocol/Methods.swift` (currently has no `@available` annotation), WHEN a consumer on macOS 13 tries to import it, THEN the compiler emits an availability error referencing the `macOS 14` minimum, not a cryptic linker error.
- AC-14.4.4: GIVEN `swift test` run against `HumanProtocolTests`, `HumanClientTests`, and `HumanChatUITests`, THEN all three test targets pass with exit code 0 (no regressions from annotation additions).

**Estimate:** S
**Dependencies:** none
**Risk:** LOW — annotation-only changes; no new logic; no signing involved.
**Test seam:** `apps/shared/HumanKit/` — `swift test` (existing CI job `human-kit-swift`); `swift build` with strict-concurrency flag added locally.
**DoD:** `swift test` green; `swift build -strict-concurrency=complete` exits 0; all public symbols have doc comments; `/verify` PASS; no `/aspect-panel` required.
**Out of scope:** adding new public APIs, language bindings (Python/Node), Swift 6 migration, `HumanOnDevice` / `HumanOnDeviceServer` targets (those have their own M3 lane).

---

### US-14.5 (P2): macOS app — first-run onboarding sheet when daemon is absent

**As a** first-time macOS user who has just opened the DMG and launched the app,
**I want** to see a setup sheet that tells me I need the `human` daemon and offers a "Install via Homebrew" button,
**so that** I can start the service without opening a terminal.

**Acceptance criteria:**

- AC-14.5.1: GIVEN `StatusViewModel.isServiceRunning == false` AND `ProcessManager.humanPath() == nil` (binary not in PATH), WHEN the app finishes launching (after a 0.5 s grace period), THEN a modal sheet titled "Welcome to h-uman" appears over the Dashboard window with a "Install via Homebrew" button and a "I'll do this later" dismiss button.
- AC-14.5.2: GIVEN the onboarding sheet is visible, WHEN the user taps "Install via Homebrew", THEN `NSWorkspace.shared.open(URL(string: "https://brew.sh")!)` is called AND a `Terminal.app`-targeting `open` command runs `brew install human/tap/human` (using the sprint-9 formula tap), and the sheet transitions to a "Waiting for installation…" state with a spinner.
- AC-14.5.3: GIVEN the onboarding sheet is in the "Waiting" state, WHEN `ProcessManager.humanPath()` returns non-nil (polled every 2 s), THEN the sheet dismisses automatically and `StatusViewModel.startService()` is called.
- AC-14.5.4: GIVEN a user who has previously dismissed the sheet or completed setup, WHEN the app launches on subsequent runs, THEN the sheet does NOT appear (persisted via `UserDefaults` key `onboardingCompleted`).
- AC-14.5.5: GIVEN the onboarding sheet, WHEN VoiceOver reads the "Install via Homebrew" button, THEN the accessibility label is "Install h-uman daemon via Homebrew package manager".

**Estimate:** M
**Dependencies:** Sprint-9 Homebrew formula (formula tap `human/tap/human` must exist; this story does not create it — it references it)
**Risk:** MEDIUM — calls out to Terminal / brew; must not run real network in test (`HU_IS_TEST` equivalent for Swift: compile-time flag `#if UITEST_SKIP_ONBOARDING`, matching the existing `-uitestSkipOnboarding` launch argument pattern already in `apps/ios/CLAUDE.md`).
**Test seam:** `apps/macos/Sources/HumanApp/OnboardingSheet.swift` (new file); XCTest unit test `MacOnboardingTests.swift` using a mock `ProcessManager` protocol; UI automation skipped via `-uitestSkipOnboarding` launch arg (mirrors iOS pattern).
**DoD:** unit tests green; onboarding does not appear when `-uitestSkipOnboarding` is passed; `swift build -c release` unaffected; `/verify` PASS; no `/aspect-panel` required.
**Out of scope:** daemon installation on non-Homebrew systems, Linux, Windows; actually running the brew install (operator action); the daemon itself (sprint-9 owns that).

---

### US-14.6 (P2): Release metadata stub — entitlements and Fastfile bundle-ID declaration

**As a** release manager preparing the first App Store / notarization submission,
**I want** a single checked-in `apps/macos/Human.entitlements` file and a minimal `apps/Fastfile` that declare the canonical bundle IDs, entitlements, and version,
**so that** I can run a release without hunting through Xcode project settings or asking an engineer.

**Acceptance criteria:**

- AC-14.6.1: GIVEN `apps/macos/Human.entitlements`, WHEN `plutil -lint apps/macos/Human.entitlements` runs, THEN exit code is 0 (valid plist format) and the file contains at minimum `com.apple.security.app-sandbox = true` and `com.apple.security.network.client = true`.
- AC-14.6.2: GIVEN `apps/Fastfile`, WHEN `bundle exec fastlane --version` is available (Fastlane installed), THEN `bundle exec fastlane lanes` lists at minimum two lanes: `mac_archive` and `ios_archive`, each with a comment documenting the expected environment variables (`APPLE_TEAM_ID`, `MATCH_PASSWORD` or equivalent).
- AC-14.6.3: GIVEN `apps/Fastfile`, WHEN `grep -n 'PRODUCT_BUNDLE_IDENTIFIER\|bundle_id' apps/Fastfile` runs, THEN output contains `ai.human.mac` for the macOS target and `ai.human.ios` for the iOS target (no placeholder values like `com.example`).
- AC-14.6.4: GIVEN `apps/macos/Human.entitlements` references `com.apple.security.hardened-runtime` is absent (not set), WHEN the notarization script in US-14.2 reads the entitlements file, THEN it adds `--entitlements apps/macos/Human.entitlements` to the `codesign` re-sign step.

**Estimate:** S
**Dependencies:** none (informational dependency on US-14.2 for the entitlements hookup, but can be authored independently)
**Risk:** MEDIUM — entitlements files affect sandbox and network permissions; wrong values block notarization. Requires `/aspect-panel` review before merge.
**Test seam:** `plutil -lint` in CI pre-flight (add to `.github/workflows/native-apps-fleet.yml` as a 30-second lint job); `grep` assertions in CI script.
**DoD:** `plutil -lint` exits 0; `grep` confirms bundle IDs; Fastfile lanes listed; `/verify` PASS; `/aspect-panel` CLEAN (touches signing/entitlements).
**Out of scope:** Fastlane Match cert management, actual lane execution end-to-end, Android `build.gradle` metadata (different sprint lane), marketing screenshots or App Store descriptions.

---

## Non-goals

- Android Play Store submission or AAB signing — multi-sprint epic, deferred.
- Universal (x86_64 + arm64) fat binary for macOS — deferred.
- iOS device deployment or actual TestFlight upload — operator action, not code.
- Marketing assets, App Store screenshots, or copy — content sprint.
- Daemon protocol changes or `src/` C code — sprint-7 lane.
- Fastlane Match cert provisioning — requires Apple Developer account access; operator setup.

## Open questions for stakeholder

1. Does the team have an Apple Developer account with a notarization-capable Apple ID ready for CI secrets (`APPLE_ID`, `NOTARYTOOL_APP_PASSWORD`)? US-14.2 CI leg is blocked without it; dry-run works regardless.
2. The sprint-9 Homebrew tap formula name — is it `human/tap/human` or a different tap path? US-14.5 references it but does not create it.
3. Should the macOS app bundle ID be `ai.human.mac` (matching the iOS convention in `project.yml`) or `ai.human` (simpler)? This affects US-14.6 and notarization.
