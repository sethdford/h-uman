import XCTest
@testable import HumanVoice

final class TurnTakerTests: XCTestCase {
    func testUtteranceWhenIdleIsSubmittedAtOnce() {
        var t = TurnTaker()
        XCTAssertEqual(t.utterance([0.1, 0.2]), [.submit([0.1, 0.2])])
        XCTAssertTrue(t.inFlight)
    }

    func testUtteranceDuringATurnWaitsForItToFinish() {
        var t = TurnTaker()
        _ = t.utterance([0.1])
        XCTAssertEqual(t.utterance([0.5]), [])
        XCTAssertEqual(t.turnFinished(), [.submit([0.5])])
        XCTAssertTrue(t.inFlight, "the held utterance is now the in-flight turn")
        XCTAssertEqual(t.turnFinished(), [])
        XCTAssertFalse(t.inFlight)
    }

    func testTwoHeldUtterancesAreMergedNotDropped() {
        var t = TurnTaker()
        _ = t.utterance([0.1])
        _ = t.utterance([0.5])
        _ = t.utterance([0.7])
        XCTAssertEqual(t.turnFinished(), [.submit([0.5] + TurnTaker.joinGap + [0.7])])
    }

    func testSustainedSpeechInterruptsOnlyAReplyThatIsPlaying() {
        var t = TurnTaker()
        XCTAssertEqual(t.speechSustained(assistant: .speaking), [.bargeIn])
        // Nothing is playing yet while thinking; speech then is held for the next turn.
        XCTAssertEqual(t.speechSustained(assistant: .thinking), [])
        XCTAssertEqual(t.speechSustained(assistant: .listening), [])
        XCTAssertEqual(t.speechSustained(assistant: .idle), [])
    }

    func testSpeechDuringTheGreetingIsHeldUntilItEnds() {
        var t = TurnTaker()
        t.assistantTurnStarted()
        XCTAssertEqual(t.utterance([0.4]), [])
        XCTAssertEqual(t.turnFinished(), [.submit([0.4])])
    }

    func testResetDropsHeldSpeech() {
        var t = TurnTaker()
        _ = t.utterance([0.1])
        _ = t.utterance([0.5])
        t.reset()
        XCTAssertFalse(t.inFlight)
        XCTAssertEqual(t.turnFinished(), [])
        XCTAssertEqual(t.utterance([0.9]), [.submit([0.9])])
    }
}
