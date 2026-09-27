import Combine
import HumanChatUI
import HumanProtocol
import SwiftUI

/// Motion 9 spring: response 0.35, damping 0.86 for all interactive elements.
private let springMotion9 = Animation.spring(response: 0.35, dampingFraction: 0.86)

/// Wraps content in a lazy container so it is built only when first displayed.
private struct LazyDetailView<Content: View>: View {
    let build: () -> Content
    init(_ build: @autoclosure @escaping () -> Content) { self.build = build }
    var body: some View { build() }
}

struct DashboardView: View {
    @EnvironmentObject var status: StatusViewModel
    @Environment(\.colorScheme) private var colorScheme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private var tokens: (bgSurface: Color, surfaceContainer: Color, surfaceContainerHigh: Color, text: Color, textMuted: Color, accent: Color, success: Color, error: Color) {
        if colorScheme == .dark {
            return (HUTokens.Dark.bgSurface, HUTokens.Dark.surfaceContainer, HUTokens.Dark.surfaceContainerHigh, HUTokens.Dark.text, HUTokens.Dark.textMuted, HUTokens.Dark.accent, HUTokens.Dark.success, HUTokens.Dark.error)
        } else {
            return (HUTokens.Light.bgSurface, HUTokens.Light.surfaceContainer, HUTokens.Light.surfaceContainerHigh, HUTokens.Light.text, HUTokens.Light.textMuted, HUTokens.Light.accent, HUTokens.Light.success, HUTokens.Light.error)
        }
    }

    var body: some View {
        NavigationSplitView {
            sidebar
        }         detail: {
            detailContent
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(tokens.bgSurface)
                .drawingGroup()
        }
        .navigationSplitViewStyle(.balanced)
        .animation(reduceMotion ? nil : springMotion9, value: status.selectedTab)
        .onChange(of: status.selectedTab) { _, tab in
            if tab == .chat { status.connectIfNeeded() }
        }
        .onAppear {
            status.ensureGatewayConnection()
        }
    }

    @ViewBuilder
    private var sidebar: some View {
        List(selection: $status.selectedTab) {
            Label("Overview", systemImage: "square.grid.2x2")
                .tag(MacTab.overview)
                .accessibilityLabel("Overview tab")
            Label("Chat", systemImage: "bubble.left.and.bubble.right")
                .tag(MacTab.chat)
                .accessibilityLabel("Chat tab")
            Label("Sessions", systemImage: "clock.arrow.circlepath")
                .tag(MacTab.sessions)
                .accessibilityLabel("Sessions tab")
            Label("Tools", systemImage: "wrench.and.screwdriver")
                .tag(MacTab.tools)
                .accessibilityLabel("Tools tab")

            Section("System") {
                Label("Settings", systemImage: "gearshape")
                    .tag(MacTab.settings)
                    .accessibilityLabel("Settings tab")
            }
        }
        .listStyle(.sidebar)
        .frame(minWidth: 180)
        .focusSection()
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button(action: {
                    status.selectedTab = .chat
                    status.connectIfNeeded()
                }) {
                    Label("New Chat", systemImage: "plus")
                }
                .keyboardShortcut("n", modifiers: .command)
                .accessibilityLabel("New chat")
            }
            ToolbarItem(placement: .status) {
                HStack(spacing: HUTokens.spaceXs) {
                    Circle()
                        .fill(status.isGatewayConnected ? tokens.success : tokens.error)
                        .frame(width: HUTokens.spaceSm, height: HUTokens.spaceSm)
                    if status.isGatewayConnected {
                        Text("Connected")
                            .font(.custom("Avenir-Book", size: HUTokens.textXs))
                            .foregroundStyle(tokens.textMuted)
                    } else if status.gatewayClient.onDeviceAvailable {
                        Text("On-Device")
                            .font(.custom("Avenir-Book", size: HUTokens.textXs))
                            .foregroundStyle(tokens.textMuted)
                    } else {
                        Text("Disconnected")
                            .font(.custom("Avenir-Book", size: HUTokens.textXs))
                            .foregroundStyle(tokens.textMuted)
                    }
                }
                .accessibilityElement(children: .combine)
                .accessibilityLabel("Gateway \(status.isGatewayConnected ? "connected" : status.gatewayClient.onDeviceAvailable ? "on-device" : "disconnected")")
            }
            ToolbarItem(placement: .automatic) {
                HStack(spacing: HUTokens.spaceXs) {
                    Circle()
                        .fill(status.isServiceRunning ? tokens.success : tokens.error)
                        .frame(width: HUTokens.spaceSm, height: HUTokens.spaceSm)
                    Text(status.isServiceRunning ? "Running" : "Stopped")
                        .font(.custom("Avenir-Book", size: HUTokens.textXs))
                        .foregroundStyle(tokens.textMuted)
                }
                .accessibilityElement(children: .combine)
                .accessibilityLabel("Service \(status.isServiceRunning ? "running" : "stopped")")
            }
        }
    }

    @ViewBuilder
    private var detailContent: some View {
        switch status.selectedTab {
        case .overview:
            LazyDetailView(MacOverviewPane(tokens: tokens, status: status))
                .focusSection()
        case .chat:
            LazyDetailView(MacChatPane(tokens: tokens))
                .focusSection()
        case .sessions:
            LazyDetailView(MacSessionsPane(tokens: tokens))
                .focusSection()
        case .tools:
            LazyDetailView(MacToolsPane(tokens: tokens))
                .focusSection()
        case .settings:
            LazyDetailView(SettingsView())
                .focusSection()
        }
    }
}

private struct MacGatewayCard: View {
    @ObservedObject var status: StatusViewModel
    let tokens: (bgSurface: Color, surfaceContainer: Color, surfaceContainerHigh: Color, text: Color, textMuted: Color, accent: Color, success: Color, error: Color)
    let reduceMotion: Bool
    @State private var isHovered = false

    var body: some View {
        HStack(spacing: HUTokens.spaceMd) {
            Circle()
                .fill(status.isGatewayConnected ? tokens.success : tokens.error)
                .frame(width: HUTokens.spaceMd, height: HUTokens.spaceMd)
            Text(status.isGatewayConnected ? "Gateway Connected" : "Gateway Disconnected")
                .font(.custom("Avenir-Medium", size: HUTokens.textBase))
                .foregroundStyle(tokens.text)
            Spacer()
            Text(status.gatewayURL)
                .font(.custom("Avenir-Book", size: HUTokens.textSm))
                .foregroundStyle(tokens.textMuted)
        }
        .padding(HUTokens.spaceMd)
        .background(tokens.surfaceContainerHigh)
        .clipShape(RoundedRectangle(cornerRadius: HUTokens.radiusLg, style: .continuous))
        .scaleEffect(isHovered ? 1.02 : 1.0)
        .animation(reduceMotion ? nil : springMotion9, value: isHovered)
        .onHover { isHovered = $0 }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Gateway \(status.isGatewayConnected ? "connected" : "disconnected") at \(status.gatewayURL)")
    }
}

private struct MacStatCard: View {
    let stat: (String, String, String)
    let tokens: (bgSurface: Color, surfaceContainer: Color, surfaceContainerHigh: Color, text: Color, textMuted: Color, accent: Color, success: Color, error: Color)
    let index: Int
    let appeared: Bool
    let reduceMotion: Bool
    @State private var isHovered = false

    var body: some View {
        VStack(alignment: .leading, spacing: HUTokens.spaceSm) {
            Image(systemName: stat.2)
                .font(.custom("Avenir-Medium", size: HUTokens.textLg))
                .foregroundStyle(tokens.accent)
            Text(stat.1)
                .font(.custom("Avenir-Heavy", size: 28))
                .kerning(-0.5)
                .foregroundStyle(tokens.text)
            Text(stat.0)
                .font(.custom("Avenir-Book", size: HUTokens.textXs))
                .foregroundStyle(tokens.textMuted)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(HUTokens.spaceMd)
        .background(tokens.surfaceContainerHigh)
        .clipShape(RoundedRectangle(cornerRadius: HUTokens.radiusLg, style: .continuous))
        .scaleEffect(isHovered ? 1.02 : 1.0)
        .animation(reduceMotion ? nil : springMotion9, value: isHovered)
        .onHover { isHovered = $0 }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("\(stat.0): \(stat.1)")
        .transition(.asymmetric(
            insertion: .move(edge: .bottom).combined(with: .opacity),
            removal: .opacity
        ))
        .opacity(appeared ? 1 : 0)
        .scaleEffect(appeared ? 1 : 0.9)
        .animation(
            reduceMotion ? nil : springMotion9.delay(Double(Swift.min(index, 6)) * 0.05),
            value: appeared
        )
    }
}

struct MacOverviewPane: View {
    let tokens: (bgSurface: Color, surfaceContainer: Color, surfaceContainerHigh: Color, text: Color, textMuted: Color, accent: Color, success: Color, error: Color)
    @ObservedObject var status: StatusViewModel
    @State private var appeared = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private var overviewStats: [(String, String, String)] {
        let modelLine = status.overviewModel
        let modelShort =
            modelLine.count > 20 ? String(modelLine.prefix(20)) + "…" : modelLine
        let cotLine = status.isGatewayConnected ? "Auditing" : "—"
        let promptCacheLine = status.isGatewayConnected ? "Active" : "—"
        let emotionVoiceLine = status.isGatewayConnected ? "Active" : "—"
        return [
            ("Channels", status.overviewChannelCount, "bubble.left.and.bubble.right"),
            ("Tools", status.overviewToolCount, "wrench.and.screwdriver"),
            ("Model", modelShort == "—" ? "—" : modelShort, "cpu"),
            ("Uptime", status.overviewUptime, "clock"),
            ("HuLa", status.overviewHulaCount, "terminal"),
            ("CoT Audit", cotLine, "checkmark.shield"),
            ("Prompt Cache", promptCacheLine, "internaldrive"),
            ("Emotion Voice", emotionVoiceLine, "waveform"),
        ]
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: HUTokens.spaceLg) {
                Text("Overview")
                    .font(.custom("Avenir-Heavy", size: 24, relativeTo: .title2))
                    .kerning(-0.5)
                    .foregroundStyle(tokens.text)

                MacGatewayCard(status: status, tokens: tokens, reduceMotion: reduceMotion)

                LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: HUTokens.spaceMd), count: 4), spacing: HUTokens.spaceMd) {
                    ForEach(Array(overviewStats.enumerated()), id: \.offset) { index, stat in
                        MacStatCard(stat: stat, tokens: tokens, index: index, appeared: appeared, reduceMotion: reduceMotion)
                    }
                }

                Text("Service Status")
                    .font(.custom("Avenir-Heavy", size: HUTokens.textLg))
                    .foregroundStyle(tokens.text)

                HStack(spacing: HUTokens.spaceLg) {
                    VStack(alignment: .leading, spacing: HUTokens.spaceXs) {
                        Text("Binary")
                            .font(.custom("Avenir-Book", size: HUTokens.textSm))
                            .foregroundStyle(tokens.textMuted)
                        Text("~3 MB")
                            .font(.custom("Avenir-Heavy", size: HUTokens.textBase))
                            .foregroundStyle(tokens.text)
                    }
                    VStack(alignment: .leading, spacing: HUTokens.spaceXs) {
                        Text("Uptime")
                            .font(.custom("Avenir-Book", size: HUTokens.textSm))
                            .foregroundStyle(tokens.textMuted)
                        Text("99.8%")
                            .font(.custom("Avenir-Heavy", size: HUTokens.textBase))
                            .foregroundStyle(tokens.text)
                    }
                    VStack(alignment: .leading, spacing: HUTokens.spaceXs) {
                        Text("Model")
                            .font(.custom("Avenir-Book", size: HUTokens.textSm))
                            .foregroundStyle(tokens.textMuted)
                        Text(status.isGatewayConnected ? status.overviewModel : "—")
                            .font(.custom("Avenir-Heavy", size: HUTokens.textBase))
                            .foregroundStyle(tokens.text)
                    }
                    VStack(alignment: .leading, spacing: HUTokens.spaceXs) {
                        Text("Memory entries")
                            .font(.custom("Avenir-Book", size: HUTokens.textSm))
                            .foregroundStyle(tokens.textMuted)
                        Text("1,247")
                            .font(.custom("Avenir-Heavy", size: HUTokens.textBase))
                            .foregroundStyle(tokens.text)
                    }
                    VStack(alignment: .leading, spacing: HUTokens.spaceXs) {
                        Text("Startup")
                            .font(.custom("Avenir-Book", size: HUTokens.textSm))
                            .foregroundStyle(tokens.textMuted)
                        Text("<10 ms")
                            .font(.custom("Avenir-Heavy", size: HUTokens.textBase))
                            .foregroundStyle(tokens.text)
                    }
                    VStack(alignment: .leading, spacing: HUTokens.spaceXs) {
                        Text("Peak RSS")
                            .font(.custom("Avenir-Book", size: HUTokens.textSm))
                            .foregroundStyle(tokens.textMuted)
                        Text("6.9 MB")
                            .font(.custom("Avenir-Heavy", size: HUTokens.textBase))
                            .foregroundStyle(tokens.text)
                    }
                    Spacer()
                }
                .padding(HUTokens.spaceMd)
                .background(tokens.surfaceContainerHigh)
                .clipShape(RoundedRectangle(cornerRadius: HUTokens.radiusLg, style: .continuous))
            }
            .padding(HUTokens.spaceLg)
        }
        .onAppear {
            status.refreshOverviewFromGateway()
            if reduceMotion {
                appeared = true
            } else {
                withAnimation(springMotion9) { appeared = true }
            }
        }
    }
}

private struct MacChatMessage: Identifiable {
    let id: UUID
    let text: String
    let role: ChatBubble.Role
}

private struct MacToolCallInfo: Identifiable {
    let id: UUID
    let name: String
    let arguments: String?
    var status: ToolCallCard.Status
    var result: String?
}

struct MacChatPane: View {
    let tokens: (bgSurface: Color, surfaceContainer: Color, surfaceContainerHigh: Color, text: Color, textMuted: Color, accent: Color, success: Color, error: Color)
    @EnvironmentObject private var status: StatusViewModel
    @Environment(\.colorScheme) private var colorScheme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @FocusState private var isInputFocused: Bool
    @State private var messages: [MacChatMessage] = []
    @State private var toolCalls: [MacToolCallInfo] = []
    @State private var inputText = ""
    @State private var errorBanner: String?
    @State private var sendTrigger = 0
    @State private var isSending = false
    @State private var isSendingWatchdogGeneration = 0

    private static let suggestionChips = ["What can you do?", "Summarize my notes", "Help me plan my day"]

    private let messageTransition = AnyTransition.asymmetric(
        insertion: .move(edge: .bottom).combined(with: .opacity),
        removal: .opacity
    )

    var body: some View {
        VStack(spacing: 0) {
            messageList
            if isSending {
                HStack(spacing: HUTokens.spaceSm) {
                    ProgressView()
                        .scaleEffect(0.9)
                    Text("Sending…")
                        .font(.custom("Avenir-Book", size: HUTokens.textXs, relativeTo: .caption))
                        .foregroundStyle(tokens.textMuted)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, HUTokens.spaceSm)
            }
            if let err = errorBanner {
                errorBannerView(err)
            }
            ChatInputBar(text: $inputText, onSend: { sendMessage() }, sendTrigger: sendTrigger, focus: $isInputFocused)
                .background(.thinMaterial)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(tokens.bgSurface)
        .onAppear {
            status.connectIfNeeded()
        }
        .onReceive(status.gatewayClient.chatEventsPublisher) { event, payload in
            handleGatewayEvent(event, payload: payload)
        }
        .accessibilityLabel("Chat")
    }

    @ViewBuilder
    private var messageList: some View {
        Group {
            if messages.isEmpty && toolCalls.isEmpty && !isSending {
                emptyStateView
            } else {
                ScrollViewReader { proxy in
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: HUTokens.spaceMd) {
                            ForEach(messages) { msg in
                                ChatBubble(text: msg.text, role: msg.role)
                                    .id(msg.id)
                                    .transition(messageTransition)
                            }
                            ForEach(toolCalls) { tc in
                                ToolCallCard(name: tc.name, arguments: tc.arguments, status: tc.status, result: tc.result)
                                    .id(tc.id)
                                    .transition(messageTransition)
                            }
                        }
                        .animation(reduceMotion ? nil : HUTokens.springInteractive, value: messages.count + toolCalls.count)
                        .padding()
                    }
                    .onChange(of: messages.count) { _, _ in
                        if let last = messages.last {
                            if reduceMotion {
                                proxy.scrollTo(last.id, anchor: .bottom)
                            } else {
                                withAnimation(HUTokens.springInteractive) { proxy.scrollTo(last.id, anchor: .bottom) }
                            }
                        }
                    }
                }
            }
        }
    }

    @ViewBuilder
    private var emptyStateView: some View {
        VStack(spacing: HUTokens.spaceLg) {
            Spacer()
            VStack(spacing: HUTokens.spaceMd) {
                Image(systemName: "bubble.left.and.bubble.right")
                    .font(.custom("Avenir-Medium", size: HUTokens.text3Xl, relativeTo: .title))
                    .foregroundStyle(tokens.accent)
                    .accessibilityHidden(true)
                Text("Start a conversation")
                    .font(.custom("Avenir-Heavy", size: HUTokens.textLg, relativeTo: .body))
                    .foregroundStyle(tokens.text)
                Text("Send a message or choose a suggestion below")
                    .font(.custom("Avenir-Book", size: HUTokens.textSm, relativeTo: .subheadline))
                    .foregroundStyle(tokens.textMuted)
                VStack(spacing: HUTokens.spaceSm) {
                    ForEach(Self.suggestionChips, id: \.self) { chip in
                        Button {
                            inputText = chip
                            isInputFocused = true
                        } label: {
                            Text(chip)
                                .font(.custom("Avenir-Medium", size: HUTokens.textSm, relativeTo: .subheadline))
                                .foregroundStyle(tokens.accent)
                                .padding(.horizontal, HUTokens.spaceMd)
                                .padding(.vertical, HUTokens.spaceSm)
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(.top, HUTokens.spaceXs)
            }
            .padding(HUTokens.spaceXl)
            .background(.ultraThinMaterial)
            .clipShape(RoundedRectangle(cornerRadius: HUTokens.radiusXl, style: .continuous))
            .padding(.horizontal, HUTokens.spaceLg)
            Spacer()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    @ViewBuilder
    private func errorBannerView(_ err: String) -> some View {
        HStack {
            Text(err)
                .font(.custom("Avenir-Book", size: HUTokens.textXs, relativeTo: .caption))
                .foregroundStyle(tokens.error)
            Spacer()
            Button("Dismiss") { errorBanner = nil }
                .font(.custom("Avenir-Book", size: HUTokens.textXs, relativeTo: .caption))
        }
        .padding(HUTokens.spaceSm)
        .background(colorScheme == .dark ? HUTokens.Dark.errorDim : HUTokens.Light.errorDim)
        .cornerRadius(HUTokens.radiusMd)
    }

    private func sendMessage() {
        let trimmed = inputText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        inputText = ""
        sendTrigger += 1
        isSending = true
        if reduceMotion {
            messages.append(MacChatMessage(id: UUID(), text: trimmed, role: .user))
        } else {
            withAnimation(HUTokens.springInteractive) {
                messages.append(MacChatMessage(id: UUID(), text: trimmed, role: .user))
            }
        }

        isSendingWatchdogGeneration += 1
        let watchdogId = isSendingWatchdogGeneration
        DispatchQueue.main.asyncAfter(deadline: .now() + 30) {
            if watchdogId == isSendingWatchdogGeneration {
                isSending = false
            }
        }

        Task {
            if status.isGatewayConnected {
                status.gatewayClient.request(method: Methods.chatSend, params: ["message": trimmed]) { result in
                    Task { @MainActor in
                        if case let .failure(err) = result {
                            Task { await self.tryOnDeviceFallback(trimmed, gatewayError: err) }
                        }
                    }
                }
            } else {
                await tryOnDeviceFallback(trimmed, gatewayError: nil)
            }
        }
    }

    private func tryOnDeviceFallback(_ message: String, gatewayError: Error?) async {
        if let reply = await status.gatewayClient.chatOnDevice(message: message) {
            await MainActor.run {
                isSending = false
                let append = { self.messages.append(MacChatMessage(id: UUID(), text: reply, role: .assistant)) }
                if reduceMotion { append() } else { withAnimation(HUTokens.springInteractive) { append() } }
            }
        } else {
            let desc = gatewayError?.localizedDescription ?? "Gateway disconnected and on-device AI unavailable"
            await MainActor.run {
                isSending = false
                let line = "Failed to send: \(desc)"
                let append = { self.messages.append(MacChatMessage(id: UUID(), text: line, role: .assistant)) }
                if reduceMotion { append() } else { withAnimation(HUTokens.springInteractive) { append() } }
            }
        }
    }

    private func handleGatewayEvent(_ event: String, payload: [String: Any]?) {
        switch event {
        case "error":
            isSending = false
            let msg = payload?["message"] as? String ?? payload?["error"] as? String ?? "Unknown error"
            errorBanner = msg.isEmpty ? "Unknown error" : msg
        case "chat":
            let state = payload?["state"] as? String
            let content = payload?["message"] as? String
            if let content = content, !content.isEmpty {
                if state == "sent" || state == "chunk" { isSending = false }
                if reduceMotion {
                    applyChatState(state: state, content: content)
                } else {
                    withAnimation(HUTokens.springInteractive) {
                        applyChatState(state: state, content: content)
                    }
                }
            }
        case "agent.tool":
            let name = payload?["message"] as? String ?? payload?["tool"] as? String ?? payload?["name"] as? String ?? "tool"
            let args = (payload?["arguments"] as? [String: Any])
                .flatMap { try? JSONSerialization.data(withJSONObject: $0) }
                .flatMap { String(data: $0, encoding: .utf8) }
            if reduceMotion {
                applyToolPayload(name: name, args: args, payload: payload)
            } else {
                withAnimation(HUTokens.springInteractive) {
                    applyToolPayload(name: name, args: args, payload: payload)
                }
            }
        default:
            break
        }
    }

    private func applyChatState(state: String?, content: String) {
        switch state {
        case "sent":
            messages.append(MacChatMessage(id: UUID(), text: content, role: .assistant))
        case "chunk":
            if let last = messages.last, last.role == .assistant {
                messages[messages.count - 1] = MacChatMessage(
                    id: last.id, text: last.text + content, role: .assistant
                )
            } else {
                messages.append(MacChatMessage(id: UUID(), text: content, role: .assistant))
            }
        default:
            break
        }
    }

    private func applyToolPayload(name: String, args: String?, payload: [String: Any]?) {
        if let ok = payload?["success"] as? Bool, !toolCalls.isEmpty {
            let idx = toolCalls.count - 1
            let result = (payload?["detail"] as? String) ?? (payload?["message"] as? String)
            toolCalls[idx] = MacToolCallInfo(
                id: toolCalls[idx].id,
                name: toolCalls[idx].name,
                arguments: toolCalls[idx].arguments,
                status: ok ? .completed : .failed,
                result: result
            )
        } else {
            toolCalls.append(MacToolCallInfo(
                id: UUID(),
                name: name,
                arguments: args,
                status: .running,
                result: nil
            ))
        }
    }
}

struct MacSessionsPane: View {
    let tokens: (bgSurface: Color, surfaceContainer: Color, surfaceContainerHigh: Color, text: Color, textMuted: Color, accent: Color, success: Color, error: Color)
    @EnvironmentObject private var status: StatusViewModel
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var appeared = false

    private var activeSessions: [MacSessionRow] {
        status.sessionRows.filter { !$0.isArchived }
    }

    private var archivedSessions: [MacSessionRow] {
        status.sessionRows.filter(\.isArchived)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: HUTokens.spaceMd) {
            Text("Sessions")
                .font(.custom("Avenir-Heavy", size: HUTokens.textXl))
                .foregroundStyle(tokens.text)
                .padding(.horizontal, HUTokens.spaceLg)
                .padding(.top, HUTokens.spaceLg)

            if status.sessionRows.isEmpty {
                Text(status.isGatewayConnected ? "No sessions yet." : "Connect to the gateway to load sessions.")
                    .font(.custom("Avenir-Book", size: HUTokens.textSm))
                    .foregroundStyle(tokens.textMuted)
                    .padding(.horizontal, HUTokens.spaceLg)
            } else {
                List {
                    ForEach(Array(activeSessions.enumerated()), id: \.element.id) { index, session in
                        HStack {
                            Image(systemName: "bubble.left.and.bubble.right")
                                .foregroundStyle(tokens.accent)
                                .accessibilityHidden(true)
                            VStack(alignment: .leading, spacing: HUTokens.spaceXs) {
                                HStack(spacing: HUTokens.spaceXs) {
                                    Text(session.title)
                                        .font(.custom("Avenir-Heavy", size: HUTokens.textBase))
                                        .foregroundStyle(tokens.text)
                                    Text("\(session.messageCount)")
                                        .font(.custom("Avenir-Medium", size: HUTokens.textXs, relativeTo: .caption))
                                        .foregroundStyle(tokens.accent)
                                        .padding(.horizontal, HUTokens.spaceXs)
                                        .padding(.vertical, 2)
                                        .background(tokens.accent.opacity(HUTokens.opacityOverlayLight))
                                        .clipShape(Capsule())
                                }
                                Text(String(session.preview.prefix(40)) + (session.preview.count > 40 ? "…" : ""))
                                    .font(.custom("Avenir-Book", size: HUTokens.textXs))
                                    .foregroundStyle(tokens.textMuted)
                                    .lineLimit(1)
                                Text(session.relativeTime)
                                    .font(.custom("Avenir-Book", size: HUTokens.textXs))
                                    .foregroundStyle(tokens.textMuted)
                            }
                            Spacer()
                            Text("\(session.messageCount) msgs")
                                .font(.custom("Avenir-Book", size: HUTokens.textXs))
                                .foregroundStyle(tokens.textMuted)
                        }
                        .accessibilityElement(children: .combine)
                        .accessibilityLabel("\(session.title), \(session.messageCount) messages, \(session.relativeTime), preview: \(session.preview)")
                        .transition(.asymmetric(
                            insertion: .move(edge: .bottom).combined(with: .opacity),
                            removal: .opacity
                        ))
                        .opacity(appeared ? 1 : 0)
                        .animation(
                            reduceMotion ? nil : springMotion9.delay(Double(Swift.min(index, 6)) * 0.05),
                            value: appeared
                        )
                    }
                    if !archivedSessions.isEmpty {
                        Section {
                            ForEach(Array(archivedSessions.enumerated()), id: \.element.id) { index, session in
                                HStack {
                                    Image(systemName: "archivebox")
                                        .foregroundStyle(tokens.textMuted)
                                        .accessibilityHidden(true)
                                    VStack(alignment: .leading, spacing: HUTokens.spaceXs) {
                                        Text(session.title)
                                            .font(.custom("Avenir-Heavy", size: HUTokens.textBase))
                                            .foregroundStyle(tokens.textMuted)
                                        Text(String(session.preview.prefix(40)) + (session.preview.count > 40 ? "…" : ""))
                                            .font(.custom("Avenir-Book", size: HUTokens.textXs))
                                            .foregroundStyle(tokens.textMuted)
                                            .lineLimit(1)
                                    }
                                    Spacer()
                                }
                                .opacity(appeared ? 1 : 0)
                                .animation(
                                    reduceMotion ? nil : springMotion9.delay(Double(Swift.min(index, 4)) * 0.05),
                                    value: appeared
                                )
                            }
                        } header: {
                            Text("Archived")
                                .font(.custom("Avenir-Medium", size: HUTokens.textSm))
                                .foregroundStyle(tokens.textMuted)
                        }
                    }
                }
            }
        }
        .onAppear {
            status.fetchSessionsList()
            if reduceMotion {
                appeared = true
            } else {
                withAnimation(springMotion9) { appeared = true }
            }
        }
    }
}

struct MacToolsPane: View {
    let tokens: (bgSurface: Color, surfaceContainer: Color, surfaceContainerHigh: Color, text: Color, textMuted: Color, accent: Color, success: Color, error: Color)
    @EnvironmentObject private var status: StatusViewModel
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var appeared = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: HUTokens.spaceMd) {
                Text("Tools")
                    .font(.custom("Avenir-Heavy", size: HUTokens.textXl))
                    .foregroundStyle(tokens.text)

                if status.toolRows.isEmpty {
                    Text(status.isGatewayConnected ? "No tools in catalog." : "Connect to the gateway to load tools.")
                        .font(.custom("Avenir-Book", size: HUTokens.textSm))
                        .foregroundStyle(tokens.textMuted)
                } else {
                    LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: HUTokens.spaceMd), count: 3), spacing: HUTokens.spaceMd) {
                        ForEach(Array(status.toolRows.enumerated()), id: \.element.id) { index, tool in
                            MacToolCard(tool: tool, tokens: tokens, index: index, appeared: appeared, reduceMotion: reduceMotion)
                        }
                    }
                }
            }
            .padding(HUTokens.spaceLg)
        }
        .onAppear {
            status.fetchToolsCatalog()
            if reduceMotion {
                appeared = true
            } else {
                withAnimation(springMotion9) { appeared = true }
            }
        }
    }
}

private struct MacToolCard: View {
    let tool: MacToolRow
    let tokens: (bgSurface: Color, surfaceContainer: Color, surfaceContainerHigh: Color, text: Color, textMuted: Color, accent: Color, success: Color, error: Color)
    let index: Int
    let appeared: Bool
    let reduceMotion: Bool
    @State private var isHovered = false

    var body: some View {
        VStack(alignment: .leading, spacing: HUTokens.spaceSm) {
            Image(systemName: "wrench.and.screwdriver")
                .font(.custom("Avenir-Medium", size: HUTokens.textLg))
                .foregroundStyle(tokens.accent)
                .accessibilityHidden(true)
            Text(tool.name)
                .font(.custom("Avenir-Heavy", size: HUTokens.textBase))
                .foregroundStyle(tokens.text)
            Text(tool.description)
                .font(.custom("Avenir-Book", size: HUTokens.textXs))
                .foregroundStyle(tokens.textMuted)
                .lineLimit(3)
            Text(tool.category)
                .font(.custom("Avenir-Book", size: HUTokens.textXs))
                .foregroundStyle(tokens.textMuted)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(HUTokens.spaceMd)
        .background(tokens.surfaceContainerHigh)
        .clipShape(RoundedRectangle(cornerRadius: HUTokens.radiusLg, style: .continuous))
        .scaleEffect(isHovered ? 1.02 : 1.0)
        .animation(reduceMotion ? nil : springMotion9, value: isHovered)
        .onHover { isHovered = $0 }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("\(tool.name): \(tool.description), category \(tool.category)")
        .transition(.asymmetric(
            insertion: .move(edge: .bottom).combined(with: .opacity),
            removal: .opacity
        ))
        .opacity(appeared ? 1 : 0)
        .animation(
            reduceMotion ? nil : springMotion9.delay(Double(Swift.min(index, 6)) * 0.05),
            value: appeared
        )
    }
}

private struct SparklineView: View {
    let data: [CGFloat]
    let color: Color

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width / CGFloat(max(1, data.count - 1))
            let maxVal = data.max() ?? 1
            let minVal = data.min() ?? 0
            let range = max(maxVal - minVal, 0.001)
            Path { path in
                for (i, v) in data.enumerated() {
                    let x = CGFloat(i) * w
                    let y = geo.size.height * (1 - (v - minVal) / range)
                    if i == 0 {
                        path.move(to: CGPoint(x: x, y: y))
                    } else {
                        path.addLine(to: CGPoint(x: x, y: y))
                    }
                }
            }
            .stroke(color, style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
        }
    }
}
