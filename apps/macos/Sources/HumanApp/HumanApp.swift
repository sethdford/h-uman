import SwiftUI

@main
struct HumanApp: App {
    @StateObject private var status = StatusViewModel()

    var body: some Scene {
        Settings {
            SettingsView()
        }
        MenuBarExtra {
            VStack(alignment: .leading, spacing: 0) {
                Button("Open Dashboard") {
                    if let url = URL(string: "http://localhost:3000") {
                        NSWorkspace.shared.open(url)
                    }
                }
                Button("Start Service") {
                    status.startService()
                }
                .disabled(status.isServiceRunning)
                Button("Stop Service") {
                    status.stopService()
                }
                .disabled(!status.isServiceRunning)
                Divider()
                Button("Settings...") {
                    NSApplication.shared.sendAction(Selector(("showSettingsWindow:")), to: nil, from: nil)
                }
                Divider()
                Button("Quit") {
                    status.stopService()
                    NSApplication.shared.terminate(nil)
                }
            }
            .frame(minWidth: 200)
        } label: {
            Image(systemName: "antenna.radiowaves.left.and.right")
                .symbolRenderingMode(.hierarchical)
                .foregroundStyle(status.statusColor)
        }
    }
}
