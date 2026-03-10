import SwiftUI
import Combine
import AppKit

@MainActor
class StatusViewModel: ObservableObject {
    @Published var isServiceRunning = false
    @Published var gatewayURL: String = "ws://localhost:3000/ws"

    var isGatewayConnected: Bool { gatewayClient.isConnected }
    var statusColor: Color {
        if isServiceRunning && isGatewayConnected {
            return .green
        }
        if isServiceRunning || isGatewayConnected {
            return .yellow
        }
        return .red
    }

    let gatewayClient: GatewayClient
    private let processManager = ProcessManager()
    private var pollTimer: Timer?
    private var cancellables = Set<AnyCancellable>()

    init() {
        gatewayClient = GatewayClient(url: "ws://localhost:3000/ws")
        gatewayClient.eventHandler = { [weak self] event, payload in
            Task { @MainActor in
                switch event {
                case "error":
                    let msg = payload?["message"] as? String ?? payload?["error"] as? String ?? "Unknown error"
                    #if os(macOS)
                    let alert = NSAlert()
                    alert.messageText = "Human Error"
                    alert.informativeText = msg
                    alert.alertStyle = .warning
                    alert.addButton(withTitle: "OK")
                    alert.runModal()
                    #endif
                case "health":
                    self?.objectWillChange.send()
                default:
                    break
                }
            }
        }
        gatewayClient.objectWillChange
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)
        startPolling()
        gatewayClient.connect()
    }

    deinit {
        pollTimer?.invalidate()
        gatewayClient.disconnect()
    }

    private func startPolling() {
        pollTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.isServiceRunning = self?.processManager.isRunning ?? false
            }
        }
        pollTimer?.tolerance = 0.2
        RunLoop.main.add(pollTimer!, forMode: .common)
    }

    func startService() {
        processManager.start()
    }

    func stopService() {
        processManager.stop()
    }
}
