// OnboardingState.swift
//
// Observable wrapper around the `shouldShow` decision for the macOS
// first-run OnboardingSheet (US-14.5).
//
// `shouldShow` is a pure function of three inputs:
//   1. Whether the `human` daemon binary is resolvable on PATH
//      (`ProcessManager.humanPath() == nil`).
//   2. Whether the user has previously completed onboarding
//      (UserDefaults key `humanInstallationCompleted == true`).
//   3. Whether XCUITest passed `-uitestSkipOnboarding` as a launch arg
//      (mirroring the iOS precedent at `apps/ios/Sources/HumaniOS/HumanApp.swift`).
//
// `markCompleted()` is idempotent — setting an already-set UserDefaults
// flag is a no-op for our purposes. Per the US-14.5 design "UserDefaults
// race on first launch" mitigation, no lock is needed.
//
// The state is re-evaluated whenever `recheck()` is invoked. The root
// scene calls `recheck()` on `Scene.phase == .active` so the sheet
// self-heals after the user installs the daemon mid-session.

import AppKit
import Combine
import Foundation

@MainActor
final class OnboardingState: ObservableObject {
    /// UserDefaults key that records whether the user has either (a) tapped
    /// the install CTA, (b) tapped "Skip for now", or (c) been observed
    /// with the daemon already on PATH. Once set to `true` the sheet does
    /// not appear on subsequent launches.
    static let completedDefaultsKey = "humanInstallationCompleted"

    /// Launch argument honoured by both macOS (US-14.5) and iOS (existing
    /// precedent) so XCUITest can skip the onboarding surface without
    /// mutating UserDefaults.
    static let uitestSkipLaunchArg = "-uitestSkipOnboarding"

    /// Whether the onboarding sheet should be presented. Re-evaluated on
    /// `recheck()`.
    @Published private(set) var shouldShow: Bool = false

    private let defaults: UserDefaults
    private let arguments: [String]
    private let humanPathProvider: () -> String?

    /// - Parameters:
    ///   - defaults: UserDefaults suite. Default `.standard` for production;
    ///     tests inject an in-memory suite to avoid cross-test contamination.
    ///   - arguments: Process arguments. Default `ProcessInfo.processInfo.arguments`;
    ///     tests inject `["-uitestSkipOnboarding"]` to exercise the launch-arg gate.
    ///   - humanPathProvider: Closure returning the resolved `human` binary path,
    ///     or `nil` if the daemon is not installed. Default delegates to
    ///     `ProcessManager.humanPath()` (an instance method on a freshly
    ///     constructed `ProcessManager`, which has no instance state for path
    ///     resolution). Tests inject `{ nil }` or `{ "/usr/local/bin/human" }`.
    init(
        defaults: UserDefaults = .standard,
        arguments: [String] = ProcessInfo.processInfo.arguments,
        humanPathProvider: @escaping () -> String? = { ProcessManager().humanPath() }
    ) {
        self.defaults = defaults
        self.arguments = arguments
        self.humanPathProvider = humanPathProvider
        recheck()
    }

    /// Recompute `shouldShow`. Called on init and on every `Scene.phase` change
    /// to `.active`, so a user who installs the daemon in another window mid-
    /// session sees the sheet auto-dismiss on next focus.
    ///
    /// Side effect: if the daemon path is now resolvable, `markCompleted()`
    /// is invoked. This is the "self-healing" branch documented in the
    /// design: once the daemon exists, we never want to show the sheet again.
    func recheck() {
        if arguments.contains(Self.uitestSkipLaunchArg) {
            shouldShow = false
            return
        }
        if humanPathProvider() != nil {
            // Daemon is present. Persist the completed flag so we don't
            // re-prompt next launch when PATH temporarily lacks the binary.
            if !defaults.bool(forKey: Self.completedDefaultsKey) {
                defaults.set(true, forKey: Self.completedDefaultsKey)
            }
            shouldShow = false
            return
        }
        let completed = defaults.bool(forKey: Self.completedDefaultsKey)
        shouldShow = !completed
    }

    /// Persist `humanInstallationCompleted = true` and dismiss the sheet.
    /// Idempotent — safe to call multiple times.
    func markCompleted() {
        defaults.set(true, forKey: Self.completedDefaultsKey)
        shouldShow = false
    }

    /// Primary CTA action: open the install landing page in the user's default
    /// browser and mark onboarding complete. URL is a compile-time constant
    /// from `OnboardingURLs` — see that file's header for the security
    /// rationale (no runtime URL input).
    func openInstallPage() {
        NSWorkspace.shared.open(OnboardingURLs.installPageURL)
        markCompleted()
    }
}
