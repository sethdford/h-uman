import SwiftUI

/// Text field plus send button for chat input.
///
/// Wraps a vertically-resizing `TextField` (1–6 lines) and a circular send
/// button. The button is disabled while `text` is whitespace-only. Pass a
/// `FocusState` binding to drive keyboard focus from the parent view.
@available(macOS 14.0, iOS 17.0, *)
public struct ChatInputBar: View {
    @Environment(\.colorScheme) private var colorScheme
    /// Two-way binding to the message text being composed.
    @Binding public var text: String
    /// Invoked when the user taps Send or submits the field.
    public let onSend: () -> Void
    /// Placeholder string shown when the field is empty.
    public var placeholder: String = "Message"
    /// Monotonic counter the parent increments to trigger the send-button bounce animation.
    public var sendTrigger: Int = 0
    /// Optional focus binding letting the parent drive keyboard focus.
    public var focusBinding: FocusState<Bool>.Binding?

    /// Build a chat input bar.
    ///
    /// - Parameters:
    ///   - text: Two-way binding to the composed message.
    ///   - onSend: Action invoked when the user taps Send or submits.
    ///   - placeholder: Placeholder for the empty field (defaults to `"Message"`).
    ///   - sendTrigger: Monotonic counter to trigger the bounce animation.
    ///   - focus: Optional focus binding (defaults to `nil`).
    public init(
        text: Binding<String>,
        onSend: @escaping () -> Void,
        placeholder: String = "Message",
        sendTrigger: Int = 0,
        focus: FocusState<Bool>.Binding? = nil
    ) {
        self._text = text
        self.onSend = onSend
        self.placeholder = placeholder
        self.sendTrigger = sendTrigger
        self.focusBinding = focus
    }

    private var tokens: (bgSurface: Color, accent: Color, textMuted: Color) {
        colorScheme == .dark ? (HUTokens.Dark.bgSurface, HUTokens.Dark.accent, HUTokens.Dark.textMuted) : (HUTokens.Light.bgSurface, HUTokens.Light.accent, HUTokens.Light.textMuted)
    }

    @ViewBuilder
    private var inputField: some View {
        if let focus = focusBinding {
            TextField(placeholder, text: $text, axis: .vertical)
                .focused(focus)
        } else {
            TextField(placeholder, text: $text, axis: .vertical)
        }
    }

    /// SwiftUI body for the input bar.
    public var body: some View {
        HStack(spacing: HUTokens.spaceMd) {
            inputField
                .textFieldStyle(.plain)
                .lineLimit(1...6)
                .font(.custom("Avenir-Book", size: HUTokens.textBase, relativeTo: .body))
                .padding(.horizontal, HUTokens.spaceMd)
                .padding(.vertical, HUTokens.spaceSm)
                .background(tokens.bgSurface)
                .clipShape(RoundedRectangle(cornerRadius: HUTokens.radiusXl, style: .continuous))
                .accessibilityLabel("Message input")
                .onSubmit { onSend() }

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

#Preview("Light") {
    struct PreviewWrapper: View {
        @State private var text = ""
        var body: some View {
            ChatInputBar(text: $text) {}
        }
    }
    return PreviewWrapper()
        .padding()
        .preferredColorScheme(.light)
}

#Preview("Dark") {
    struct PreviewWrapper: View {
        @State private var text = ""
        var body: some View {
            ChatInputBar(text: $text) {}
        }
    }
    return PreviewWrapper()
        .padding()
        .preferredColorScheme(.dark)
}
