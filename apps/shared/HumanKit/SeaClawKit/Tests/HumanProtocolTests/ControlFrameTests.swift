import XCTest
@testable import HumanProtocol

final class ControlFrameTests: XCTestCase {

    // MARK: - ControlRequest

    func testRequestEncoding() throws {
        let req = ControlRequest(id: "r1", method: "health")
        let data = try JSONEncoder().encode(req)
        let json = try JSONSerialization.jsonObject(with: data) as! [String: Any]
        XCTAssertEqual(json["type"] as? String, "req")
        XCTAssertEqual(json["id"] as? String, "r1")
        XCTAssertEqual(json["method"] as? String, "health")
    }

    func testRequestRoundTrip() throws {
        let original = ControlRequest(id: "r2", method: "chat.send", params: ["msg": AnyCodable("hello")])
        let data = try JSONEncoder().encode(original)
        let decoded = try JSONDecoder().decode(ControlRequest.self, from: data)
        XCTAssertEqual(decoded.type, "req")
        XCTAssertEqual(decoded.id, "r2")
        XCTAssertEqual(decoded.method, "chat.send")
    }

    func testRequestWithNilParams() throws {
        let req = ControlRequest(id: "r3", method: "health", params: nil)
        let data = try JSONEncoder().encode(req)
        let decoded = try JSONDecoder().decode(ControlRequest.self, from: data)
        XCTAssertNil(decoded.params)
    }

    // MARK: - ControlResponse

    func testResponseDecoding() throws {
        let json = """
        {"type":"res","id":"r1","ok":true,"payload":{"version":"0.1.0"}}
        """.data(using: .utf8)!
        let resp = try JSONDecoder().decode(ControlResponse.self, from: json)
        XCTAssertEqual(resp.type, "res")
        XCTAssertEqual(resp.id, "r1")
        XCTAssertTrue(resp.ok)
        XCTAssertNotNil(resp.payload?["version"])
    }

    func testResponseDecodingFailure() throws {
        let json = """
        {"type":"res","id":"r2","ok":false}
        """.data(using: .utf8)!
        let resp = try JSONDecoder().decode(ControlResponse.self, from: json)
        XCTAssertFalse(resp.ok)
        XCTAssertNil(resp.payload)
    }

    // MARK: - ControlEvent

    func testEventDecoding() throws {
        let json = """
        {"type":"event","event":"chat.token","payload":{"text":"hi"},"seq":42}
        """.data(using: .utf8)!
        let event = try JSONDecoder().decode(ControlEvent.self, from: json)
        XCTAssertEqual(event.type, "event")
        XCTAssertEqual(event.event, "chat.token")
        XCTAssertEqual(event.seq, 42)
    }

    func testEventWithoutSeq() throws {
        let json = """
        {"type":"event","event":"status.change"}
        """.data(using: .utf8)!
        let event = try JSONDecoder().decode(ControlEvent.self, from: json)
        XCTAssertNil(event.seq)
        XCTAssertNil(event.payload)
    }

    // MARK: - HelloOk

    func testHelloOkDecoding() throws {
        let json = """
        {"type":"hello-ok","server":{"version":"0.1.0"},"protocol":1,"features":{"methods":["health","chat.send"]}}
        """.data(using: .utf8)!
        let hello = try JSONDecoder().decode(HelloOk.self, from: json)
        XCTAssertEqual(hello.type, "hello-ok")
        XCTAssertEqual(hello.server?.version, "0.1.0")
        XCTAssertEqual(hello.protocolVersion, 1)
        XCTAssertEqual(hello.features?.methods?.count, 2)
    }

    // MARK: - AnyCodable

    func testAnyCodableString() throws {
        let ac = AnyCodable("test")
        let data = try JSONEncoder().encode(ac)
        let decoded = try JSONDecoder().decode(AnyCodable.self, from: data)
        XCTAssertEqual(decoded.value as? String, "test")
    }

    func testAnyCodableInt() throws {
        let ac = AnyCodable(42)
        let data = try JSONEncoder().encode(ac)
        let decoded = try JSONDecoder().decode(AnyCodable.self, from: data)
        XCTAssertEqual(decoded.value as? Int, 42)
    }

    func testAnyCodableNull() throws {
        let json = "null".data(using: .utf8)!
        let decoded = try JSONDecoder().decode(AnyCodable.self, from: json)
        XCTAssertTrue(decoded.value is NSNull)
    }

    func testAnyCodableBool() throws {
        let ac = AnyCodable(true)
        let data = try JSONEncoder().encode(ac)
        let decoded = try JSONDecoder().decode(AnyCodable.self, from: data)
        XCTAssertEqual(decoded.value as? Bool, true)
    }

    // MARK: - Methods

    func testMethodsConstants() {
        XCTAssertEqual(Methods.connect, "connect")
        XCTAssertEqual(Methods.health, "health")
        XCTAssertEqual(Methods.chatSend, "chat.send")
        XCTAssertEqual(Methods.toolsCatalog, "tools.catalog")
    }

    func testMethodsAllContainsKnownMethods() {
        XCTAssertTrue(Methods.all.contains("health"))
        XCTAssertTrue(Methods.all.contains("chat.send"))
        XCTAssertTrue(Methods.all.contains("config.get"))
        XCTAssertFalse(Methods.all.contains("bogus"))
    }

    func testMethodsAllCount() {
        XCTAssertGreaterThanOrEqual(Methods.all.count, 20)
    }
}
