import SwiftUI
import HumanChatUI

struct OnboardingView: View {
    @Binding var hasOnboarded: Bool
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @EnvironmentObject var connectionManager: ConnectionManager
    @Environment(\.colorScheme) private var colorScheme
    @State private var currentPage = 0
    @State private var urlValidationError: String?

    private var isGatewayURLValid: Bool {
        urlValidationError == nil && !connectionManager.gatewayURL.trimmingCharacters(in: .whitespaces).isEmpty
    }

    private var tokens: (bgSurface: Color, surfaceContainer: Color, text: Color, textMuted: Color, accent: Color, success: Color, error: Color) {
        if colorScheme == .dark {
            return (HUTokens.Dark.bgSurface, HUTokens.Dark.surfaceContainer, HUTokens.Dark.text, HUTokens.Dark.textMuted, HUTokens.Dark.accent, HUTokens.Dark.success, HUTokens.Dark.error)
        } else {
            return (HUTokens.Light.bgSurface, HUTokens.Light.surfaceContainer, HUTokens.Light.text, HUTokens.Light.textMuted, HUTokens.Light.accent, HUTokens.Light.success, HUTokens.Light.error)
        }
    }

    private let pages: [(String, String, String)] = [
        ("sparkles", "Welcome to h-uman", "Your autonomous AI assistant runtime. Minimal footprint, maximum capability."),
        ("bolt.fill", "Lightning Fast", "~3 MB binary, <9 MB RAM, <10 ms startup. Zero dependencies beyond libc."),
        ("bubble.left.and.bubble.right.fill", "34 Channels", "Connect Telegram, Discord, Slack, email, and 30 more messaging platforms."),
    ]

    var body: some View {
        VStack(spacing: 0) {
            TabView(selection: $currentPage) {
                ForEach(Array(pages.enumerated()), id: \.offset) { index, page in
                    VStack(spacing: HUTokens.spaceLg) {
                        Spacer()
                        Image(systemName: page.0)
                            .font(.system(size: HUTokens.space2xl + HUTokens.spaceLg))
                            .foregroundStyle(tokens.accent)
                            .frame(height: 100)
                        Text(page.1)
                            .font(.custom("Avenir-Heavy", size: HUTokens.text2Xl, relativeTo: .title))
                            .foregroundStyle(tokens.text)
                            .multilineTextAlignment(.center)
                        Text(page.2)
                            .font(.custom("Avenir-Book", size: HUTokens.textBase, relativeTo: .body))
                            .foregroundStyle(tokens.textMuted)
                            .multilineTextAlignment(.center)
                            .padding(.horizontal, HUTokens.spaceXl)
                        Spacer()
                    }
                    .tag(index)
                    .accessibilityElement(children: .combine)
                    .accessibilityLabel("\(page.1). \(page.2)")
                }
            }
            #if os(iOS)
            .tabViewStyle(.page(indexDisplayMode: .always))
            #endif

            VStack(spacing: HUTokens.spaceMd) {
                TextField("Gateway URL", text: $connectionManager.gatewayURL)
                    .font(.custom("Avenir-Book", size: HUTokens.textBase, relativeTo: .body))
                    .padding(HUTokens.spaceMd)
                    .background(tokens.surfaceContainer)
                    .clipShape(RoundedRectangle(cornerRadius: HUTokens.radiusMd, style: .continuous))
                    .autocorrectionDisabled()
#if os(iOS)
                    .textInputAutocapitalization(.never)
                    .keyboardType(.URL)
#endif
                    .accessibilityLabel("Gateway URL")
                    .onChange(of: connectionManager.gatewayURL) { _, newValue in
                        urlValidationError = Self.validateGatewayURL(newValue)
                    }
                    .onAppear {
                        urlValidationError = Self.validateGatewayURL(connectionManager.gatewayURL)
                    }

                if let err = urlValidationError, !err.isEmpty {
                    Text(err)
                        .font(.custom("Avenir-Book", size: HUTokens.textXs, relativeTo: .caption))
                        .foregroundStyle(tokens.error)
                }

                Button {
#if os(iOS)
                    let impact = UIImpactFeedbackGenerator(style: .medium)
                    impact.impactOccurred()
#endif
                    if reduceMotion {
                        connectionManager.connect()
                        hasOnboarded = true
                    } else {
                        withAnimation(HUTokens.springExpressive) {
                            connectionManager.connect()
                            hasOnboarded = true
                        }
                    }
                } label: {
                    Text("Get Started")
                        .font(.custom("Avenir-Heavy", size: HUTokens.textBase, relativeTo: .body))
                        .foregroundStyle(.white)
                        .frame(maxWidth: .infinity)
                        .padding(HUTokens.spaceMd)
                        .background(tokens.accent)
                        .clipShape(RoundedRectangle(cornerRadius: HUTokens.radiusMd, style: .continuous))
                }
                .disabled(!isGatewayURLValid)
                .opacity(isGatewayURLValid ? 1 : HUTokens.opacityDisabled)
                .accessibilityLabel("Get started and connect to gateway")

                Button {
                    if reduceMotion {
                        hasOnboarded = true
                    } else {
                        withAnimation(HUTokens.springExpressive) {
                            hasOnboarded = true
                        }
                    }
                } label: {
                    Text("Skip for now")
                        .font(.custom("Avenir-Book", size: HUTokens.textSm, relativeTo: .subheadline))
                        .foregroundStyle(tokens.textMuted)
                }
                .accessibilityLabel("Skip onboarding")
            }
            .padding(HUTokens.spaceLg)
        }
        .background(tokens.bgSurface)
    }

    private static func validateGatewayURL(_ url: String) -> String? {
        let trimmed = url.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return "Enter a gateway URL" }
        guard trimmed.hasPrefix("ws://") || trimmed.hasPrefix("wss://") else {
            return "URL must start with ws:// or wss://"
        }
        let withoutScheme = trimmed.hasPrefix("wss://")
            ? String(trimmed.dropFirst(6))
            : String(trimmed.dropFirst(5))
        guard !withoutScheme.isEmpty else { return "URL must include host" }
        let hostPart = withoutScheme.split(separator: "/").first ?? Substring(withoutScheme)
        guard !hostPart.isEmpty else { return "URL must include host" }
        if hostPart.contains(":") {
            let parts = hostPart.split(separator: ":")
            guard parts.count == 2, let port = Int(parts[1]), (1...65535).contains(port) else {
                return "Invalid port"
            }
        }
        return nil
    }
}
