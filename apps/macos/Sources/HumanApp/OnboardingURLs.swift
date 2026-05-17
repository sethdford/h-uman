// OnboardingURLs.swift
//
// Hard-coded URL constants for the macOS first-run OnboardingSheet (US-14.5).
//
// Per US-14.5 risk mitigation: URL values are compile-time `static let`
// constants built from string literals — never sourced from user input,
// config, environment, defaults, or runtime data. This is a deliberate
// security choice: `NSWorkspace.shared.open(_:)` honours the OS default
// scheme handler, so an attacker-controlled URL string could redirect to
// a phishing site or a custom scheme handler. By inlining the values here
// we make the install destination grep-auditable and code-review-locked.
//
// If a follow-up story needs additional URLs, add them here — not as
// configurable fields on `OnboardingState` or `OnboardingSheet`.

import Foundation

enum OnboardingURLs {
    /// Install landing page that documents the `brew install humanlabs/human/human`
    /// command (and any alternative installers). Opened via
    /// `NSWorkspace.shared.open(_:)` when the user taps the primary CTA.
    static let installPageURL = URL(string: "https://gettheconsultant.com/install")!
}

/// Build-time feature flags for the onboarding surface.
///
/// `openTerminalCTA` is documented in the US-14.5 design as a deliberate
/// out-of-scope seam: a future story may toggle it `true` to render an
/// "Open Terminal with brew command" secondary CTA. Until then it stays
/// `false` and `OnboardingSheet` does not render that button.
enum OnboardingFeature {
    /// When `true`, `OnboardingSheet` renders the "Open Terminal" CTA that
    /// would spawn a `Terminal.app` process running `brew install ...`. In
    /// US-14.5 we keep this `false` — spawning a process from a freshly
    /// launched, unsandboxed app is the riskier path. The CTA seam is
    /// preserved so a follow-up story can flip it on without touching this
    /// file's structure.
    static let openTerminalCTA: Bool = false
}
