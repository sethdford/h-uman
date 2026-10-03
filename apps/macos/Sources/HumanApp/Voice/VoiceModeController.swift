import AVFoundation
import HumanClient
import HumanVoice
import SwiftUI

/// Settings for voice mode, stored in the app's user defaults.
enum VoiceSettings {
    static let gatewayURLKey = "voice.gatewayURL"
    static let voiceIdKey = "voice.voiceId"
    static let modelIdKey = "voice.modelId"

    /// The voice gateway: a second h-uman gateway with a spoken-turn config.
    static let defaultGatewayURL = "ws://127.0.0.1:3007/ws"
    /// Cartesia "Lester Nare (Pro) V3".
    static let defaultVoiceId = "ebaf7477-b6ae-417e-be54-19e6176777ea"
    static let defaultModelId = "sonic-3.6-2026-08-27"

    static func string(_ key: String, _ fallback: String) -> String {
        let v = UserDefaults.standard.string(forKey: key)?.trimmingCharacters(in: .whitespaces)
        return (v?.isEmpty == false) ? v! : fallback
    }
}

/// VAD state touched on the capture queue and, for the speaking/muted flags,
/// from the main actor.
private final class VADBox: @unchecked Sendable {
    private let lock = NSLock()
    private var vad = EnergyVAD()
    private var _muted = false

    var assistantSpeaking: Bool {
        get { lock.withLock { vad.assistantSpeaking } }
        set { lock.withLock { vad.assistantSpeaking = newValue } }
    }

    var muted: Bool {
        get { lock.withLock { _muted } }
        set { lock.withLock { _muted = newValue; if newValue { vad.reset() } } }
    }

    func process(_ frame: [Float]) -> [EnergyVAD.Event] {
        lock.withLock { _muted ? [] : vad.process(frame) }
    }

    func reset() { lock.withLock { vad.reset() } }
}

/// Always-on spoken conversation with h-uman: listens continuously, sends each
/// utterance to the voice gateway, plays the reply in the configured Cartesia
/// voice, and lets you talk over it.
@MainActor
final class VoiceModeController: ObservableObject {
    enum Phase: Equatable {
        case off
        case connecting
        case listening
        /// The user is talking.
        case hearing
        case thinking
        case speaking
        case failed(String)
    }

    @Published private(set) var phase: Phase = .off
    /// Smoothed microphone level, 0...1, for the orb.
    @Published private(set) var level: CGFloat = 0
    @Published private(set) var heard = ""
    @Published private(set) var said = ""
    @Published var muted = false {
        didSet { vadBox.muted = muted }
    }

    var isOn: Bool { phase != .off }

    private var connection: HumanConnection?
    private var session: VoiceSession?
    private var audio: AudioIO?
    private let vadBox = VADBox()
    private var turns = TurnTaker()
    private var hearing = false
    private lazy var orb = OrbPanel(controller: self)

    func toggle() {
        if isOn { stop() } else { Task { await start() } }
    }

    func start() async {
        guard !isOn else { return }
        guard await microphoneAllowed() else {
            phase = .failed("Microphone access is off. Allow Human in System Settings › Privacy & Security › Microphone.")
            orb.show()
            return
        }

        let urlString = VoiceSettings.string(VoiceSettings.gatewayURLKey, VoiceSettings.defaultGatewayURL)
        guard let url = URL(string: urlString) else {
            phase = .failed("Voice gateway URL is not valid: \(urlString)")
            orb.show()
            return
        }

        let audio = AudioIO()
        let connection = HumanConnection(url: url)
        let session = VoiceSession(
            transport: connection,
            voiceId: VoiceSettings.string(VoiceSettings.voiceIdKey, VoiceSettings.defaultVoiceId),
            modelId: VoiceSettings.string(VoiceSettings.modelIdKey, VoiceSettings.defaultModelId)
        ) { [audio] samples in
            audio.enqueue(samples)
        }
        wire(session: session, audio: audio, connection: connection)

        do {
            try audio.start()
        } catch {
            phase = .failed("Could not start the microphone: \(error.localizedDescription)")
            orb.show()
            return
        }
        self.audio = audio
        self.connection = connection
        self.session = session
        phase = .connecting
        connection.connect()
        orb.show()
    }

    func stop() {
        let session = self.session
        Task { await session?.stop() }
        audio?.stop()
        connection?.disconnect()
        audio = nil
        connection = nil
        self.session = nil
        turns.reset()
        vadBox.reset()
        vadBox.assistantSpeaking = false
        hearing = false
        level = 0
        phase = .off
        orb.hide()
    }

    // MARK: - Wiring

    private func wire(session: VoiceSession, audio: AudioIO, connection: HumanConnection) {
        session.onStateChange = { [weak self] state in
            self?.sessionChanged(state)
        }
        session.onUserTranscript = { [weak self] text in self?.heard = text }
        session.onAssistantText = { [weak self] text in self?.said = text }
        session.onReplyAudioDone = { [weak audio] in audio?.markReplyDone() }
        session.onError = { error in
            NSLog("[voice] turn error: %@", String(describing: error))
        }

        audio.onDrained = { [weak self] in
            Task { @MainActor in self?.session?.playbackFinished() }
        }
        let vadBox = self.vadBox
        audio.onFrame = { [weak self] frame in
            let events = vadBox.process(frame)
            let rms = EnergyVAD.rms(frame)
            Task { @MainActor in self?.captured(events, rms: rms) }
        }

        connection.stateHandler = { [weak self] state in
            Task { @MainActor in self?.connectionChanged(state) }
        }
    }

    private func connectionChanged(_ state: HumanConnection.ConnectionState) {
        guard isOn, let session else { return }
        switch state {
        case .connected:
            Task {
                do {
                    try await session.start()
                } catch {
                    phase = .failed("The voice gateway refused the session (\(error)).")
                }
            }
        case .connecting:
            phase = .connecting
        case .disconnected:
            // The gateway forgets the session with the socket; it is reopened
            // on reconnect. Speech held for the lost turn goes with it.
            turns.reset()
            audio?.flush()
            vadBox.assistantSpeaking = false
            phase = .connecting
        }
    }

    private func sessionChanged(_ state: VoiceSession.State) {
        vadBox.assistantSpeaking = (state == .speaking)
        switch state {
        case .idle: break
        case .listening: phase = hearing ? .hearing : .listening
        case .thinking: phase = .thinking
        case .speaking: phase = .speaking
        }
    }

    private func captured(_ events: [EnergyVAD.Event], rms: Float) {
        guard isOn else { return }
        // Map RMS (speech is roughly 0.02...0.2) onto 0...1 and smooth it.
        let target = CGFloat(min(1, max(0, (rms - 0.005) / 0.15)))
        level = level * 0.7 + target * 0.3

        guard let session else { return }
        for event in events {
            switch event {
            case .speechStarted:
                hearing = true
                perform(turns.speechStarted(assistant: session.state))
                if session.state == .listening { phase = .hearing }
            case .utterance(let samples):
                hearing = false
                perform(turns.utterance(samples))
            case .discarded:
                hearing = false
                if session.state == .listening { phase = .listening }
            }
        }
    }

    private func perform(_ actions: [TurnTaker.Action]) {
        guard let session else { return }
        for action in actions {
            switch action {
            case .bargeIn:
                session.bargeIn()
                audio?.flush()
                vadBox.assistantSpeaking = false
            case .submit(let samples):
                audio?.beginReply()
                Task {
                    await session.submit(utterance: samples)
                    // The session may have been replaced by stop()/start().
                    guard self.session === session else { return }
                    self.perform(self.turns.turnFinished())
                }
            }
        }
    }

    private func microphoneAllowed() async -> Bool {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized: return true
        case .notDetermined: return await AVCaptureDevice.requestAccess(for: .audio)
        default: return false
        }
    }
}
