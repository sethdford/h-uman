#!/usr/bin/env bash
# notarize-mac.sh — package a signed Human.app into a UDZO DMG, submit it to
# Apple's notarytool, staple the ticket, and run a Gatekeeper assess on the
# result. Single source of truth for the macOS notarization recipe; both
# local operators and CI invoke this identically.
#
# Design: sprints/sprint-14/designs/US-14.2.md
# Story:  US-14.2 — notarized DMG produced by release pipeline
#
# Exit codes:
#   0  success (real-mode: notarized + stapled + spctl accepted; dry-run: arg
#                          parse + DMG build path validated)
#   1  generic / usage error
#   2  notarytool submit failed (Apple rejected or transport error)
#   3  stapler staple failed (or staple validate failed post-staple)
#   4  spctl assess rejected the stapled DMG
#
# Security contract (HIGH risk story):
#   - The decoded API key never appears in any log line. The block that
#     touches the decoded key is bracketed with `set +x` and re-enables
#     `set -x` only AFTER the credential has gone out of scope (file
#     written, path captured, decoded bytes shredded).
#   - The decoded `.p8` is written with `umask 077` into $RUNNER_TEMP (or
#     $TMPDIR / /tmp fallback) and is unlinked by the EXIT trap.
#   - `--dry-run` short-circuits any `xcrun` invocation (test-friendly).
#   - Any mounted DMG (`/Volumes/Human*`) is detached on EXIT/INT/TERM.

set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
SCRIPT_VERSION="0.1.0"

# ---------------------------------------------------------------------------
# Cleanup state. Anything appended to CLEANUP_PATHS is `rm -f`'d on EXIT;
# anything appended to CLEANUP_MOUNTS is `hdiutil detach`'d on EXIT.
# ---------------------------------------------------------------------------
CLEANUP_PATHS=()
CLEANUP_MOUNTS=()

cleanup() {
    local rc=$?
    # Detach any mounts we created. `|| true` so cleanup never masks rc.
    if [[ ${#CLEANUP_MOUNTS[@]} -gt 0 ]]; then
        for mount in "${CLEANUP_MOUNTS[@]}"; do
            [[ -d "$mount" ]] && hdiutil detach "$mount" -quiet -force >/dev/null 2>&1 || true
        done
    fi
    # Shred any tmp credential files we wrote.
    if [[ ${#CLEANUP_PATHS[@]} -gt 0 ]]; then
        for p in "${CLEANUP_PATHS[@]}"; do
            [[ -f "$p" ]] && rm -f "$p" || true
        done
    fi
    exit "$rc"
}
trap cleanup EXIT INT TERM

usage() {
    cat <<EOF
$SCRIPT_NAME v$SCRIPT_VERSION — Notarize a macOS .app into a stapled DMG.

USAGE:
    $SCRIPT_NAME --app PATH [--version VERSION] [--output-dmg PATH]
                 [--bundle-id ID] [--enable-hardened-runtime] [--dry-run]
                 [-h|--help]

REQUIRED:
    --app PATH            Path to the signed Human.app bundle.

OPTIONAL:
    --version VERSION     Version string baked into DMG filename
                          (default: 0.0.0-dev).
    --output-dmg PATH     Output DMG path (default: build/Human-<ver>.dmg).
    --bundle-id ID        CFBundleIdentifier expected in the app
                          (informational; default: ai.human.mac).
    --enable-hardened-runtime
                          Re-sign the .app with --options runtime before DMG
                          packaging. Requires the cert identity in the
                          keychain. Skipped under --dry-run.
    --dry-run             Build the DMG but never invoke xcrun submit/staple.
                          Prints what would have been run. Exits 0.
    -h, --help            Print this help and exit.

ENVIRONMENT (real-mode only; ignored under --dry-run):
    APP_STORE_CONNECT_API_KEY_B64   Base64-encoded .p8 private key contents.
    APP_STORE_CONNECT_KEY_ID        Apple App Store Connect API key ID.
    APP_STORE_CONNECT_ISSUER_ID     Apple App Store Connect issuer UUID.
    CODE_SIGN_IDENTITY              Identity for hardened-runtime re-sign
                                    (default: "Developer ID Application").
    RUNNER_TEMP                     If set, used for tmp key staging.

EXIT CODES:
    0  success         2  notarytool submit failed
    1  usage error     3  stapler staple/validate failed
                       4  spctl --assess rejected the DMG

SECURITY:
    The decoded API key file is written under umask 077 and unlinked by an
    EXIT trap. The credential-handling block runs with set +x. Never echo
    the key bytes.
EOF
}

# ---------------------------------------------------------------------------
# Arg parsing.
# ---------------------------------------------------------------------------
APP=""
VERSION="0.0.0-dev"
OUTPUT_DMG=""
BUNDLE_ID="ai.human.mac"
ENABLE_HARDENED_RUNTIME=0
DRY_RUN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app)
            APP="${2:-}"
            shift 2
            ;;
        --version)
            VERSION="${2:-}"
            shift 2
            ;;
        --output-dmg)
            OUTPUT_DMG="${2:-}"
            shift 2
            ;;
        --bundle-id)
            BUNDLE_ID="${2:-}"
            shift 2
            ;;
        --enable-hardened-runtime)
            ENABLE_HARDENED_RUNTIME=1
            shift
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "$SCRIPT_NAME: unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ -z "$APP" ]]; then
    echo "$SCRIPT_NAME: --app PATH is required" >&2
    usage >&2
    exit 1
fi

if [[ ! -d "$APP" ]]; then
    echo "$SCRIPT_NAME: --app path does not exist or is not a directory: $APP" >&2
    exit 1
fi

if [[ -z "$OUTPUT_DMG" ]]; then
    mkdir -p build
    OUTPUT_DMG="build/Human-${VERSION}.dmg"
fi

# Pre-flight: a stale mount from a previous failed run will fail hdiutil create.
hdiutil detach "/Volumes/Human" -quiet -force >/dev/null 2>&1 || true

# ---------------------------------------------------------------------------
# Optional hardened-runtime re-sign.
# AC-14.2.5: codesign --display ... contains flags=0x10000(runtime) on the
# re-signed app. Skipped under --dry-run (no xcrun in dry-run path).
# ---------------------------------------------------------------------------
if [[ "$ENABLE_HARDENED_RUNTIME" -eq 1 ]]; then
    if [[ "$DRY_RUN" -eq 1 ]]; then
        echo "[DRY-RUN] would re-sign $APP with --options runtime"
    else
        IDENTITY="${CODE_SIGN_IDENTITY:-Developer ID Application}"
        ENTITLEMENTS_ARG=()
        if [[ -f "apps/macos/Human.entitlements" ]]; then
            ENTITLEMENTS_ARG=(--entitlements "apps/macos/Human.entitlements")
        fi
        # --deep is intentionally NOT used here (signing). It's only safe for
        # verify per US-14.1 design. We sign the bundle root; nested binaries
        # must already be signed by their producer.
        codesign --force --options runtime \
            --sign "$IDENTITY" \
            --timestamp \
            "${ENTITLEMENTS_ARG[@]}" \
            "$APP"
    fi
fi

# ---------------------------------------------------------------------------
# DMG creation.
# Always run, even under --dry-run, because the dry-run smoke test asserts
# the DMG was actually built (AC-14.2.1).
# ---------------------------------------------------------------------------
echo "==> Building DMG: $OUTPUT_DMG (from $APP)"
rm -f "$OUTPUT_DMG"
hdiutil create \
    -volname Human \
    -srcfolder "$APP" \
    -ov \
    -format UDZO \
    "$OUTPUT_DMG"

# ---------------------------------------------------------------------------
# Dry-run path: short-circuit. Print exactly what real-mode would do and
# leave a placeholder so AC-14.2.1's "placeholder staple marker file" check
# can find it.
# ---------------------------------------------------------------------------
if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "DRY RUN: would call xcrun notarytool submit ${OUTPUT_DMG} --key [REDACTED] --key-id [REDACTED] --issuer [REDACTED] --wait --output-format json"
    echo "DRY RUN: would call xcrun stapler staple ${OUTPUT_DMG}"
    echo "DRY RUN: would call xcrun stapler validate ${OUTPUT_DMG}"
    echo "DRY RUN: would call spctl --assess --type open --context context:primary-signature --verbose ${OUTPUT_DMG}"
    : > "${OUTPUT_DMG}.dryrun"
    echo "$SCRIPT_NAME: dry-run complete; placeholder marker: ${OUTPUT_DMG}.dryrun"
    exit 0
fi

# ---------------------------------------------------------------------------
# Real-mode credential loading.
# Bracketed with `set +x` so the decoded key bytes can never be xtrace'd into
# CI logs even if a future maintainer forgets and adds `set -x` at top.
# ---------------------------------------------------------------------------
: "${APP_STORE_CONNECT_API_KEY_B64:?APP_STORE_CONNECT_API_KEY_B64 required in real mode}"
: "${APP_STORE_CONNECT_KEY_ID:?APP_STORE_CONNECT_KEY_ID required in real mode}"
: "${APP_STORE_CONNECT_ISSUER_ID:?APP_STORE_CONNECT_ISSUER_ID required in real mode}"

# === BEGIN CREDENTIAL BLOCK — DO NOT ADD echo/cat/set -x IN THIS BLOCK ===
{ set +x; } 2>/dev/null
_old_umask="$(umask)"
umask 077

KEY_DIR="${RUNNER_TEMP:-${TMPDIR:-/tmp}}"
KEY_PATH="${KEY_DIR}/asc_key_$$.p8"
# Use a temp variable so a base64 failure can't leak partial bytes via
# tee/redirect. printf-then-pipe routes through stdin only.
if ! printf '%s' "$APP_STORE_CONNECT_API_KEY_B64" | base64 --decode > "$KEY_PATH" 2>/dev/null; then
    rm -f "$KEY_PATH"
    umask "$_old_umask"
    echo "$SCRIPT_NAME: failed to decode APP_STORE_CONNECT_API_KEY_B64" >&2
    exit 1
fi
CLEANUP_PATHS+=("$KEY_PATH")
umask "$_old_umask"
unset _old_umask
# === END CREDENTIAL BLOCK ===

# ---------------------------------------------------------------------------
# Submission.
# ---------------------------------------------------------------------------
echo "==> Submitting $OUTPUT_DMG to notarytool (this can take several minutes)"
SUBMISSION_JSON="${KEY_DIR}/notarize_submission_$$.json"
CLEANUP_PATHS+=("$SUBMISSION_JSON")

if ! xcrun notarytool submit "$OUTPUT_DMG" \
        --key "$KEY_PATH" \
        --key-id "$APP_STORE_CONNECT_KEY_ID" \
        --issuer "$APP_STORE_CONNECT_ISSUER_ID" \
        --wait \
        --output-format json \
        > "$SUBMISSION_JSON"; then
    echo "$SCRIPT_NAME: notarytool submit failed (transport or auth error)" >&2
    [[ -f "$SUBMISSION_JSON" ]] && cat "$SUBMISSION_JSON" >&2 || true
    exit 2
fi

# Surface the submission status; ID-only JSON is safe to print.
cat "$SUBMISSION_JSON"

# Parse status without jq dependency (notarytool JSON is shallow).
STATUS="$(grep -o '"status"[[:space:]]*:[[:space:]]*"[^"]*"' "$SUBMISSION_JSON" | head -1 | sed 's/.*:[[:space:]]*"\([^"]*\)".*/\1/')"
SUB_ID="$(grep -o '"id"[[:space:]]*:[[:space:]]*"[^"]*"' "$SUBMISSION_JSON" | head -1 | sed 's/.*:[[:space:]]*"\([^"]*\)".*/\1/')"

if [[ "$STATUS" != "Accepted" ]]; then
    echo "$SCRIPT_NAME: notarytool returned status='$STATUS' (id=$SUB_ID); fetching log" >&2
    if [[ -n "$SUB_ID" ]]; then
        xcrun notarytool log "$SUB_ID" \
            --key "$KEY_PATH" \
            --key-id "$APP_STORE_CONNECT_KEY_ID" \
            --issuer "$APP_STORE_CONNECT_ISSUER_ID" >&2 || true
    fi
    exit 2
fi

# ---------------------------------------------------------------------------
# Staple.
# ---------------------------------------------------------------------------
echo "==> Stapling notarization ticket onto $OUTPUT_DMG"
if ! xcrun stapler staple "$OUTPUT_DMG"; then
    echo "$SCRIPT_NAME: stapler staple failed" >&2
    exit 3
fi
if ! xcrun stapler validate "$OUTPUT_DMG"; then
    echo "$SCRIPT_NAME: stapler validate failed post-staple" >&2
    exit 3
fi

# ---------------------------------------------------------------------------
# Gatekeeper assess.
# AC-14.2.3: spctl --assess --type open --context context:primary-signature
# --verbose Human.dmg exits 0 and prints "accepted".
# ---------------------------------------------------------------------------
echo "==> Running spctl --assess on the stapled DMG"
if ! spctl --assess --type open --context context:primary-signature --verbose "$OUTPUT_DMG"; then
    echo "$SCRIPT_NAME: spctl --assess rejected the stapled DMG" >&2
    exit 4
fi

echo "$SCRIPT_NAME: notarization complete: $OUTPUT_DMG"
exit 0
