import Foundation
import HumanClient
import HumanProtocol

/// What `VoiceSession` needs from a gateway connection. `HumanConnection`
/// conforms; tests use a scripted fake.
@available(macOS 14.0, iOS 17.0, *)
public protocol VoiceTransport: AnyObject, Sendable {
    func request(method: String, params: [String: AnyCodable]?,
                 timeout: TimeInterval?) async throws -> ControlResponse
    @discardableResult func sendData(_ data: Data) -> Bool
    var dataHandler: (@Sendable (Data) -> Void)? { get set }
    var eventHandler: (@Sendable (String, [String: AnyCodable]?) -> Void)? { get set }
}

@available(macOS 14.0, iOS 17.0, *)
extension HumanConnection: VoiceTransport {}

public enum VoiceSessionError: Error, Equatable, Sendable {
    case startRejected(String)
    case turnRejected(String)
    case notStarted
}

/// Lock-guarded switch read on the transport's receive thread: after a barge-in
/// the gateway keeps streaming the interrupted reply until its turn finishes
/// (the standard pipeline answers inside `voice.audio.end`), so those frames
/// must be dropped, not played.
final class AudioGate: @unchecked Sendable {
    private let lock = NSLock()
    private var _open = true
    var isOpen: Bool { lock.lock(); defer { lock.unlock() }; return _open }
    func set(_ open: Bool) { lock.lock(); _open = open; lock.unlock() }
}

/// Drives one always-on voice conversation over the gateway's standard voice
/// pipeline: utterance (16 kHz WAV) -> `voice.audio.end` -> Cartesia STT ->
/// persona turn -> Cartesia TTS streamed back as 24 kHz float PCM.
///
/// Audio is delivered to `audioSink` on the transport's receive thread, in
/// order. State and text callbacks run on the main actor.
@available(macOS 14.0, iOS 17.0, *)
@MainActor
public final class VoiceSession {
    public enum State: Equatable, Sendable {
        case idle
        /// Waiting for the user to speak.
        case listening
        /// Utterance sent; waiting for the reply to start.
        case thinking
        /// Reply audio is arriving.
        case speaking
    }

    public private(set) var state: State = .idle {
        didSet { if oldValue != state { onStateChange?(state) } }
    }

    public var onStateChange: ((State) -> Void)?
    public var onUserTranscript: ((String) -> Void)?
    public var onAssistantText: ((String) -> Void)?
    /// The gateway finished streaming this reply's audio. Playback may still be
    /// draining; call `playbackFinished()` when it has.
    public var onReplyAudioDone: (() -> Void)?
    public var onError: ((Error) -> Void)?

    private let transport: VoiceTransport
    private let voiceId: String?
    private let modelId: String?
    private let gate = AudioGate()
    private let audioSink: @Sendable ([Float]) -> Void
    /// How long one spoken turn may take end to end.
    public let turnTimeout: TimeInterval
    private var replyAudioSeen = false

    public init(transport: VoiceTransport, voiceId: String? = nil, modelId: String? = nil,
                turnTimeout: TimeInterval = 120,
                audioSink: @escaping @Sendable ([Float]) -> Void) {
        self.transport = transport
        self.voiceId = voiceId
        self.modelId = modelId
        self.turnTimeout = turnTimeout
        self.audioSink = audioSink
        let gate = self.gate
        let sink = audioSink
        transport.dataHandler = { [weak self] data in
            guard gate.isOpen else { return }
            let samples = PCMCodec.float32LE(from: data)
            guard !samples.isEmpty else { return }
            sink(samples)
            Task { @MainActor [weak self] in self?.noteReplyAudio() }
        }
        transport.eventHandler = { [weak self] name, payload in
            let fields = VoiceSession.stringFields(payload)
            Task { @MainActor [weak self] in self?.handleEvent(name, fields) }
        }
    }

    /// Reduces an event payload to its string fields (the only ones used here),
    /// which keeps what crosses to the main actor plainly Sendable.
    nonisolated static func stringFields(_ payload: [String: AnyCodable]?) -> [String: String] {
        var out: [String: String] = [:]
        for (k, v) in payload ?? [:] {
            if let s = v.value as? String { out[k] = s }
        }
        return out
    }

    /// Opens the gateway voice session (standard pipeline, Cartesia TTS).
    public func start() async throws {
        var params: [String: AnyCodable] = ["mode": AnyCodable("standard")]
        if let voiceId { params["voiceId"] = AnyCodable(voiceId) }
        if let modelId { params["modelId"] = AnyCodable(modelId) }
        let res = try await transport.request(method: "voice.session.start", params: params,
                                              timeout: 15)
        guard res.ok else {
            throw VoiceSessionError.startRejected(Self.errorText(res))
        }
        state = .listening
    }

    /// Sends one utterance (16 kHz mono float samples) and waits for the turn.
    /// Reply audio streams to `audioSink` while this awaits.
    public func submit(utterance: [Float]) async {
        guard state != .idle else {
            onError?(VoiceSessionError.notStarted)
            return
        }
        gate.set(true)
        replyAudioSeen = false
        state = .thinking
        let wav = PCMCodec.wav(samples: PCMCodec.int16(from: utterance))
        for chunk in PCMCodec.chunks(wav) {
            transport.sendData(chunk)
        }
        do {
            let res = try await transport.request(
                method: "voice.audio.end",
                params: ["mimeType": AnyCodable("audio/wav"), "sessionKey": AnyCodable("voice")],
                timeout: turnTimeout)
            if !res.ok {
                onError?(VoiceSessionError.turnRejected(Self.errorText(res)))
            }
        } catch {
            onError?(error)
        }
        // No audio at all (empty reply, or it was interrupted): back to listening.
        if state == .thinking { state = .listening }
    }

    /// The user started talking over the reply: stop it here and tell the gateway.
    /// Frames still in flight for this turn are dropped.
    public func bargeIn() {
        guard state == .speaking || state == .thinking else { return }
        gate.set(false)
        state = .listening
        let transport = self.transport
        Task {
            _ = try? await transport.request(method: "voice.session.interrupt", params: nil,
                                             timeout: 5)
        }
    }

    /// Call when local playback of the reply has drained.
    public func playbackFinished() {
        if state == .speaking { state = .listening }
    }

    public func stop() async {
        gate.set(false)
        _ = try? await transport.request(method: "voice.session.stop", params: nil, timeout: 5)
        state = .idle
    }

    // MARK: - Private

    private func noteReplyAudio() {
        replyAudioSeen = true
        if state == .thinking { state = .speaking }
    }

    private func handleEvent(_ name: String, _ fields: [String: String]) {
        switch name {
        case "voice.transcript":
            if let text = fields["text"], !text.isEmpty { onUserTranscript?(text) }
        case "chat":
            if fields["state"] == "sent", let text = fields["message"], !text.isEmpty {
                onAssistantText?(text)
            }
        case "voice.audio.done":
            if gate.isOpen { onReplyAudioDone?() }
        default:
            break
        }
    }

    private static func errorText(_ res: ControlResponse) -> String {
        (res.payload?["error"]?.value as? String) ?? "unknown"
    }
}
