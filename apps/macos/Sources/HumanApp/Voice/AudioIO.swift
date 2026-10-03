import AVFoundation
import HumanVoice

/// Microphone in, reply audio out, with the OS echo canceller between them.
///
/// Voice processing is what makes always-on listening workable on a laptop: it
/// subtracts the speaker output from the microphone signal, so the assistant
/// does not hear (and answer) itself. Whatever echo survives is handled by the
/// VAD's higher barge-in threshold while a reply is playing.
final class AudioIO: @unchecked Sendable {
    /// 20 ms of 16 kHz audio, the frame size the VAD is tuned for.
    static let frameSamples = PCMCodec.uplinkSampleRate / 50

    /// Called with each 20 ms capture frame, on a private serial queue.
    var onFrame: (([Float]) -> Void)?
    /// Called once all audio of a finished reply has been played back.
    var onDrained: (() -> Void)?

    private let engine = AVAudioEngine()
    private let player = AVAudioPlayerNode()
    private let queue = DispatchQueue(label: "ai.human.voice.capture")
    private let captureFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                              sampleRate: Double(PCMCodec.uplinkSampleRate),
                                              channels: 1, interleaved: false)!
    private let playbackFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                               sampleRate: Double(PCMCodec.downlinkSampleRate),
                                               channels: 1, interleaved: false)!
    private var converter: AVAudioConverter?
    private var pending: [Float] = []

    // Playback bookkeeping, shared between the gateway receive thread (enqueue),
    // AVAudioPlayerNode completion callbacks and the main actor (flush).
    private let lock = NSLock()
    private var scheduled = 0
    private var replyDone = false
    /// Bumped by `flush()` so callbacks for discarded buffers are ignored.
    private var generation = 0

    enum AudioIOError: Error { case noConverter }

    func start() throws {
        let input = engine.inputNode
        try input.setVoiceProcessingEnabled(true)
        // Voice processing ducks other apps' audio by default; keep that light.
        input.voiceProcessingOtherAudioDuckingConfiguration =
            .init(enableAdvancedDucking: true, duckingLevel: .min)

        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: playbackFormat)

        let inFormat = input.outputFormat(forBus: 0)
        guard let conv = AVAudioConverter(from: inFormat, to: captureFormat) else {
            throw AudioIOError.noConverter
        }
        // The voice-processing input can be multichannel; fold it to mono.
        conv.downmix = true
        converter = conv

        input.installTap(onBus: 0, bufferSize: 1024, format: inFormat) { [weak self] buffer, _ in
            guard let self else { return }
            let samples = self.convert(buffer)
            guard !samples.isEmpty else { return }
            self.queue.async { self.frame(samples) }
        }
        engine.prepare()
        try engine.start()
        player.play()
    }

    func stop() {
        engine.inputNode.removeTap(onBus: 0)
        player.stop()
        engine.stop()
        try? engine.inputNode.setVoiceProcessingEnabled(false)
        lock.withLock { generation += 1; scheduled = 0; replyDone = false }
        queue.async { self.pending.removeAll() }
    }

    // MARK: - Playback

    /// A new reply is about to stream in.
    func beginReply() {
        lock.withLock { replyDone = false }
    }

    /// Queues 24 kHz mono reply audio. Safe from any thread.
    func enqueue(_ samples: [Float]) {
        guard !samples.isEmpty,
              let buffer = AVAudioPCMBuffer(pcmFormat: playbackFormat,
                                            frameCapacity: AVAudioFrameCount(samples.count)),
              let channel = buffer.floatChannelData?[0] else { return }
        buffer.frameLength = AVAudioFrameCount(samples.count)
        samples.withUnsafeBufferPointer { src in
            channel.update(from: src.baseAddress!, count: samples.count)
        }
        let gen = lock.withLock { () -> Int in
            scheduled += 1
            return generation
        }
        player.scheduleBuffer(buffer, completionCallbackType: .dataPlayedBack) { [weak self] _ in
            self?.played(generation: gen)
        }
    }

    /// The gateway sent the last of this reply's audio.
    func markReplyDone() {
        let drained = lock.withLock { () -> Bool in
            replyDone = true
            return scheduled == 0
        }
        if drained { onDrained?() }
    }

    /// Drops everything queued for playback (barge-in).
    func flush() {
        lock.withLock {
            generation += 1
            scheduled = 0
            replyDone = false
        }
        player.stop()
        player.play()
    }

    private func played(generation gen: Int) {
        let drained = lock.withLock { () -> Bool in
            guard gen == generation else { return false }
            scheduled -= 1
            return scheduled == 0 && replyDone
        }
        if drained { onDrained?() }
    }

    // MARK: - Capture

    private func convert(_ buffer: AVAudioPCMBuffer) -> [Float] {
        guard let converter else { return [] }
        let ratio = captureFormat.sampleRate / buffer.format.sampleRate
        let capacity = AVAudioFrameCount(Double(buffer.frameLength) * ratio) + 32
        guard let out = AVAudioPCMBuffer(pcmFormat: captureFormat, frameCapacity: capacity) else {
            return []
        }
        var fed = false
        var error: NSError?
        converter.convert(to: out, error: &error) { _, status in
            if fed {
                status.pointee = .noDataNow
                return nil
            }
            fed = true
            status.pointee = .haveData
            return buffer
        }
        guard error == nil, let channel = out.floatChannelData?[0] else { return [] }
        return Array(UnsafeBufferPointer(start: channel, count: Int(out.frameLength)))
    }

    private func frame(_ samples: [Float]) {
        pending.append(contentsOf: samples)
        let n = Self.frameSamples
        while pending.count >= n {
            let frame = Array(pending[0..<n])
            pending.removeFirst(n)
            onFrame?(frame)
        }
    }
}
