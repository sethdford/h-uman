import XCTest
import os
@testable import HumanClient

/// US-45.4 AC-45.4.3: this test exercises the concurrent-access path on
/// `HumanConnection.requestTimeoutSeconds`. The static var was migrated
/// from `nonisolated public static var ... = 10` (a Swift 6 data-race
/// error) to a `nonisolated(unsafe)` storage + `configQueue`-guarded
/// accessor pair. This test fires N=1000 concurrent reads and writes
/// against the accessor and asserts no crash, every read observes a
/// previously-written value, and the final value is preserved.
///
/// Under TSan (`swift test --sanitize=thread`) this is the proof; under
/// release builds the queue discipline ensures correctness by
/// construction. A regression that drops the queue guard would surface
/// either as a TSan flag or as a sporadic mismatch in the read-value
/// invariant on contended cores.
final class HumanConnectionConcurrencyTests: XCTestCase {

    override func tearDown() {
        // Restore the documented default so other suites are not perturbed.
        HumanConnection.requestTimeoutSeconds = 10
        super.tearDown()
    }

    func testRequestTimeoutSecondsConcurrentReadsAndWritesDoesNotCrash() async {
        // Half writers (500) and half readers (500). Each writer chooses a
        // value from a small known set, each reader records what it saw.
        let known: [TimeInterval] = [1, 5, 10, 15, 30, 60]
        HumanConnection.requestTimeoutSeconds = known[0]

        let observed = ObservedValuesBox()

        await withTaskGroup(of: Void.self) { group in
            for i in 0..<500 {
                let v = known[i % known.count]
                group.addTask {
                    HumanConnection.requestTimeoutSeconds = v
                }
            }
            for _ in 0..<500 {
                group.addTask {
                    let r = HumanConnection.requestTimeoutSeconds
                    observed.append(r)
                }
            }
        }

        let allObserved = observed.snapshot()
        XCTAssertEqual(allObserved.count, 500, "every reader must have completed")
        for v in allObserved {
            XCTAssertTrue(known.contains(v),
                          "observed value \(v) must be one of the writes: \(known)")
        }
        // Final value must also be one of the writes — no torn write.
        XCTAssertTrue(known.contains(HumanConnection.requestTimeoutSeconds))
    }

    func testRequestTimeoutSecondsIsolatedSetVisibleOnNextRead() async {
        HumanConnection.requestTimeoutSeconds = 42
        // Read on a detached task — the queue's serial discipline
        // guarantees the prior write is observed.
        let observed = await Task.detached { HumanConnection.requestTimeoutSeconds }.value
        XCTAssertEqual(observed, 42)
    }
}

/// Lock-guarded scratchpad: XCTest expects observations to be aggregated
/// on the test thread, but each `addTask` records on whatever executor
/// dispatched it. `OSAllocatedUnfairLock` is itself `Sendable`, so this
/// box conforms via the lock's own Sendable conformance — no escape hatch.
final class ObservedValuesBox: Sendable {
    private let storage = OSAllocatedUnfairLock<[TimeInterval]>(initialState: [])
    func append(_ v: TimeInterval) {
        storage.withLock { $0.append(v) }
    }
    func snapshot() -> [TimeInterval] {
        storage.withLock { $0 }
    }
}
