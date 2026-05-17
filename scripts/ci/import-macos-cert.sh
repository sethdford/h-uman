#!/usr/bin/env bash
# Import an Apple Development codesigning certificate (P12) into an ephemeral
# keychain for use by `xcodebuild ... archive` + `codesign` in CI.
#
# Required env (all are CI secrets; the script fails fast if any is empty):
#   MACOS_DEV_CERT_P12_BASE64   base64-encoded .p12
#   MACOS_DEV_CERT_P12_PWD      password protecting the .p12
#   MACOS_KEYCHAIN_PWD          password for the temporary keychain we create
#
# On success:
#   - A temp keychain (mktemp) is created, unlocked, and made the default.
#   - The cert is imported with /usr/bin/codesign allowed to use the private key
#     without a UI prompt (set-key-partition-list).
#   - The decoded .p12 on disk is shredded before exit.
#
# Cleanup of the keychain itself is intentionally NOT done here: the hosted CI
# runner is destroyed at job end so the keychain is bounded; explicit deletion
# would race with subsequent codesign steps in the same job. If we ever move to
# self-hosted runners, add an `if: always()` step that runs:
#   security delete-keychain "$KEYCHAIN_PATH"
#
# Why "Apple Development" by name (and not a SHA-1 hash) — see
# sprints/sprint-14/designs/US-14.1.md, "Risk 1".

set -euo pipefail

err() { printf 'import-macos-cert: %s\n' "$*" >&2; }

require_env() {
    local name=$1
    if [ -z "${!name:-}" ]; then
        err "missing required env var: $name"
        exit 1
    fi
}

require_env MACOS_DEV_CERT_P12_BASE64
require_env MACOS_DEV_CERT_P12_PWD
require_env MACOS_KEYCHAIN_PWD

WORKDIR=$(mktemp -d)
P12_PATH="${WORKDIR}/dev.p12"
KEYCHAIN_PATH="${WORKDIR}/build.keychain-db"

cleanup_p12() {
    # Shred the .p12 even on failure; the keychain holds the imported key.
    if [ -f "$P12_PATH" ]; then
        rm -f "$P12_PATH" || true
    fi
}
trap cleanup_p12 EXIT

# Decode the cert.
printf '%s' "$MACOS_DEV_CERT_P12_BASE64" | base64 --decode > "$P12_PATH"
if [ ! -s "$P12_PATH" ]; then
    err "decoded P12 is empty — MACOS_DEV_CERT_P12_BASE64 is malformed?"
    exit 1
fi

# Create the keychain. macOS appends `-db` automatically; using the explicit
# path keeps `security` predictable across runner versions.
security create-keychain -p "$MACOS_KEYCHAIN_PWD" "$KEYCHAIN_PATH"
security set-keychain-settings -lut 21600 "$KEYCHAIN_PATH"
security unlock-keychain -p "$MACOS_KEYCHAIN_PWD" "$KEYCHAIN_PATH"

# Make our keychain the default + add it to the search list so codesign can
# find the identity. Preserve the system login keychain after ours.
ORIGINAL_LIST=$(security list-keychains -d user | tr -d '"' | xargs)
security list-keychains -d user -s "$KEYCHAIN_PATH" $ORIGINAL_LIST
security default-keychain -s "$KEYCHAIN_PATH"

# Import the cert + key. -T grants codesign access to the private key without
# a UI prompt; set-key-partition-list seals that for unattended use.
security import "$P12_PATH" \
    -k "$KEYCHAIN_PATH" \
    -P "$MACOS_DEV_CERT_P12_PWD" \
    -T /usr/bin/codesign \
    -T /usr/bin/security
security set-key-partition-list \
    -S apple-tool:,apple:,codesign: \
    -s \
    -k "$MACOS_KEYCHAIN_PWD" \
    "$KEYCHAIN_PATH" > /dev/null

# Sanity-check: at least one "Apple Development" identity is now visible.
if ! security find-identity -p codesigning -v "$KEYCHAIN_PATH" \
        | grep -q "Apple Development"; then
    err "no Apple Development identity in keychain after import"
    security find-identity -p codesigning -v "$KEYCHAIN_PATH" >&2 || true
    exit 1
fi

printf 'KEYCHAIN_PATH=%s\n' "$KEYCHAIN_PATH"
err "imported Apple Development cert into $KEYCHAIN_PATH"
