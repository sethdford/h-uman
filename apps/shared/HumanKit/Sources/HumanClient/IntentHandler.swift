import Foundation
import HumanProtocol

/// Typed response payload returned by `HumanGatewayClient.request(...)`.
///
/// Replaces the prior `Result<Any, Error>` API: `Any` is not `Sendable`,
/// so completion handlers could not safely cross task boundaries.
/// `GatewayResponse` is a value type with a `[String: AnyCodable]`
/// payload (`AnyCodable` is value-typed and `Sendable` via the
/// `AnyCodable: Sendable` conformance in `HumanProtocol`).
@available(macOS 14.0, iOS 17.0, *)
public struct GatewayResponse: Sendable {
    /// Decoded JSON payload returned by the gateway, or `nil` for empty
    /// responses.
    public let payload: [String: AnyCodable]?

    /// Create a `GatewayResponse` from a decoded payload.
    public init(payload: [String: AnyCodable]?) {
        self.payload = payload
    }

    /// Project the payload as a plain `[String: Any]` for legacy call
    /// sites that switch on dictionary keys.
    public var asDictionary: [String: Any] {
        payload?.mapValues { $0.value } ?? [:]
    }
}

/// Shared WebSocket client for App Intents and other call sites that
/// need a completion-handler API. Completion handlers are invoked on
/// the main actor — UI consumers do not need to hop explicitly.
@available(macOS 14.0, iOS 17.0, *)
public final class HumanGatewayClient: Sendable {
    /// Process-wide shared client.
    public static let shared = HumanGatewayClient()

    private let lock = NSLock()
    private nonisolated(unsafe) var _connection: HumanConnection?

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

    /// Send an RPC to the gateway. The `completion` closure is invoked
    /// on the main actor; pass a `@Sendable` closure since it crosses
    /// task boundaries.
    public func request(method: String,
                        params: [String: AnyCodable] = [:],
                        completion: @Sendable @escaping (Result<GatewayResponse, Error>) -> Void) {
        Task { [params] in
            do {
                let conn = try await ensureConnected()
                let codableParams: [String: AnyCodable]? = params.isEmpty ? nil : params
                let res = try await conn.request(method: method, params: codableParams)
                if res.ok {
                    let response = GatewayResponse(payload: res.payload)
                    await MainActor.run {
                        completion(.success(response))
                    }
                } else {
                    await MainActor.run {
                        completion(.failure(HumanGatewayClientError.rpcFailed))
                    }
                }
            } catch {
                await MainActor.run {
                    completion(.failure(error))
                }
            }
        }
    }
}

/// Errors thrown or returned by `HumanGatewayClient`.
@available(macOS 14.0, iOS 17.0, *)
public enum HumanGatewayClientError: Error, Sendable {
    /// The shared client could not establish a connection within the
    /// timeout window (`ensureConnected` retry budget exhausted).
    case notConnected
    /// The RPC succeeded at the transport layer but returned `ok: false`.
    case rpcFailed
}

/// Gateway client extension for App Intents / Siri integration.
/// Provides async message sending that App Intents can call.
@available(macOS 14.0, iOS 17.0, *)
public extension HumanGatewayClient {
    /// Send a message to the gateway and return assistant-facing text
    /// from the RPC payload or a status summary. Used by `AskHumanIntent`
    /// and `SendMessageIntent`.
    func sendMessage(_ message: String, channel: String? = nil) async throws -> String {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<String, Error>) in
            var params: [String: AnyCodable] = ["message": AnyCodable(message)]
            if let ch = channel {
                params["channel"] = AnyCodable(ch)
            }

            request(method: Methods.chatSend, params: params) { result in
                switch result {
                case .success(let response):
                    let payload = response.asDictionary
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
