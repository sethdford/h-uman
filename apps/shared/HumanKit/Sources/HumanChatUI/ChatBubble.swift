import SwiftUI

/// Chat message bubble with user vs assistant styling and basic Markdown rendering.
/// Supports: **bold**, *italic*, `inline code`, ```code blocks```, [links](url).
@available(macOS 14.0, iOS 17.0, *)
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

    private var tokens: (bgElevated: Color, accent: Color, onAccent: Color, text: Color,
                         codeBackground: Color, link: Color) {
        if colorScheme == .dark {
            return (HUTokens.Dark.bgElevated, HUTokens.Dark.accent, HUTokens.Dark.onAccent,
                    HUTokens.Dark.text, HUTokens.Dark.bgInset, HUTokens.Dark.link)
        } else {
            return (HUTokens.Light.bgElevated, HUTokens.Light.accent, HUTokens.Light.onAccent,
                    HUTokens.Light.text, HUTokens.Light.bgInset, HUTokens.Light.link)
        }
    }

    // MARK: - Content Segments

    private enum ContentSegment {
        case text(String)
        case codeBlock(String)
    }

    private var segments: [ContentSegment] {
        let parts = text.components(separatedBy: "```")
        var result: [ContentSegment] = []
        for (i, part) in parts.enumerated() {
            if part.isEmpty { continue }
            if i % 2 == 1 {
                // Code block — strip optional language identifier on first line
                var code = part
                if let newline = code.firstIndex(of: "\n") {
                    let firstLine = String(code[code.startIndex..<newline])
                    if firstLine.count < 20 &&
                        firstLine.allSatisfy({ $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" }) {
                        code = String(code[code.index(after: newline)...])
                    }
                }
                result.append(.codeBlock(code.trimmingCharacters(in: .whitespacesAndNewlines)))
            } else {
                result.append(.text(part))
            }
        }
        return result.isEmpty ? [.text(text)] : result
    }

    // MARK: - Body

    public var body: some View {
        HStack(alignment: .bottom, spacing: HUTokens.spaceSm) {
            if role == .user { Spacer(minLength: HUTokens.space2xl) }
            VStack(alignment: .leading, spacing: 0) {
                ForEach(Array(segments.enumerated()), id: \.offset) { _, segment in
                    switch segment {
                    case .text(let str):
                        let trimmed = str.trimmingCharacters(in: .whitespacesAndNewlines)
                        if !trimmed.isEmpty {
                            Text(renderInlineMarkdown(trimmed))
                        }
                    case .codeBlock(let code):
                        Text(code)
                            .font(.custom(HUTokens.fontMono, size: HUTokens.textSm, relativeTo: .body))
                            .foregroundStyle(role == .user ? tokens.onAccent : tokens.text)
                            .padding(HUTokens.spaceSm)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(tokens.codeBackground.opacity(0.5))
                            .clipShape(RoundedRectangle(cornerRadius: HUTokens.radiusSm, style: .continuous))
                            .padding(.vertical, HUTokens.spaceXs)
                    }
                }
            }
            .padding(.horizontal, HUTokens.spaceMd)
            .padding(.vertical, HUTokens.spaceSm)
            .background(role == .user ? tokens.accent : tokens.bgElevated)
            .clipShape(RoundedRectangle(cornerRadius: HUTokens.radiusXl, style: .continuous))
            if role == .assistant { Spacer(minLength: HUTokens.space2xl) }
        }
        .padding(.horizontal, HUTokens.spaceXs)
        .accessibilityLabel("\(role == .user ? "You" : "Assistant") said: \(text)")
    }

    // MARK: - Inline Markdown

    private func renderInlineMarkdown(_ input: String) -> AttributedString {
        let textColor = role == .user ? tokens.onAccent : tokens.text
        let linkColor = role == .user ? tokens.onAccent : tokens.link

        guard var attributed = try? AttributedString(
            markdown: input,
            options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)
        ) else {
            var plain = AttributedString(input)
            plain.font = .custom("Avenir-Book", size: HUTokens.textBase, relativeTo: .body)
            plain.foregroundColor = textColor
            return plain
        }

        attributed.font = .custom("Avenir-Book", size: HUTokens.textBase, relativeTo: .body)
        attributed.foregroundColor = textColor

        for run in attributed.runs {
            let range = run.range
            if let intent = run.inlinePresentationIntent {
                if intent.contains(.code) {
                    attributed[range].font = .custom(HUTokens.fontMono, size: HUTokens.textSm, relativeTo: .body)
                } else if intent.contains(.stronglyEmphasized) && intent.contains(.emphasized) {
                    attributed[range].font = .custom("Avenir-HeavyOblique", size: HUTokens.textBase, relativeTo: .body)
                } else if intent.contains(.stronglyEmphasized) {
                    attributed[range].font = .custom("Avenir-Heavy", size: HUTokens.textBase, relativeTo: .body)
                } else if intent.contains(.emphasized) {
                    attributed[range].font = .custom("Avenir-MediumOblique", size: HUTokens.textBase, relativeTo: .body)
                }
            }
            if run.link != nil {
                attributed[range].foregroundColor = linkColor
                attributed[range].underlineStyle = .single
            }
        }

        return attributed
    }
}

#Preview("Light") {
    VStack(spacing: HUTokens.spaceMd) {
        ChatBubble(text: "Hello, how can I help?", role: .assistant)
        ChatBubble(text: "What's the weather today?", role: .user)
        ChatBubble(text: "Here's some **bold** and *italic* with `code`", role: .assistant)
        ChatBubble(text: "```swift\nlet x = 42\nprint(x)\n```", role: .assistant)
        ChatBubble(text: "Check [this link](https://example.com) out", role: .assistant)
    }
    .padding()
    .preferredColorScheme(.light)
}

#Preview("Dark") {
    VStack(spacing: HUTokens.spaceMd) {
        ChatBubble(text: "Hello, how can I help?", role: .assistant)
        ChatBubble(text: "What's the weather today?", role: .user)
        ChatBubble(text: "Here's some **bold** and *italic* with `code`", role: .assistant)
        ChatBubble(text: "```swift\nlet x = 42\nprint(x)\n```", role: .assistant)
    }
    .padding()
    .preferredColorScheme(.dark)
}
