import Foundation
import Network
import XCTest
@testable import HumanClient

/// Loopback WebSocket server for exercising HumanConnection over a real socket.
/// Answers `connect` with hello-ok, echoes binary frames, records what it got,
/// and answers method "slow" after `slowDelay` seconds.
@available(macOS 14.0, iOS 17.0, *)
final class LoopbackGateway: @unchecked Sendable {
    private let listener: NWListener
    private let queue = DispatchQueue(label: "test.loopback.gateway")
    private let lock = NSLock()
    private var _binaryReceived: [Data] = []
    let slowDelay: TimeInterval
    let ready = DispatchSemaphore(value: 0)

    var binaryReceived: [Data] {
        lock.lock()
        defer { lock.unlock() }
        return _binaryReceived
    }

    var port: UInt16 { listener.port?.rawValue ?? 0 }

    init(slowDelay: TimeInterval = 1.0) throws {
        self.slowDelay = slowDelay
        let params = NWParameters.tcp
        let ws = NWProtocolWebSocket.Options()
        ws.autoReplyPing = true
        params.defaultProtocolStack.applicationProtocols.insert(ws, at: 0)
        params.requiredLocalEndpoint = NWEndpoint.hostPort(host: "127.0.0.1", port: .any)
        listener = try NWListener(using: params)
        listener.stateUpdateHandler = { [weak self] state in
            if case .ready = state { self?.ready.signal() }
        }
        listener.newConnectionHandler = { [weak self] conn in
            guard let self = self else { return }
            conn.start(queue: self.queue)
            self.receive(on: conn)
        }
        listener.start(queue: queue)
    }

    func stop() { listener.cancel() }

    private func send(_ conn: NWConnection, text: String) {
        let meta = NWProtocolWebSocket.Metadata(opcode: .text)
        let ctx = NWConnection.ContentContext(identifier: "t", metadata: [meta])
        conn.send(content: text.data(using: .utf8), contentContext: ctx, isComplete: true,
                  completion: .idempotent)
    }

    private func send(_ conn: NWConnection, binary: Data) {
        let meta = NWProtocolWebSocket.Metadata(opcode: .binary)
        let ctx = NWConnection.ContentContext(identifier: "b", metadata: [meta])
        conn.send(content: binary, contentContext: ctx, isComplete: true, completion: .idempotent)
    }

    private func receive(on conn: NWConnection) {
        conn.receiveMessage { [weak self] data, context, _, error in
            guard let self = self, error == nil else { return }
            let meta = context?.protocolMetadata(definition: NWProtocolWebSocket.definition)
                as? NWProtocolWebSocket.Metadata
            if let data = data, let meta = meta {
                if meta.opcode == .binary {
                    self.lock.lock()
                    self._binaryReceived.append(data)
                    self.lock.unlock()
                    self.send(conn, binary: data) // echo
                } else if meta.opcode == .text,
                          let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                          let id = obj["id"] as? String, let method = obj["method"] as? String {
                    if method == "connect" {
                        self.send(conn, text: #"{"type":"res","id":"\#(id)","ok":true,"payload":{"type":"hello-ok"}}"#)
                    } else if method == "slow" {
                        self.queue.asyncAfter(deadline: .now() + self.slowDelay) {
                            self.send(conn, text: #"{"type":"res","id":"\#(id)","ok":true,"payload":{}}"#)
                        }
                    }
                }
            }
            self.receive(on: conn)
        }
    }
}

@available(macOS 14.0, iOS 17.0, *)
final class BinaryFrameTests: XCTestCase {
    private func connected(_ gw: LoopbackGateway) -> HumanConnection {
        XCTAssertEqual(gw.ready.wait(timeout: .now() + 5), .success)
        let conn = HumanConnection(url: URL(string: "ws://127.0.0.1:\(gw.port)/ws")!)
        let up = expectation(description: "connected")
        conn.stateHandler = { state in if state == .connected { up.fulfill() } }
        conn.connect()
        wait(for: [up], timeout: 5)
        return conn
    }

    func testBinaryFrameRoundTripReachesServerAndDataHandler() throws {
        let gw = try LoopbackGateway()
        defer { gw.stop() }
        let conn = connected(gw)
        defer { conn.disconnect() }

        let payload = Data((0..<4096).map { UInt8($0 & 0xff) })
        let echoed = expectation(description: "binary echoed back")
        conn.dataHandler = { data in
            if data == payload { echoed.fulfill() }
        }
        XCTAssertTrue(conn.sendData(payload))
        wait(for: [echoed], timeout: 5)
        XCTAssertEqual(gw.binaryReceived, [payload])
    }

    func testSendDataWhenDisconnectedReturnsFalse() {
        let conn = HumanConnection(url: URL(string: "ws://127.0.0.1:1/ws")!)
        XCTAssertFalse(conn.sendData(Data([1, 2, 3])))
    }

    func testPerRequestTimeoutOverridesTheDefault() async throws {
        let gw = try LoopbackGateway(slowDelay: 1.0)
        defer { gw.stop() }
        let conn = connected(gw)
        defer { conn.disconnect() }
        let saved = HumanConnection.requestTimeoutSeconds
        HumanConnection.requestTimeoutSeconds = 0.3
        defer { HumanConnection.requestTimeoutSeconds = saved }

        // Default (0.3 s) is shorter than the server's 1 s answer: times out.
        do {
            _ = try await conn.request(method: "slow")
            XCTFail("expected timeout with the 0.3 s default")
        } catch HumanConnectionError.timeout {
            // expected
        }
        // A per-call timeout longer than the answer succeeds.
        let res = try await conn.request(method: "slow", timeout: 5)
        XCTAssertTrue(res.ok)
    }
}
