# Critic findings — US-14.6 (Entitlements + Fastfile + Bundle ID)

**Branch:** impl/US-14.6
**Commit:** 5d226268
**Date:** 2026-05-17
**Critic:** claude-sonnet-4-6

---

## HIGH (2)

- `apps/macos/Human.entitlements` + `.github/workflows/native-apps-fleet.yml:51` — The negative-grep pattern in CI blocks 4 sandbox-escape keys but omits `com.apple.security.cs.allow-jit`, `com.apple.security.cs.debugger`, `com.apple.security.cs.disable-executable-page-protection` (already blocked under a different key name), and every `com.apple.security.temporary-exception.*` variant. A future contributor adding `com.apple.security.temporary-exception.files.absolute-path.read-write` or `com.apple.security.cs.allow-jit` would pass CI with no warning. The CI negative-grep pattern must be extended to cover `temporary-exception` and `cs.allow-jit` and `cs.debugger`. Fix: add `|temporary-exception|allow-jit|cs.debugger` to the pattern on line 51.

- `apps/Fastfile:41-47` and `apps/Fastfile:59-65` — `UI.user_error!` raises an exception and terminates the fastlane process when the delegate script is absent. This is the correct behavior for a required dependency, but the comment on line 8 says "missing values cause the underlying helper to fail loudly, not this Fastfile" — which is contradicted by the actual code that raises inside the Fastfile. The discrepancy means whoever reads the comment will believe the lane runs past the missing script; in practice the lane aborts. The comment must be corrected or the behavior changed to match. As written the behavior is correct (fail loud), but the comment is wrong and will mislead the US-14.2/14.3 implementers when they wire up those scripts. Fix: delete or correct the comment on lines 7-9.

---

## MED (2)

- `apps/macos/project.yml:34` — `CODE_SIGN_ENTITLEMENTS: Human.entitlements` is a bare relative path. XcodeGen resolves this relative to the project.yml's directory (`apps/macos/`), which is correct when xcodegen runs with `working-directory: apps/macos` (as CI does on line 237). However the xcodebuild invocation on line 253 runs with `working-directory: apps/macos` too — but if a developer runs `xcodebuild` from the repo root (e.g. `xcodebuild -project apps/macos/Human.xcodeproj …`) the generated xcodeproj will have the relative path baked in as `Human.entitlements`, which resolves to `$(SRCROOT)/Human.entitlements` inside the xcodeproj — correct because XcodeGen generates the path relative to SRCROOT. No actual breakage today, but the path resolution relies on XcodeGen's generated xcodeproj; if someone edits the xcodeproj directly and relocates the entitlements key, it silently breaks. Not actionable as a code fix, but the comment on line 43 should note the XcodeGen resolution model.

- `apps/fastlane/Appfile:13` — The array literal `app_identifier(["ai.human.mac", "ai.human.ios"])` is used by fastlane's `produce` / `deliver` actions as a fallback when no platform block matches. App Store Connect treats macOS and iOS as separate platform records under the same bundle ID. If a future lane calls `produce` without entering a `platform` block (e.g. from a top-level lane outside `platform :mac do`), fastlane will pick the first element of the array (`ai.human.mac`) for whichever platform the runner happens to be. The `for_platform` blocks immediately below are the correct override, but the array fallback is a latent footgun. Fix: replace the array literal with `app_identifier "ai.human.mac"` as the mac default (since `default_platform(:mac)` is set in Fastfile) and document why.

---

## LOW (1)

- `apps/macos/project.yml:42` — `INFOPLIST_KEY_NSHumanReadableCopyright: "Copyright Human Labs"` uses the old "Human Labs" entity name while the product thesis uses "human" (lowercase) throughout all other files. Minor brand inconsistency; fix when the legal entity name is confirmed.

---

## Cross-agent regression risk

- `resources/human.entitlements` is used by `scripts/deploy.sh:100` (daemon codesign, identifier `ai.human.daemon`). This file is entirely separate from `apps/macos/Human.entitlements` and the US-14.6 change does not touch it. No cross-contamination confirmed. However: any agent working on US-14.2 (notarize DMG) that greps for `*.entitlements` will find both files; they must not conflate them. The CI entitlements-check job hard-codes `apps/macos/Human.entitlements` so the daemon file is not scanned — this is correct and intentional per the impl note.

- `apps/Fastfile` is shared by US-14.1 (macOS archive), US-14.2 (notarize), and US-14.3 (iOS archive). The stub lanes raise `UI.user_error!` until the delegate scripts land. Any agent implementing US-14.2 or US-14.3 that runs `fastlane mac_archive` or `fastlane ios_archive` to smoke-test their work will get a hard failure, not a skip. This is the intended behavior but must be documented in the US-14.2/14.3 task descriptions so those agents do not interpret the error as a regression introduced by their own work.

---

RESULT_critic=HAS_FINDINGS story=US-14.6 severity=HIGH
