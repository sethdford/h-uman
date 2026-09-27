import SwiftUI
import HumanChatUI

enum AppTab: Int, CaseIterable {
    case overview, chat, memory, sessions, tools, settings
}

struct ContentView: View {
    @Environment(\.colorScheme) private var colorScheme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @EnvironmentObject var connectionManager: ConnectionManager
    @State private var selectedTab: AppTab = .overview

    private var accentColor: Color {
        colorScheme == .dark ? HUTokens.Dark.accent : HUTokens.Light.accent
    }

    var body: some View {
        TabView(selection: $selectedTab) {
            LazyView(OverviewView())
                .tabItem {
                    Label("Overview", systemImage: "square.grid.2x2")
                }
                .tag(AppTab.overview)
                .accessibilityIdentifier("tab_overview")
            LazyView(ChatView())
                .tabItem {
                    Label("Chat", systemImage: "bubble.left.and.bubble.right")
                }
                .tag(AppTab.chat)
                .accessibilityIdentifier("tab_chat")
            LazyView(MemoryView())
                .tabItem {
                    Label("Memory", systemImage: "memorychip")
                }
                .tag(AppTab.memory)
                .accessibilityIdentifier("tab_memory")
            LazyView(SessionsView())
                .tabItem {
                    Label("Sessions", systemImage: "clock.arrow.circlepath")
                }
                .tag(AppTab.sessions)
                .accessibilityIdentifier("tab_sessions")
            // Tools and Settings are NOT wrapped in LazyView: on iPhone they overflow
            // into the system More list, and a destination pushed from there never
            // fires the placeholder's onAppear on iOS 26, so LazyView stayed
            // Color.clear forever (More → Settings rendered a blank screen).
            ToolsView()
                .tabItem {
                    Label("Tools", systemImage: "wrench.and.screwdriver")
                }
                .tag(AppTab.tools)
                .accessibilityIdentifier("tab_tools")
            SettingsView()
                .tabItem {
                    Label("Settings", systemImage: "gear")
                }
                .tag(AppTab.settings)
                .accessibilityIdentifier("tab_settings")
        }
        .tint(accentColor)
#if os(iOS)
        .overlay(alignment: .topLeading) {
            // iPad keyboard shortcuts: Cmd+1..6 for tabs, Cmd+N for new chat
            Group {
                Button { selectTab(.overview) } label: { EmptyView() }
                    .keyboardShortcut("1", modifiers: .command)
                Button { selectTab(.chat) } label: { EmptyView() }
                    .keyboardShortcut("2", modifiers: .command)
                Button { selectTab(.memory) } label: { EmptyView() }
                    .keyboardShortcut("3", modifiers: .command)
                Button { selectTab(.sessions) } label: { EmptyView() }
                    .keyboardShortcut("4", modifiers: .command)
                Button { selectTab(.tools) } label: { EmptyView() }
                    .keyboardShortcut("5", modifiers: .command)
                Button { selectTab(.settings) } label: { EmptyView() }
                    .keyboardShortcut("6", modifiers: .command)
                Button { selectTab(.chat) } label: { EmptyView() }
                    .keyboardShortcut("n", modifiers: .command)
            }
            .frame(width: 0, height: 0)
            .opacity(0)
            .allowsHitTesting(false)
        }
#endif
        .task {
            if !connectionManager.isConnected {
                connectionManager.connect()
            }
            connectionManager.prefetchDataForTab(selectedTab)
        }
        .onReceive(NotificationCenter.default.publisher(for: .navigateToTab)) { notification in
            if let tab = notification.userInfo?["tab"] as? AppTab {
                withAnimation(HUTokens.springExpressive) {
                    selectedTab = tab
                }
            }
        }
        .onChange(of: selectedTab) { _, tab in
#if os(iOS)
            HUTokens.Haptic.selection.trigger()
#endif
            connectionManager.prefetchDataForTab(tab)
        }
        .onChange(of: connectionManager.isConnected) { _, isConnected in
            withAnimation(HUTokens.springExpressive) {
                if isConnected {
#if os(iOS)
                    let notification = UINotificationFeedbackGenerator()
                    notification.notificationOccurred(.success)
#endif
                    connectionManager.prefetchDataForTab(selectedTab)
                } else {
#if os(iOS)
                    let notification = UINotificationFeedbackGenerator()
                    notification.notificationOccurred(.error)
#endif
                }
            }
        }
    }

    private func selectTab(_ tab: AppTab) {
        withAnimation(HUTokens.springExpressive) {
            selectedTab = tab
        }
        connectionManager.prefetchDataForTab(tab)
    }
}
