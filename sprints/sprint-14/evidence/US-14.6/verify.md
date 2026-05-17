# Verifier Evidence — US-14.6 (Entitlements + Fastfile + Bundle ID)

**Branch:** impl/US-14.6  
**Commit:** 5d226268  
**Date:** 2026-05-17  
**Verifier:** claude-sonnet-4-6

Verified 8/8 behaviors. 0 failed. 0 inconclusive.

---

## BEHAVIOR 1: plutil -lint Human.entitlements exits 0

```
BEHAVIOR: plutil-lint-clean
COMMAND: plutil -lint apps/macos/Human.entitlements
EXIT: 0
EVIDENCE:
  /Users/sethford/Projects/h-uman/.claude/worktrees/impl-sprint-14-US-14.6/apps/macos/Human.entitlements: OK
RESULT: PASS
```

---

## BEHAVIOR 2: app-sandbox TRUE, network.client TRUE, network.server absent/FALSE

```
BEHAVIOR: required-entitlements-present
COMMAND: cat apps/macos/Human.entitlements
EXIT: 0
EVIDENCE:
  <?xml version="1.0" encoding="UTF-8"?>
  <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
  <plist version="1.0">
  <dict>
      <key>com.apple.security.app-sandbox</key>
      <true/>
      <key>com.apple.security.network.client</key>
      <true/>
      <key>com.apple.security.network.server</key>
      <false/>
  </dict>
  </plist>

  app-sandbox: <true/> — PRESENT
  network.client: <true/> — PRESENT
  network.server: <false/> — PRESENT and FALSE (satisfies "absent or FALSE")
RESULT: PASS
```

---

## BEHAVIOR 3: NEGATIVE — forbidden sandbox-escape keys absent

```
BEHAVIOR: no-sandbox-escape-entitlements
COMMAND: grep -E "disable-library-validation|allow-unsigned-executable-memory|disable-executable-page-protection|allow-dyld-environment-variables" apps/macos/Human.entitlements
EXIT: 1 (grep found no matches — expected)
EVIDENCE:
  (no output — zero matches)
  GREP_EXIT:1
RESULT: PASS
```

No forbidden key appears in the file. The expected exit code is 1 (no match), confirming all four sandbox-escape entitlements are absent.

---

## BEHAVIOR 4: project.yml has CODE_SIGN_ENTITLEMENTS: Human.entitlements

```
BEHAVIOR: code-sign-entitlements-wired
COMMAND: grep -n "CODE_SIGN_ENTITLEMENTS" apps/macos/project.yml
EXIT: 0
EVIDENCE:
  34:        CODE_SIGN_ENTITLEMENTS: Human.entitlements
  47:        # apps/macos/Human.entitlements (wired above via CODE_SIGN_ENTITLEMENTS).
RESULT: PASS
```

---

## BEHAVIOR 5: project.yml has PRODUCT_BUNDLE_IDENTIFIER: ai.human.mac (not ai.humanlabs.mac)

```
BEHAVIOR: bundle-id-reconciled
COMMAND: grep -n "PRODUCT_BUNDLE_IDENTIFIER" apps/macos/project.yml
EXIT: 0
EVIDENCE:
  33:        PRODUCT_BUNDLE_IDENTIFIER: ai.human.mac
RESULT: PASS
```

Value is `ai.human.mac` — NOT `ai.humanlabs.mac`.

---

## BEHAVIOR 6: Appfile has app_identifier(['ai.human.mac', 'ai.human.ios'])

```
BEHAVIOR: appfile-bundle-ids
COMMAND: cat apps/fastlane/Appfile
EXIT: 0
EVIDENCE:
  app_identifier(["ai.human.mac", "ai.human.ios"])
  
  for_platform :mac do
    app_identifier "ai.human.mac"
  end
  
  for_platform :ios do
    app_identifier "ai.human.ios"
  end
RESULT: PASS
```

Both `ai.human.mac` and `ai.human.ios` present in the array literal.

---

## BEHAVIOR 7: Fastfile has both mac_archive and ios_archive lanes

```
BEHAVIOR: fastfile-lanes
COMMAND: grep -n "mac_archive\|ios_archive" apps/Fastfile
EXIT: 0
EVIDENCE:
  3:# Two lanes: mac_archive, ios_archive. Each delegates to the per-platform
  21:#   * NOTARYTOOL_APP_PASSWORD — app-specific password for notarytool (mac_archive)
  38:  lane :mac_archive do
  56:  lane :ios_archive do
RESULT: PASS
```

Both `lane :mac_archive` (line 38) and `lane :ios_archive` (line 56) defined.

---

## BEHAVIOR 8: CI entitlements-check job exists with negative-grep step, no continue-on-error

```
BEHAVIOR: ci-entitlements-check-job
COMMAND: grep -n "entitlements-check\|continue-on-error" .github/workflows/native-apps-fleet.yml
         + awk to confirm no continue-on-error inside the job block
EXIT: 0 for existence; GREP_EXIT:1 for absence of continue-on-error in job block
EVIDENCE:
  native-apps-fleet.yml:34:  entitlements-check:
  native-apps-fleet.yml:307:      - entitlements-check
  native-apps-fleet.yml:317:          ENT: ${{ needs.entitlements-check.result }}

  Job definition (lines 34–87, abridged):
    entitlements-check:
      name: macOS app · entitlements + Fastfile lint (US-14.6)
      runs-on: macos-latest
      timeout-minutes: 5
      steps:
        - name: Sandbox-escape entitlements MUST NOT appear (negative grep)
          run: |
            pattern='disable-library-validation|allow-unsigned-executable-memory|
                     disable-executable-page-protection|allow-dyld-environment-variables'
            if grep -E "$pattern" apps/macos/Human.entitlements; then
              exit 1
            fi
            echo "ok: no sandbox-escape entitlements"

  awk search for continue-on-error within the job block:
    GREP_EXIT:1 (not present)
RESULT: PASS
```

Job exists, negative-grep step is present, `continue-on-error` is absent from the job block.

---

## Summary

| # | Behavior | Result |
|---|----------|--------|
| 1 | plutil -lint exits 0 | PASS |
| 2 | app-sandbox TRUE + network.client TRUE + network.server FALSE | PASS |
| 3 | NEGATIVE: no sandbox-escape keys | PASS |
| 4 | CODE_SIGN_ENTITLEMENTS: Human.entitlements | PASS |
| 5 | PRODUCT_BUNDLE_IDENTIFIER: ai.human.mac | PASS |
| 6 | Appfile app_identifier(['ai.human.mac', 'ai.human.ios']) | PASS |
| 7 | mac_archive + ios_archive lanes in Fastfile | PASS |
| 8 | CI entitlements-check job, negative-grep, no continue-on-error | PASS |

RESULT_verifier=PASS story=US-14.6
