// OnboardingTests.swift
//
// XCUITest coverage for the macOS first-run OnboardingSheet (US-14.5).
//
// Per `tests-that-pin-bugs.md`: assertions name positive observable behaviour.
// When we expect the sheet to appear we assert `XCTAssertTrue(sheet.exists)`;
// when we expect it absent we assert `XCTAssertFalse(sheet.exists)`. The test
// names describe the contract the production code MUST satisfy — they do not
// describe the buggy state we want to lock in.
//
// We control the three inputs to `OnboardingState.shouldShow` from the test
// harness:
//
//   1. `-uitestForceShowOnboarding` — production code does NOT honour this;
//      we instead manipulate the other two inputs.
//   2. `-com.apple.CommandLineUITestArguments` — XCUITest passes
//      `app.launchArguments` straight through to `ProcessInfo.processInfo.arguments`.
//   3. `app.launchEnvironment` — used to set the `humanInstallationCompleted`
//      UserDefaults key via the standard `-AppleArgumentName value` pattern.
//
// We can't trivially manipulate `humanPath()` from a test process — instead
// the production gate is satisfied either by ensuring the daemon is NOT in
// the test simulator's PATH (default) or by pointing PATH at an empty dir.

import XCTest

/// Accessibility identifiers MUST stay in sync with `OnboardingSheet.AXID`
/// in `apps/macos/Sources/HumanApp/OnboardingSheet.swift`. They are
/// duplicated here as string literals (rather than imported) because
/// XCUITest targets do not link the app's Swift module. Drift between
/// the two is a CI failure surface — a test that can't find the sheet
/// is the signal.
private enum AXID {
    static let root = "onboarding.sheet.root"
    static let installCTA = "onboarding.cta.install"
    static let skipCTA = "onboarding.cta.skip"
}

/// Launch-arg + UserDefaults key names. Must stay in sync with
/// `OnboardingState.completedDefaultsKey` and
/// `OnboardingState.uitestSkipLaunchArg`.
private enum LaunchKeys {
    static let completedDefaultsKey = "humanInstallationCompleted"
    static let uitestSkipLaunchArg = "-uitestSkipOnboarding"
}

final class OnboardingTests: XCTestCase {
    private var app: XCUIApplication!

    private enum Timeout {
        static let launch: TimeInterval = 30
        static let sheet: TimeInterval = 8
        static let absence: TimeInterval = 3
    }

    override func setUpWithError() throws {
        continueAfterFailure = false
        app = XCUIApplication()
    }

    override func tearDownWithError() throws {
        app = nil
    }

    /// Common launch helper. Resets `humanInstallationCompleted` to `false`
    /// via the `-humanInstallationCompleted NO` UserDefaults argument so
    /// each test starts from a deterministic state. Tests that want to
    /// pre-seed `completed = true` pass `completed: true` explicitly.
    private func launch(
        completed: Bool = false,
        skipOnboarding: Bool = false,
        pathOverride: String? = "/var/empty"
    ) {
        app.launchArguments = [
            "-\(LaunchKeys.completedDefaultsKey)", completed ? "YES" : "NO",
        ]
        if skipOnboarding {
            app.launchArguments.append(LaunchKeys.uitestSkipLaunchArg)
        }
        // Force `ProcessManager.humanPath()` to return nil by pointing
        // PATH at a directory that cannot contain the `human` binary. This
        // is the simplest way to exercise the "daemon missing" branch on
        // a CI runner that may have installed the binary for other tests.
        if let pathOverride {
            app.launchEnvironment["PATH"] = pathOverride
        }
        app.launch()
        XCTAssertTrue(
            app.wait(for: .runningForeground, timeout: Timeout.launch),
            "App should reach foreground on launch"
        )
    }

    private var onboardingSheet: XCUIElement {
        app.otherElements[AXID.root]
    }

    // MARK: - AC-14.5.1 (sheet shown on cold launch when daemon missing)

    /// Positive observable: with PATH stripped of the daemon and no completed
    /// flag, the onboarding sheet's root element exists after launch. If this
    /// test ever asserts `sheet.exists == false`, the contract documented by
    /// the test name has been inverted — that would pin the bug, not the fix.
    func test_onboardingSheet_appears_on_cold_launch_when_daemon_missing() throws {
        launch(completed: false, skipOnboarding: false)
        XCTAssertTrue(
            onboardingSheet.waitForExistence(timeout: Timeout.sheet),
            "OnboardingSheet should appear when humanPath() == nil and humanInstallationCompleted == false"
        )
        // Both CTAs must be visible and reachable.
        XCTAssertTrue(app.buttons[AXID.installCTA].exists,
                      "Install CTA must be rendered")
        XCTAssertTrue(app.buttons[AXID.skipCTA].exists,
                      "Skip CTA must be rendered")
    }

    // MARK: - AC-14.5.4 (sheet does NOT appear after completion / skip)

    /// Positive observable: with `humanInstallationCompleted = true` seeded
    /// in defaults, the sheet does NOT appear. We assert the sheet root
    /// element does NOT exist within the timeout — phrasing the contract as
    /// "absence within bound" rather than "presence" matches the production
    /// behaviour we want to lock in (no surprise sheet for returning users).
    func test_onboardingSheet_stays_dismissed_after_completion_persisted() throws {
        launch(completed: true, skipOnboarding: false)
        // Give SwiftUI a moment to evaluate `.sheet(isPresented:)`; the sheet
        // must not appear during that window.
        let appeared = onboardingSheet.waitForExistence(timeout: Timeout.absence)
        XCTAssertFalse(appeared, "Sheet must not appear when humanInstallationCompleted == true")
    }

    // MARK: - AC-14.5.5 (accessibility label on install CTA)

    /// AC-14.5.5: VoiceOver reads the install CTA as "Install h-uman daemon
    /// via Homebrew package manager". XCUITest exposes the
    /// `accessibilityLabel` as the element's `label` property; assert that
    /// the literal copy is wired so a future renaming of the button's
    /// visible label doesn't silently break VoiceOver narration.
    func test_installCTA_accessibility_label_matches_voiceover_copy() throws {
        launch(completed: false, skipOnboarding: false)
        XCTAssertTrue(onboardingSheet.waitForExistence(timeout: Timeout.sheet),
                      "Sheet must appear so we can inspect the install CTA")
        let installCTA = app.buttons[AXID.installCTA]
        XCTAssertTrue(installCTA.exists, "Install CTA must be present")
        XCTAssertEqual(
            installCTA.label,
            "Install h-uman daemon via Homebrew package manager",
            "Install CTA's VoiceOver label must match AC-14.5.5 exactly"
        )
    }

    // MARK: - launch-arg gate (XCUITest opt-out, mirrors iOS pattern)

    /// Positive observable: passing `-uitestSkipOnboarding` short-circuits
    /// `shouldShow` to `false` regardless of the other two inputs. This is
    /// the seam other XCUITest suites use to bypass onboarding when their
    /// scenario does not exercise it.
    func test_launchArg_uitestSkipOnboarding_suppresses_sheet() throws {
        launch(completed: false, skipOnboarding: true)
        let appeared = onboardingSheet.waitForExistence(timeout: Timeout.absence)
        XCTAssertFalse(appeared,
                       "Sheet must not appear when -uitestSkipOnboarding is passed, even with humanPath() == nil")
    }
}
