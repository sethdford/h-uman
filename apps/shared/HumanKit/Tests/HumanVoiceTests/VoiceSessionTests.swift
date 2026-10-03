import Foundation
import XCTest
import HumanProtocol
@testable import HumanVoice

/// Scripted gateway. `script` decides each RPC's response and may push audio or
/// events while the call is in flight, the way the real gateway streams a reply
/// inside `voice.audio.end`.
final class FakeTransport: VoiceTransport, @unchecked Sendable {
    private let lock = NSLock()
    private(set) var calls: [(method: String, params: [String: AnyCodable]?, timeout: TimeInterval?)] = []
    private(set) var sent: [Data] = []
    var dataHandler: (@Sendable (Data) -> Void)?
    var eventHandler: (@Sendable (String, [String: AnyCodable]?) -> Void)?
    var script: (FakeTransport, String) async -> ControlResponse = { _, _ in FakeTransport.ok() }

    static func response(ok: Bool, _ payload: String = "{}") -> ControlResponse {
        let json = #"{"type":"res","id":"x","ok":\#(ok),"payload":\#(payload)}"#
        return try! JSONDecoder().decode(ControlResponse.self, from: Data(json.utf8))
    }
    static func ok() -> ControlResponse { response(ok: true) }

    func request(method: String, params: [String: AnyCodable]?, timeout: TimeInterval?) async throws -> ControlResponse {
        lock.withLock { calls.append((method, params, timeout)) }
        return await script(self, method)
    }

    func sendData(_ data: Data) -> Bool {
        lock.lock(); sent.append(data); lock.unlock()
        return true
    }

    func pushAudio(_ samples: [Float]) {
        var d = Data()
        for s in samples { var le = s.bitPattern.littleEndian; d.append(Data(bytes: &le, count: 4)) }
        dataHandler?(d)
    }

    func methods() -> [String] { lock.lock(); defer { lock.unlock() }; return calls.map(\.method) }
}

final class SampleBox: @unchecked Sendable {
    private let lock = NSLock()
    private var _samples: [Float] = []
    func append(_ s: [Float]) { lock.lock(); _samples += s; lock.unlock() }
    var samples: [Float] { lock.lock(); defer { lock.unlock() }; return _samples }
}

@MainActor
final class VoiceSessionTests: XCTestCase {
    private func settle() async { for _ in 0..<20 { await Task.yield() } }

    func testStartSendsStandardModeWithVoiceAndModel() async throws {
        let t = FakeTransport()
        let s = VoiceSession(transport: t, voiceId: "voice-123", modelId: "sonic-3.6-2026-08-27") { _ in }
        try await s.start()
        XCTAssertEqual(s.state, .listening)
        let call = try XCTUnwrap(t.calls.first)
        XCTAssertEqual(call.method, "voice.session.start")
        XCTAssertEqual(call.params?["mode"]?.value as? String, "standard")
        XCTAssertEqual(call.params?["voiceId"]?.value as? String, "voice-123")
        XCTAssertEqual(call.params?["modelId"]?.value as? String, "sonic-3.6-2026-08-27")
    }

    func testStartRejectedThrows() async {
        let t = FakeTransport()
        t.script = { _, _ in FakeTransport.response(ok: false, #"{"error":"invalid_token"}"#) }
        let s = VoiceSession(transport: t) { _ in }
        do {
            try await s.start()
            XCTFail("expected rejection")
        } catch let e as VoiceSessionError {
            XCTAssertEqual(e, .startRejected("invalid_token"))
        } catch { XCTFail("wrong error \(error)") }
        XCTAssertEqual(s.state, .idle)
    }

    func testTurnSendsWavThenAudioEndAndPlaysTheReply() async throws {
        let t = FakeTransport()
        let box = SampleBox()
        let s = VoiceSession(transport: t, turnTimeout: 90) { box.append($0) }
        var states: [VoiceSession.State] = []
        s.onStateChange = { states.append($0) }
        var doneCalled = false
        s.onReplyAudioDone = { doneCalled = true }
        try await s.start()
        t.script = { tr, method in
            if method == "voice.audio.end" {
                tr.pushAudio([0.1, 0.2, 0.3])
                tr.eventHandler?("voice.audio.done", nil)
            }
            return FakeTransport.ok()
        }
        await s.submit(utterance: [Float](repeating: 0.1, count: 16_000))
        await settle()

        let first = try XCTUnwrap(t.sent.first)
        XCTAssertEqual(String(data: first.prefix(4), encoding: .ascii), "RIFF")
        XCTAssertEqual(t.sent.reduce(0) { $0 + $1.count }, 44 + 16_000 * 2)
        let end = try XCTUnwrap(t.calls.last)
        XCTAssertEqual(end.method, "voice.audio.end")
        XCTAssertEqual(end.params?["mimeType"]?.value as? String, "audio/wav")
        XCTAssertEqual(end.timeout, 90)
        XCTAssertEqual(box.samples, [0.1, 0.2, 0.3])
        XCTAssertTrue(states.contains(.thinking))
        XCTAssertEqual(s.state, .speaking)
        XCTAssertTrue(doneCalled)
        s.playbackFinished()
        XCTAssertEqual(s.state, .listening)
    }

    func testBargeInStopsTheReplyAndDropsLateAudio() async throws {
        let t = FakeTransport()
        let box = SampleBox()
        let s = VoiceSession(transport: t) { box.append($0) }
        try await s.start()
        t.script = { tr, method in
            if method == "voice.audio.end" {
                tr.pushAudio([0.5])
                await MainActor.run { s.bargeIn() }   // user talks over the first frame
                tr.pushAudio([0.6, 0.7])              // rest of the interrupted reply
            }
            return FakeTransport.ok()
        }
        await s.submit(utterance: [0.1])
        await settle()
        XCTAssertEqual(box.samples, [0.5])
        XCTAssertEqual(s.state, .listening)
        XCTAssertTrue(t.methods().contains("voice.session.interrupt"))
    }

    func testEmptyReplyReturnsToListening() async throws {
        let t = FakeTransport()
        let s = VoiceSession(transport: t) { _ in }
        try await s.start()
        await s.submit(utterance: [0.1])
        XCTAssertEqual(s.state, .listening)
    }

    func testTranscriptAndAssistantTextEventsReachCallbacks() async throws {
        let t = FakeTransport()
        let s = VoiceSession(transport: t) { _ in }
        var heard: [String] = []
        var said: [String] = []
        s.onUserTranscript = { heard.append($0) }
        s.onAssistantText = { said.append($0) }
        try await s.start()
        t.eventHandler?("voice.transcript", ["text": AnyCodable("how was your day")])
        t.eventHandler?("chat", ["state": AnyCodable("chunk"), "message": AnyCodable("par")])
        t.eventHandler?("chat", ["state": AnyCodable("sent"), "message": AnyCodable("pretty good")])
        await settle()
        XCTAssertEqual(heard, ["how was your day"])
        XCTAssertEqual(said, ["pretty good"])
    }

    func testSubmitBeforeStartReportsNotStarted() async {
        let t = FakeTransport()
        let s = VoiceSession(transport: t) { _ in }
        var err: Error?
        s.onError = { err = $0 }
        await s.submit(utterance: [0.1])
        XCTAssertEqual(err as? VoiceSessionError, .notStarted)
        XCTAssertTrue(t.calls.isEmpty)
    }
}
