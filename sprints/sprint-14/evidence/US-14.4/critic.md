# Critic findings — US-14.4 (HumanKit @available + doc comments + strict-concurrency-complete)

Reviewed commit 15895e82, branch impl/US-14.4.
Verifier passed 7/7. These findings address correctness, safety, and forward-compatibility
that the verifier's build+test run does not cover.

---

## HIGH (2)

- `apps/shared/HumanKit/Sources/HumanClient/HumanConnection.swift:44` —
  **`requestTimeoutSeconds` is a mutable static on `@unchecked Sendable`; concurrent
  mutation is a real data race in current code, not just a future Swift 6 error.**
  The class is marked `@unchecked Sendable` precisely because it manages its own
  locking discipline via `NSLock` (line 40) and a serial `DispatchQueue` (line 36).
  But `requestTimeoutSeconds` is neither protected by `pendingLock` nor dispatched
  through `queue`. Any caller that sets this from a background thread (e.g. an App
  Intent handler or a unit test running on a concurrent executor) while an in-flight
  `request()` reads it (line 159 `Self.requestTimeoutSeconds`) has a TOCTOU window
  with no lock. The warning correctly identifies this as `[MutableGlobalVariable]`.
  Deferral is not safe — the fix is `nonisolated(unsafe)` + a single actor/queue
  guard, or make it `let` with a per-instance override path.

- `apps/shared/HumanKit/Sources/HumanClient/IntentHandler.swift:58` —
  **`Task { ... }` in `request(method:params:completion:)` captures `self` and
  `completion` across an implicit executor hop; in Swift 5.10 with
  `-strict-concurrency=complete` this produces a `[SendingClosureRisksDataRace]`
  warning because `self` is `@unchecked Sendable` and `completion` is
  `@escaping (Result<Any,Error>) -> Void` with no `@Sendable` annotation.**
  The completion is called from inside the `Task` on the Swift concurrency
  scheduler after the `await ensureConnected()` resumes, but the caller may hold
  `self` on a different actor (e.g. `@MainActor`-isolated `AppDelegate`). If the
  App Intent host calls `shared.request(...)` from `@MainActor` and the completion
  mutates `@MainActor`-isolated state, the race is live today. The fix is annotating
  `completion` as `@Sendable` and auditing whether callers must marshal back to
  `@MainActor` themselves.

---

## MED (3)

- `apps/shared/HumanKit/Package.swift:19-33` —
  **`.unsafeFlags(["-strict-concurrency=complete"])` is the wrong migration vehicle.**
  `unsafeFlags` are stripped when the package is consumed as a dependency by an
  Xcode project or another Swift package (SPM silently ignores `unsafeFlags` on
  non-root packages). The project-level Xcode build settings will NOT inherit these
  flags, so the concurrency checking is only active in `swift build` / `swift test`
  from inside the package root — not in the iOS/macOS app targets that actually
  ship. The correct approach for Swift 5.x toolchains is
  `.enableExperimentalFeature("StrictConcurrency")` or for Swift 6 readiness,
  `swiftLanguageMode: .v6` at the package level. When the project upgrades to Swift 6
  these `unsafeFlags` will conflict with the compiler's built-in mode switch.

- `apps/shared/HumanKit/Sources/HumanChatUI/SCGlassModifier.swift:81-89` —
  **`public extension View` with `@available(macOS 14.0, iOS 17.0, *)` on the
  extension block does not restrict callers on platforms outside that set.**
  `View` is defined on all Apple platforms including watchOS, tvOS, and visionOS.
  The `*` wildcard in `@available(macOS 14.0, iOS 17.0, *)` means the extension
  and its `scGlass(_:)` method are available on ALL other platforms without any
  version floor. A visionOS or watchOS target that adds HumanChatUI as a dependency
  will compile `scGlass` without restriction but hit missing `SCGlassModifier`
  internals (e.g. `glassEffect` is an iOS/macOS API). Package.swift currently limits
  to `.macOS(.v14)` and `.iOS(.v17)`, which prevents that compilation today, but
  the `*` is still semantically wrong and will cause noise if watchOS or visionOS
  support is ever added. Either drop the extension's `@available` (it is implied by
  the enclosing type's annotation) or make the `*` explicit: mark the extension
  `@available(macOS 14.0, iOS 17.0, watchOS, tvOS, *)` with appropriate floors.

- `apps/shared/HumanKit/Sources/HumanOnDeviceServer/HTTPServer.swift:7` —
  **`HTTPServer` is `@unchecked Sendable` in a target (`HumanOnDeviceServer`) that
  has NO `-strict-concurrency=complete` swiftSettings, so the compiler never
  validates the claim.** `HTTPServer` holds a mutable `NWListener?` (line 11)
  and calls `handleConnection` from a concurrent `DispatchQueue` (line 13
  `.concurrent` attribute, line 54). The `start()` / `stop()` methods mutate
  `listener` without any lock. If two threads call `start()` concurrently, or
  `stop()` races with a `newConnectionHandler` callback, `listener` is corrupted.
  Because the target opted out of strict concurrency, the compiler emits no warning.
  Either add `-strict-concurrency=complete` to `HumanOnDeviceServer` (and fix the
  resulting warnings) or document that `HTTPServer` must be called on a single
  queue and enforce that with an assert.

---

## LOW (1)

- `apps/shared/HumanKit/Sources/HumanClient/HumanConnection.swift:216` —
  `DispatchQueue.main.async { self.state = .disconnected }` in the `receive()`
  failure branch dispatches a state mutation to the main queue, but all other state
  mutations in the class go through `self.queue` (the private serial queue). This
  is an inconsistency: `stateHandler` (line 24) will be invoked on the main queue
  for disconnect but on `self.queue` for connect/reconnect transitions. Callers that
  rely on a single-queue guarantee will see interleaved callbacks. LOW because it is
  a pre-existing pattern not introduced by this PR, but the doc comment added in this
  PR ("Setting this value notifies `stateHandler`") says nothing about which queue,
  making the inconsistency harder to spot in future audits.

---

## Cross-agent regression risk

None identified. The changed Swift files are self-contained inside
`apps/shared/HumanKit/`. No C source, CMakeLists, or shared headers were modified
by this PR. US-14.1 / US-14.2 / US-14.3 touch the Fastlane / Xcode archive path,
not HumanKit sources; no conflict surface detected.

---

RESULT_critic=HAS_FINDINGS story=US-14.4 severity=HIGH
