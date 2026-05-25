# Design for US-C1.5: GitHub Actions macOS Release Workflow

## Approach

US-C1.5 implements `.github/workflows/release-macos.yml` — a GitHub Actions workflow that automates the build, sign, notarize, and release pipeline for macOS .pkg artifacts. The design splits signing into two paths to balance safety and development velocity:

1. **Push to main (continuous):** Build + package without signing. Unsigned artifact is published as a pre-release asset. Allows rapid iteration without Apple notary queue delays or cost.
2. **Push to v* tags (releases):** Build + package + sign + notarize + staple + release as a full release. Signed artifact ready for distribution.
3. **Workflow dispatch (manual):** Developers can trigger either path for testing before landing real tags.

This split respects the constraint that **every signing submission costs time (notary queue ~5-15 min) and money (Apple App Store Connect API quota)**. A developer who pushes to main 10 times shouldn't incur 10 notarization submissions; only maintainers pushing tags should.

The workflow uses **macos-14-arm64** (GitHub-hosted ARM runner, M-series macOS) to build native ARM64 binaries matching production hardware. If ARM runners become unavailable, fallback is documented but not auto-detected — maintainer chooses fallback consciously.

Secrets (Apple certs, API keys) are stored as GitHub Actions secrets and injected into a temporary keychain that is destroyed at workflow exit. This is the standard pattern for macOS CI; documented failure modes include cert import errors and partition-list omissions.

Test strategy: `scripts/agent-preflight.sh` runs on every build, catching regressions before signing. A new `test_release_workflow.sh` validates the YAML syntax without running it. Real signing is tested only on tags.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `.github/workflows/release-macos.yml` | New workflow file (build + package + sign + notarize + release) | +180 |
| `.github/actions/setup-build` | (already exists; no change) | 0 |
| `scripts/release/sign-and-notarize.sh` | New script stub; actual implementation in US-C1.3 | +20 |
| `scripts/release/diagnose-notary.sh` | New script stub; actual implementation in US-C1.3a | +10 |
| `tests/test_release_workflow.sh` | Shell script test for YAML validity | +100 |
| `.github/workflows/ci.yml` | (existing; no change for US-C1.5) | 0 |

## Implementation steps (for the implementer agent)

1. Create `.github/workflows/release-macos.yml` with the full skeleton:
   - Trigger: on push to main, push to v* tags, workflow_dispatch
   - Job: build release binary on macos-14-arm64
   - Job: (conditional on tags) sign + notarize + release
   - Concurrency control: no cancellation during signing
   - Artifact handling: upload to GitHub Releases on tag push

2. Add secret documentation (no live implementation; just comments):
   - APPLE_DEVELOPER_ID_APPLICATION_CERT (base64-encoded .p12)
   - APPLE_DEVELOPER_ID_INSTALLER_CERT (base64-encoded .p12)
   - APPLE_DEVELOPER_ID_CERT_PASSWORD (string)
   - APPLE_API_KEY_ID, APPLE_API_KEY_ISSUER, APPLE_API_KEY_BASE64 (for xcrun notarytool)

3. Wire keychain bootstrap step (calls `security create-keychain` → import certs → set partition list)

4. Add preflight check: run `scripts/agent-preflight.sh` before any signing to catch code changes that broke tests

5. Create stub scripts `scripts/release/sign-and-notarize.sh` and `scripts/release/diagnose-notary.sh`
   - Stubs exit with "not implemented" messages
   - Actual implementations happen in US-C1.3 and US-C1.3a

6. Create `tests/test_release_workflow.sh`:
   - Validate YAML syntax using `yq` (parse JSON, verify required keys exist)
   - Verify no secrets are echoed in any job step
   - Check trigger conditions are correct

7. Test on main: push a dummy commit, verify workflow runs, build succeeds, artifact is unsigned, published as pre-release

## Risks

- **ARM runner availability (LOW/MEDIUM):** GitHub's macos-14-arm64 runners are GA but occasionally oversubscribed. If unavailable, fallback to macos-13-arm64 (slightly older) or macos-14 (Intel, requires cross-compile). Mitigation: document fallback explicitly in comments; don't auto-fallback (maintainer decides).

- **Secrets in logs (MEDIUM/LARGE):** If the workflow echoes `$APPLE_DEVELOPER_ID_CERT_PASSWORD` or API keys, they're leaked in the run log. Mitigation: use `add-mask` for all secrets, never `echo` them, and test this explicitly in `test_release_workflow.sh` (grep run steps for secret variable names).

- **Keychain bootstrap failures (HIGH/MEDIUM):** Cert import can fail if password is wrong, cert is malformed, or partition-list is missing. Mitigation: document the 3-step bootstrap (create → import → partition), include troubleshooting in comments, and have a clear error message if any step fails.

- **Notarization timeout (MEDIUM/SMALL):** Apple's notary service can be slow; 15-minute timeout may expire. Mitigation: set timeout to 20 minutes (published docs recommend 15–20), retry on timeout, and capture the Apple log for diagnosis via `xcrun notarytool log --id <request-id>`.

- **Backward compat (LOW/SMALL):** Existing main pushes (before this workflow ships) produce no unsigned .pkg asset. After the workflow lands, every main push produces one. Documentation and users should expect this change. Mitigation: release notes on the main deployment note "new: unsigned builds on main".

- **Fork PR restriction (HIGH/LARGE):** GitHub Actions secrets are NOT available in pull requests from forks (security feature). Developers cannot test the full signing path on forks. Mitigation: document this clearly; maintainers test signing on branches within the main repo before merging.

- **Observer visibility (MEDIUM):** If signing fails silently or Apple notarization hangs, the run log is the only diagnostic. Mitigation: emit structured logs at each step, capture `xcrun notarytool` JSON output, upload notary diagnostic logs as workflow artifacts on failure.

## Test strategy

1. **YAML validation (every PR):** `test_release_workflow.sh` parses `.github/workflows/release-macos.yml` and verifies:
   - No secrets echoed in any step
   - Trigger conditions are correct (main, v* tags, workflow_dispatch)
   - Required jobs exist (build, sign)
   - Conditional logic for sign job is correct

2. **Build-only test (every push to main):** Workflow runs, builds release binary, creates unsigned .pkg, uploads as pre-release.

3. **Full signing test (on v* tags):** Maintainer pushes a test tag `v0.0.0-test` and verifies:
   - Signing succeeds (or gracefully skips if cert env vars not set)
   - Notarization submits
   - Workflow artifact contains notary response log
   - Release asset is published with correct filename

4. **Dry-run for new developers:** `scripts/agent-preflight.sh` can be run locally to catch failures before pushing to main.

5. **Escape hatch for testing workflow changes:** Maintainer can use `workflow_dispatch` to manually trigger the workflow without pushing a real tag, testing signing path without publishing a release.

## Acceptance criteria mapping

- **AC-C1.5.1 (Trigger on push to main)** → Workflow file line `on: push: branches: [main]` + test in `test_release_workflow.sh` verifies trigger exists
- **AC-C1.5.2 (Build release binary)** → Job `build` runs `cmake --preset release`, produces `build/Release/Human.app`
- **AC-C1.5.3 (Skip signing if secret missing)** → Job `sign` is conditional on `if: github.ref_type == 'tag'`, echoes clear message if secret not set
- **AC-C1.5.4 (Upload to GitHub Releases)** → Job `release` uses `softprops/action-gh-release`, marks pre-release if not tagged
- **AC-C1.5.5 (Preflight check)** → Step in `build` job runs `./scripts/agent-preflight.sh; test $? -eq 0` before packaging
- **AC-C1.5.6 (YAML test)** → `test_release_workflow.sh` validates syntax

## Out of scope for US-C1.5

- **Actual signing implementation** → US-C1.3 (sign-and-notarize.sh)
- **Notary diagnostics** → US-C1.3a (diagnose-notary.sh)
- **Homebrew formula wiring** → US-C1.4
- **Installation guide** → US-C1.6
- **App bundle structure** → US-C1.1
- **Pkg build script** → US-C1.2

US-C1.5 assumes those stories land first; the workflow wires them together.

## Secret setup instructions (for maintainer, not implementer)

Before this workflow is active, maintainer must:

1. **Export Developer ID certs as base64:**
   ```bash
   # Export from Keychain (local cert)
   security export-cert -k ~/Library/Keychains/login.keychain-db \
     -t certs -f pkcs12 -P <cert-password> \
     -o /tmp/cert.p12
   base64 -i /tmp/cert.p12 | pbcopy
   ```

2. **Create GitHub secret APPLE_DEVELOPER_ID_APPLICATION_CERT** with the base64 string

3. **Create GitHub secret APPLE_DEVELOPER_ID_INSTALLER_CERT** (if packaging uses separate installer cert)

4. **Create GitHub secret APPLE_DEVELOPER_ID_CERT_PASSWORD** with the .p12 password

5. **Create GitHub secret APPLE_API_KEY_ID, APPLE_API_KEY_ISSUER, APPLE_API_KEY_BASE64** from App Store Connect (Integrations → App Store Connect API → generate key)

These steps are manual; documentation will be added to CONTRIBUTING.md or a release-runbook doc.

## Failure mode playbook

| Symptom | Likely cause | Fix |
|---------|---|---|
| `security import` fails with "invalid keychain format" | Cert password is wrong | Check secret value; re-export cert with correct password |
| `codesign` fails with "Malformed entitlements" | Entitlements file syntax error | Run `scripts/release/diagnose-notary.sh` on Apple's rejection log |
| Notary submission hangs for >20 min | Apple service slow or quota hit | Manually check `xcrun notarytool history --issuer XXXX` for stuck submission; wait or contact Apple |
| Workflow secret not available on fork PR | GitHub security feature | Document that signing only works on internal branches; use `workflow_dispatch` on main repo for testing |
| Release asset filename is wrong | `github.ref_name` not set correctly | Check trigger event; `v*` tags populate `github.ref_name` correctly |

---

## Summary

- **Minimal viable workflow:** Build → package → (conditionally) sign → release, gated by trigger event
- **Safe for iteration:** Main pushes produce unsigned artifacts; only tags trigger full signing pipeline
- **Observable failures:** Structured logs, artifact uploads on failure, clear error messages for missing secrets
- **Documented escape hatches:** ARM runner fallback, manual `workflow_dispatch`, dry-run via preflight
- **Depends on:** US-C1.2 (build-pkg.sh), US-C1.3 (sign-and-notarize.sh), US-C1.3a (diagnose-notary.sh)
- **Test strategy:** YAML validation every PR, unsigned build every main push, full signing test on v* tags
