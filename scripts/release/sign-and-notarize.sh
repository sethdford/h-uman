#!/bin/bash
# Code-sign and notarize macOS .pkg for distribution (US-C1.3)
# Usage: sign-and-notarize.sh [--pkg <path>] [--dry-run] [--verify]
# Environment variables (required for signing):
#   APPLE_DEV_ID:           "Developer ID Application: ..." cert identity
#   APPLE_DEV_ID_INSTALLER: "Developer ID Installer: ..." cert identity for .pkg
#   NOTARY_PROFILE:         keychain profile name (from `xcrun notarytool store-credentials`)
#   APPLE_TEAM_ID:          (optional) team ID for error messages

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Default values
PKG_PATH=""
DRY_RUN=0
VERIFY_ONLY=0
NOTARY_TIMEOUT=900  # 15 minutes in seconds

# Parse command-line arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pkg)
            PKG_PATH="$2"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        --verify)
            VERIFY_ONLY=1
            shift
            ;;
        *)
            echo "Usage: sign-and-notarize.sh [--pkg <path>] [--dry-run] [--verify]"
            exit 1
            ;;
    esac
done

# Sanity check: are we on macOS?
if [[ ! "$OSTYPE" == "darwin"* ]]; then
    echo "ERROR: sign-and-notarize.sh only runs on macOS"
    exit 79
fi

# Read environment variables early; graceful skip if missing
APPLE_DEV_ID="${APPLE_DEV_ID:-}"
APPLE_DEV_ID_INSTALLER="${APPLE_DEV_ID_INSTALLER:-}"
NOTARY_PROFILE="${NOTARY_PROFILE:-}"
APPLE_TEAM_ID="${APPLE_TEAM_ID:-}"

# Check if signing creds are present (fail-fast before validating .pkg)
if [[ -z "$APPLE_DEV_ID" ]]; then
    echo "APPLE_DEV_ID not set; code-signing skipped."
    echo "Set APPLE_DEV_ID to \"Developer ID Application: ...\" to enable signing."
    exit 0
fi

if [[ -z "$APPLE_DEV_ID_INSTALLER" ]]; then
    echo "APPLE_DEV_ID_INSTALLER not set; .pkg signing skipped."
    echo "Set APPLE_DEV_ID_INSTALLER to \"Developer ID Installer: ...\" to enable signing."
    exit 0
fi

if [[ -z "$NOTARY_PROFILE" ]]; then
    echo "NOTARY_PROFILE not set; notarization skipped."
    echo "Run 'xcrun notarytool store-credentials <profile-name> --apple-id <email> --password <app-password>' locally first."
    exit 0
fi

# If no pkg provided, infer from build
if [[ -z "$PKG_PATH" ]]; then
    PKG_PATH="${PROJECT_DIR}/human-release.pkg"
fi

# Validate .pkg exists
if [[ ! -f "$PKG_PATH" ]]; then
    echo "ERROR: .pkg file not found at $PKG_PATH"
    exit 1
fi

# Entitlements file path
ENTITLEMENTS="${SCRIPT_DIR}/entitlements.plist"
if [[ ! -f "$ENTITLEMENTS" ]]; then
    echo "ERROR: entitlements.plist not found at $ENTITLEMENTS"
    exit 1
fi

echo "Signing and notarizing: $PKG_PATH"

# Helper function to run a command and optionally dry-run
run_cmd() {
    local cmd="$*"
    if [[ $DRY_RUN -eq 1 ]]; then
        echo "[DRY-RUN] $cmd"
    else
        echo "Running: $cmd"
        eval "$cmd" || return 1
    fi
}

# === Stage 1: Extract and sign the app bundle inside the .pkg ===
# Notarization requires a signed bundle; we sign the extracted app before notarization
echo ""
echo "=== Stage 1: Extract and sign app bundle ==="

PKG_BASENAME=$(basename "$PKG_PATH" .pkg)
TEMP_DIR=$(mktemp -d)
trap "rm -rf '$TEMP_DIR'" EXIT

# Extract the .pkg to a temporary directory
if [[ $DRY_RUN -eq 0 ]]; then
    pkgutil --expand "$PKG_PATH" "$TEMP_DIR/expanded"
    if [[ ! -d "$TEMP_DIR/expanded/Payload" ]]; then
        echo "ERROR: Could not extract .pkg payload"
        exit 1
    fi

    # Decompress Payload to access the app bundle
    cd "$TEMP_DIR/expanded/Payload" && cpio -i --make-directories < Payload.cpio 2>/dev/null || true
    cd - > /dev/null
fi

APP_IN_PKG="$TEMP_DIR/expanded/Payload/Applications/Human.app"

# Sign the binary (or re-sign if already signed)
SIGN_CMD="codesign \
  --sign \"$APPLE_DEV_ID\" \
  --options runtime \
  --timestamp \
  --entitlements \"$ENTITLEMENTS\" \
  --verbose=4 \
  \"$APP_IN_PKG/Contents/MacOS/human\""

run_cmd "$SIGN_CMD"

# Deep-sign the bundle (signs all nested binaries, frameworks, etc.)
DEEP_SIGN_CMD="codesign \
  --sign \"$APPLE_DEV_ID\" \
  --options runtime \
  --timestamp \
  --entitlements \"$ENTITLEMENTS\" \
  --deep \
  --verbose=4 \
  \"$APP_IN_PKG\""

run_cmd "$DEEP_SIGN_CMD"

# === Stage 2: Re-package and sign the .pkg ===
echo ""
echo "=== Stage 2: Re-package and sign .pkg ==="

if [[ $DRY_RUN -eq 0 ]]; then
    # Recompress the payload
    cd "$TEMP_DIR/expanded/Payload"
    find . -depth -not -name "." | cpio -o -R 0:80 > Payload.cpio.tmp 2>/dev/null || true
    gzip -9 Payload.cpio.tmp
    mv Payload.cpio.tmp.gz Payload
    cd - > /dev/null

    # Repackage the .pkg
    REPACKED="${TEMP_DIR}/repacked.pkg"
    pkgutil --flatten "$TEMP_DIR/expanded" "$REPACKED"
    if [[ ! -f "$REPACKED" ]]; then
        echo "ERROR: Failed to repackage .pkg"
        exit 1
    fi

    # Replace the original with the signed version
    cp "$REPACKED" "$PKG_PATH"
fi

# productsign the .pkg itself
PRODUCTSIGN_CMD="productsign \
  --sign \"$APPLE_DEV_ID_INSTALLER\" \
  --timestamp \
  \"$PKG_PATH\" \
  \"${PKG_PATH}.signed\""

run_cmd "$PRODUCTSIGN_CMD"

if [[ $DRY_RUN -eq 0 ]]; then
    mv "${PKG_PATH}.signed" "$PKG_PATH"
    echo "✓ .pkg signed successfully"
fi

# === Stage 3: Notarize via xcrun notarytool ===
echo ""
echo "=== Stage 3: Submit for notarization ==="

NOTARY_CMD="xcrun notarytool submit \"$PKG_PATH\" \
  --keychain-profile \"$NOTARY_PROFILE\" \
  --wait \
  --timeout $NOTARY_TIMEOUT"

if [[ $DRY_RUN -eq 1 ]]; then
    echo "[DRY-RUN] $NOTARY_CMD"
    SUBMISSION_ID="dry-run-submission-id"
else
    # Capture both stdout and stderr; parse submission ID
    NOTARY_OUTPUT=$(mktemp)
    trap "rm -f '$NOTARY_OUTPUT' '$TEMP_DIR'" EXIT

    if ! xcrun notarytool submit "$PKG_PATH" \
        --keychain-profile "$NOTARY_PROFILE" \
        --wait \
        --timeout $NOTARY_TIMEOUT 2>&1 | tee "$NOTARY_OUTPUT"; then

        # Notarization failed; capture submission ID and fetch log
        echo ""
        echo "❌ Notarization failed."

        # Try to extract submission ID from output or query history
        SUBMISSION_ID=$(grep -oP 'id: \K[a-f0-9-]+' "$NOTARY_OUTPUT" | head -1 || \
                        xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" --json 2>/dev/null | \
                        jq -r '.[0].id' || echo "unknown")

        if [[ "$SUBMISSION_ID" != "unknown" && "$SUBMISSION_ID" != "" ]]; then
            LOG_PATH="${PROJECT_DIR}/notarization-log-${SUBMISSION_ID}.json"
            echo "Fetching rejection log..."
            xcrun notarytool log "$SUBMISSION_ID" \
                --keychain-profile "$NOTARY_PROFILE" > "$LOG_PATH" 2>/dev/null || true

            if [[ -f "$LOG_PATH" ]]; then
                echo ""
                echo "Rejection log saved to: $LOG_PATH"
                echo ""
                echo "To diagnose the issue, run:"
                echo "  scripts/release/diagnose-notary.sh --log $LOG_PATH"
                echo ""
            fi
        fi

        exit 1
    fi

    # Extract submission ID from successful output
    SUBMISSION_ID=$(grep -oP 'id: \K[a-f0-9-]+' "$NOTARY_OUTPUT" | head -1 || echo "unknown")
    echo "✓ Notarization succeeded (submission ID: $SUBMISSION_ID)"
fi

# === Stage 4: Staple the notarization ticket ===
echo ""
echo "=== Stage 4: Staple notarization ticket ==="

STAPLE_CMD="xcrun stapler staple \"$PKG_PATH\""
run_cmd "$STAPLE_CMD"

if [[ $DRY_RUN -eq 0 ]]; then
    echo "✓ Staple succeeded"
fi

# === Optional: Verify the signature chain ===
if [[ $VERIFY_ONLY -eq 1 || $DRY_RUN -eq 0 ]]; then
    echo ""
    echo "=== Verification ==="

    if [[ $DRY_RUN -eq 0 ]]; then
        run_cmd "codesign --verify --deep --strict --verbose=4 \"$APP_IN_PKG\""
        run_cmd "xcrun stapler validate \"$PKG_PATH\""
        echo "✓ Signature chain verified"
    fi
fi

echo ""
echo "✓ Sign and notarize complete"
echo "  Artifact: $PKG_PATH"
if [[ -n "$SUBMISSION_ID" && "$SUBMISSION_ID" != "unknown" ]]; then
    echo "  Submission ID: $SUBMISSION_ID"
fi

exit 0
