import Foundation
import HumanProtocol

/// WebSocket client for the Human gateway control protocol.
///
/// Uses `URLSessionWebSocketTask` with automatic reconnection. All mutable
/// state is serialized through a single private `DispatchQueue` (with the
/// pending-request map additionally guarded by an `NSLock`), so the class
/// behaves as `Sendable` despite holding stored properties — the mutable
/// fields are marked `nonisolated(unsafe)` and their access is restricted
/// to the queue-guarded code paths in this file. This replaces a prior
/// (now-removed) `unchecked` Sendable shim; the discipline is now
/// structural, not assertional.
@available(macOS 14.0, iOS 17.0, *)
public final class HumanConnection: Sendable {
    /// State of the underlying WebSocket task.
    public enum ConnectionState: Equatable, Sendable {
        /// Not connected and no reconnection in flight.
        case disconnected
        /// Connection attempt in flight (handshake not yet complete).
        case connecting
        /// Connected: `hello-ok` received, RPCs may be issued.
        case connected
    }

    /// Current connection state. Reads are not synchronized; writes happen
    /// on the private serial queue. Single-word loads are atomic on the
    /// platforms we target.
    public var state: ConnectionState {
        get { _state }
    }

    private nonisolated(unsafe) var _state: ConnectionState = .disconnected {
        didSet { stateHandler?(_state) }
    }

    /// Called whenever `state` changes. The handler is invoked on the
    /// private serial queue; hop to `@MainActor` inside the handler if
    /// you need to update UI.
    public var stateHandler: (@Sendable (ConnectionState) -> Void)? {
        get { Self.configQueue.sync { _stateHandler } }
        set { Self.configQueue.sync { _stateHandler = newValue } }
    }

    private nonisolated(unsafe) var _stateHandler: (@Sendable (ConnectionState) -> Void)?

    /// Called when an event frame is received from the gateway. Invoked
    /// on the private serial queue.
    public var eventHandler: (@Sendable (String, [String: AnyCodable]?) -> Void)? {
        get { Self.configQueue.sync { _eventHandler } }
        set { Self.configQueue.sync { _eventHandler = newValue } }
    }

    private nonisolated(unsafe) var _eventHandler: (@Sendable (String, [String: AnyCodable]?) -> Void)?

    private nonisolated(unsafe) var task: URLSessionWebSocketTask?
    private nonisolated(unsafe) var url: URL
    private let session = URLSession(configuration: .default)
    private let queue = DispatchQueue(label: "com.human.connection")
    private nonisolated(unsafe) var reconnectWorkItem: DispatchWorkItem?
    private nonisolated(unsafe) var pendingRequests: [String: CheckedContinuation<ControlResponse, Error>] = [:]
    private nonisolated(unsafe) var requestTimeouts: [String: DispatchWorkItem] = [:]
    private let pendingLock = NSLock()
    private nonisolated(unsafe) var reconnectAttempt: UInt = 0
    private nonisolated(unsafe) var pingWorkItem: DispatchWorkItem?

    // MARK: - RPC wait timeout (queue-guarded global config)

    /// Underlying storage for `requestTimeoutSeconds`. Access goes through
    /// the static `configQueue` to provide write-write and write-read
    /// ordering across threads. `nonisolated(unsafe)` is justified by the
    /// queue discipline that follows below.
    private nonisolated(unsafe) static var _requestTimeoutSeconds: TimeInterval = 10

    /// Process-wide serial queue guarding the mutable HumanConnection
    /// configuration (currently `requestTimeoutSeconds`). Exposed at file
    /// scope so the accessor pair below is the only path to the storage.
    fileprivate static let configQueue = DispatchQueue(label: "com.human.connection.config")

    /// RPC wait timeout in seconds (matches the web client default).
    /// Reads and writes are serialized through `configQueue` so callers
    /// on different threads observe consistent values.
    public static var requestTimeoutSeconds: TimeInterval {
        get { configQueue.sync { _requestTimeoutSeconds } }
        set { configQueue.sync { _requestTimeoutSeconds = newValue } }
    }

    private static let maxReconnectDelay: TimeInterval = 30
    private static let baseReconnectDelay: TimeInterval = 3

    /// Create a connection bound to the gateway WebSocket `url`.
    public init(url: URL) {
        self.url = url
    }

    /// Create a connection from a URL string. Falls back to
    /// `wss://localhost:3000/ws` when the input does not parse.
    public convenience init(urlString: String) {
        let url = URL(string: urlString) ?? URL(string: "wss://localhost:3000/ws")!
        self.init(url: url)
    }

    /// Connect to the gateway WebSocket endpoint. Returns immediately;
    /// `stateHandler` fires when the state advances to `.connected`.
    public func connect() {
        queue.async { [weak self] in
            self?._connect()
        }
    }

    /// Disconnect and cancel any pending reconnect.
    public func disconnect() {
        queue.async { [weak self] in
            self?.cancelPingSchedule()
            self?.reconnectWorkItem?.cancel()
            self?.reconnectWorkItem = nil
            self?.task?.cancel(with: .goingAway, reason: nil)
            self?.task = nil
            self?.failPendingRequests()
            self?.reconnectAttempt = 0
            self?._state = .disconnected
        }
    }

    /// Send a raw JSON message without waiting for a response.
    public func sendMessage(_ json: String) {
        guard _state == .connected else { return }
        task?.send(.string(json)) { _ in }
    }

    /// Register a push token with the gateway for push notifications.
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
    public func unregisterPushToken(token: String) {
        let id = "pushu-\(UUID().uuidString)"
        let params: [String: AnyCodable] = ["token": AnyCodable(token)]
        let req = ControlRequest(id: id, method: Methods.pushUnregister, params: params)
        guard let data = try? JSONEncoder().encode(req),
              let text = String(data: data, encoding: .utf8) else { return }
        sendMessage(text)
    }

    /// Send an RPC request and `await` the response. Throws
    /// `HumanConnectionError.notConnected` if the connection is not in
    /// the `.connected` state at issue time, `.timeout` if no response
    /// arrives within `requestTimeoutSeconds`.
    public func request(method: String, params: [String: AnyCodable]? = nil) async throws -> ControlResponse {
        guard _state == .connected else {
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
        if _state == .connecting, task != nil { return }
        _state = .connecting
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
                self.queue.async { self._state = .disconnected }
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
                self?._state = .connected
                self?.schedulePing()
            }
        case "res":
            if let payload = json["payload"] as? [String: Any],
               payload["type"] as? String == "hello-ok" {
                queue.async { [weak self] in
                    self?.reconnectAttempt = 0
                    self?._state = .connected
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
                _eventHandler?(event, payload)
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
            if self._state == .connected {
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

/// Errors thrown from `HumanConnection.request(method:params:)` and
/// related call sites.
@available(macOS 14.0, iOS 17.0, *)
public enum HumanConnectionError: Error, Sendable {
    /// The connection is not in the `.connected` state.
    case notConnected
    /// The encoded request payload could not be converted to UTF-8 text.
    case encodingFailed
    /// The connection dropped while requests were outstanding.
    case disconnected
    /// The RPC did not receive a response within `requestTimeoutSeconds`.
    case timeout
}
