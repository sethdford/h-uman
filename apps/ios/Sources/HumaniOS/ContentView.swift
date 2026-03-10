import SwiftUI

struct ContentView: View {
    @EnvironmentObject var connectionManager: ConnectionManager

    var body: some View {
        TabView {
            ChatView()
                .tabItem {
                    Label("Chat", systemImage: "bubble.left.and.bubble.right")
                }
            SettingsView()
                .tabItem {
                    Label("Settings", systemImage: "gear")
                }
        }
        .onChange(of: connectionManager.isConnected) { _, isConnected in
#if os(iOS)
            let notification = UINotificationFeedbackGenerator()
            if isConnected {
                notification.notificationOccurred(.success)
            } else {
                notification.notificationOccurred(.error)
            }
#endif
        }
    }
}
