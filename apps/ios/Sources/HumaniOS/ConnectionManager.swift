import Foundation
import HumanClient
import HumanProtocol

/// Wraps HumanConnection as an ObservableObject for SwiftUI.
public final class ConnectionManager: ObservableObject {
    @Published public private(set) var isConnected = false
    @Published public var gatewayURL: String {
        didSet { UserDefaults.standard.set(gatewayURL, forKey: "Human.gatewayURL") }
    }

    private var connection: HumanConnection?
    private var eventHandlerStorage: ((String, [String: AnyCodable]?) -> Void)?
    private let queue = DispatchQueue(label: "com.human.connectionManager")

    public init() {
        self.gatewayURL = UserDefaults.standard.string(forKey: "Human.gatewayURL")
            ?? "wss://localhost:3000/ws"
    }

    public func connect() {
        queue.async { [weak self] in
            guard let self = self else { return }
            let conn = HumanConnection(urlString: self.gatewayURL)
            conn.stateHandler = { [weak self] state in
                DispatchQueue.main.async {
                    self?.isConnected = (state == .connected)
                }
            }
            conn.eventHandler = self.eventHandlerStorage
            conn.connect()
            self.connection = conn
        }
    }

    public func disconnect() {
        queue.async { [weak self] in
            self?.connection?.disconnect()
            self?.connection = nil
            DispatchQueue.main.async {
                self?.isConnected = false
            }
        }
    }

    public func reconnect() {
        disconnect()
        connect()
    }

    public func request(method: String, params: [String: AnyCodable]? = nil) async throws -> ControlResponse {
        guard let conn = connection else { throw HumanConnectionError.notConnected }
        return try await conn.request(method: method, params: params)
    }

    public func registerPushToken(token: String) {
        connection?.registerPushToken(token: token)
    }

    public func unregisterPushToken(token: String) {
        connection?.unregisterPushToken(token: token)
    }

    public func setEventHandler(_ handler: @escaping (String, [String: AnyCodable]?) -> Void) {
        queue.async { [weak self] in
            guard let self = self else { return }
            self.eventHandlerStorage = handler
            self.connection?.eventHandler = handler
        }
    }
}
