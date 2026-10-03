import Foundation
import XCTest
import HumanClient
@testable import HumanVoice

/// End-to-end check against a running voice gateway: one spoken utterance in,
/// transcript + reply text + reply audio out. Opt-in; skipped unless
/// `HU_VOICE_E2E_URL` and `HU_VOICE_E2E_WAV` are set. Run it with
/// `scripts/voice-e2e.sh`, which renders the WAV with `say`.
@MainActor
final class VoiceGatewayE2ETests: XCTestCase {
    func testSpokenTurnRoundTrip() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let urlString = env["HU_VOICE_E2E_URL"], let url = URL(string: urlString),
              let wavPath = env["HU_VOICE_E2E_WAV"] else {
            throw XCTSkip("set HU_VOICE_E2E_URL and HU_VOICE_E2E_WAV (see scripts/voice-e2e.sh)")
        }
        let utterance = try Self.readWav16kMono(path: wavPath)

        let connection = HumanConnection(url: url)
        let connected = expectation(description: "connected")
        connection.stateHandler = { state in
            if state == .connected { connected.fulfill() }
        }
        connection.connect()
        await fulfillment(of: [connected], timeout: 10)
        defer { connection.disconnect() }

        let audio = SampleBox()
        let firstAudio = FirstTime()
        let session = VoiceSession(transport: connection,
                                   voiceId: env["HU_VOICE_E2E_VOICE"],
                                   modelId: env["HU_VOICE_E2E_MODEL"],
                                   turnTimeout: 90) { samples in
            firstAudio.mark()
            audio.append(samples)
        }
        var heard = "", said = ""
        var errors: [Error] = []
        session.onUserTranscript = { heard = $0 }
        session.onAssistantText = { said = $0 }
        session.onError = { errors.append($0) }

        try await session.start()
        let sent = Date()
        await session.submit(utterance: utterance)
        let total = Date().timeIntervalSince(sent)
        for _ in 0..<50 { await Task.yield() }

        let replySeconds = Double(audio.samples.count) / Double(PCMCodec.downlinkSampleRate)
        let ttfa = firstAudio.time.map { $0.timeIntervalSince(sent) }
        print(String(format: "[voice-e2e] heard=%@ | said=%@ | reply_audio=%.2fs ttfa=%@ turn=%.2fs",
                     heard, said, replySeconds,
                     ttfa.map { String(format: "%.2fs", $0) } ?? "none", total))

        if let save = env["HU_VOICE_E2E_SAVE"] {
            // The reply exactly as the gateway streamed it (Cartesia PCM), for listening.
            let wav = PCMCodec.wav(samples: PCMCodec.int16(from: audio.samples),
                                   sampleRate: PCMCodec.downlinkSampleRate)
            try wav.write(to: URL(fileURLWithPath: save))
        }
        XCTAssertTrue(errors.isEmpty, "turn errors: \(errors)")
        XCTAssertFalse(heard.isEmpty, "gateway produced no transcript")
        XCTAssertFalse(said.isEmpty, "gateway produced no reply text")
        XCTAssertGreaterThan(replySeconds, 0.3, "reply audio too short or missing")
        await session.stop()
    }

    /// Reads a 16 kHz mono 16-bit WAV (as written by `say --data-format=LEI16@16000`).
    static func readWav16kMono(path: String) throws -> [Float] {
        let data = try Data(contentsOf: URL(fileURLWithPath: path))
        // Walk the chunks rather than assuming a 44-byte header: `say` adds others.
        var offset = 12
        while offset + 8 <= data.count {
            let id = String(data: data.subdata(in: offset..<offset + 4), encoding: .ascii) ?? ""
            let size = Int(data.subdata(in: offset + 4..<offset + 8)
                .withUnsafeBytes { $0.loadUnaligned(as: UInt32.self).littleEndian })
            if id == "fmt " {
                let rate = data.subdata(in: offset + 12..<offset + 16)
                    .withUnsafeBytes { $0.loadUnaligned(as: UInt32.self).littleEndian }
                let channels = data.subdata(in: offset + 10..<offset + 12)
                    .withUnsafeBytes { $0.loadUnaligned(as: UInt16.self).littleEndian }
                guard rate == 16_000, channels == 1 else {
                    throw NSError(domain: "voice-e2e", code: 1,
                                  userInfo: [NSLocalizedDescriptionKey: "need 16 kHz mono, got \(rate) Hz x\(channels)"])
                }
            } else if id == "data" {
                let end = min(data.count, offset + 8 + size)
                return data.subdata(in: offset + 8..<end).withUnsafeBytes { raw in
                    (0..<(raw.count / 2)).map { i in
                        Float(Int16(littleEndian: raw.loadUnaligned(fromByteOffset: i * 2, as: Int16.self))) / 32767
                    }
                }
            }
            offset += 8 + size + (size & 1)
        }
        throw NSError(domain: "voice-e2e", code: 2, userInfo: [NSLocalizedDescriptionKey: "no data chunk"])
    }
}

private final class FirstTime: @unchecked Sendable {
    private let lock = NSLock()
    private var _time: Date?
    func mark() { lock.withLock { if _time == nil { _time = Date() } } }
    var time: Date? { lock.withLock { _time } }
}
