import Foundation

/// Energy-based voice-activity detector for always-on listening.
///
/// Feed fixed-size frames of mono float samples; it reports when speech starts
/// and hands back the whole utterance (with a little pre-roll) once speech has
/// been followed by enough silence. Defaults match the web dashboard's tuned
/// detector (RMS 0.018, 1.4 s of silence, at least 0.7 s of capture).
///
/// While the assistant is talking, raise `threshold` (see `bargeInThreshold`) so
/// residual echo that survives the OS echo canceller does not count as speech.
public struct EnergyVAD: Sendable {
    public struct Config: Sendable, Equatable {
        public var sampleRate: Int
        public var threshold: Float
        public var bargeInThreshold: Float
        public var silenceToEndMs: Int
        public var minSpeechMs: Int
        public var preRollMs: Int
        public var maxUtteranceMs: Int

        public init(sampleRate: Int = PCMCodec.uplinkSampleRate, threshold: Float = 0.018,
                    bargeInThreshold: Float = 0.06, silenceToEndMs: Int = 1_400,
                    minSpeechMs: Int = 700, preRollMs: Int = 300, maxUtteranceMs: Int = 30_000) {
            self.sampleRate = sampleRate
            self.threshold = threshold
            self.bargeInThreshold = bargeInThreshold
            self.silenceToEndMs = silenceToEndMs
            self.minSpeechMs = minSpeechMs
            self.preRollMs = preRollMs
            self.maxUtteranceMs = maxUtteranceMs
        }
    }

    public enum Event: Sendable, Equatable {
        /// Speech crossed the threshold (use it to barge in on playback).
        case speechStarted
        /// A complete utterance, pre-roll included.
        case utterance([Float])
        /// Speech ended too soon to be an utterance (a cough, a click); dropped.
        case discarded
    }

    public let config: Config
    /// When true the detector uses `bargeInThreshold` (assistant is speaking).
    public var assistantSpeaking = false

    private var inSpeech = false
    private var buffer: [Float] = []
    private var preRoll: [Float] = []
    private var silenceSamples = 0
    private var speechSamples = 0

    public init(config: Config = Config()) {
        self.config = config
    }

    public static func rms(_ frame: [Float]) -> Float {
        guard !frame.isEmpty else { return 0 }
        var sum: Float = 0
        for s in frame { sum += s * s }
        return (sum / Float(frame.count)).squareRoot()
    }

    private func samples(ms: Int) -> Int { config.sampleRate * ms / 1000 }

    /// Processes one frame and returns the events it produced (usually none).
    public mutating func process(_ frame: [Float]) -> [Event] {
        let level = Self.rms(frame)
        let threshold = assistantSpeaking ? config.bargeInThreshold : config.threshold
        let loud = level >= threshold
        var events: [Event] = []

        if !inSpeech {
            if loud {
                inSpeech = true
                buffer = preRoll + frame
                speechSamples = frame.count
                silenceSamples = 0
                events.append(.speechStarted)
            } else {
                preRoll.append(contentsOf: frame)
                let keep = samples(ms: config.preRollMs)
                if preRoll.count > keep { preRoll.removeFirst(preRoll.count - keep) }
            }
            return events
        }

        buffer.append(contentsOf: frame)
        if loud {
            speechSamples += frame.count
            silenceSamples = 0
        } else {
            silenceSamples += frame.count
        }

        let ended = silenceSamples >= samples(ms: config.silenceToEndMs)
        let tooLong = buffer.count >= samples(ms: config.maxUtteranceMs)
        if ended || tooLong {
            if speechSamples >= samples(ms: config.minSpeechMs) || tooLong {
                events.append(.utterance(buffer))
            } else {
                events.append(.discarded)
            }
            reset()
        }
        return events
    }

    /// Drops any partial utterance (e.g. after a barge-in or a mute).
    public mutating func reset() {
        inSpeech = false
        buffer = []
        preRoll = []
        silenceSamples = 0
        speechSamples = 0
    }
}
