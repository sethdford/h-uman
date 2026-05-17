import SwiftUI

/// Card displaying tool-call status (running, completed, or failed).
///
/// Shows a status icon, tool name, optional arguments preview, optional result
/// preview, and a spinner while running. Colors come from `HUTokens`.
@available(macOS 14.0, iOS 17.0, *)
public struct ToolCallCard: View {
    @Environment(\.colorScheme) private var colorScheme

    /// Lifecycle state of the tool call.
    public enum Status {
        /// The tool is currently executing.
        case running
        /// The tool finished successfully.
        case completed
        /// The tool exited with an error.
        case failed
    }

    /// Tool name (e.g. `"shell"`).
    public let name: String
    /// JSON-encoded arguments preview, or `nil` to hide the row.
    public let arguments: String?
    /// Current lifecycle state.
    public let status: Status
    /// Stringified result preview, or `nil` to hide the row.
    public let result: String?

    /// Build a tool-call card.
    ///
    /// - Parameters:
    ///   - name: Tool name to display in the header.
    ///   - arguments: Optional JSON-encoded arguments preview.
    ///   - status: Lifecycle state to render.
    ///   - result: Optional stringified result preview.
    public init(name: String, arguments: String? = nil, status: Status, result: String? = nil) {
        self.name = name
        self.arguments = arguments
        self.status = status
        self.result = result
    }

    private var tokens: (bgElevated: Color, textMuted: Color, warning: Color, success: Color, error: Color) {
        if colorScheme == .dark {
            return (HUTokens.Dark.bgElevated, HUTokens.Dark.textMuted, HUTokens.Dark.warning, HUTokens.Dark.success, HUTokens.Dark.error)
        } else {
            return (HUTokens.Light.bgElevated, HUTokens.Light.textMuted, HUTokens.Light.warning, HUTokens.Light.success, HUTokens.Light.error)
        }
    }

    /// SwiftUI body for the tool-call card.
    public var body: some View {
        VStack(alignment: .leading, spacing: HUTokens.spaceSm) {
            HStack(spacing: HUTokens.spaceSm) {
                Image(systemName: statusIcon)
                    .foregroundStyle(statusColor)
                Text(name)
                    .font(.custom("Avenir-Medium", size: HUTokens.textSm, relativeTo: .subheadline))
                Spacer()
                if status == .running {
                    ProgressView()
                        .scaleEffect(0.8)
                }
            }

            if let args = arguments, !args.isEmpty {
                Text(args)
                    .font(.custom("Avenir-Book", size: HUTokens.textXs, relativeTo: .caption))
                    .foregroundStyle(tokens.textMuted)
                    .lineLimit(2)
            }

            if let res = result, !res.isEmpty {
                Text(res)
                    .font(.custom("Avenir-Book", size: HUTokens.textXs, relativeTo: .caption))
                    .foregroundStyle(tokens.textMuted)
                    .lineLimit(3)
            }
        }
        .padding(HUTokens.spaceMd)
        .background(tokens.bgElevated)
        .clipShape(RoundedRectangle(cornerRadius: HUTokens.radiusLg, style: .continuous))
        .accessibilityElement(children: .combine)
    }

    private var statusIcon: String {
        switch status {
        case .running: return "gearshape.2"
        case .completed: return "checkmark.circle.fill"
        case .failed: return "exclamationmark.triangle.fill"
        }
    }

    private var statusColor: Color {
        switch status {
        case .running: return tokens.warning
        case .completed: return tokens.success
        case .failed: return tokens.error
        }
    }
}

#Preview("Light") {
    VStack(spacing: HUTokens.spaceMd) {
        ToolCallCard(name: "shell", arguments: "{\"cmd\":\"ls\"}", status: .running)
        ToolCallCard(name: "browser", status: .completed, result: "Opened page")
    }
    .padding()
    .preferredColorScheme(.light)
}

#Preview("Dark") {
    VStack(spacing: HUTokens.spaceMd) {
        ToolCallCard(name: "shell", arguments: "{\"cmd\":\"ls\"}", status: .running)
        ToolCallCard(name: "browser", status: .completed, result: "Opened page")
    }
    .padding()
    .preferredColorScheme(.dark)
}
