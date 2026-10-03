import Foundation
import XCTest
@testable import HumanVoice

final class PCMCodecTests: XCTestCase {
    func testWavHeaderDescribes16kMono16bit() {
        let samples: [Int16] = [0, 1, -1, 32767, -32768]
        let wav = PCMCodec.wav(samples: samples)
        XCTAssertEqual(wav.count, 44 + samples.count * 2)
        XCTAssertEqual(String(data: wav.subdata(in: 0..<4), encoding: .ascii), "RIFF")
        XCTAssertEqual(String(data: wav.subdata(in: 8..<12), encoding: .ascii), "WAVE")
        func u32(_ o: Int) -> UInt32 { wav.subdata(in: o..<o + 4).withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) } }
        func u16(_ o: Int) -> UInt16 { wav.subdata(in: o..<o + 2).withUnsafeBytes { $0.loadUnaligned(as: UInt16.self) } }
        XCTAssertEqual(u32(4), UInt32(36 + samples.count * 2))
        XCTAssertEqual(u16(20), 1)       // PCM
        XCTAssertEqual(u16(22), 1)       // mono
        XCTAssertEqual(u32(24), 16_000)  // sample rate
        XCTAssertEqual(u16(34), 16)      // bits
        XCTAssertEqual(u32(40), UInt32(samples.count * 2))
        XCTAssertEqual(wav.subdata(in: 46..<48).withUnsafeBytes { $0.loadUnaligned(as: Int16.self) }, 1)
    }

    func testInt16ClampsAndScales() {
        XCTAssertEqual(PCMCodec.int16(from: [0, 1, -1, 2, -2, 0.5]), [0, 32767, -32767, 32767, -32767, 16384])
    }

    func testFloat32DecodeRoundTripAndDropsPartialSample() {
        let values: [Float] = [0, 0.25, -0.5, 1]
        var data = Data()
        for v in values { var le = v.bitPattern.littleEndian; data.append(Data(bytes: &le, count: 4)) }
        XCTAssertEqual(PCMCodec.float32LE(from: data), values)
        data.append(contentsOf: [0xAB, 0xCD]) // half a sample
        XCTAssertEqual(PCMCodec.float32LE(from: data), values)
    }

    func testChunksRespectMaxSizeAndReassemble() {
        let data = Data((0..<1000).map { UInt8($0 & 0xff) })
        let parts = PCMCodec.chunks(data, maxBytes: 300)
        XCTAssertEqual(parts.map(\.count), [300, 300, 300, 100])
        XCTAssertEqual(parts.reduce(Data(), +), data)
        XCTAssertEqual(PCMCodec.chunks(Data()), [])
    }
}

final class EnergyVADTests: XCTestCase {
    /// 20 ms frames at 16 kHz.
    private let frame = 320
    private func tone(_ amp: Float) -> [Float] { (0..<320).map { i in amp * sin(Float(i) * 0.3) } }
    private func run(_ vad: inout EnergyVAD, _ frames: [[Float]]) -> [EnergyVAD.Event] {
        frames.flatMap { vad.process($0) }
    }
    private func ms(_ n: Int, _ f: [Float]) -> [[Float]] { Array(repeating: f, count: n / 20) }

    func testSilenceProducesNothing() {
        var vad = EnergyVAD()
        XCTAssertTrue(run(&vad, ms(3_000, tone(0.001))).isEmpty)
    }

    func testSpeechThenSilenceYieldsOneUtteranceWithPreRoll() {
        var vad = EnergyVAD()
        let events = run(&vad, ms(400, tone(0.001)) + ms(1_000, tone(0.2)) + ms(1_500, tone(0.001)))
        XCTAssertEqual(events.first, .speechStarted)
        guard case .utterance(let samples)? = events.last else { return XCTFail("no utterance: \(events)") }
        XCTAssertEqual(events.count, 2)
        // 300 ms pre-roll + 1 s speech + 1.4 s trailing silence, give or take a frame.
        XCTAssertGreaterThanOrEqual(samples.count, 16_000 * 27 / 10 - frame)
        XCTAssertLessThanOrEqual(samples.count, 16_000 * 27 / 10 + 2 * frame)
    }

    func testShortBlipIsDiscarded() {
        var vad = EnergyVAD()
        let events = run(&vad, ms(200, tone(0.2)) + ms(1_500, tone(0.001)))
        XCTAssertEqual(events, [.speechStarted, .discarded])
    }

    func testAssistantSpeakingRaisesTheThreshold() {
        let moderate = tone(0.045) // RMS ~0.03: above 0.018, below barge-in 0.06
        var listening = EnergyVAD()
        XCTAssertEqual(listening.process(moderate), [.speechStarted])
        var talking = EnergyVAD()
        talking.assistantSpeaking = true
        XCTAssertTrue(talking.process(moderate).isEmpty)
        XCTAssertEqual(talking.process(tone(0.2)), [.speechStarted])
    }

    func testMaxUtteranceCutsContinuousSpeech() {
        var vad = EnergyVAD(config: .init(maxUtteranceMs: 2_000))
        let events = run(&vad, ms(2_500, tone(0.2)))
        XCTAssertTrue(events.contains { if case .utterance = $0 { return true } else { return false } })
    }
}
