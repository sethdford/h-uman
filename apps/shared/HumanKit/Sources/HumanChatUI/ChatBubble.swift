import SwiftUI

/// Chat message bubble with user vs assistant styling.
/// Uses HUTokens for accent, spacing, and radius.
public struct ChatBubble: View {
    @Environment(\.colorScheme) private var colorScheme

    public enum Role {
        case user
        case assistant
    }

    public let text: String
    public let role: Role

    public init(text: String, role: Role) {
        self.text = text
        self.role = role
    }

    private var tokens: (bgElevated: Color, accent: Color) {
        colorScheme == .dark ? (HUTokens.Dark.bgElevated, HUTokens.Dark.accent) : (HUTokens.Light.bgElevated, HUTokens.Light.accent)
    }

    public var body: some View {
        HStack(alignment: .bottom, spacing: HUTokens.spaceSm) {
            if role == .user { Spacer(minLength: HUTokens.space2xl) }
            Text(text)
                .font(.custom("Avenir-Book", size: HUTokens.textBase, relativeTo: .body))
                .padding(.horizontal, HUTokens.spaceMd)
                .padding(.vertical, HUTokens.spaceSm)
                .background(role == .user ? tokens.accent : tokens.bgElevated)
                .foregroundColor(role == .user ? .white : .primary)
                .clipShape(RoundedRectangle(cornerRadius: HUTokens.radiusXl, style: .continuous))
            if role == .assistant { Spacer(minLength: HUTokens.space2xl) }
        }
        .padding(.horizontal, HUTokens.spaceXs)
    }
}

#Preview {
    VStack(spacing: HUTokens.spaceMd) {
        ChatBubble(text: "Hello, how can I help?", role: .assistant)
        ChatBubble(text: "What's the weather today?", role: .user)
    }
    .padding()
}
