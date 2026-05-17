# US-14.5 Verifier Evidence — macOS First-Run Onboarding Sheet

Branch: impl/US-14.5  
Commit: 873178e2  
Verifier run: 2026-05-17  

---

## Contract

1. `swift build -c release` in `apps/macos/` — clean
2. `xcodegen generate` — clean
3. `OnboardingURLs.swift` uses `static let` only — no `var`, no runtime input
4. `NSWorkspace.shared.open` is the only URL opener in onboarding path — no `Process(executable:)`, no `URL(string: userInput)`
5. `openTerminalCTA` defaults to `false`
6. `OnboardingTests.swift` uses `-uitestSkipOnboarding` launch arg
7. Tests assert positive observables via accessibility identifiers

---

## Evidence

### BEHAVIOR: swift build release clean
COMMAND: cd apps/macos && swift build -c release
EXIT: 0
EVIDENCE:
  [0/1] Planning build
  Building for production...
  [0/2] Write swift-version--58304C5D6DBC2206.txt
  Build complete! (0.10s)
RESULT: PASS

### BEHAVIOR: xcodegen generate clean
COMMAND: cd apps/macos && xcodegen generate
EXIT: 0
EVIDENCE:
  ⚙️  Generating plists...
  ⚙️  Generating project...
  ⚙️  Writing project...
  Created project at apps/macos/Human.xcodeproj
RESULT: PASS

### BEHAVIOR: OnboardingURLs.swift uses static let only
COMMAND: grep -n "^[[:space:]]*var " apps/macos/Sources/HumanApp/OnboardingURLs.swift
EXIT: 1 (no matches)
EVIDENCE:
  (no output — zero `var` declarations in file)
  File contains: `static let installPageURL = URL(string: "https://gettheconsultant.com/install")!`
                 `static let openTerminalCTA: Bool = false`
  Both in enum bodies; no `var` declarations present.
RESULT: PASS

### BEHAVIOR: NSWorkspace.shared.open is the only URL opener; no Process(executable:) or URL(string: userInput)
COMMAND: grep -rn "Process(" apps/macos/Sources/
EVIDENCE:
  ProcessManager.swift:61: let p = Process()    [executableURL = URL(fileURLWithPath: path) — path from resolveBinary()]
  ProcessManager.swift:85: let p = Process()    [executableURL = URL(fileURLWithPath: path) — same pattern]
  Both Process() usages are in ProcessManager, not onboarding code.
  p.executableURL is always set to URL(fileURLWithPath: <resolved binary path>) — never user input.
  
  Onboarding URL openers:
    OnboardingState.swift:105: NSWorkspace.shared.open(OnboardingURLs.installPageURL)
      -> installPageURL is a compile-time `static let` constant.
    HumanApp.swift:107:    NSWorkspace.shared.open(url)
      -> url = URL(string: "http://localhost:3000") — hardcoded localhost, not onboarding.
  
  No URL(string: userInput) pattern found in any onboarding file.
  No Process(executable: ...) pattern found anywhere.
RESULT: PASS

### BEHAVIOR: openTerminalCTA defaults to false
COMMAND: grep -n "openTerminalCTA" apps/macos/Sources/HumanApp/OnboardingURLs.swift
EXIT: 0
EVIDENCE:
  OnboardingURLs.swift:38: static let openTerminalCTA: Bool = false
RESULT: PASS

### BEHAVIOR: OnboardingTests.swift uses -uitestSkipOnboarding launch arg
COMMAND: grep -n "uitestSkipOnboarding" apps/macos/HumanUITests/OnboardingTests.swift
EXIT: 0
EVIDENCE:
  OnboardingTests.swift:44: static let uitestSkipLaunchArg = "-uitestSkipOnboarding"
  OnboardingTests.swift:78: app.launchArguments.append(LaunchKeys.uitestSkipLaunchArg)
  OnboardingTests.swift:159: launch(completed: false, skipOnboarding: true)
  Production code wired: OnboardingState.swift:37: static let uitestSkipLaunchArg = "-uitestSkipOnboarding"
  Production code consumes it: OnboardingState.swift:recheck() checks arguments.contains(Self.uitestSkipLaunchArg)
RESULT: PASS

### BEHAVIOR: Tests assert positive observables via accessibility identifiers (not buggy state)
COMMAND: read apps/macos/HumanUITests/OnboardingTests.swift
EVIDENCE:
  test_onboardingSheet_appears_on_cold_launch_when_daemon_missing:
    XCTAssertTrue(onboardingSheet.waitForExistence(...))   — positive: sheet exists
    XCTAssertTrue(app.buttons[AXID.installCTA].exists)     — positive: CTA rendered
    XCTAssertTrue(app.buttons[AXID.skipCTA].exists)        — positive: skip rendered
  test_onboardingSheet_stays_dismissed_after_completion_persisted:
    XCTAssertFalse(appeared)   — positive: sheet MUST NOT appear after completion
  test_installCTA_accessibility_label_matches_voiceover_copy:
    XCTAssertTrue(installCTA.exists) + XCTAssertEqual(installCTA.label, "Install h-uman daemon via Homebrew package manager")
  test_launchArg_uitestSkipOnboarding_suppresses_sheet:
    XCTAssertFalse(appeared)   — positive: skip arg suppresses sheet
  
  Accessibility identifiers used: "onboarding.sheet.root", "onboarding.cta.install", "onboarding.cta.skip"
  All assertions describe the contract the code MUST satisfy (per tests-that-pin-bugs.md).
  Zero assertions accept permissive/dangerous outcomes.
RESULT: PASS

---

## Summary

Verified 7/7 behaviors. 0 failed. 0 inconclusive.

RESULT_verifier=PASS story=US-14.5
