import Foundation

/// Turn-taking for always-on conversation: decides, from VAD events and the
/// reply's progress, when to interrupt and when to send an utterance.
///
/// The gateway runs one turn at a time per connection and streams that turn's
/// audio before answering `voice.audio.end`. Sending a second utterance while
/// the first is still in flight would open the audio gate early and play the
/// tail of the old reply as if it answered the new one, so utterances spoken
/// during a turn are held and sent when it finishes.
public struct TurnTaker: Sendable {
    public enum Action: Equatable, Sendable {
        /// Stop the reply that is playing (or about to) and drop its audio.
        case bargeIn
        /// Send this utterance as the next turn.
        case submit([Float])
    }

    /// A turn has been sent and its response has not come back yet.
    public private(set) var inFlight = false
    private var held: [Float]?

    /// Silence placed between utterances that are merged into one turn.
    static let joinGap = [Float](repeating: 0, count: PCMCodec.uplinkSampleRate / 4)

    public init() {}

    /// The user has been speaking for a moment (`EnergyVAD.Event.sustained`). Only a
    /// reply that is playing is interrupted. While the assistant is still thinking there
    /// is nothing to stop, and what the user says then is held and sent next; cutting
    /// the pending reply there let a cough or a door silently drop whole answers.
    public mutating func speechSustained(assistant: VoiceSession.State) -> [Action] {
        assistant == .speaking ? [.bargeIn] : []
    }

    /// The VAD produced a complete utterance.
    public mutating func utterance(_ samples: [Float]) -> [Action] {
        if inFlight {
            // Both utterances were said to the assistant, so keep both rather
            // than letting the later one silently replace the earlier.
            if let earlier = held {
                held = earlier + Self.joinGap + samples
            } else {
                held = samples
            }
            return []
        }
        inFlight = true
        return [.submit(samples)]
    }

    /// A turn the user didn't speak (Lester's greeting) is now in flight: utterances
    /// spoken during it are held for after it, like during any other turn.
    public mutating func assistantTurnStarted() {
        inFlight = true
    }

    /// The in-flight turn's response arrived (or it failed).
    public mutating func turnFinished() -> [Action] {
        guard let next = held else {
            inFlight = false
            return []
        }
        held = nil
        return [.submit(next)]
    }

    /// Forget any held speech (voice mode turned off, connection lost).
    public mutating func reset() {
        inFlight = false
        held = nil
    }
}
