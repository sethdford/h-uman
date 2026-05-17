import Foundation
import HumanProtocol

/// Shared WebSocket client for App Intents and other call sites that need a
/// completion-handler API on top of `HumanConnection`'s async surface.
///
/// Backed by a lazily-constructed singleton `HumanConnection`; the URL is
/// read from `UserDefaults` key `Human.gatewayURL`, defaulting to
/// `wss://localhost:3000/ws`. Use `shared` rather than constructing instances.
@available(macOS 14.0, iOS 17.0, *)
public final class HumanGatewayClient: @unchecked Sendable {
    /// Process-wide singleton instance.
    public static let shared = HumanGatewayClient()

    private let lock = NSLock()
    private var _connection: HumanConnection?

    private init() {}

    private func connection() -> HumanConnection {
        lock.lock()
        defer { lock.unlock() }
        if let c = _connection {
            return c
        }
        let url = UserDefaults.standard.string(forKey: "Human.gatewayURL") ?? "wss://localhost:3000/ws"
        let c = HumanConnection(urlString: url)
        _connection = c
        return c
    }

    private func ensureConnected() async throws -> HumanConnection {
        let conn = connection()
        if conn.state == .connected {
            return conn
        }
        conn.connect()
        for _ in 0..<150 {
            if conn.state == .connected {
                return conn
            }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        throw HumanGatewayClientError.notConnected
    }

    /// Send an RPC to the gateway; on success the completion handler is invoked
    /// with the decoded payload dictionary (may be empty).
    ///
    /// - Parameters:
    ///   - method: RPC method name (see `Methods`).
    ///   - params: Method parameters keyed by name (defaults to empty).
    ///   - completion: Result callback invoked on the Swift concurrency
    ///     scheduler. Success wraps a `[String: Any]`; failure wraps a
    ///     `HumanGatewayClientError` or an underlying transport error.
    public func request(method: String, params: [String: Any] = [:],
                       completion: @escaping (Result<Any, Error>) -> Void) {
        Task {
            do {
                let conn = try await ensureConnected()
                let codableParams: [String: AnyCodable]? = params.isEmpty ? nil : params.mapValues { AnyCodable($0) }
                let res = try await conn.request(method: method, params: codableParams)
                if res.ok {
                    let dict = res.payload?.mapValues { $0.value } ?? [:]
                    completion(.success(dict))
                } else {
                    completion(.failure(HumanGatewayClientError.rpcFailed))
                }
            } catch {
                completion(.failure(error))
            }
        }
    }
}

/// Errors surfaced by `HumanGatewayClient.request(method:params:completion:)`.
@available(macOS 14.0, iOS 17.0, *)
public enum HumanGatewayClientError: Error {
    /// The shared connection never reached `.connected` within the wait budget.
    case notConnected
    /// The server returned `ok: false` for the request.
    case rpcFailed
}

/// Gateway client extension for App Intents / Siri integration.
///
/// Provides an async message-send entry point that App Intents (`AskHumanIntent`,
/// `SendMessageIntent`) call from intent handlers.
@available(macOS 14.0, iOS 17.0, *)
public extension HumanGatewayClient {
    /// Send a chat message to the gateway and return assistant-facing text from
    /// the response payload, falling back to a status summary when no text is
    /// present.
    ///
    /// - Parameters:
    ///   - message: User-facing message text.
    ///   - channel: Optional channel hint (e.g. `"siri"`); defaults to `nil`.
    /// - Returns: The first non-empty value among `response`, `content`, `text`,
    ///   or a `"<status> (session: <key>)"` summary, or the payload's
    ///   `String(describing:)` as a last resort.
    /// - Throws: Any error surfaced by the underlying RPC, including
    ///   `HumanGatewayClientError.rpcFailed` if the server responds with `ok: false`.
    func sendMessage(_ message: String, channel: String? = nil) async throws -> String {
        try await withCheckedThrowingContinuation { continuation in
            var params: [String: Any] = ["message": message]
            if let ch = channel {
                params["channel"] = ch
            }

            request(method: Methods.chatSend, params: params) { result in
                switch result {
                case .success(let response):
                    guard let payload = response as? [String: Any] else {
                        continuation.resume(returning: String(describing: response))
                        return
                    }
                    if let text = payload["response"] as? String, !text.isEmpty {
                        continuation.resume(returning: text)
                        return
                    }
                    if let text = payload["content"] as? String, !text.isEmpty {
                        continuation.resume(returning: text)
                        return
                    }
                    if let text = payload["text"] as? String, !text.isEmpty {
                        continuation.resume(returning: text)
                        return
                    }
                    if let status = payload["status"] as? String {
                        let sk = payload["sessionKey"] as? String
                        let suffix = sk.map { " (session: \($0))" } ?? ""
                        continuation.resume(returning: "\(status)\(suffix)")
                        return
                    }
                    continuation.resume(returning: String(describing: payload))
                case .failure(let error):
                    continuation.resume(throwing: error)
                }
            }
        }
    }
}
