# Critic findings — US-14.1 (macOS XcodeGen scaffold + archive CI + cert import)

branch: impl/US-14.1
commit: 6b16d8ce
critic: claude-sonnet-4-6
date: 2026-05-17

---

## CRITICAL (0)

None.

---

## HIGH (2)

- `scripts/ci/import-macos-cert.sh:69` — **Unquoted `$ORIGINAL_LIST` expansion allows word-splitting on keychain paths with spaces.** `security list-keychains -d user -s "$KEYCHAIN_PATH" $ORIGINAL_LIST` — if any existing keychain path contains a space (e.g. `/Users/runner/Library/Keychains/login keychain-db`) the unquoted expansion splits it into two tokens, causing `security list-keychains` to receive malformed arguments. On GitHub-hosted macos-14 runners the login keychain path contains no spaces today, but this is a silent correctness assumption with no enforcement. Fix: quote the expansion or use an array: `ORIGINAL_LIST=(); while IFS= read -r line; do ORIGINAL_LIST+=("$(echo "$line" | tr -d '"')"); done < <(security list-keychains -d user); security list-keychains -d user -s "$KEYCHAIN_PATH" "${ORIGINAL_LIST[@]}"`.

- `.github/workflows/native-apps-fleet.yml:207` — **`CODE_SIGN_IDENTITY="Apple Development"` is ambiguous when the org keychain holds multiple Apple Development certs.** The design acknowledges this in Risk 1 but only documents using the generic name to avoid SHA-1 drift; it does not address the multi-cert ambiguity. When an org has certificates for multiple developers (`Apple Development: Alice (AAAA)` and `Apple Development: Bob (BBBB)`), `xcodebuild CODE_SIGN_IDENTITY="Apple Development"` picks the first match in the keychain — which is non-deterministic across runner invocations and cert rotations. The import script only validates that _some_ "Apple Development" identity is present (line 87), not that exactly one is. Fix: after `import-macos-cert.sh` runs, emit the full certificate CN to a step output (`CERT_CN`) and change `CODE_SIGN_IDENTITY` to consume that value (the full common name, e.g. `"Apple Development: Human CI Bot (XXXX)"`). The import script can extract it via `security find-identity -p codesigning -v "$KEYCHAIN_PATH" | grep -m1 "Apple Development" | sed 's/.*"\(.*\)"/\1/'`.

---

## MED (3)

- `scripts/ci/import-macos-cert.sh:51` — **EXIT trap only deletes the `.p12`; the temp directory `$WORKDIR` is never removed.** `cleanup_p12` calls `rm -f "$P12_PATH"` but never `rm -rf "$WORKDIR"`. On a self-hosted runner (or any retained runner) this leaks a `mktemp -d` directory per run. The comment at line 19 acknowledges self-hosted runner risk for the keychain, but misses the temp directory. Fix: `trap 'rm -f "$P12_PATH"; rm -rf "$WORKDIR"' EXIT`.

- `.github/workflows/native-apps-fleet.yml:180` — **`brew install xcodegen` without a version pin.** `brew list --versions xcodegen >/dev/null 2>&1 || brew install xcodegen` installs whatever version Homebrew serves at that moment. A xcodegen major version bump that drops or renames `bundleIdPrefix` or changes the `packages` schema will silently break the `xcodegen generate` step on new runners while the Homebrew cache on existing runners stays green. This creates a split-brain failure mode that is hard to diagnose. Fix: pin the version (`brew install xcodegen@X.Y`) or use a `Brewfile` with an explicit version lock that is committed to the repo.

- `.github/workflows/native-apps-fleet.yml:215` — **Fork PR unsigned-smoke path is conditioned on `steps.import-cert.outcome != 'success'`, which also fires when the import step fails for reasons other than missing secrets (e.g. malformed P12, network timeout on Homebrew, `security` command failure).** In that scenario the unsigned smoke runs and the job passes green even though the _signed_ path broke due to an infrastructure error, not an intentional fork PR. The fleet-sota-gate (line 289) then marks `macos-app-archive` as success, silently masking the cert-import failure. Fix: distinguish the "skipped due to missing secrets" case from "failed" by checking `steps.import-cert.outcome == 'skipped'` for the unsigned path, not `!= 'success'`. When `outcome == 'failure'` the job should hard-fail, not fall through to unsigned smoke.

---

## LOW (1)

- `scripts/ci/import-macos-cert.sh:93` — **`printf 'KEYCHAIN_PATH=%s\n'` writes to stdout but no caller captures or exports it.** The CI step that runs `import-macos-cert.sh` does not parse stdout into `$GITHUB_OUTPUT` or `$GITHUB_ENV`, so `KEYCHAIN_PATH` is printed to the log and discarded. The subsequent `xcodebuild` step does not reference it. The keychain is located by `security default-keychain`, not by this variable. The output is therefore dead. Either remove the `printf` line and only use `err` (which goes to stderr where CI captures it as log output), or wire it properly: `echo "KEYCHAIN_PATH=$KEYCHAIN_PATH" >> "$GITHUB_OUTPUT"` and consume it from the archive step.

---

## Cross-agent regression risk

None. This change adds files only (`apps/macos/project.yml`, `scripts/ci/import-macos-cert.sh`, new job in `native-apps-fleet.yml`). No existing C source, test, or shared workflow file is modified. No other in-flight worktree was found to touch the same files.

---

RESULT_critic=HAS_FINDINGS_0_2
