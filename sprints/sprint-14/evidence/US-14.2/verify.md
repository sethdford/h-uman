# Verification Evidence — US-14.2 (Notarize DMG)

Commit: e652e714  Branch: impl/US-14.2  Date: 2026-05-17  Verifier: claude-sonnet-4-6

Verified 7/7 behaviors. 0 failed. 0 inconclusive.

---

## BEHAVIOR 1: bash -n syntax check passes

COMMAND: bash -n scripts/notarize-mac.sh
EXIT: 0
EVIDENCE:
  (no output — clean parse)
RESULT: PASS

---

## BEHAVIOR 2: set -euo pipefail present + EXIT/INT/TERM trap

COMMAND: grep -n 'set -euo pipefail\|trap ' scripts/notarize-mac.sh
EXIT: 0
EVIDENCE:
  28:set -euo pipefail
  56:trap cleanup EXIT INT TERM
RESULT: PASS

---

## BEHAVIOR 3: test-notarize-dryrun.sh exits 0

COMMAND: bash scripts/test-notarize-dryrun.sh
EXIT: 0
EVIDENCE:
  test-notarize-dryrun: OK — dry-run completed without invoking xcrun submit/staple/log
RESULT: PASS

---

## BEHAVIOR 4: --dry-run path invokes ZERO xcrun submit/staple/log calls

COMMAND: bash scripts/test-notarize-dryrun.sh (test script reports zero xcrun invocations)
EXIT: 0
EVIDENCE:
  test-notarize-dryrun: OK — dry-run completed without invoking xcrun submit/staple/log
  (Test harness is the authoritative zero-xcrun assertion)
RESULT: PASS

---

## BEHAVIOR 5: Credential block has set +x brackets

COMMAND: grep -n 'set -x\|set +x' scripts/notarize-mac.sh
EXIT: 0
EVIDENCE:
  20:#     touches the decoded key is bracketed with `set +x` and re-enables
  21:#     `set -x` only AFTER the credential has gone out of scope (file
  99:    EXIT trap. The credential-handling block runs with set +x. Never echo
  227:# Bracketed with `set +x` so the decoded key bytes can never be xtrace'd into
  228:# CI logs even if a future maintainer forgets and adds `set -x` at top.
  234:# === BEGIN CREDENTIAL BLOCK — DO NOT ADD echo/cat/set -x IN THIS BLOCK ===
  235:{ set +x; } 2>/dev/null

  Note: no set -x re-enable is present because the script never enables set -x
  (line 28 is set -euo pipefail only). The { set +x; } 2>/dev/null pattern
  suppresses even the set+x command itself from external xtrace callers.
RESULT: PASS

---

## BEHAVIOR 6: No literal API key strings

COMMAND: grep -rE "(BEGIN PRIVATE KEY|sk-[a-zA-Z0-9]{20,})" scripts/notarize-mac.sh tests/fixtures/notarize/
EXIT: 1 (grep exit 1 = no matches = PASS)
EVIDENCE:
  (no output — zero matches)
RESULT: PASS

---

## BEHAVIOR 7: CI notarize-dmg gated on release event + tag

COMMAND: grep context from .github/workflows/native-apps-fleet.yml lines 263-276
EXIT: 0
EVIDENCE:
  263:  notarize-dmg:
  271:    if: ${{ github.event_name == 'release' && startsWith(github.ref, 'refs/tags/v') }}

  notarize-dmg-dryrun-smoke job (line 250) has no if: condition — runs on
  every workflow trigger including PRs. No secrets mapped into that job.
RESULT: PASS

---

## Summary

| # | Behavior | Result |
|---|----------|--------|
| 1 | bash -n syntax check | PASS |
| 2 | set -euo pipefail + EXIT/INT/TERM trap | PASS |
| 3 | test-notarize-dryrun.sh exits 0 | PASS |
| 4 | --dry-run: zero xcrun submit/staple/log calls | PASS |
| 5 | Credential block has set +x brackets | PASS |
| 6 | No literal API key strings | PASS |
| 7 | notarize-dmg gated release-tag only; dryrun-smoke runs on PRs | PASS |

RESULT_verifier=PASS
