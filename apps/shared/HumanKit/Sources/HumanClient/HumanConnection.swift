import Foundation
import HumanProtocol

/// WebSocket client for the Human gateway control protocol.
///
/// Uses `URLSessionWebSocketTask` with automatic exponential-backoff
/// reconnection and a 25 s keepalive ping. Send-side methods are safe
/// to call from any thread; per-request continuations are guarded by an
/// internal lock.
@available(macOS 14.0, iOS 17.0, *)
public final class HumanConnection: @unchecked Sendable {
    /// Current state of the underlying WebSocket task.
    public enum ConnectionState: Equatable {
        /// No WebSocket task is active.
        case disconnected
        /// A task has been created but the `hello-ok` handshake hasn't completed.
        case connecting
        /// The handshake completed and the channel is ready for RPCs.
        case connected
    }

    /// Latest observed connection state. Setting this value notifies `stateHandler`.
    public private(set) var state: ConnectionState = .disconnected {
        didSet { stateHandler?(state) }
    }

    /// Invoked on every state transition with the new state.
    public var stateHandler: ((ConnectionState) -> Void)?

    /// Invoked once per server-pushed event frame with `(event, payload)`.
    public var eventHandler: ((String, [String: AnyCodable]?) -> Void)?

    private var task: URLSessionWebSocketTask?
    private var url: URL
    private let session = URLSession(configuration: .default)
    private let queue = DispatchQueue(label: "com.human.connection")
    private var reconnectWorkItem: DispatchWorkItem?
    private var pendingRequests: [String: CheckedContinuation<ControlResponse, Error>] = [:]
    private var requestTimeouts: [String: DispatchWorkItem] = [:]
    private let pendingLock = NSLock()
    private var reconnectAttempt: UInt = 0
    private var pingWorkItem: DispatchWorkItem?
    /// RPC wait timeout in seconds (matches web client default).
    public static var requestTimeoutSeconds: TimeInterval = 10
    private static let maxReconnectDelay: TimeInterval = 30
    private static let baseReconnectDelay: TimeInterval = 3

    /// Build a connection bound to a fully-formed gateway WebSocket URL.
    ///
    /// - Parameter url: WebSocket endpoint (e.g. `wss://host:3000/ws`).
    public init(url: URL) {
        self.url = url
    }

    /// Convenience initializer parsing a string URL, falling back to
    /// `wss://localhost:3000/ws` if the string is malformed.
    ///
    /// - Parameter urlString: Candidate WebSocket endpoint.
    public convenience init(urlString: String) {
        let url = URL(string: urlString) ?? URL(string: "wss://localhost:3000/ws")!
        self.init(url: url)
    }

    /// Open the WebSocket task and begin the `connect` handshake.
    public func connect() {
        queue.async { [weak self] in
            self?._connect()
        }
    }

    /// Tear down the WebSocket task, cancel any scheduled reconnect, and
    /// fail any in-flight RPCs with `HumanConnectionError.disconnected`.
    public func disconnect() {
        queue.async { [weak self] in
            self?.cancelPingSchedule()
            self?.reconnectWorkItem?.cancel()
            self?.reconnectWorkItem = nil
            self?.task?.cancel(with: .goingAway, reason: nil)
            self?.task = nil
            self?.failPendingRequests()
            self?.reconnectAttempt = 0
            self?.state = .disconnected
        }
    }

    /// Send a raw JSON message without waiting for a response.
    ///
    /// - Parameter json: Pre-encoded JSON text. Caller is responsible for shape.
    public func sendMessage(_ json: String) {
        guard state == .connected else { return }
        task?.send(.string(json)) { _ in }
    }

    /// Register a push token with the gateway for push notifications.
    ///
    /// - Parameter token: APNs or FCM device token.
    public func registerPushToken(token: String) {
        let id = "push-\(UUID().uuidString)"
        let params: [String: AnyCodable] = [
            "token": AnyCodable(token),
            "provider": AnyCodable("apns")
        ]
        let req = ControlRequest(id: id, method: Methods.pushRegister, params: params)
        guard let data = try? JSONEncoder().encode(req),
              let text = String(data: data, encoding: .utf8) else { return }
        sendMessage(text)
    }

    /// Unregister a push token from the gateway.
    ///
    /// - Parameter token: The token previously passed to `registerPushToken`.
    public func unregisterPushToken(token: String) {
        let id = "pushu-\(UUID().uuidString)"
        let params: [String: AnyCodable] = ["token": AnyCodable(token)]
        let req = ControlRequest(id: id, method: Methods.pushUnregister, params: params)
        guard let data = try? JSONEncoder().encode(req),
              let text = String(data: data, encoding: .utf8) else { return }
        sendMessage(text)
    }

    /// Send an RPC request and asynchronously await the matching response.
    ///
    /// - Parameters:
    ///   - method: RPC method name (see `Methods`).
    ///   - params: Method parameters (defaults to `nil`).
    /// - Returns: The decoded `ControlResponse` correlated by request ID.
    /// - Throws: `HumanConnectionError.notConnected` if not currently connected,
    ///   `HumanConnectionError.encodingFailed` if the request can't be encoded,
    ///   `HumanConnectionError.timeout` if no response arrives within
    ///   `requestTimeoutSeconds`, or `HumanConnectionError.disconnected` if the
    ///   socket closes while the request is pending.
    public func request(method: String, params: [String: AnyCodable]? = nil) async throws -> ControlResponse {
        guard state == .connected else {
            throw HumanConnectionError.notConnected
        }
        let id = "req-\(UUID().uuidString)"
        let req = ControlRequest(id: id, method: method, params: params)
        let data = try JSONEncoder().encode(req)
        guard let text = String(data: data, encoding: .utf8) else {
            throw HumanConnectionError.encodingFailed
        }

        return try await withCheckedThrowingContinuation { cont in
            pendingLock.lock()
            pendingRequests[id] = cont
            let timeout = DispatchWorkItem { [weak self] in
                guard let self = self else { return }
                self.pendingLock.lock()
                self.requestTimeouts.removeValue(forKey: id)
                guard let c = self.pendingRequests.removeValue(forKey: id) else {
                    self.pendingLock.unlock()
                    return
                }
                self.pendingLock.unlock()
                c.resume(throwing: HumanConnectionError.timeout)
            }
            requestTimeouts[id] = timeout
            pendingLock.unlock()
            queue.asyncAfter(deadline: .now() + Self.requestTimeoutSeconds, execute: timeout)

            task?.send(.string(text)) { [weak self] error in
                if let error = error {
                    self?.pendingLock.lock()
                    self?.requestTimeouts[id]?.cancel()
                    self?.requestTimeouts.removeValue(forKey: id)
                    _ = self?.pendingRequests.removeValue(forKey: id)
                    self?.pendingLock.unlock()
                    cont.resume(throwing: error)
                }
            }
        }
    }

    // MARK: - Private

    private func _connect() {
        if state == .connecting, task != nil { return }
        state = .connecting
        task?.cancel(with: .goingAway, reason: nil)
        var request = URLRequest(url: url)
        request.setValue("websocket", forHTTPHeaderField: "Upgrade")
        request.setValue("Upgrade", forHTTPHeaderField: "Connection")
        task = session.webSocketTask(with: request)
        task?.resume()
        sendConnect()
        receive()
    }

    private func sendConnect() {
        let connect: [String: Any] = [
            "type": "req",
            "id": "connect-\(UUID().uuidString)",
            "method": "connect",
            "params": [:]
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: connect),
              let text = String(data: data, encoding: .utf8) else { return }
        task?.send(.string(text)) { _ in }
    }

    private func receive() {
        task?.receive { [weak self] result in
            guard let self = self else { return }
            switch result {
            case .success(let message):
                switch message {
                case .string(let text):
                    self.handleMessage(text)
                case .data:
                    break
                @unknown default:
                    break
                }
                self.receive()
            case .failure:
                DispatchQueue.main.async { self.state = .disconnected }
                self.failPendingRequests()
                self.scheduleReconnect()
            }
        }
    }

    private func handleMessage(_ text: String) {
        guard let data = text.data(using: .utf8),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let type = json["type"] as? String else { return }

        switch type {
        case "hello-ok":
            queue.async { [weak self] in
                self?.reconnectAttempt = 0
                self?.state = .connected
                self?.schedulePing()
            }
        case "res":
            if let payload = json["payload"] as? [String: Any],
               payload["type"] as? String == "hello-ok" {
                queue.async { [weak self] in
                    self?.reconnectAttempt = 0
                    self?.state = .connected
                    self?.schedulePing()
                }
            }
            if let id = json["id"] as? String {
                pendingLock.lock()
                requestTimeouts[id]?.cancel()
                requestTimeouts.removeValue(forKey: id)
                if let cont = pendingRequests.removeValue(forKey: id) {
                    pendingLock.unlock()
                    do {
                        let res = try parseResponse(from: json)
                        cont.resume(returning: res)
                    } catch {
                        cont.resume(throwing: error)
                    }
                } else {
                    pendingLock.unlock()
                }
            }
        case "event":
            if let event = json["event"] as? String {
                let payload = (json["payload"] as? [String: Any]).map { dict in
                    dict.mapValues { AnyCodable($0) }
                }
                eventHandler?(event, payload)
            }
        default:
            break
        }
    }

    private func parseResponse(from json: [String: Any]) throws -> ControlResponse {
        let data = try JSONSerialization.data(withJSONObject: json)
        return try JSONDecoder().decode(ControlResponse.self, from: data)
    }

    private func failPendingRequests() {
        pendingLock.lock()
        for (_, tw) in requestTimeouts { tw.cancel() }
        requestTimeouts.removeAll()
        let pending = pendingRequests
        pendingRequests.removeAll()
        pendingLock.unlock()
        for cont in pending.values {
            cont.resume(throwing: HumanConnectionError.disconnected)
        }
    }

    private func cancelPingSchedule() {
        pingWorkItem?.cancel()
        pingWorkItem = nil
    }

    private func schedulePing() {
        cancelPingSchedule()
        let work = DispatchWorkItem { [weak self] in
            guard let self = self else { return }
            if self.state == .connected {
                self.task?.sendPing { _ in }
            }
            self.schedulePing()
        }
        pingWorkItem = work
        queue.asyncAfter(deadline: .now() + 25, execute: work)
    }

    private func scheduleReconnect() {
        reconnectWorkItem?.cancel()
        reconnectAttempt += 1
        let exp = min(Self.maxReconnectDelay, Self.baseReconnectDelay * pow(2.0, Double(min(reconnectAttempt, 4))))
        let jitter = Double.random(in: 0...0.5)
        let delay = exp + jitter
        let work = DispatchWorkItem { [weak self] in
            self?._connect()
        }
        reconnectWorkItem = work
        queue.asyncAfter(deadline: .now() + delay, execute: work)
    }
}

/// Errors surfaced by `HumanConnection.request(method:params:)` and related calls.
@available(macOS 14.0, iOS 17.0, *)
public enum HumanConnectionError: Error {
    /// A request was attempted while the connection state was not `.connected`.
    case notConnected
    /// The request body could not be encoded to JSON text.
    case encodingFailed
    /// The socket closed before the response arrived.
    case disconnected
    /// No response was received within `HumanConnection.requestTimeoutSeconds`.
    case timeout
}
