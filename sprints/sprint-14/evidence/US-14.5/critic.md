# Critic findings — US-14.5 macOS first-run onboarding sheet

Branch: impl/US-14.5
Commit: 873178e2
Critic run: 2026-05-17

---

## HIGH (2)

- `apps/macos/Sources/HumanApp/OnboardingState.swift:105` — `NSWorkspace.shared.open` is called
  before `markCompleted()`, so if the OS fails to open the URL (browser not installed, sandbox
  denial, URL unreachable), the sheet still dismisses and the UserDefaults flag is still set.
  The user ends up with no browser window and no recourse — the sheet will never reappear.
  Fix: call `markCompleted()` only inside the `NSWorkspace.shared.open(_:configuration:completionHandler:)`
  completion handler; use the async overload and set the flag only on a non-nil `NSRunningApplication`
  response. If that API is unavailable at the target deployment target, at minimum reverse the call
  order so `open` fires first and `markCompleted` fires last — the current order is correct but
  wrapping in the async API makes the failure observable.

- `apps/macos/HumanUITests/OnboardingTests.swift` — AC-14.5.3 ("Skip persists flag") is listed
  as a verified acceptance criterion in the design and in the verifier evidence, but no test in
  `OnboardingTests.swift` actually taps the skip CTA and relaunches the app to assert the sheet
  stays dismissed. `test_onboardingSheet_stays_dismissed_after_completion_persisted` pre-seeds the
  UserDefaults flag via a launch argument — it never exercises the CTA tap path that writes the
  flag. The test covers the read side of the contract; the write side (skip button → flag set →
  persists across relaunch) is untested at the XCUITest level. Fix: add a test that taps
  `app.buttons[AXID.skipCTA]`, calls `app.terminate()`, relaunches without the `-completed YES`
  seed, and asserts the sheet does not appear.

---

## MED (3)

- `apps/macos/Sources/HumanApp/OnboardingState.swift:37` — `uitestSkipLaunchArg` is a `static let`
  in a production class with no `#if DEBUG` guard. Any process that receives `-uitestSkipOnboarding`
  as a launch argument — including a production app launched by an automation script, a Shortcuts
  action, or a malicious helper that prepends arguments — silently suppresses the onboarding gate
  forever (for that process invocation). The iOS precedent cited in the design has the same
  pattern, but that does not make it safe. Fix: wrap the `arguments.contains` branch in
  `#if DEBUG || targetEnvironment(simulator)` so release builds ignore the flag entirely.

- `apps/macos/Sources/HumanApp/HumanApp.swift:26-28` — the `.sheet(isPresented:)` binding's `set`
  closure calls `onboarding.markCompleted()` whenever `newValue == false`. SwiftUI can call this
  setter when the sheet is dismissed by any means — including a system gesture (swiping down on
  a trackpad, pressing Escape, or a programmatic `dismiss()` from a child view). This means a
  user who accidentally dismisses the sheet via Escape before reading it silently sets the
  `humanInstallationCompleted` flag and never sees the sheet again. The design acknowledges
  the "already installed, click Skip" hint in the sheet body, but the Escape-dismissal path
  bypasses even that affordance. Fix: remove `markCompleted()` from the `set` closure; let the
  two explicit CTA buttons remain the only write paths. Use `.interactiveDismissDisabled(true)` to
  prevent swipe/Escape dismissal, or accept that non-CTA dismissal is intentional "skip" and
  document it explicitly.

- `apps/macos/HumanUITests/OnboardingTests.swift` — the skip CTA's `accessibilityLabel` ("Skip
  h-uman daemon setup for now") has no corresponding assertion in any test. The install CTA label
  is pinned by `test_installCTA_accessibility_label_matches_voiceover_copy`, but the skip CTA
  label is only verified to exist (not for its text). A future rename of the button label
  silently breaks VoiceOver narration without any CI signal. Fix: add an assertion equivalent
  to the install CTA test for the skip CTA label.

---

## LOW (1)

- `apps/macos/Sources/HumanApp/OnboardingURLs.swift:22` — `URL(string:)!` force-unwrap on a
  hardcoded literal. The string is correct today and the design explicitly calls this acceptable,
  but the force-unwrap gives no diagnostic if the string is ever edited to a malformed value.
  Fix: add a `precondition` or a `#if DEBUG` `assert` with a descriptive message, or use
  `URL(staticString:)` (available in Foundation for literal URLs that are guaranteed valid at
  compile time) to make the contract self-documenting.

---

## Cross-agent regression risk

None identified. US-14.5 touches only `apps/macos/` Swift files and adds no shared C headers,
vtable interfaces, or provider/channel registrations. The C test suite (`./build/human_tests`)
is unaffected. No other in-flight worktree in the sprint-14 wave (`US-14.1`, `US-14.2`,
`US-14.3`, `US-14.4`, `US-14.6`) shares the modified files.

---

RESULT_critic=HAS_FINDINGS story=US-14.5 severity=HIGH
