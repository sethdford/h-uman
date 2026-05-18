# Sprint 45 Plan — "Native App Ship-It"

## Header

| Field | Value |
|---|---|
| Sprint | 45 |
| Branch | `sprint-45-native-apps-ship` |
| Working directory | `/Users/sethford/Documents/h-uman/.worktrees/sprint-45-native-apps` |
| Base SHA | `5b57ff2b` |
| Estimate | ~5M-equivalents |
| Stories | US-45.1, US-45.2, US-45.3, US-45.4, US-45.5, US-45.6 |
| Designs | `US-45.1.md` through `US-45.6.md` (US-45.4 coupled into US-45.3 commit `3a873ef4` — design file exists, harmless) |

---

## §1 Sequencing

### Wave 0 — Parallel, no dependencies
- **US-45.1** — macOS archive (XcodeGen scaffold + `xcodebuild archive` Makefile target)
- **US-45.4** — HumanKit `@available` guards (Swift 6 concurrency-complete, availability annotations)

### Wave 1 — Depends on US-45.1 (prerequisite for signing)
- **US-45.6** — Entitlements + Fastfile + bundle ID consolidation

Rationale: US-45.6 is P0 ordered-second because the entitlements file and Fastfile
configuration are prerequisites for both notarization (US-45.2) and iOS provisioning
(US-45.3). Wave 1 must complete before Wave 2 dispatches.

### Wave 2 — Depends on US-45.1 + US-45.6; all parallel
- **US-45.2** — Notarize DMG (macOS notarytool submission, staple, Sparkle feed update)
- **US-45.3** — iOS archive (provisioning UUID wiring, XCUITest smoke, TestFlight upload)
- **US-45.5** — First-run sheet (onboarding UI, Homebrew formula URL, privacy-consent gate)

Dependency summary:
```
US-45.1 ─┐
          ├─▶ US-45.6 ─┬─▶ US-45.2
US-45.4   │             ├─▶ US-45.3
          │             └─▶ US-45.5
          └─────────────▶ (US-45.1 also feeds US-45.2, US-45.3 directly)
```

---

## §2 Wave Assignments

### Wave 0

| Story | Implementer | Risk | Review gate | Isolation |
|---|---|---|---|---|
| US-45.1 | `general-purpose` | MEDIUM | critic-only sufficient | `impl/US-45.1` worktree |
| US-45.4 | `general-purpose` | MEDIUM | critic-only sufficient | `impl/US-45.4` worktree |

### Wave 1

| Story | Implementer | Risk | Review gate | Isolation |
|---|---|---|---|---|
| US-45.6 | `general-purpose` | HIGH | `/aspect-panel` MANDATORY | `impl/US-45.6` worktree |

### Wave 2

| Story | Implementer | Risk | Review gate | Isolation |
|---|---|---|---|---|
| US-45.2 | `general-purpose` | HIGH | `/aspect-panel` MANDATORY | `impl/US-45.2` worktree |
| US-45.3 | `general-purpose` | HIGH | `/aspect-panel` MANDATORY | `impl/US-45.3` worktree |
| US-45.5 | `general-purpose` | MEDIUM | critic-only sufficient | `impl/US-45.5` worktree |

HIGH-risk designation rationale:
- **US-45.2**: notarytool API key custody; credentials in CI secrets; Apple notarization
  service is an external dependency. Failure mode: DMG ships unsigned or staple fails
  silently.
- **US-45.3**: provisioning profile UUIDs are operator-supplied; wrong UUID breaks
  every TestFlight build silently until submission is rejected by App Store Connect.
- **US-45.6**: entitlements are a security surface; over-provisioned entitlements create
  App Review rejections and sandbox escapes; Fastfile changes affect every release lane.

---

## §3 Implementer Commit Discipline

Every implementer agent MUST commit to `sprint-45-native-apps-ship` before
reporting DONE. Working-tree-only DONE reports are rejected and the story
re-opens.

Required commit pattern:
```
git -C /Users/sethford/Documents/h-uman/.worktrees/sprint-45-native-apps/<impl-dir> \
    add <paths> && \
git -C /Users/sethford/Documents/h-uman/.worktrees/sprint-45-native-apps/<impl-dir> \
    commit -m "feat(US-45.N): <description>"
```

Sprint master verifies commit exists after every DONE report:
```
git log sprint-45-native-apps-ship ^5b57ff2b --oneline | grep -q "US-45.N"
```

If the commit is absent, the DONE report is rejected and the story re-dispatches.

---

## §4 Quality Gates

### Per-story (before closing)

- [ ] Implementer commit exists on `sprint-45-native-apps-ship` (verified by SM)
- [ ] `/verify` ran and returned `RESULT_verifier=PASS`
- [ ] Per-story critic ran immediately after DONE (not batched at sprint end)
  - MEDIUM-risk: critic CLEAN (no HIGH/CRITICAL findings) is sufficient
  - HIGH-risk: `/aspect-panel` returned PASS or CLEAN (not ESCALATE)
- [ ] No outstanding `RESULT_critic=HAS_FINDINGS` of HIGH or CRITICAL severity
- [ ] Tests added or updated; AC have evidence (CI output, behavior demo)
- [ ] Docs updated if public API or entitlements changed

### Per-sprint (before review)

- [ ] All 6 stories closed with commit evidence
- [ ] Adversarial audit via `sprint-auditor` invoked
- [ ] Retro filed at `sprints/sprint-45/retro.md`
- [ ] Close tag: `scripts/tag-sprint-close.sh sprint-45`

---

## §5 Cross-Sprint Coordination

### Sprint-43 — Homebrew formula
US-45.5 first-run sheet references the Homebrew formula onboarding URL owned by
sprint-43. Before Wave 2 dispatches, confirm sprint-43 has merged its formula PR and
the URL is stable. If sprint-43 is still in flight, US-45.5 implementer uses a
placeholder URL (documented in the story) and a follow-up chip is spawned.

### Sprint-42 — Signing artifacts (US-42.4)
US-45.2 notarized DMG signs the macOS binary separately via notarytool — this is
a distinct chain from sprint-42's signing artifacts (US-42.4 covers the CI codesign
step). No direct dependency, but both sprints touch `apps/macos/` build output.
Coordinate: ensure sprint-42's signing certificate (Developer ID Application) is the
same cert used in US-45.2's notarytool submission. Mismatch = notarization rejection.

---

## §6 Top 3 Risks

### Risk 1 — Apple signing-cert + notarytool API key custody (P0, operator pre-req)
**Description:** US-45.2 and US-45.6 require a valid Apple Developer ID certificate and
a notarytool API key (`.p8` file + Issuer ID + Key ID). These are operator-supplied
secrets and cannot be generated by the implementer.

**Mitigation:** Scrum master flags this to the operator BEFORE dispatching Wave 1.
US-45.2 implementer is instructed to wire the notarytool invocation as a
`NOTARYTOOL_KEY_PATH` / `NOTARYTOOL_ISSUER_ID` / `NOTARYTOOL_KEY_ID` env-var contract
and to fail fast with a clear error if any var is unset — rather than silently
submitting without credentials.

**Blocker threshold:** If credentials are unresolved at Wave 2 dispatch, US-45.2 is
deferred to Sprint 46 and Wave 2 dispatches with US-45.3 + US-45.5 only.

### Risk 2 — `apps/macos/` scaffold mismatch (MEDIUM, caught in Wave 0)
**Description:** US-45.1 uses XcodeGen to regenerate `apps/macos/HumanMac.xcodeproj`.
If the worktree's `apps/macos/project.yml` diverges from main (e.g., concurrent work
on `feat/sota-m1-infra`), the archive target may fail with a missing scheme error.

**Mitigation:** US-45.1 implementer runs `xcodebuild -list` after XcodeGen and asserts
the `HumanMac` scheme is present before claiming Wave 0 done. The Scrum Master
verifies this in the DONE evidence.

### Risk 3 — Swift 6 concurrency-complete cascade in US-45.4 (MEDIUM, scope risk)
**Description:** Enabling Swift 6 strict concurrency checking on HumanKit may surface
real actor-isolation bugs (not just warnings). These are correctness issues, not style
issues — they cannot be suppressed with `@unchecked Sendable` without scoping
justification.

**Mitigation:** US-45.4 is scoped to add `@available` guards and fix `Sendable`
conformances. If the implementer encounters actor-isolation errors that require
architectural changes (e.g., moving a `@Published` property behind an actor), the
story scope is renegotiated: the failing files are annotated with
`// TODO(US-45.4): actor isolation — deferred` and a follow-up chip is spawned. The
story closes on the `@available` guards alone.

---

## §7 Open Questions (Operator Action Required)

| # | Question | Blocking | Owner |
|---|---|---|---|
| OQ-1 | Apple Developer Team ID (10-char string) needed for Fastfile + entitlements bundle ID | US-45.6 (Wave 1) | Operator |
| OQ-2 | Provisioning Profile UUID for iOS distribution (AdHoc + App Store) | US-45.3 (Wave 2) | Operator |
| OQ-3 | notarytool API key: `.p8` path, Issuer ID, Key ID for CI secret | US-45.2 (Wave 2) | Operator |
| OQ-4 | Sprint-43 Homebrew formula — is the tap URL stable? | US-45.5 (Wave 2) | Sprint-43 SM |

Operator is flagged on OQ-1 and OQ-3 before Wave 1 dispatch. OQ-2 before Wave 2.
OQ-4 is a soft dependency — US-45.5 can proceed with a placeholder.

---

## Pre-flight Checks Per Wave

### Wave 0 pre-flight
- [ ] Worktree at `/Users/sethford/Documents/h-uman/.worktrees/sprint-45-native-apps` is clean except sprint-45 work
- [ ] `apps/macos/project.yml` exists and XcodeGen version matches CI
- [ ] `apps/HumanKit/` Swift package structure confirmed present
- [ ] OQ-1 not yet needed (Wave 0 has no signing)

### Wave 1 pre-flight
- [ ] US-45.1 commit verified on `sprint-45-native-apps-ship`
- [ ] US-45.4 critic CLEAN
- [ ] OQ-1 (Team ID) answered by operator OR Wave 1 proceeds with placeholder and OQ-1 blocker note

### Wave 2 pre-flight
- [ ] US-45.6 commit verified + `/aspect-panel` PASS
- [ ] OQ-2 (iOS provisioning UUID) answered OR US-45.3 deferred
- [ ] OQ-3 (notarytool key) answered OR US-45.2 deferred
- [ ] OQ-4 (Homebrew URL) resolved or US-45.5 using placeholder

---

*Plan authored by Scrum Master, Sprint 45, 2026-05-17.*
