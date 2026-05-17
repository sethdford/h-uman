// OnboardingSheet.swift
//
// First-run setup sheet for the macOS app (US-14.5).
//
// Shown when the `human` daemon is not on PATH and the user has not yet
// dismissed/completed onboarding. Hosts:
//   - Headline + body copy explaining the daemon dependency.
//   - A copyable `Text` with the brew install command for users who would
//     rather run it themselves.
//   - "I've Installed" primary action: opens the install landing page in
//     the default browser and dismisses.
//   - "I'll do this later" secondary action: marks onboarding complete and
//     dismisses without opening a URL.
//
// Accessibility identifiers on every interactive element so XCUITest can
// observe sheet presence and tap CTAs deterministically.

import AppKit
import HumanChatUI
import SwiftUI

struct OnboardingSheet: View {
    @ObservedObject var state: OnboardingState
    @Environment(\.colorScheme) private var colorScheme

    /// Stable identifiers for XCUITest. Mirror the iOS pattern of dotted
    /// names rooted at the surface (`onboarding.sheet.*`, `onboarding.cta.*`).
    enum AXID {
        static let root = "onboarding.sheet.root"
        static let installCTA = "onboarding.cta.install"
        static let skipCTA = "onboarding.cta.skip"
        static let brewCommand = "onboarding.brew.command"
    }

    /// Verbatim brew command users can copy-paste. The string is intentionally
    /// the same shape as the Sprint-9 Homebrew tap formula (`humanlabs/human/human`).
    /// Not executed by the app — purely displayed for the user to run themselves.
    private static let brewCommand = "brew install humanlabs/human/human"

    private var tokens: (bg: Color, surface: Color, surfaceHigh: Color, text: Color, textMuted: Color, accent: Color) {
        if colorScheme == .dark {
            return (HUTokens.Dark.bgSurface, HUTokens.Dark.surfaceContainer, HUTokens.Dark.surfaceContainerHigh, HUTokens.Dark.text, HUTokens.Dark.textMuted, HUTokens.Dark.accent)
        } else {
            return (HUTokens.Light.bgSurface, HUTokens.Light.surfaceContainer, HUTokens.Light.surfaceContainerHigh, HUTokens.Light.text, HUTokens.Light.textMuted, HUTokens.Light.accent)
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: HUTokens.spaceLg) {
            Text("Welcome to h-uman")
                .font(.custom("Avenir-Heavy", size: 24, relativeTo: .title2))
                .kerning(-0.5)
                .foregroundStyle(tokens.text)
                .accessibilityAddTraits(.isHeader)

            Text("To run h-uman locally we need the `human` daemon binary. The fastest way to get it is via Homebrew — run the command below in Terminal, or open the install page for other options.")
                .font(.custom("Avenir-Book", size: HUTokens.textBase, relativeTo: .body))
                .foregroundStyle(tokens.textMuted)
                .fixedSize(horizontal: false, vertical: true)

            // Selectable / copyable brew command. SwiftUI's `Text` is selectable
            // when wrapped in `.textSelection(.enabled)`; XCUITest can verify
            // the literal command is present via `staticTexts`.
            Text(Self.brewCommand)
                .font(.system(.body, design: .monospaced))
                .foregroundStyle(tokens.text)
                .textSelection(.enabled)
                .padding(HUTokens.spaceSm)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(tokens.surfaceHigh)
                .clipShape(RoundedRectangle(cornerRadius: HUTokens.radiusMd, style: .continuous))
                .accessibilityIdentifier(AXID.brewCommand)
                .accessibilityLabel("Brew install command: \(Self.brewCommand)")

            // If you've already installed it but the sheet keeps appearing,
            // tap Skip — it persists the flag and stops the loop. This copy
            // is documented in the US-14.5 design's risk mitigation.
            Text("If you've already installed the daemon, click \"I'll do this later\" to stop seeing this.")
                .font(.custom("Avenir-Book", size: HUTokens.textSm, relativeTo: .subheadline))
                .foregroundStyle(tokens.textMuted)
                .fixedSize(horizontal: false, vertical: true)

            Spacer(minLength: 0)

            HStack(spacing: HUTokens.spaceMd) {
                Button {
                    state.markCompleted()
                } label: {
                    Text("I'll do this later")
                        .frame(minWidth: 140)
                }
                .keyboardShortcut(.cancelAction)
                .accessibilityIdentifier(AXID.skipCTA)
                .accessibilityLabel("Skip h-uman daemon setup for now")

                Spacer()

                Button {
                    state.openInstallPage()
                } label: {
                    Text("Open install page")
                        .frame(minWidth: 160)
                }
                .keyboardShortcut(.defaultAction)
                .accessibilityIdentifier(AXID.installCTA)
                .accessibilityLabel("Install h-uman daemon via Homebrew package manager")
            }
        }
        .padding(HUTokens.spaceLg)
        .frame(width: 520, height: 340)
        .background(tokens.bg)
        .accessibilityIdentifier(AXID.root)
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Welcome to h-uman setup")
    }
}
