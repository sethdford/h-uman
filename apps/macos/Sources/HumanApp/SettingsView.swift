import SwiftUI

struct SettingsView: View {
    @State private var gatewayURL: String = "ws://localhost:3000"
    @State private var autoStartOnLogin: Bool = false
    @State private var binaryPath: String = ""

    var body: some View {
        Form {
            TextField("Gateway URL", text: $gatewayURL)
                .textFieldStyle(.roundedBorder)
            Toggle("Auto-start on login", isOn: $autoStartOnLogin)
            Section("Binary") {
                Text(binaryPath.isEmpty ? "Not found" : binaryPath)
                    .font(.system(.body, design: .monospaced))
                    .foregroundColor(binaryPath.isEmpty ? .secondary : .primary)
            }
        }
        .padding()
        .frame(minWidth: 400, minHeight: 200)
        .onAppear {
            let pm = ProcessManager()
            binaryPath = pm.humanPath() ?? ""
        }
    }
}
