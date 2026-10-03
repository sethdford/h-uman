import ServiceManagement
import SwiftUI
import HumanChatUI

struct SettingsView: View {
    @Environment(\.colorScheme) private var colorScheme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @EnvironmentObject var status: StatusViewModel
    @State private var gatewayURL: String = "ws://localhost:3000"
    @State private var autoStartOnLogin: Bool = false
    @State private var loginItemBanner: String?
    @State private var binaryPath: String = ""
    @State private var appeared = false
    @State private var advancedExpanded = false
    @AppStorage(VoiceSettings.gatewayURLKey) private var voiceGatewayURL = VoiceSettings.defaultGatewayURL
    @AppStorage(VoiceSettings.voiceIdKey) private var voiceId = VoiceSettings.defaultVoiceId

    private var tokens: (text: Color, textMuted: Color, accent: Color, bgSurface: Color, border: Color, surfaceContainer: Color, success: Color, error: Color) {
        if colorScheme == .dark {
            return (HUTokens.Dark.text, HUTokens.Dark.textMuted, HUTokens.Dark.accent, HUTokens.Dark.bgSurface, HUTokens.Dark.border, HUTokens.Dark.surfaceContainer, HUTokens.Dark.success, HUTokens.Dark.error)
        } else {
            return (HUTokens.Light.text, HUTokens.Light.textMuted, HUTokens.Light.accent, HUTokens.Light.bgSurface, HUTokens.Light.border, HUTokens.Light.surfaceContainer, HUTokens.Light.success, HUTokens.Light.error)
        }
    }

    var body: some View {
        Form {
            Section {
                HStack {
                    Circle()
                        .fill(status.isGatewayConnected ? tokens.success : tokens.error)
                        .frame(width: HUTokens.spaceSm, height: HUTokens.spaceSm)
                    Text(status.isGatewayConnected ? "Connected" : "Disconnected")
                        .font(.custom("Avenir-Book", size: HUTokens.textSm))
                        .foregroundStyle(tokens.textMuted)
                }
                .accessibilityElement(children: .combine)
                .accessibilityLabel("Gateway \(status.isGatewayConnected ? "connected" : "disconnected")")
            } header: {
                Text("Connection")
                    .font(.custom("Avenir-Medium", size: HUTokens.textSm))
                    .foregroundStyle(tokens.textMuted)
            }

            DisclosureGroup(isExpanded: $advancedExpanded) {
                TextField("Gateway URL", text: $gatewayURL)
                    .textFieldStyle(.plain)
                    .font(.custom("Avenir-Book", size: HUTokens.textBase))
                    .foregroundStyle(tokens.text)
                    .padding(HUTokens.spaceSm)
                    .background(
                        RoundedRectangle(cornerRadius: HUTokens.radiusSm)
                            .fill(tokens.surfaceContainer)
                            .overlay(
                                RoundedRectangle(cornerRadius: HUTokens.radiusSm)
                                    .stroke(tokens.border, lineWidth: 1)
                            )
                    )
                    .accessibilityLabel("Gateway URL")
                Toggle("Auto-start on login", isOn: $autoStartOnLogin)
                    .accessibilityLabel("Auto-start on login")
                    .accessibilityValue(autoStartOnLogin ? "On" : "Off")
                    .onChange(of: autoStartOnLogin) { _, wantsOn in
                        if let msg = MacLoginItem.setEnabled(wantsOn) {
                            loginItemBanner = msg
                        } else {
                            loginItemBanner = nil
                        }
                        if SMAppService.mainApp.status == .requiresApproval {
                            autoStartOnLogin = true
                        } else {
                            autoStartOnLogin = MacLoginItem.isEnabled
                        }
                    }
                if let note = loginItemBanner {
                    Text(note)
                        .font(.custom("Avenir-Book", size: HUTokens.textXs))
                        .foregroundStyle(tokens.textMuted)
                        .fixedSize(horizontal: false, vertical: true)
                }
            } label: {
                Text("Advanced")
                    .font(.custom("Avenir-Medium", size: HUTokens.textSm))
                    .foregroundStyle(tokens.text)
            }

            Section {
                TextField("Voice gateway", text: $voiceGatewayURL)
                    .font(.custom("Avenir-Book", size: HUTokens.textSm))
                    .accessibilityLabel("Voice gateway URL")
                TextField("Cartesia voice ID", text: $voiceId)
                    .font(.custom("Avenir-Book", size: HUTokens.textSm))
                    .accessibilityLabel("Cartesia voice ID")
                Text("Takes effect the next time Voice Mode is turned on.")
                    .font(.custom("Avenir-Book", size: HUTokens.textXs))
                    .foregroundStyle(tokens.textMuted)
            } header: {
                Text("Voice Mode")
                    .font(.custom("Avenir-Medium", size: HUTokens.textSm))
                    .foregroundStyle(tokens.textMuted)
            }

            Section {
                Text(binaryPath.isEmpty ? "Not found" : binaryPath)
                    .font(.custom("Avenir-Book", size: HUTokens.textSm))
                    .foregroundStyle(binaryPath.isEmpty ? tokens.textMuted : tokens.text)
            } header: {
                Text("Binary")
                    .font(.custom("Avenir-Medium", size: HUTokens.textSm))
                    .foregroundStyle(tokens.textMuted)
            }
            .listRowBackground(
                RoundedRectangle(cornerRadius: HUTokens.radiusSm)
                    .fill(tokens.bgSurface)
                    .padding(.vertical, HUTokens.spaceXs)
            )
        }
        .padding(HUTokens.spaceSm)
        .frame(minWidth: 400, minHeight: 200)
        .background(tokens.bgSurface)
        .tint(tokens.accent)
        .animation(reduceMotion ? nil : .spring(response: 0.35, dampingFraction: 0.86), value: binaryPath)
        .opacity(appeared ? 1 : 0)
        .animation(reduceMotion ? nil : .spring(response: 0.35, dampingFraction: 0.86), value: appeared)
        .onAppear {
            appeared = true
            autoStartOnLogin = MacLoginItem.isEnabled
            loginItemBanner = nil
            let pm = ProcessManager()
            binaryPath = pm.humanPath() ?? ""
        }
    }
}
