import Foundation

class GatewayClient: ObservableObject {
    @Published var isConnected = false
    var eventHandler: ((String, [String: Any]?) -> Void)?
    private var task: URLSessionWebSocketTask?
    private var url: URL
    private var reconnectWorkItem: DispatchWorkItem?
    private let queue = DispatchQueue(label: "com.human.gateway")
    private var pendingRequests: [String: (Result<[String: Any], Error>) -> Void] = [:]

    init(url: String = "ws://localhost:3000/ws") {
        self.url = URL(string: url) ?? URL(string: "ws://localhost:3000/ws")!
    }

    func connect() {
        queue.async { [weak self] in
            guard let self = self else { return }
            self.task?.cancel(with: .goingAway, reason: nil)
            var request = URLRequest(url: self.url)
            request.setValue("websocket", forHTTPHeaderField: "Upgrade")
            request.setValue("Upgrade", forHTTPHeaderField: "Connection")
            let session = URLSession(configuration: .default)
            self.task = session.webSocketTask(with: request)
            self.task?.resume()
            self.sendConnect()
            self.receive()
        }
    }

    func disconnect() {
        queue.async { [weak self] in
            self?.reconnectWorkItem?.cancel()
            self?.reconnectWorkItem = nil
            self?.task?.cancel(with: .goingAway, reason: nil)
            self?.task = nil
            DispatchQueue.main.async { [weak self] in
                self?.isConnected = false
            }
        }
    }

    func request(method: String, params: [String: Any] = [:], completion: @escaping (Result<[String: Any], Error>) -> Void) {
        let reqId = "req-\(UUID().uuidString)"
        let msg: [String: Any] = [
            "type": "req",
            "id": reqId,
            "method": method,
            "params": params
        ]
        queue.async { [weak self] in
            guard let self = self else { return }
            self.pendingRequests[reqId] = completion
            guard let data = try? JSONSerialization.data(withJSONObject: msg),
                  let text = String(data: data, encoding: .utf8) else {
                self.pendingRequests.removeValue(forKey: reqId)
                completion(.failure(NSError(domain: "GatewayClient", code: -1, userInfo: [NSLocalizedDescriptionKey: "JSON serialization failed"])))
                return
            }
            self.task?.send(.string(text)) { error in
                if let error = error {
                    self.queue.async {
                        self.pendingRequests.removeValue(forKey: reqId)
                    }
                    completion(.failure(error))
                }
            }
        }
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
                DispatchQueue.main.async { [weak self] in
                    self?.isConnected = false
                }
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
            DispatchQueue.main.async { [weak self] in
                self?.isConnected = true
            }
        case "res":
            if let payload = json["payload"] as? [String: Any],
               payload["type"] as? String == "hello-ok" {
                DispatchQueue.main.async { [weak self] in
                    self?.isConnected = true
                }
            }
            if let reqId = json["id"] as? String {
                queue.async { [weak self] in
                    guard let self = self else { return }
                    if let callback = self.pendingRequests.removeValue(forKey: reqId) {
                        let payload = json["payload"] as? [String: Any] ?? [:]
                        if let errorMsg = payload["error"] as? String {
                            callback(.failure(NSError(domain: "GatewayClient", code: -2, userInfo: [NSLocalizedDescriptionKey: errorMsg])))
                        } else {
                            callback(.success(payload))
                        }
                    }
                }
            }
        case "event":
            if let event = json["event"] as? String {
                let payload = json["payload"] as? [String: Any]
                self.eventHandler?(event, payload)
            }
        default:
            break
        }
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

    private func scheduleReconnect() {
        reconnectWorkItem?.cancel()
        let work = DispatchWorkItem { [weak self] in
            self?.connect()
        }
        reconnectWorkItem = work
        queue.asyncAfter(deadline: .now() + 3, execute: work)
    }
}
