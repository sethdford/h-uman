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

    func testSpeechInterruptsAReplyButNotSilence() {
        var t = TurnTaker()
        XCTAssertEqual(t.speechStarted(assistant: .speaking), [.bargeIn])
        XCTAssertEqual(t.speechStarted(assistant: .thinking), [.bargeIn])
        XCTAssertEqual(t.speechStarted(assistant: .listening), [])
        XCTAssertEqual(t.speechStarted(assistant: .idle), [])
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
