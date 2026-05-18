#!/usr/bin/env bash
# scripts/ci/import-macos-cert.sh — US-45.1 (AC-45.1.2)
#
# Decode the base64-encoded MACOS_CERT_P12 secret into an EPHEMERAL keychain
# (NOT the default login.keychain) and authorize codesign/security to use it.
# An EXIT trap shreds the decoded .p12 AND deletes the temp keychain on every
# exit path, including on signal or error.
#
# Required env (provided by CI as `secrets.*`):
#   MACOS_CERT_P12        base64-encoded PKCS#12 file
#   MACOS_CERT_PASSWORD   password for the PKCS#12 file
# Optional:
#   KEYCHAIN_PASSWORD     password for the ephemeral keychain; random if unset
#
# Usage in CI:
#   - name: Import signing certificate
#     if: ${{ secrets.MACOS_CERT_P12 != '' }}
#     env:
#       MACOS_CERT_P12: ${{ secrets.MACOS_CERT_P12 }}
#       MACOS_CERT_PASSWORD: ${{ secrets.MACOS_CERT_PASSWORD }}
#     run: bash scripts/ci/import-macos-cert.sh

set -euo pipefail

# --- inputs ------------------------------------------------------------------

: "${MACOS_CERT_P12:?MACOS_CERT_P12 (base64) must be set}"
: "${MACOS_CERT_PASSWORD:?MACOS_CERT_PASSWORD must be set}"

# A random keychain password if not provided. Using /dev/urandom keeps this
# independent of the host's openssl version.
if [ -z "${KEYCHAIN_PASSWORD:-}" ]; then
    KEYCHAIN_PASSWORD="$(head -c 24 /dev/urandom | base64 | tr -d '\n=+/')"
fi

# --- scratch space + EXIT trap ----------------------------------------------

WORKDIR="$(mktemp -d -t human-macos-cert-XXXXXX)"
P12_PATH="$WORKDIR/cert.p12"
KEYCHAIN="$WORKDIR/human-ci-$$.keychain-db"

cleanup() {
    # Best-effort: never let cleanup mask the real exit code.
    security delete-keychain "$KEYCHAIN" 2>/dev/null || true
    if [ -f "$P12_PATH" ]; then
        # `rm -P` is the macOS equivalent of `shred -u` for a single file.
        rm -P "$P12_PATH" 2>/dev/null || rm -f "$P12_PATH" 2>/dev/null || true
    fi
    rm -rf "$WORKDIR" 2>/dev/null || true
}
trap cleanup EXIT

# --- decode (do NOT log the base64) -----------------------------------------

# Disable command echo around the secret material. The `+x` ... `-x` is a
# guard against future `set -x` callers; on its own this script never
# enables xtrace, but a wrapping CI step or a debug toggle might.
set +x
printf '%s' "$MACOS_CERT_P12" | base64 -d > "$P12_PATH"
set -x 2>/dev/null || true

# --- create ephemeral keychain ----------------------------------------------

security create-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN"
# 6 hour auto-lock; longer than any reasonable single CI job.
security set-keychain-settings -lut 21600 "$KEYCHAIN"
security unlock-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN"

# Import the cert and authorize codesign + security to use the private key
# without an interactive prompt.
security import "$P12_PATH" \
    -k "$KEYCHAIN" \
    -P "$MACOS_CERT_PASSWORD" \
    -T /usr/bin/codesign \
    -T /usr/bin/security

# Skip the "always allow" prompt when codesign uses the imported key.
security set-key-partition-list \
    -S apple-tool:,apple:,codesign: \
    -k "$KEYCHAIN_PASSWORD" \
    "$KEYCHAIN" >/dev/null

# --- prepend to the user keychain search-list (path-with-spaces safe) -------

# `security list-keychains -d user -s` takes positional args; a single quoted
# string would collapse multiple paths into one arg. Read the existing list
# into a bash array (`mapfile -t`) and re-pass via `"${ARRAY[@]}"`. This
# survives paths that contain spaces (a prior critic catch).
ORIGINAL_LIST=()
while IFS= read -r line; do
    # Strip leading whitespace and surrounding double-quotes from each line.
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%\"}"
    line="${line#\"}"
    if [ -n "$line" ]; then
        ORIGINAL_LIST+=("$line")
    fi
done < <(security list-keychains -d user)

security list-keychains -d user -s "$KEYCHAIN" "${ORIGINAL_LIST[@]}"

# --- echo identity name (never the SHA-1) -----------------------------------

# Help downstream `CODE_SIGN_IDENTITY` resolution by surfacing the human name
# of what we just imported. The SHA-1 column is intentionally dropped.
IDENTITY_NAME="$(security find-identity -v -p codesigning "$KEYCHAIN" \
    | awk -F'"' '/Apple/ { print $2; exit }')"
if [ -n "$IDENTITY_NAME" ]; then
    echo "Imported codesigning identity: $IDENTITY_NAME"
else
    echo "Imported keychain $KEYCHAIN but no Apple identity surfaced." >&2
    exit 1
fi
