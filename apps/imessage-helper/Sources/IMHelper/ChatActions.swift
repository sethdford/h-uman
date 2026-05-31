import Foundation

/// Chat-level actions. Typing uses IMChat's `setLocalUserIsTyping:`, which
/// triggers the real "…" bubble on the recipient — the same affordance the
/// Messages UI uses.
enum ChatActions {
    static func startTyping(data: [String: Any], transaction: String?) {
        guard let chat = getChat(guid: data["chatGuid"] as? String, transaction: transaction) else { return }
        safePerform(chat, selector: "setLocalUserIsTyping:", with: NSNumber(value: true))
        IMHelper.respond(transaction: transaction)
    }

    static func stopTyping(data: [String: Any], transaction: String?) {
        guard let chat = getChat(guid: data["chatGuid"] as? String, transaction: transaction) else { return }
        safePerform(chat, selector: "setLocalUserIsTyping:", with: NSNumber(value: false))
        IMHelper.respond(transaction: transaction)
    }
}
