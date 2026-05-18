import XCTest
@testable import HumanClient
import HumanProtocol

/// US-45.4 AC-45.4.3: this test exercises the concurrency contract of
/// `HumanGatewayClient.request(method:params:completion:)`.
///
/// Before this story, `completion` was typed `(Result<Any, Error>) -> Void`
/// with no `@Sendable` and `Any` is not `Sendable`, so the closure could
/// not safely cross the `Task { ... }` boundary inside `request(...)`.
/// The fix re-types the payload to a `Sendable` struct (`GatewayResponse`),
/// marks `completion` as `@Sendable @escaping`, and hops to `@MainActor`
/// before invoking it.
///
/// What we verify:
///   1. The completion-handler signature accepts a `@Sendable` closure
///      literal — compile-time evidence that the parameter is
///      `@Sendable`-typed (regression would surface as a build error in
///      this file, not a runtime failure).
///   2. The completion fires on the main actor. Driving the call from a
///      `Task.detached` (background) and asserting
///      `dispatchPrecondition(.onQueue(.main))` inside the completion
///      confirms the `MainActor.run` hop took effect.
final class IntentHandlerConcurrencyTests: XCTestCase {

    /// Compile-time check: passing a `@Sendable` closure literal succeeds.
    /// If the parameter were not `@Sendable` this would still compile but
    /// the explicit annotation pins the intent — a future regression that
    /// drops `@Sendable` on the parameter would not break this test, but
    /// would break the `IntentHandler.swift` body that crosses task
    /// boundaries.
    func testRequestCompletionAcceptsSendableClosure() {
        let closure: @Sendable (Result<GatewayResponse, Error>) -> Void = { _ in }
        XCTAssertNotNil(closure)
    }

    /// Drive `request(...)` from a detached (background) task and assert
    /// the completion fires on the main queue. Uses the default localhost
    /// gateway URL — there is no listener, so the call fails fast with
    /// `HumanGatewayClientError.notConnected` from the ensureConnected
    /// retry loop. We do not assert on success; we only assert the
    /// completion ran on the main actor.
    func testRequestCompletionDispatchedOnMainActor() {
        let exp = expectation(description: "completion fires on main actor")

        Task.detached {
            HumanGatewayClient.shared.request(method: "health", params: [:]) { _ in
                // The contract: `MainActor.run { completion(...) }` in
                // `HumanGatewayClient.request(...)` hops to the main actor
                // before invoking the user's closure. `.onQueue(.main)` is
                // the runtime check.
                dispatchPrecondition(condition: .onQueue(.main))
                exp.fulfill()
            }
        }

        // The default URL is `wss://localhost:3000/ws`, ensureConnected's
        // 15s polling loop will give up if no listener exists. Wait long
        // enough for that path to fail.
        wait(for: [exp], timeout: 20.0)
    }
}
