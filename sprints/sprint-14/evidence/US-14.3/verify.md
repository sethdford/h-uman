# US-14.3 Verification Evidence
Story: iOS archive + export  
Branch: impl/US-14.3  
Commit: e8efd689  
Verifier run: 2026-05-17  
Result: PASS (7/7 checks)

---

## Contract

1. `plutil -lint apps/ios/ExportOptions.plist` exits 0
2. ExportOptions.plist contains `__TEAM_ID__` and `__PROVISIONING_PROFILE_UUID__` placeholders; NO real UUID-shaped string
3. `bash scripts/check-no-provisioning-leak.sh` exits 0 on current state
4. Test-the-test: inject fake UUID -> leak guard exits 1; restore -> exits 0
5. `bash -n scripts/ios-archive-export.sh` clean; has `set -euo pipefail`; restores `.bak` on EXIT
6. `.githooks/pre-commit` exists and invokes the leak guard
7. CI job `ios-archive` gated by apps-path / schedule / workflow_dispatch (NOT every PR)

---

## Evidence

### Check 1: plutil -lint ExportOptions.plist

```
BEHAVIOR: ExportOptions.plist is valid XML plist
COMMAND: plutil -lint apps/ios/ExportOptions.plist
EXIT: 0
EVIDENCE:
  /Users/sethford/.../apps/ios/ExportOptions.plist: OK
RESULT: PASS
```

### Check 2: Placeholder tokens present, no real UUIDs

```
BEHAVIOR: ExportOptions.plist uses __TEAM_ID__ and __PROVISIONING_PROFILE_UUID__ only
COMMAND: cat apps/ios/ExportOptions.plist
EXIT: 0
EVIDENCE:
  Line 23: <string>__TEAM_ID__</string>
  Line 29: <string>__PROVISIONING_PROFILE_UUID__</string>
  Line 31: <string>__PROVISIONING_PROFILE_UUID__</string>
  No 36-char UUID pattern (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx) present anywhere.
  Comment block at top of file explicitly warns: "This file SHIPS WITH PLACEHOLDER TOKENS."
RESULT: PASS
```

### Check 3: Leak guard exits 0 on clean state

```
BEHAVIOR: check-no-provisioning-leak.sh exits 0 when placeholders intact
COMMAND: bash scripts/check-no-provisioning-leak.sh
EXIT: 0
EVIDENCE:
  check-no-provisioning-leak: OK (.../apps/ios/ExportOptions.plist clean — placeholders intact, no UUID, no team ID)
RESULT: PASS
```

### Check 4: Test-the-test (adversarial injection + restore)

```
BEHAVIOR: Leak guard exits 1 when real UUID injected; exits 0 after git restore
COMMAND (inject): sed -i.testbak 's/__PROVISIONING_PROFILE_UUID__/12345678-abcd-1234-5678-abcdef012345/g' apps/ios/ExportOptions.plist
COMMAND (guard):  bash scripts/check-no-provisioning-leak.sh
EXIT_INJECTED: 1
EVIDENCE (injected):
  grep found: 29: <string>12345678-abcd-1234-5678-abcdef012345</string>
                   31: <string>12345678-abcd-1234-5678-abcdef012345</string>
  check-no-provisioning-leak: placeholder __PROVISIONING_PROFILE_UUID__ MISSING from .../ExportOptions.plist
    -> placeholder must be present; the wrapper script substitutes it at runtime

COMMAND (restore): cp ExportOptions.plist.testbak ExportOptions.plist && bash scripts/check-no-provisioning-leak.sh
EXIT_RESTORED: 0
EVIDENCE (restored):
  check-no-provisioning-leak: OK (.../apps/ios/ExportOptions.plist clean — placeholders intact, no UUID, no team ID)
RESULT: PASS
```

### Check 5: ios-archive-export.sh syntax, pipefail, .bak restore

```
BEHAVIOR: Script is syntactically valid, has set -euo pipefail, restores .bak on EXIT
COMMAND (syntax): bash -n scripts/ios-archive-export.sh
EXIT: 0
EVIDENCE:
  No output (clean parse)

COMMAND (pipefail grep): grep -n "set -euo pipefail" scripts/ios-archive-export.sh
EVIDENCE:
  22:set -euo pipefail

COMMAND (EXIT trap grep): grep -n "trap\|cleanup\|\.bak\|EXIT" scripts/ios-archive-export.sh
EVIDENCE:
  26:EXPORT_OPTIONS_BAK="$EXPORT_OPTIONS_SRC.bak"
  34:cleanup() {
  35:    # Restore the committed ExportOptions.plist (with placeholders) from the .bak copy.
  41:trap cleanup EXIT INT TERM
RESULT: PASS
```

### Check 6: .githooks/pre-commit invokes leak guard

```
BEHAVIOR: pre-commit hook exists and invokes check-no-provisioning-leak.sh
FILE: .githooks/pre-commit
EVIDENCE:
  Lines 110-125 (verbatim):
    # US-14.3 (Sprint 14): block commits that introduce a real provisioning UUID or
    # Apple Team ID into apps/ios/ExportOptions.plist. The committed file must always
    # retain its placeholder tokens; ...
    staged_export_options=$(git diff --cached --name-only --diff-filter=ACM -- \
        'apps/ios/ExportOptions.plist')
    if [ -n "$staged_export_options" ] && [ -x "scripts/check-no-provisioning-leak.sh" ]; then
        if ! scripts/check-no-provisioning-leak.sh; then
            echo ""
            echo "Pre-commit blocked: apps/ios/ExportOptions.plist appears to contain"
            echo "a real provisioning UUID or Apple Team ID. ..."
            exit 1
        fi
    fi
RESULT: PASS
```

### Check 7: CI ios-archive job gating

```
BEHAVIOR: ios-archive job runs only on apps-path / schedule / workflow_dispatch, NOT on every PR
FILE: .github/workflows/native-apps-fleet.yml
EVIDENCE:
  on: block (lines 8-21):
    workflow_dispatch:        <- manual dispatch
    schedule:
      - cron: "0 11 * * 1"   <- weekly schedule
    pull_request:
      paths:
        - "apps/**"           <- ONLY when apps/** files change
        - ".github/workflows/native-apps-fleet.yml"
    push:
      branches:
        - main
      paths:
        - "apps/**"           <- ONLY main + apps/** path filter

  Job comment (lines 156-160):
    "Gating: the workflow itself only runs on apps-path changes, schedule, or manual
    dispatch (see `on:` block at the top of the file), so this job inherits that
    gate without needing a per-job `if:`."

  Additional hardening: CI step at line 167-168 re-runs the leak guard:
    "Verify ExportOptions.plist has no leaked provisioning identifiers"
    run: ./scripts/check-no-provisioning-leak.sh
  And at line 207-209 (if: always()) re-validates after archive+export.
RESULT: PASS
```

---

## Summary

Verified 7/7 behaviors. 0 failed. 0 inconclusive.

RESULT_verifier=PASS
