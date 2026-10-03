import Foundation

/// Audio format helpers for the gateway voice protocol.
///
/// Uplink: the gateway's standard voice pipeline transcribes an encoded file
/// (`voice.audio.end` with `mimeType`), so each utterance is sent as a 16 kHz,
/// 16-bit mono WAV. Downlink: TTS arrives as raw `pcm_f32le`, mono, 24 kHz.
public enum PCMCodec {
    /// Sample rate of utterances sent to the gateway.
    public static let uplinkSampleRate = 16_000
    /// Sample rate of TTS audio streamed back by the gateway.
    public static let downlinkSampleRate = 24_000

    /// Converts float samples in [-1, 1] to 16-bit PCM, clamping out-of-range input.
    public static func int16(from samples: [Float]) -> [Int16] {
        samples.map { s in
            let c = max(-1, min(1, s))
            return Int16((c * 32767).rounded())
        }
    }

    /// Decodes raw little-endian float32 PCM. A trailing partial sample (fewer
    /// than 4 bytes) is dropped rather than misread.
    public static func float32LE(from data: Data) -> [Float] {
        let count = data.count / 4
        var out = [Float](repeating: 0, count: count)
        data.withUnsafeBytes { raw in
            for i in 0..<count {
                let bits = raw.loadUnaligned(fromByteOffset: i * 4, as: UInt32.self)
                out[i] = Float(bitPattern: UInt32(littleEndian: bits))
            }
        }
        return out
    }

    /// A complete RIFF/WAVE file: 16-bit little-endian PCM, mono.
    public static func wav(samples: [Int16], sampleRate: Int = uplinkSampleRate) -> Data {
        let dataBytes = samples.count * 2
        var d = Data(capacity: 44 + dataBytes)
        func u32(_ v: UInt32) { var le = v.littleEndian; d.append(Data(bytes: &le, count: 4)) }
        func u16(_ v: UInt16) { var le = v.littleEndian; d.append(Data(bytes: &le, count: 2)) }
        d.append(contentsOf: Array("RIFF".utf8))
        u32(UInt32(36 + dataBytes))
        d.append(contentsOf: Array("WAVE".utf8))
        d.append(contentsOf: Array("fmt ".utf8))
        u32(16)                              // fmt chunk size
        u16(1)                               // PCM
        u16(1)                               // mono
        u32(UInt32(sampleRate))
        u32(UInt32(sampleRate * 2))          // byte rate
        u16(2)                               // block align
        u16(16)                              // bits per sample
        d.append(contentsOf: Array("data".utf8))
        u32(UInt32(dataBytes))
        for s in samples { var le = s.littleEndian; d.append(Data(bytes: &le, count: 2)) }
        return d
    }

    /// Splits `data` into frames no larger than `maxBytes` (the gateway closes the
    /// socket on frames over 512 KB).
    public static func chunks(_ data: Data, maxBytes: Int = 256 * 1024) -> [Data] {
        guard maxBytes > 0, !data.isEmpty else { return data.isEmpty ? [] : [data] }
        var out: [Data] = []
        var start = data.startIndex
        while start < data.endIndex {
            let end = data.index(start, offsetBy: maxBytes, limitedBy: data.endIndex) ?? data.endIndex
            out.append(data.subdata(in: start..<end))
            start = end
        }
        return out
    }
}
