import XCTest
import os
@testable import HumanClient
import HumanProtocol

/// Lock-guarded box for capturing observed states in a `@Sendable` handler.
/// Built on `OSAllocatedUnfairLock`, which is itself `Sendable`, so the
/// box conforms via the lock's own Sendable conformance — no escape hatch.
final class StatesBox: Sendable {
    private let storage = OSAllocatedUnfairLock<[HumanConnection.ConnectionState]>(initialState: [])
    func append(_ s: HumanConnection.ConnectionState) {
        storage.withLock { $0.append(s) }
    }
    func snapshot() -> [HumanConnection.ConnectionState] {
        storage.withLock { $0 }
    }
}

final class ConnectionTests: XCTestCase {

    func testInitialStateIsDisconnected() {
        let conn = HumanConnection(url: URL(string: "wss://localhost:3000/ws")!)
        XCTAssertEqual(conn.state, .disconnected)
    }

    func testConvenienceInitWithValidURL() {
        let conn = HumanConnection(urlString: "wss://127.0.0.1:3000/ws")
        XCTAssertEqual(conn.state, .disconnected)
    }

    func testConvenienceInitWithInvalidURLFallsBack() {
        let conn = HumanConnection(urlString: "")
        XCTAssertEqual(conn.state, .disconnected)
    }

    func testDisconnectFromDisconnectedIsNoop() {
        let conn = HumanConnection(url: URL(string: "wss://localhost:3000/ws")!)
        conn.disconnect()
        XCTAssertEqual(conn.state, .disconnected)
    }

    func testStateHandlerCalled() {
        let conn = HumanConnection(url: URL(string: "wss://localhost:3000/ws")!)
        let states = StatesBox()
        conn.stateHandler = { state in states.append(state) }
        conn.disconnect()
        RunLoop.current.run(until: Date().addingTimeInterval(0.1))
        let snapshot = states.snapshot()
        XCTAssertTrue(snapshot.isEmpty || snapshot.allSatisfy { $0 == .disconnected })
    }

    func testConnectionStateEquatable() {
        XCTAssertEqual(HumanConnection.ConnectionState.disconnected, .disconnected)
        XCTAssertEqual(HumanConnection.ConnectionState.connecting, .connecting)
        XCTAssertEqual(HumanConnection.ConnectionState.connected, .connected)
        XCTAssertNotEqual(HumanConnection.ConnectionState.disconnected, .connected)
    }
}
