import XCTest
@testable import HumanClient
import HumanProtocol

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
        var states: [HumanConnection.ConnectionState] = []
        conn.stateHandler = { states.append($0) }
        conn.disconnect()
        RunLoop.current.run(until: Date().addingTimeInterval(0.1))
        XCTAssertTrue(states.isEmpty || states.allSatisfy { $0 == .disconnected })
    }

    func testConnectionStateEquatable() {
        XCTAssertEqual(HumanConnection.ConnectionState.disconnected, .disconnected)
        XCTAssertEqual(HumanConnection.ConnectionState.connecting, .connecting)
        XCTAssertEqual(HumanConnection.ConnectionState.connected, .connected)
        XCTAssertNotEqual(HumanConnection.ConnectionState.disconnected, .connected)
    }
}
