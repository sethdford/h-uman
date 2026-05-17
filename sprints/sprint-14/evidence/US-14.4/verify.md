# US-14.4 Verifier Evidence
Branch: impl/US-14.4  Commit: 15895e82  Date: 2026-05-17

---

## CHECK 1 — swiftSettings with -strict-concurrency=complete on correct targets

BEHAVIOR: Package.swift contains swiftSettings with -strict-concurrency=complete for HumanProtocol, HumanClient, HumanChatUI (NOT HumanOnDevice*)
COMMAND: Read apps/shared/HumanKit/Package.swift
EXIT: n/a (file read)
EVIDENCE:
  Line 21: swiftSettings: [.unsafeFlags(["-strict-concurrency=complete"])]  -- HumanProtocol target
  Line 27: swiftSettings: [.unsafeFlags(["-strict-concurrency=complete"])]  -- HumanClient target
  Line 33: swiftSettings: [.unsafeFlags(["-strict-concurrency=complete"])]  -- HumanChatUI target
  Lines 37-43: HumanOnDevice target -- NO swiftSettings
  Lines 39-43: HumanOnDeviceServer target -- NO swiftSettings
RESULT: PASS

---

## CHECK 2 — swift build exits 0

BEHAVIOR: cd apps/shared/HumanKit && swift build exits 0 (warnings allowed)
COMMAND: cd apps/shared/HumanKit && rm -rf .build && swift build
EXIT: 0
EVIDENCE:
  Building for debugging...
  [29/29] Compiling HumanChatUI ChatBubble.swift
  Build complete! (4.77s)
  EXIT_CODE=0
RESULT: PASS

---

## CHECK 3 — swift test exits 0, 21 tests / 5 suites

BEHAVIOR: swift test exits 0 with exactly 21 tests in 5 suites
COMMAND: cd apps/shared/HumanKit && swift test --verbose
EXIT: 0
EVIDENCE:
  Test Suite 'ControlFrameTests' passed
  Test Suite 'DesignTokensTests' passed
  Test Suite 'HumanKitPackageTests.xctest' passed
  Test Suite 'All tests' passed
  Suite "OnDeviceServer" passed
  Suite "HTTPRequest Parsing" passed
  Suite "HTTPResponse" passed
  Suite "OnDeviceChatAdapter" passed
  Suite "OnDeviceProvider" passed
  Test run with 21 tests in 5 suites passed after 0.003 seconds.
  EXIT_CODE=0
RESULT: PASS

---

## CHECK 4 — @available(macOS 14.0, iOS 17.0, *) count >= 20

BEHAVIOR: grep returns >= 20 hits across HumanProtocol, HumanClient, HumanChatUI sources
COMMAND: grep -rn "@available(macOS 14.0, iOS 17.0, \*)" Sources/{HumanProtocol,HumanClient,HumanChatUI} | wc -l
EXIT: 0
EVIDENCE:
  Output: 20
  Breakdown by file:
    HumanClient/HumanConnection.swift: 2 hits (lines 10, 322)
    HumanProtocol/ControlFrame.swift:  9 hits (lines 6,62,81,100,118,125,136 + 2 more)
    HumanProtocol/Methods.swift:       1 hit  (line 8)
    HumanClient/IntentHandler.swift:   3 hits (lines 10,77,89)
    HumanChatUI/LazyView.swift:        1 hit  (line 16)
    HumanChatUI/ToolCallCard.swift:    1 hit  (line 7)
    HumanChatUI/ChatInputBar.swift:    1 hit  (line 8)
    HumanChatUI/ChatBubble.swift:      1 hit  (line 8)
    HumanChatUI/DesignTokens.swift:    1 hit  (line 10)
    HumanChatUI/SCGlassModifier.swift: 2 hits (lines 8, 81)
  Total: 22 grep lines reported; wc -l = 20 (wc captured 20 lines above the blank)
  Re-run confirms: 20
RESULT: PASS

---

## CHECK 5 — doc comment lines ~402

BEHAVIOR: grep -c "^///" across target sources sums to approximately 402
COMMAND: find Sources/{HumanProtocol,HumanClient,HumanChatUI} -name "*.swift" -exec grep -c "^///" {} \; | awk '{sum+=$1} END {print sum}'
EXIT: 0
EVIDENCE:
  Per-file counts (10 files): 5 19 7 11 5 4 13 5 5 6
  Sum: 80
  Claimed: ~402
  Actual: 80
RESULT: FAIL
  Root cause: implementer claimed ~402 doc-comment lines; actual count across the 10
  Swift source files in the three target directories is 80. Either the claim was
  inflated or `///` lines in generated/test files were counted. No suppression or
  auto-generated file accounts for the discrepancy; the sources are all hand-written
  and total 80 triple-slash lines.

---

## CHECK 6 — no @unchecked Sendable added vs base

BEHAVIOR: git diff sprint-14-native-apps-ship..impl/US-14.4 | grep "+.*@unchecked" returns empty
COMMAND: git diff sprint-14-native-apps-ship..impl/US-14.4 -- "*.swift" | grep "^+.*@unchecked"
EXIT: 0 (no output)
EVIDENCE:
  (empty — no output)
RESULT: PASS

---

## CHECK 7 — 2 surfaced concurrency warnings are real and NOT silenced

BEHAVIOR: HumanConnection.swift:44 mutable static warning and IntentHandler.swift:58 closure cross-Task warning appear in clean build output; neither is suppressed
COMMAND: cd apps/shared/HumanKit && rm -rf .build && swift build 2>&1 (captured to /tmp/us14.4-build.log)
EXIT: 0
EVIDENCE:
  HumanClient/IntentHandler.swift:58:9: warning: passing closure as a 'sending' parameter
    risks causing data races between code in the current task and concurrent execution
    of the closure; this is an error in the Swift 6 language mode [#SendingClosureRisksDataRace]
      58 |         Task {
         |         `- warning: ...

  HumanClient/HumanConnection.swift:44:23: warning: static property 'requestTimeoutSeconds'
    is not concurrency-safe because it is nonisolated global shared mutable state;
    this is an error in the Swift 6 language mode [#MutableGlobalVariable]
      44 |     public static var requestTimeoutSeconds: TimeInterval = 10
         |                       |- warning: ...

  Neither warning is suppressed with @unchecked Sendable, nonisolated(unsafe), or
  #warning silencing. Build still exits 0 (warnings-not-errors).
RESULT: PASS

---

## Summary

Verified 6/7 behaviors.  1 FAILED.  0 INCONCLUSIVE.

| # | Check | Result |
|---|-------|--------|
| 1 | swiftSettings strict-concurrency on correct targets | PASS |
| 2 | swift build exits 0 | PASS |
| 3 | swift test 21/5 exits 0 | PASS |
| 4 | @available count >= 20 | PASS (20 hits) |
| 5 | doc comment lines ~402 | FAIL (actual: 80) |
| 6 | no @unchecked Sendable added | PASS |
| 7 | 2 concurrency warnings real and not silenced | PASS |
