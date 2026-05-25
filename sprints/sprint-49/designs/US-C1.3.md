# Design for US-C1.3: Code-Signing and Notarization for macOS

## Approach

US-C1.3 implements `scripts/release/sign-and-notarize.sh`, a script that takes an unsigned .pkg (from US-C1.2) and applies Apple's code-signing and notarization workflow to produce a distributable artifact that passes macOS Gatekeeper. The design chains three stages: (1) codesign the bundle pre-package, (2) productsign the .pkg, (3) notarize via xcrun notarytool, (4) staple the notarization ticket.

The key insight: signing happens at two levels — the binary/bundle (code signature embedded in mach-o headers and _CodeSignature dir), and the .pkg itself (productsign wrapper). Notarization is asynchronous; the script polls or blocks with --wait.

This design differs from older altool-based approaches: Apple deprecated altool in 2023. We use xcrun notarytool (current standard) which requires Keychain credentials (NOTARY_PROFILE). Failure diagnostics are handed to US-C1.3a's translator script.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `scripts/release/sign-and-notarize.sh` | New script: codesign, productsign, xcrun notarytool submit/wait, xcrun stapler | 250 |
| `scripts/release/entitlements.plist` | New: minimal entitlements (empty or near-empty; no keychain/hardware) | 20 |
| `tests/test_sign_and_notarize.c` | New: mock codesign/xcrun calls, verify command sequences, env-var checks | 120 |
| `CMakeLists.txt` | Add entitlements.plist to install target (copy to release-build output) | 5 |

## Implementation steps (for the implementer agent)

1. Create entitlements.plist with minimal contents (empty codesign requirements; no keychain/HID).
2. Add entitlements.plist copy to CMakeLists.txt release build.
3. Stub sign-and-notarize.sh with argument parsing + env-var validation.
4. Add codesign and productsign stages; test with --dry-run.
5. Add xcrun notarytool submit + --wait polling logic.
6. On success: staple. On timeout/rejection: capture submission-id and log path for US-C1.3a.
7. Implement test mocks for all Apple tool invocations.
8. Run agent-preflight.sh; verify build/test/daemon start.

## Risks

- **Keychain unlock prompts on CI**: xcrun notarytool needs keychain access (NOTARY_PROFILE stored via `xcrun notarytool store-credentials`). On GitHub Actions runners without the cert, the script must exit cleanly with a message ("Signing requires NOTARY_PROFILE credential; run \`xcrun notarytool store-credentials\` locally first"). Mitigation: check NOTARY_PROFILE env var up-front; fail fast.
- **Cert expiry (annual)**: Developer ID Application cert expires yearly. Error handling: if codesign reports "certificate expired", script should suggest renewal. Mitigation: log the cert expiry date at first signing success (one-time per process).
- **Apple notarization service unavailable**: xcrun notarytool may fail transiently. Spec says 15-min timeout; script should retry with exponential backoff (1s, 2s, 4s, 8s, cap at 60s per retry, max 3 retries) if submission itself fails; once submitted, --wait is atomic. Mitigation: document timeout behavior in comments.
- **Malware false-positives**: Apple's scanner occasionally flags legitimate code. Error log will include "malware detected: X"; US-C1.3a translator will suggest resubmitting or filing an appeal. Mitigation: US-C1.3a is the owner of translating this; this story captures the rejection log.
- **Notarization log access**: if notarization fails, we must fetch the detailed log to diagnose. Command: `xcrun notarytool log <submission-id> --keychain-profile <NOTARY_PROFILE>` returns JSON with detailed issue list. Mitigation: save submission-id and log to `<output-dir>/notarization-log-<submission-id>.json` for US-C1.3a to read.

## Secrets and environment variables

Script must validate these env vars at startup and fail with actionable error:

| Var | Sample value | Role |
|---|---|---|
| `APPLE_DEV_ID` | `Developer ID Application: Human, Inc. (ABC1D23E4F)` | codesign --sign value |
| `APPLE_DEV_ID_INSTALLER` | `Developer ID Installer: Human, Inc. (ABC1D23E4F)` | productsign --sign value |
| `NOTARY_PROFILE` | `human-notary` (keychain profile name from `xcrun notarytool store-credentials human-notary --apple-id <email> --password <app-password>`) | notarytool --keychain-profile value |
| `APPLE_TEAM_ID` | `ABC1D23E4F` | Optional; used in error messages for cert lookup |

Behavior on missing secret:
- `APPLE_DEV_ID` missing → `echo "APPLE_DEV_ID not set; code-signing skipped. Set to \"Developer ID Application: ...\" to enable"; exit 0`
- `NOTARY_PROFILE` missing → `echo "NOTARY_PROFILE not set; notarization skipped. Run 'xcrun notarytool store-credentials' locally first"; exit 0`

This allows CI to run without secrets (gracefully skip signing); local machines with certs will sign automatically.

## Signing stages

### Stage 1: codesign the binary

Before packaging, sign the daemon binary (or re-sign the already-packaged bundle):

```bash
codesign \
  --sign "$APPLE_DEV_ID" \
  --options runtime \
  --timestamp \
  --entitlements "$SCRIPT_DIR/entitlements.plist" \
  --verbose=4 \
  "$BUNDLE_PATH/Contents/MacOS/human"
```

Why `--options runtime`:
- Enables hardened runtime; required for notarization.
- Hardens the binary against code-injection attacks (DEP, ASLR enforcement).
- Without it, Apple's notarization service rejects the submission.

Why `--timestamp`:
- Attaches a timestamp from Apple's timestamp server.
- Allows the signature to remain valid after the cert expires (signature was valid AT signing time, even if cert is now expired).
- Required for notarization; codesign will fail with "timestamp required" error if omitted.

Why `--entitlements`:
- entitlements.plist defines capabilities (keychain access, hardware I/O, etc.).
- For human: use a minimal plist with no sensitive entitlements (no com.apple.security.keychain-access-groups, no com.apple.security.device.usb, etc.).
- An empty entitlements.plist is valid: `<?xml version="1.0"?><plist...><dict></dict></plist>`.

### Stage 2: productsign the .pkg

After pkgbuild (from US-C1.2), sign the .pkg itself:

```bash
productsign \
  --sign "$APPLE_DEV_ID_INSTALLER" \
  --timestamp \
  unsigned.pkg \
  signed.pkg
```

Why separate cert (DEVELOPER_ID_INSTALLER vs DEVELOPER_ID_APPLICATION):
- Apple requires two distinct certs: one for app code (APPLICATION), one for .pkg distribution (INSTALLER).
- Both are Developer ID certs but serve different purposes.
- If only one cert is available, productsign will fail with "certificate not appropriate for signing packages."

Why `--timestamp`:
- Same reason as codesign: allows offline timestamp validation after cert expiry.

### Stage 3: Notarization

Submit the signed .pkg to Apple for malware scanning and staple:

```bash
xcrun notarytool submit signed.pkg \
  --keychain-profile "$NOTARY_PROFILE" \
  --wait \
  --timeout 900  # 15 minutes in seconds
```

Behavior:
- `--wait` blocks until notarization completes (success or failure), polling every 5-10 seconds.
- Timeout: if Apple's service is slow, the submission may exceed 15 minutes. Script should handle timeout gracefully (capture submission-id, fetch log async for US-C1.3a).
- On success, notarytool returns `0` and prints submission-id.
- On failure, notarytool returns non-zero and prints an error. Script must:
  1. Capture the submission-id (parse from xcrun output or query the notarization service).
  2. Fetch the detailed rejection log: `xcrun notarytool log <submission-id> --keychain-profile <NOTARY_PROFILE> > notarization-log-<submission-id>.json`.
  3. Exit with non-zero; optionally call `scripts/release/diagnose-notary.sh` (US-C1.3a) to translate the log.

### Stage 4: Stapling

If notarization succeeded, attach the ticket to the .pkg:

```bash
xcrun stapler staple signed.pkg
```

Why stapling:
- The notarization ticket is a small certificate that Gatekeeper checks offline (when the user's machine is not connected to the internet, or when Apple's servers are unavailable).
- After stapling, Gatekeeper will use the embedded ticket instead of contacting Apple's OCSP server.
- Without stapling, Gatekeeper will reach out to the internet the first time the user installs; if Apple's servers are down or the user is offline, Gatekeeper will reject the install.

## Verification post-sign

Script should include a --verify flag that runs the full signature chain:

```bash
codesign --verify --deep --strict --verbose=4 "$BUNDLE_PATH"
spctl --assess --type install --verbose "$SIGNED_PKG"
xcrun stapler validate "$SIGNED_PKG"
```

Each tool catches different issues:
- `codesign --verify --strict`: syntactic signature validity + entitlements match.
- `spctl`: policy enforcement (does this signature satisfy the current Gatekeeper policy?).
- `stapler validate`: is the notarization ticket properly embedded?

## Failure mode handoff (US-C1.3a integration)

When notarization fails:

1. Script captures the submission-id and rejection log:
   ```bash
   # After xcrun notarytool submit returns non-zero
   SUBMISSION_ID=$(xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" --json | jq -r '.[0].id')
   xcrun notarytool log "$SUBMISSION_ID" --keychain-profile "$NOTARY_PROFILE" > "notarization-log-${SUBMISSION_ID}.json"
   ```

2. Script prints the path to the log:
   ```
   Notarization failed. Rejection log saved to: notarization-log-<submission-id>.json
   Run: scripts/release/diagnose-notary.sh --log notarization-log-<submission-id>.json
   ```

3. US-C1.3a's diagnose-notary.sh reads the JSON and translates common failures into actionable fixes.

## Test strategy

`tests/test_sign_and_notarize.c`:

1. **Mock codesign, productsign, xcrun**: Use HU_IS_TEST guards to mock tool invocations (build a fake process output).
2. **Env-var validation tests**: 
   - APPLE_DEV_ID missing → script exits 0 with "skipped" message
   - NOTARY_PROFILE missing → script exits 0 with "skipped" message
   - Both present → script proceeds
3. **Command sequence tests**:
   - codesign called with correct --sign, --options runtime, --timestamp, --entitlements
   - productsign called with correct --sign and --timestamp
   - xcrun notarytool submit called with --wait and correct --keychain-profile
4. **Failure handling tests**:
   - Notarization times out → script captures submission-id and logs path
   - Notarization rejected → script fetches rejection log and prints path
5. **Integration smoke test** (macOS CI only):
   - Build release binary (cmake --preset release)
   - Run sign-and-notarize.sh --dry-run on the .pkg
   - Verify no crash, no ASan errors

## Acceptance criteria mapping

- **AC-C1.3.1** → env-var validation tests; script reads APPLE_DEV_ID, NOTARY_PROFILE, etc. and fails fast if missing (on CI, gracefully skips; locally, user provides them)
- **AC-C1.3.2** → codesign stage tests; entitlements.plist exists and contains no sensitive entries
- **AC-C1.3.3** → notarytool submit + --wait tests; timeout is 15 minutes (900 seconds)
- **AC-C1.3.4** → stapler staple tests; called after notarytool succeeds
- **AC-C1.3.5** → failure log capture tests; submission-id and rejection log saved to notarization-log-<id>.json; diagnose-notary.sh called or path printed
- **AC-C1.3.6** → test mocks all xcrun/codesign calls; integration test on macOS CI with --dry-run flag

## Out of scope

- **Sandboxing entitlements**: AC-C1.3.2 specifies "no sensitive entitlements"; sandboxing (com.apple.security.app-sandbox) is explicitly excluded from Sprint C. Entitlements file is minimal (empty).
- **Hardware/keychain access**: No USB, no iCloud Keychain, no local keychain access entitlements. (Future sprints may add these if human gains hardware integrations.)
- **Notarization retry logic**: Script attempts submit once; if it fails, user re-runs manually. Exponential backoff is mentioned in risk analysis but not implemented in scope (can be added post-sprint if Apple service reliability becomes a concern).
- **Certificate renewal**: Script logs cert expiry date; human intervention required to renew. (No auto-renewal in scope.)

## Risk summary

| Risk | Prob | Impact | Mitigation |
|---|---|---|---|
| Keychain unlock prompt on CI | Medium | Medium | Check NOTARY_PROFILE early; exit gracefully if missing. Document in CI setup that local machines must run `xcrun notarytool store-credentials` once. |
| Notarization timeout (>15 min) | Low | Medium | --wait with 15-min timeout; capture submission-id and log path on timeout; user can check status manually or re-submit. |
| Cert expired | Low | Large | Script logs cert expiry date at first sign. User must renew cert (annual task); CI will fail with clear error if cert expired. |
| Malware false-positive | Very Low | Large | Apple can be appealed; US-C1.3a translator script will flag this in diagnosis. User can file appeal with Apple. |
| Apple deprecates notarytool | Very Low | Large | Notarytool is current standard (replaced altool in 2023); unlikely to change in next 2 years. If it does, script needs complete rewrite. |

