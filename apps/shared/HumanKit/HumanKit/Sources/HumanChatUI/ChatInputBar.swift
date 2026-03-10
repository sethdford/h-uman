import SwiftUI

/// Text field and send button for chat input.
public struct ChatInputBar: View {
    @Environment(\.colorScheme) private var colorScheme
    @Binding public var text: String
    public let onSend: () -> Void
    public var placeholder: String = "Message"
    public var sendTrigger: Int = 0

    public init(text: Binding<String>, onSend: @escaping () -> Void, placeholder: String = "Message", sendTrigger: Int = 0) {
        self._text = text
        self.onSend = onSend
        self.placeholder = placeholder
        self.sendTrigger = sendTrigger
    }

    private var tokens: (bgSurface: Color, accent: Color, textMuted: Color) {
        colorScheme == .dark ? (HUTokens.Dark.bgSurface, HUTokens.Dark.accent, HUTokens.Dark.textMuted) : (HUTokens.Light.bgSurface, HUTokens.Light.accent, HUTokens.Light.textMuted)
    }

    public var body: some View {
        HStack(spacing: HUTokens.spaceMd) {
            TextField(placeholder, text: $text, axis: .vertical)
                .textFieldStyle(.plain)
                .lineLimit(1...6)
                .font(.custom("Avenir-Book", size: HUTokens.textBase, relativeTo: .body))
                .padding(.horizontal, HUTokens.spaceMd)
                .padding(.vertical, HUTokens.spaceSm)
                .background(tokens.bgSurface)
                .clipShape(RoundedRectangle(cornerRadius: HUTokens.radiusXl, style: .continuous))
                .accessibilityLabel("Message input")

            Button(action: onSend) {
                if #available(iOS 17, *) {
                    Image(systemName: "arrow.up.circle.fill")
                        .font(.title2)
                        .foregroundStyle(text.isEmpty ? tokens.textMuted : tokens.accent)
                        .symbolEffect(.bounce, value: sendTrigger)
                } else {
                    Image(systemName: "arrow.up.circle.fill")
                        .font(.title2)
                        .foregroundStyle(text.isEmpty ? tokens.textMuted : tokens.accent)
                }
            }
            .disabled(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            .accessibilityLabel("Send message")
            .accessibilityHint("Sends the current message")
        }
        .padding(.horizontal, HUTokens.spaceMd)
        .padding(.vertical, HUTokens.spaceSm)
    }
}

#Preview {
    struct PreviewWrapper: View {
        @State private var text = ""
        var body: some View {
            ChatInputBar(text: $text) {}
        }
    }
    return PreviewWrapper()
        .padding()
}
