import AVFoundation
import HumanVoice
import OSLog

private let audioLog = Logger(subsystem: "ai.human.macos", category: "audio")

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
    /// Replies play here, outside voice processing: routed through the voice-processing
    /// engine they sound like a phone call. The cost is that the echo canceller no longer
    /// sees them, so the controller gates the mic while a reply plays.
    private let playbackEngine = AVAudioEngine()
    let cleanPlayback = !ProcessInfo.processInfo.arguments.contains("--voice-vp-playback")
    private let queue = DispatchQueue(label: "ai.human.voice.capture")
    private let captureFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                              sampleRate: Double(PCMCodec.uplinkSampleRate),
                                              channels: 1, interleaved: false)!
    private let playbackFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                               sampleRate: Double(PCMCodec.downlinkSampleRate),
                                               channels: 1, interleaved: false)!
    private var converter: AVAudioConverter?
    private var monoFormat: AVAudioFormat?
    private var convertErrors = 0
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
        let useVP = !ProcessInfo.processInfo.arguments.contains("--voice-no-vp")
        if useVP {
            try input.setVoiceProcessingEnabled(true)
            // Voice processing ducks other apps' audio by default; keep that light.
            input.voiceProcessingOtherAudioDuckingConfiguration =
                .init(enableAdvancedDucking: true, duckingLevel: .min)
        }

        let out = cleanPlayback ? playbackEngine : engine
        out.attach(player)
        out.connect(player, to: out.mainMixerNode, format: playbackFormat)

        let inFormat = input.outputFormat(forBus: 0)
        audioLog.notice("mic format: \(inFormat.sampleRate) Hz, \(inFormat.channelCount) ch, interleaved=\(inFormat.isInterleaved), common=\(inFormat.commonFormat.rawValue)")
        // The voice-processing input reports several channels (5 on a MacBook) that all
        // carry the same processed voice, with no channel layout. AVAudioConverter cannot
        // downmix that and silently outputs zeros, so take channel 0 and only resample.
        guard let mono = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                       sampleRate: inFormat.sampleRate,
                                       channels: 1, interleaved: false),
              let conv = AVAudioConverter(from: mono, to: captureFormat) else {
            throw AudioIOError.noConverter
        }
        monoFormat = mono
        converter = conv

        input.installTap(onBus: 0, bufferSize: 1024, format: inFormat) { [weak self] buffer, _ in
            guard let self else { return }
            self.noteRaw(buffer)
            let samples = self.convert(buffer)
            guard !samples.isEmpty else { return }
            self.queue.async { self.frame(samples) }
        }
        engine.prepare()
        try engine.start()
        if cleanPlayback {
            playbackEngine.prepare()
            try playbackEngine.start()
        }
        player.play()
        audioLog.notice("audio engine started (voice processing \(useVP ? "on" : "off", privacy: .public), playback \(self.cleanPlayback ? "clean" : "through voice processing", privacy: .public))")
    }

    func stop() {
        engine.inputNode.removeTap(onBus: 0)
        player.stop()
        engine.stop()
        if cleanPlayback { playbackEngine.stop() }
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

    // Raw tap diagnostics: per-channel peak of the unconverted buffers, every ~3 s.
    private var rawBuffers = 0
    private var rawPeaks: [Float] = []

    private func noteRaw(_ buffer: AVAudioPCMBuffer) {
        guard let data = buffer.floatChannelData else { return }
        let ch = Int(buffer.format.channelCount), n = Int(buffer.frameLength)
        if rawPeaks.count != ch { rawPeaks = Array(repeating: 0, count: ch) }
        for c in 0..<ch {
            var peak: Float = 0
            for i in 0..<n { peak = max(peak, abs(data[c][i])) }
            rawPeaks[c] = max(rawPeaks[c], peak)
        }
        rawBuffers += 1
        if rawBuffers >= 130 {
            let peaks = rawPeaks.map { String(format: "%.4f", $0) }.joined(separator: ",")
            audioLog.notice("mic raw: buffers=\(self.rawBuffers) frames/buf=\(n) peak_by_channel=[\(peaks, privacy: .public)]")
            rawBuffers = 0
            rawPeaks = Array(repeating: 0, count: ch)
        }
    }

    private func convert(_ multichannel: AVAudioPCMBuffer) -> [Float] {
        guard let converter, let monoFormat,
              let src = multichannel.floatChannelData?[0],
              let buffer = AVAudioPCMBuffer(pcmFormat: monoFormat,
                                            frameCapacity: multichannel.frameLength),
              let dst = buffer.floatChannelData?[0] else { return [] }
        buffer.frameLength = multichannel.frameLength
        dst.update(from: src, count: Int(multichannel.frameLength))
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
        if let error {
            convertErrors += 1
            if convertErrors <= 3 { audioLog.error("mic convert failed: \(error.localizedDescription, privacy: .public)") }
            return []
        }
        guard let channel = out.floatChannelData?[0] else { return [] }
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
