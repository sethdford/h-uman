import Foundation

/// Main coordinator for the helper dylib. Called from DylibEntry on load.
/// Resolves IMCore symbols, eagerly initializes the IMCore singletons (so the
/// first action doesn't block 30-120s), connects to the daemon, and dispatches
/// JSON action commands to the IMCore selectors in MessageActions/ChatActions.
final class IMHelper {
    static var shared: IMHelper?
    static var tcp: TCPClient?

    static func bootstrap() {
        let bundleId = Bundle.main.bundleIdentifier ?? "unknown"
        let isMessages = bundleId == "com.apple.MobileSMS" || bundleId == "com.apple.Messages"
        guard isMessages else {
            Log.error("bootstrap: unsupported process \(bundleId)")
            return
        }

        shared = IMHelper()
        resolvePrivateSymbols()

        let client = TCPClient()
        tcp = client

        client.onConnect = {
            client.send(["event": "ping", "message": "Helper Connected!", "process": bundleId])
            // Eagerly init IMCore singletons on the main queue, then signal ready.
            DispatchQueue.main.async {
                Log.info("eagerly initializing IMCore…")
                if let controller = getSharedInstance("IMAccountController") {
                    _ = safePerformReturning(controller, selector: "activeIMessageAccount")
                }
                _ = getSharedInstance("IMChatRegistry")
                Log.info("IMCore initialized, sending ready")
                client.send(["event": "ready", "process": bundleId])
            }
        }
        client.onMessage = { IMHelper.handleMessage($0) }
        client.connect()
    }

    // MARK: - Dispatch

    static func handleMessage(_ raw: String) {
        guard let jsonData = raw.data(using: .utf8),
              let dict = try? JSONSerialization.jsonObject(with: jsonData) as? [String: Any] else {
            Log.error("handleMessage: failed to parse JSON: \(raw)")
            return
        }
        let action = dict["action"] as? String ?? ""
        let data = dict["data"] as? [String: Any] ?? [:]
        let transaction: String? = {
            guard let val = dict["transactionId"], !(val is NSNull) else { return nil }
            return val as? String
        }()

        switch action {
        case "send-message", "send-reaction":
            MessageActions.sendMessage(data: data, transaction: transaction)
        case "edit-message":
            MessageActions.editMessage(data: data, transaction: transaction)
        case "unsend-message":
            MessageActions.unsendMessage(data: data, transaction: transaction)
        case "delete-message":
            MessageActions.deleteMessage(data: data, transaction: transaction)
        case "start-typing":
            ChatActions.startTyping(data: data, transaction: transaction)
        case "stop-typing":
            ChatActions.stopTyping(data: data, transaction: transaction)
        default:
            Log.info("handleMessage: unimplemented action '\(action)'")
            IMHelper.respondError(transaction: transaction, error: "unimplemented action: \(action)")
        }
    }

    // MARK: - Responses

    static func respond(transaction: String?, extra: [String: Any] = [:]) {
        guard let transaction = transaction else { return }
        var msg: [String: Any] = ["transactionId": transaction]
        for (k, v) in extra { msg[k] = v }
        tcp?.send(msg)
    }

    static func respondError(transaction: String?, error: String) {
        guard let transaction = transaction else { return }
        tcp?.send(["transactionId": transaction, "error": error])
    }
}
