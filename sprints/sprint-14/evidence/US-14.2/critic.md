# Critic findings — US-14.2 (Notarize DMG + stapler + dry-run smoke)

Commit: e652e714  Branch: impl/US-14.2  Critic: claude-sonnet-4-6  Date: 2026-05-17

## HIGH (2)

- `scripts/notarize-mac.sh:280-288` — `notarytool submit` exit code 2 collapses
  Apple content-rejection AND network/auth failure into the same bucket. When
  `xcrun notarytool submit` exits non-zero (transport/auth), the script exits 2.
  When it exits 0 but `STATUS != "Accepted"`, the script also exits 2. Operators
  (and CI retry logic) cannot distinguish "Apple rejected your binary" (don't
  retry — fix the binary) from "notarytool quota exhausted / network blip"
  (safe to retry). The design doc acknowledges this at the Observability risk
  item and says "exit code distinguishes submission failed (2) from staple failed
  (3)" but never separates the two submission failure modes.
  Fix: add exit code 5 (or reuse 2 for transport/auth, introduce 6 for
  Apple rejection) so callers can branch. At minimum, the error message on
  the non-Accepted path must print the raw status value — it does, but the
  exit code conflation leaves automated callers blind.

- `scripts/notarize-mac.sh:295-302` + design `US-14.2.md:145-148` — stapler
  failure after successful notarytool submit leaves the DMG in a limbo state:
  Apple has recorded the notarization, the artifact exists on disk, but the
  ticket is not stapled to it. The script exits 3 with no offline-use warning
  and the CI "Upload notarized DMG" step uses `if: always()` (line 303 of
  workflow), meaning an un-stapled DMG gets uploaded to the release artifact
  store and may be distributed. The design acknowledges "retry once before
  erroring" but the implementation has zero retry; the design's own stapler-
  retry mitigation was not implemented.
  Fix: add at least one retry with a short sleep (staple CDN propagation can
  lag 10-30 s); add an explicit `if: success()` guard (or remove `always()`)
  on the artifact upload step so an exit-3 script guarantees no artifact upload.

## MED (2)

- `scripts/notarize-mac.sh:310` — `spctl --assess --type open` is the correct
  type for a DMG. The dry-run banner (line 219) and the real invocation (line
  310) both use `--type open`, which is correct for DMGs on macOS 13+. However,
  the design doc step 7 (US-14.2.md:87) specifies `--type execute` applied to
  the mounted `.app`, not `--type open` on the DMG. The implementation diverges
  from the design silently: it dropped the mount-and-assess-app step entirely
  and instead assesses the DMG directly with `--type open`. `--type open` on a
  DMG will pass if the *ticket* is present but will not catch a Gatekeeper
  policy that blocks the inner `.app` at execution time (e.g., missing
  notarization of a bundled dylib). The acceptance criteria in the design doc
  (US-14.2.md:183) still references `--type execute --verbose Human.app`.
  The implementation satisfies the weaker check, not the stronger one the
  design mandated.
  Fix: add the mount + `spctl --assess --type execute` on the inner app as a
  second gate after the DMG-level check, matching the design's AC-14.2.3
  intent; or update the design to explicitly retract the `--type execute`
  requirement and document why `--type open` on the DMG is sufficient.

- `scripts/notarize-mac.sh:111` + `.github/workflows/native-apps-fleet.yml:297-300`
  — `ENABLE_HARDENED_RUNTIME` defaults to 0 and the CI job does not pass
  `--enable-hardened-runtime`. Apple's notarytool requires hardened runtime for
  macOS 10.15+ apps; submitting without it produces a rejection with
  `ITMS-90338` (or equivalent). The script's opt-in default means the CI job
  ships a non-hardened binary to Apple's notarization service, which will reject
  it. The design doc marks this "opt-in" without noting Apple's requirement.
  Fix: either flip the default to 1 (hardened on unless the caller opts out) or
  add a guard in the script that warns/fails when real-mode is entered without
  `--enable-hardened-runtime` and `APP_STORE_CONNECT_*` are set.

## LOW (1)

- `scripts/notarize-mac.sh:239` — `KEY_DIR` falls back to `/tmp` when both
  `RUNNER_TEMP` and `TMPDIR` are unset. On macOS, `TMPDIR` is always set by
  launchd; on Linux CI without `RUNNER_TEMP`, `/tmp` is world-readable by
  default (sticky-bit, not mode-restricted). The `umask 077` at creation time
  sets correct permissions on the file, but `/tmp` itself is accessible to
  other users on shared runners (e.g., hosted macOS-14 runners that reuse the
  same VM). `RUNNER_TEMP` is the correct location for GitHub-hosted runners and
  is always set; the fallback to `/tmp` is safe only on single-user machines.
  Informational — not exploitable given umask 077 on the file itself, but the
  comment in the security contract ("$RUNNER_TEMP (or $TMPDIR / /tmp fallback)")
  understates the risk on shared runners.

## Cross-agent regression risk

- None. `scripts/notarize-mac.sh` and `scripts/test-notarize-dryrun.sh` are new
  files with no callers in main. `native-apps-fleet.yml` is modified but the
  added jobs are append-only; no existing job depends on them.

---

RESULT_critic=HAS_FINDINGS story=US-14.2 severity=HIGH
