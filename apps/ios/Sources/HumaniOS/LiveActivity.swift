import ActivityKit
import SwiftUI
import WidgetKit

struct HumanChatAttributes: ActivityAttributes {
    public struct ContentState: Codable, Hashable {
        var lastMessage: String
        var messageCount: Int
        var isProcessing: Bool
    }

    var sessionName: String
    var channelName: String
}

@available(iOS 16.2, *)
struct HumanLiveActivity: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: HumanChatAttributes.self) { context in
            HStack(spacing: 12) {
                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 6) {
                        Circle()
                            .fill(context.state.isProcessing ? Color.green : Color.gray)
                            .frame(width: 8, height: 8)
                        Text(context.attributes.sessionName)
                            .font(.custom("Avenir-Heavy", size: 14))
                            .foregroundStyle(.primary)
                    }
                    Text(context.state.lastMessage)
                        .font(.custom("Avenir-Book", size: 12))
                        .foregroundStyle(.secondary)
                        .lineLimit(2)
                }
                Spacer()
                VStack(alignment: .trailing, spacing: 2) {
                    Text("\(context.state.messageCount)")
                        .font(.custom("Avenir-Black", size: 20))
                        .foregroundStyle(.primary)
                    Text("messages")
                        .font(.custom("Avenir-Book", size: 10))
                        .foregroundStyle(.tertiary)
                }
            }
            .padding(16)
        } dynamicIsland: { context in
            DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    HStack(spacing: 6) {
                        Circle()
                            .fill(context.state.isProcessing ? Color.green : Color.gray)
                            .frame(width: 8, height: 8)
                        Text(context.attributes.channelName)
                            .font(.custom("Avenir-Medium", size: 12))
                            .foregroundStyle(.secondary)
                    }
                }
                DynamicIslandExpandedRegion(.trailing) {
                    Text("\(context.state.messageCount) msgs")
                        .font(.custom("Avenir-Medium", size: 12))
                        .foregroundStyle(.secondary)
                }
                DynamicIslandExpandedRegion(.bottom) {
                    Text(context.state.lastMessage)
                        .font(.custom("Avenir-Book", size: 14))
                        .lineLimit(2)
                }
            } compactLeading: {
                Circle()
                    .fill(context.state.isProcessing ? Color.green : Color.gray)
                    .frame(width: 10, height: 10)
            } compactTrailing: {
                Text("\(context.state.messageCount)")
                    .font(.custom("Avenir-Heavy", size: 14))
            } minimal: {
                Circle()
                    .fill(context.state.isProcessing ? Color.green : Color.gray)
                    .frame(width: 10, height: 10)
            }
        }
    }
}
