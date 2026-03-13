import SwiftUI
import HumanChatUI
import HumanProtocol

struct ChatView: View {
    @Environment(\.colorScheme) private var colorScheme
    @EnvironmentObject var connectionManager: ConnectionManager
    @State private var messages: [ChatMessage] = []
    @State private var inputText = ""
    @State private var toolCalls: [ToolCallInfo] = []
    @State private var errorBanner: String?
    @State private var sendTrigger: Int = 0

    private var tokens: (bgSurface: Color, error: Color, errorDim: Color, success: Color) {
        if colorScheme == .dark {
            return (HUTokens.Dark.bgSurface, HUTokens.Dark.error, HUTokens.Dark.errorDim, HUTokens.Dark.success)
        } else {
            return (HUTokens.Light.bgSurface, HUTokens.Light.error, HUTokens.Light.errorDim, HUTokens.Light.success)
        }
    }

    private let messageTransition = AnyTransition.asymmetric(
        insertion: .move(edge: .bottom).combined(with: .opacity),
        removal: .opacity
    )

    var body: some View {
        NavigationStack {
            mainContent
                .navigationTitle("Chat")
                .toolbar { toolbarContent }
                .onAppear {
                    connectionManager.setEventHandler { event, payload in
                        handleEvent(event, payload: payload)
                    }
                    connectionManager.connect()
                }
        }
    }

    @ViewBuilder
    private var mainContent: some View {
        VStack(spacing: 0) {
            messageList
            if let err = errorBanner { errorBannerView(err) }
            ChatInputBar(text: $inputText, onSend: { sendMessage() }, sendTrigger: sendTrigger)
                .background(tokens.bgSurface)
        }
    }

    @ViewBuilder
    private var messageList: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: HUTokens.spaceMd) {
                    ForEach(messages) { msg in
                        ChatBubble(text: msg.text, role: msg.role)
                            .id(msg.id)
                            .transition(messageTransition)
                            .accessibilityLabel(msg.text)
                    }
                    ForEach(toolCalls) { tc in
                        ToolCallCard(name: tc.name, arguments: tc.arguments, status: tc.status, result: tc.result)
                            .id(tc.id)
                            .transition(messageTransition)
                    }
                }
                .animation(HUTokens.springExpressive, value: messages.count + toolCalls.count)
                .padding()
            }
            .onChange(of: messages.count) { _, _ in
                if let last = messages.last {
                    withAnimation(HUTokens.springExpressive) { proxy.scrollTo(last.id, anchor: .bottom) }
                }
            }
        }
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
        .background(tokens.errorDim)
        .cornerRadius(HUTokens.radiusMd)
    }

    @ToolbarContentBuilder
    private var toolbarContent: some ToolbarContent {
        ToolbarItem(placement: .primaryAction) {
            HStack(spacing: HUTokens.spaceSm) {
                connectionIndicator
                if connectionManager.isConnected {
                    Text("Connected")
                        .font(.custom("Avenir-Book", size: HUTokens.textXs, relativeTo: .caption))
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    @ViewBuilder
    private var connectionIndicator: some View {
        if #available(iOS 17, *) {
            Image(systemName: connectionManager.isConnected ? "antenna.radiowaves.left.and.right" : "wifi.slash")
                .font(.caption)
                .foregroundStyle(connectionManager.isConnected ? tokens.success : tokens.error)
                .contentTransition(.symbolEffect(.replace))
        } else {
            Circle()
                .fill(connectionManager.isConnected ? tokens.success : tokens.error)
                .frame(width: 8, height: 8)
        }
    }

    private func sendMessage() {
        let trimmed = inputText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }

#if os(iOS)
        let impact = UIImpactFeedbackGenerator(style: .light)
        impact.impactOccurred()
#endif

        inputText = ""
        sendTrigger += 1

        withAnimation(HUTokens.springExpressive) {
            messages.append(ChatMessage(id: UUID(), text: trimmed, role: .user))
        }

        Task {
            do {
                _ = try await connectionManager.request(
                    method: Methods.chatSend,
                    params: ["message": AnyCodable(trimmed)]
                )
            } catch {
                await MainActor.run {
                    withAnimation(HUTokens.springExpressive) {
                        messages.append(ChatMessage(
                            id: UUID(),
                            text: "Failed to send: \(error.localizedDescription)",
                            role: .assistant
                        ))
                    }
                }
            }
        }
    }

    private func handleEvent(_ event: String, payload: [String: AnyCodable]?) {
        DispatchQueue.main.async {
            switch event {
            case "error":
                let msg = payload?["message"]?.value as? String ?? payload?["error"]?.value as? String ?? "Unknown error"
                errorBanner = msg.isEmpty ? "Unknown error" : msg
            case "health":
                break // Connection status handled by ConnectionManager state
            case "chat":
                let state = payload?["state"]?.value as? String
                let content = payload?["message"]?.value as? String
                if let content = content, !content.isEmpty {
                    withAnimation(HUTokens.springExpressive) {
                        switch state {
                        case "received":
                            messages.append(ChatMessage(id: UUID(), text: content, role: .user))
                        case "sent":
                            messages.append(ChatMessage(id: UUID(), text: content, role: .assistant))
                        case "chunk":
                            if let last = messages.last, last.role == .assistant {
                                messages[messages.count - 1] = ChatMessage(
                                    id: last.id, text: last.text + content, role: .assistant
                                )
                            } else {
                                messages.append(ChatMessage(id: UUID(), text: content, role: .assistant))
                            }
                        default:
                            break
                        }
                    }
                }
            case "agent.tool":
                let name = payload?["message"]?.value as? String ?? payload?["tool"]?.value as? String ?? payload?["name"]?.value as? String ?? "tool"
                let args = (payload?["arguments"]?.value as? [String: Any])
                    .flatMap { try? JSONSerialization.data(withJSONObject: $0) }
                    .flatMap { String(data: $0, encoding: .utf8) }
                withAnimation(HUTokens.springExpressive) {
                    if let ok = payload?["success"]?.value as? Bool, !toolCalls.isEmpty {
                        let idx = toolCalls.count - 1
                        let result = (payload?["detail"]?.value ?? payload?["message"]?.value).map { "\($0)" }
                        toolCalls[idx] = ToolCallInfo(
                            id: toolCalls[idx].id,
                            name: toolCalls[idx].name,
                            arguments: toolCalls[idx].arguments,
                            status: ok ? .completed : .failed,
                            result: result
                        )
                    } else {
                        toolCalls.append(ToolCallInfo(
                            id: UUID(),
                            name: name,
                            arguments: args,
                            status: .running
                        ))
                    }
                }
            default:
                break
            }
        }
    }
}

struct ChatMessage: Identifiable {
    let id: UUID
    let text: String
    let role: ChatBubble.Role
}

struct ToolCallInfo: Identifiable {
    let id: UUID
    let name: String
    let arguments: String?
    var status: ToolCallCard.Status
    var result: String?
}
