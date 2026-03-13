import SwiftUI
import HumanChatUI

struct ContentView: View {
    @Environment(\.colorScheme) private var colorScheme
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
        .tint(colorScheme == .dark ? HUTokens.Dark.accent : HUTokens.Light.accent)
        .onChange(of: connectionManager.isConnected) { _, isConnected in
            withAnimation(HUTokens.springExpressive) {
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
}
