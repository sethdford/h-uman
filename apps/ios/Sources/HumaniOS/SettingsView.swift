import SwiftUI
import HumanChatUI

struct SettingsView: View {
    @Environment(\.colorScheme) private var colorScheme
    @EnvironmentObject var connectionManager: ConnectionManager

    private var tokens: (success: Color, error: Color) {
        colorScheme == .dark ? (HUTokens.Dark.success, HUTokens.Dark.error) : (HUTokens.Light.success, HUTokens.Light.error)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Gateway URL", text: $connectionManager.gatewayURL)
                        .autocorrectionDisabled()
                        .accessibilityLabel("Server URL")
#if os(iOS)
                        .textInputAutocapitalization(.never)
                        .keyboardType(.URL)
#endif
                } header: {
                    Text("Connection")
                } footer: {
                    Text("WebSocket URL, e.g. wss://localhost:3000/ws")
                }

                Section {
                    HStack {
                        Text("Status")
                        Spacer()
                        HStack(spacing: HUTokens.spaceSm) {
                            Circle()
                                .fill(connectionManager.isConnected ? tokens.success : tokens.error)
                                .frame(width: 8, height: 8)
                            Text(connectionManager.isConnected ? "Connected" : "Disconnected")
                                .foregroundStyle(.secondary)
                        }
                        .accessibilityLabel("Connection status: \(connectionManager.isConnected ? "Connected" : "Disconnected")")
                    }

                    Button(connectionManager.isConnected ? "Disconnect" : "Connect") {
                        if connectionManager.isConnected {
                            connectionManager.disconnect()
                        } else {
                            connectionManager.connect()
                        }
                    }

                    if connectionManager.isConnected {
                        Button("Reconnect") {
                            connectionManager.reconnect()
                        }
                    }
                } header: {
                    Text("Connection Status")
                }
            }
            .navigationTitle("Settings")
        }
    }
}
