import AppIntents
import SwiftUI

struct SendMessageIntent: AppIntent {
    static var title: LocalizedStringResource = "Send Message to h-uman"
    static var description = IntentDescription("Send a message to your h-uman AI assistant.")
    static var openAppWhenRun = true

    @Parameter(title: "Message")
    var message: String

    func perform() async throws -> some IntentResult & ProvidesDialog {
        return .result(dialog: "Sending '\(message)' to h-uman...")
    }
}

struct CheckStatusIntent: AppIntent {
    static var title: LocalizedStringResource = "Check h-uman Status"
    static var description = IntentDescription("Check whether your h-uman AI assistant is connected and running.")
    static var openAppWhenRun = false

    func perform() async throws -> some IntentResult & ProvidesDialog {
        return .result(dialog: "h-uman is ready.")
    }
}

struct HumanShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: SendMessageIntent(),
            phrases: [
                "Send \(\.$message) to \(.applicationName)",
                "Ask \(.applicationName) \(\.$message)",
                "Tell \(.applicationName) \(\.$message)",
            ],
            shortTitle: "Send Message",
            systemImageName: "bubble.left.fill"
        )
        AppShortcut(
            intent: CheckStatusIntent(),
            phrases: [
                "Check \(.applicationName) status",
                "Is \(.applicationName) running",
            ],
            shortTitle: "Check Status",
            systemImageName: "antenna.radiowaves.left.and.right"
        )
    }
}
