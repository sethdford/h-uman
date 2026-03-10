import SwiftUI

/// Card displaying tool call status (running, completed, failed).
public struct ToolCallCard: View {
    @Environment(\.colorScheme) private var colorScheme

    public enum Status {
        case running
        case completed
        case failed
    }

    public let name: String
    public let arguments: String?
    public let status: Status
    public let result: String?

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

#Preview {
    VStack(spacing: HUTokens.spaceMd) {
        ToolCallCard(name: "shell", arguments: "{\"cmd\":\"ls\"}", status: .running)
        ToolCallCard(name: "browser", status: .completed, result: "Opened page")
    }
    .padding()
}
