# Sprint 14 Plan — "Native App Ship-It"

## Header

| Field | Value |
|---|---|
| Sprint | 14 |
| Branch | `sprint-14-native-apps-ship` |
| Working directory | `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-14-native-apps` |
| Base SHA | `34b34cb5` |
| Estimate | M + S + M + XS + M + S ≈ 4.5 sprint-days (per stories.md) |
| Stories | US-14.1, US-14.2, US-14.3, US-14.4, US-14.5, US-14.6 |
| P0 stories | US-14.1, US-14.2 |
| HIGH-risk stories | US-14.2, US-14.3 (aspect-panel MANDATORY) |
| Open question | Bundle ID convention: stories.md says `ai.human.mac`/`ai.human.ios`; US-14.6 design resolves to `ai.humanlabs.HumanMac`/`ai.humanlabs.HumaniOS`. Implementer for US-14.6 must confirm with product-owner before landing. |

---

## §1 Sequencing

Wave naming follows stories.md convention (Wave 1 / Wave 2 / Wave 3).

### Wave 1 — Parallel (no dependencies)

| Story | Title | Why independent |
|---|---|---|
| US-14.1 | macOS archive (Xcode) | No deps; foundation for Wave 2 |
| US-14.4 | HumanKit `@available` / strict-concurrency | No deps; isolated framework target under `apps/shared/HumanKit/` |

Wave 1 gate: both US-14.1 AND US-14.4 must have verified commits on `sprint-14-native-apps-ship` before Wave 2 dispatches.

### Wave 2 — Gated on US-14.1; US-14.3 is independently archivable but grouped here

| Story | Title | Dependency |
|---|---|---|
| US-14.2 | Notarize DMG (staple + Gatekeeper) | US-14.1 (signed `.app` artifact required) |
| US-14.3 | iOS archive + TestFlight IPA | No hard dep on US-14.1; grouped by convention (both need signing cert infra; macOS runner overhead batched) |

Wave 2 gate: both US-14.2 AND US-14.3 must have verified commits before Wave 3 dispatches.

### Wave 3 — Depends on US-14.1 (macOS app scaffold) + US-14.3 (iOS app scaffold)

| Story | Title | Dependency |
|---|---|---|
| US-14.5 | First-run onboarding sheet (macOS) | US-14.1 (macOS app target exists for `ContentView.swift` wiring) |
| US-14.6 | Entitlements + Fastfile lanes | Informational dep on US-14.1/14.3 for script paths; can be authored independently |

---

## §2 Wave Assignments

### Wave 0

| Story | Title | Risk | Key Files | Implementer | Verifier Scope | Critic Scope | Aspect-Panel | Worktree Isolation |
|---|---|---|---|---|---|---|---|---|
| US-14.1 | macOS archive (Xcode + CI) | MEDIUM | `apps/macos/project.yml` or `.xcodeproj`, `.github/workflows/native-apps-fleet.yml`, `scripts/ci/import-macos-cert.sh` (new) | general-purpose | `xcodebuild archive` exits 0; `Human.xcarchive/Products/Applications/Human.app` exists; `codesign --verify --deep --strict` exits 0; `macos-app-archive` CI job green | Signing identity by name not hash; no `--deep` on signing invocations; keychain cleanup; AC-14.1.3 regression (swift build release unaffected) | NO (critic-only per stories.md DoD) | YES |
| US-14.4 | HumanKit `@available` + strict-concurrency | LOW | `apps/shared/HumanKit/Package.swift`, `Sources/HumanProtocol/*.swift`, `Sources/HumanClient/*.swift`, `Sources/HumanChatUI/*.swift` | general-purpose | `swift build` exits 0 (strict-concurrency=complete baked into Package.swift); `swift test` green for 3 in-scope targets; `swift package generate-documentation` 0 undocumented warnings | `@available(macOS 14.0, iOS 17.0, *)` matches Package.swift floor; no renames or logic changes; `HumanOnDevice*` targets untouched; doc comments follow semantic pattern not placeholder filler | NO (critic-only) | YES |

### Wave 2

| Story | Title | Risk | Key Files | Implementer | Verifier Scope | Critic Scope | Aspect-Panel | Worktree Isolation |
|---|---|---|---|---|---|---|---|---|
| US-14.2 | Notarize DMG | HIGH | `scripts/notarize-mac.sh` (new), `scripts/test-notarize-dryrun.sh` (new), `tests/fixtures/notarize/`, `.github/workflows/native-apps-fleet.yml` | general-purpose | `notarize-mac.sh --dry-run` exits 0; `Human-*.dmg` created; stub `xcrun submit/staple/log` never tripped in dry-run test; CI `notarize-dmg` job gated on `refs/tags/v*` only; distinct exit codes (2/3/4) for failure modes | Credential lifecycle (umask 077, EXIT trap unlinks .p8, set +x bracketed, no echo of key bytes); tag gate cannot fire on fork PRs; `--help` exits 0 with documented flags; test asserts NEGATIVE (no network call) per `tests-that-pin-bugs.md` | MANDATORY | YES |
| US-14.3 | iOS archive + IPA export | HIGH | `apps/ios/ExportOptions.plist` (new), `scripts/ios-archive-export.sh` (new), `.github/workflows/native-apps-fleet.yml`, `apps/ios/README.md` | general-purpose | `plutil -lint ExportOptions.plist` exits 0; `.xcarchive` artifact created; `plutil -p Info.plist` confirms bundle ID and version; placeholder token `__HU_IOS_PROVISIONING_PROFILE_UUID__` present in committed plist; `.bak` restore leaves repo clean | No real UUID committed; CI gated correctly (not on every PR); negative plutil test verified; `CODE_SIGNING_ALLOWED=NO` fallback documented for first land | MANDATORY | YES |

### Wave 3

| Story | Title | Risk | Key Files | Implementer | Verifier Scope | Critic Scope | Aspect-Panel | Worktree Isolation |
|---|---|---|---|---|---|---|---|---|
| US-14.5 | First-run onboarding sheet (macOS) | MEDIUM | `apps/macos/Human/Onboarding/OnboardingSheet.swift` (new), `OnboardingURLs.swift` (new), `OnboardingState.swift` (new), `ContentView.swift`, `HumanApp.swift`, `HumanUITests/OnboardingTests.swift` (new), `LaunchArguments.swift` | general-purpose | XCUITest green (3 cases: cold-launch present, skip persists, `-uitestSkipOnboarding` honored); `swift build -c release` unaffected; `./build/human_tests` 0 failures | URL is hard-coded `static let` constant — not from config/env/runtime; `OnboardingFeature.openTerminalCTA == false`; Terminal CTA button NOT rendered; `-uitestSkipOnboarding` literal matches iOS precedent; onboarding URL is `gettheconsultant.com/install` as per design (pre-flight: `curl -sI` before writing XCUITest) | NO (critic-only) | YES |
| US-14.6 | Entitlements + Fastfile lanes | MEDIUM | `apps/macos/Human.entitlements` (new), `apps/Fastfile` (new), `apps/fastlane/Appfile` (new), `.github/workflows/native-apps-fleet.yml` | general-purpose | `plutil -lint Human.entitlements` exits 0; negative grep confirms none of the 4 blacklisted entitlement keys present; `fastlane lanes` lists `mac_archive` and `ios_archive`; Appfile bundle-ID greps pass; duplicate `*.entitlements` scan clean | Least-privilege: no disable-library-validation/allow-unsigned-executable-memory/disable-executable-page-protection/allow-dyld-environment-variables; `network.server = false`; Fastfile does NOT duplicate signing logic from US-14.1/3; bundle ID open question flagged to product-owner | MANDATORY (stories.md DoD: "touches signing/entitlements") | YES |

---

## §3 Implementer Commit Discipline

Every implementer MUST:

1. Work exclusively inside their assigned worktree — no writes outside it.
2. Before reporting DONE, run:
   ```bash
   git -C <impl-worktree> add <paths>
   git -C <impl-worktree> commit -m "feat(US-14.X): <description>"
   ```
3. Include the resulting commit SHA in their DONE report.
4. DONE reports with only working-tree changes (no commit SHA) are **rejected**. The story re-opens and the implementer re-dispatches.

Verification command (run by Scrum Master after each DONE report):
```bash
git log sprint-14-native-apps-ship ^34b34cb5 --oneline | grep -q "US-14.X"
```
If the grep fails, the story is not done.

---

## §4 Quality Gates

### Per-story (before closure)

- [ ] Implementer commit exists on `sprint-14-native-apps-ship` (or merged from impl branch)
- [ ] `/verify` returned `RESULT_verifier=PASS`
- [ ] Per-story critic ran immediately after DONE (not batched at sprint end) — returned CLEAN or LOW/INFO only
- [ ] For US-14.2, US-14.3, US-14.6: `/aspect-panel` returned PASS or CLEAN (ESCALATE blocks closure)
- [ ] Tests added or updated; test command documented in evidence file
- [ ] No HIGH or CRITICAL critic findings outstanding

### Per-sprint (before Phase 5 Review)

- [ ] All 6 stories closed with evidence
- [ ] No open CRITIC- or REGRESSION- tasks
- [ ] `scripts/agent-preflight.sh` passes on the sprint branch
- [ ] Sprint Review summary written to `sprints/sprint-14/review.md`

---

## §5 Cross-Sprint Coordination

**Sprint 14 depends on Sprint 9 (Homebrew formula) for US-14.5 onboarding URL.**

The first-run onboarding sheet in US-14.5 references `https://gettheconsultant.com/install` (per the US-14.5 design) and the brew tap `humanlabs/human/human`. Sprint 9 owns the Homebrew formula; Sprint 14 does not create it.

Handling:
- Implementer for US-14.5 uses `OnboardingURLs.install = URL(string: "https://gettheconsultant.com/install")!` as a hard-coded constant (design-spec'd, not a placeholder).
- Pre-flight check for the implementer: `curl -sI https://gettheconsultant.com/install` before writing any XCUITest that references the live page — the test asserts the URL string only, not that the page returns 200.
- If the URL is a 404 at implementation time: the CTA still functions (browser opens to a 404); document this in PR description as a known transitional state pending Sprint 9.
- CI smoke-test for the live URL is **out of scope for Sprint 14**.
- The brew command copy in the sheet references `brew install humanlabs/human/human` — confirm tap name with product-owner matches Sprint 9 formula before landing.
- Scrum Master will flag the Sprint 9 dependency in the Sprint Review.

**US-14.5 also has an informational dependency on US-14.1.**

The `ContentView.swift` wiring in US-14.5 requires the macOS app target structure established in US-14.1. US-14.5 implementer must not begin until US-14.1 commit is verified.

---

## §6 Top 3 Sprint Risks

### Risk 1 — Signing-cert provisioning blocks US-14.2 full CI path (HIGH)

US-14.2's real notarization path requires `APPLE_ID`, `APPLE_TEAM_ID`, `NOTARYTOOL_APP_PASSWORD` (or App Store Connect API key set) provisioned as CI secrets. The CI job is gated on `refs/tags/v*` so it does not fire on PRs. But the secrets must exist before the first tag push or the job silently skips submission.

**Mitigation (from design):**
- `notarize-mac.sh --dry-run` path is the primary verifier seam; it exercises arg-parse + DMG build without secrets.
- Dry-run smoke job runs on every PR (no secrets needed).
- The real notarization CI job uses `APP_STORE_CONNECT_API_KEY`/`KEY_ID`/`ISSUER_ID` (not Apple ID + password) to avoid 2FA.
- Pre-sprint operator action item: confirm secrets are provisioned in the GitHub repo settings before Sprint 14 tag push. Flag explicitly in Sprint Review if not confirmed.

### Risk 2 — Swift strict-concurrency-complete cascade in HumanKit (MEDIUM)

US-14.4 bakes `-strict-concurrency=complete` into `Package.swift` for `HumanProtocol`, `HumanClient`, `HumanChatUI`. Both `HumanGatewayClient` and `HumanConnection` are marked `@unchecked Sendable` — the flag may surface real actor-isolation gaps.

**Mitigation (from design):**
- Implementer captures a baseline warning list in step 1 (before annotating anything).
- AC-14.4.1 scope is narrow: "zero new warnings about missing availability or concurrency annotations on public symbols." Pre-existing non-public concurrency warnings are not blockers for THIS story — they are surfaced as a separate finding.
- Implementer must NOT silence genuine concurrency bugs with additional `@unchecked Sendable` to make the build green. That is the `tests-that-pin-bugs.md` failure pattern applied to Swift.
- If structural isolation bugs appear: implementer files a CRITIC- finding; Scrum Master triages. 1–3 line fixes are absorbed into US-14.4; deeper isolation work gets a Sprint 15 story.

### Risk 3 — `apps/` scaffold mismatch (MEDIUM)

All six designs assume specific paths (`apps/macos/project.yml`, `apps/shared/HumanKit/`, `apps/ios/ExportOptions.plist`, etc.). The actual scaffold may differ in target names, directory structure, or existence.

**Mitigation:**
- Each Wave 1 implementer's first action is the gating check from the design: `ls apps/macos/` (US-14.1) or `ls apps/shared/HumanKit/Sources/` (US-14.4).
- US-14.1 design explicitly says: if no `.xcodeproj`, `project.yml`, or `Package.swift` found in `apps/macos/`, STOP and report `RESULT=NEEDS_AC_REFINEMENT`. Do not invent a project structure.
- US-14.3 design says: if signing fails without real secrets, use `CODE_SIGNING_ALLOWED=NO` for the archive step and track as `DEBT-ios-signing-secrets`.
- Any mismatch surfaces immediately to Scrum Master before proceeding. Sprint scope adjusts rather than implementer silently expanding the story.

---

## Status

- [ ] Wave 1 dispatched (US-14.1 + US-14.4 in parallel)
- [ ] Wave 1 complete (US-14.1 committed + verified; US-14.4 committed + verified)
- [ ] Wave 2 dispatched (US-14.2 + US-14.3 in parallel)
- [ ] Wave 2 complete (US-14.2 committed + verified + aspect-panel CLEAN; US-14.3 committed + verified + aspect-panel CLEAN)
- [ ] Wave 3 dispatched (US-14.5 + US-14.6 in parallel)
- [ ] Wave 3 complete (US-14.5 committed + verified; US-14.6 committed + verified + aspect-panel CLEAN)
- [ ] Sprint Review written to `sprints/sprint-14/review.md`
- [ ] sprint-auditor invoked
- [ ] Retro filed to `sprints/sprint-14/retro.md`
- [ ] Close tag pinned via `scripts/tag-sprint-close.sh sprint-14`
