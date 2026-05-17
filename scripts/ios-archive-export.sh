#!/usr/bin/env bash
# ios-archive-export.sh — produce a Release archive + signed App Store IPA for HumaniOS.
#
# US-14.3 (Sprint 14): the script does two xcodebuild calls (archive + exportArchive)
# against apps/ios/, then asserts the IPA's bundle ID and version via `plutil`.
#
# Placeholders in apps/ios/ExportOptions.plist are SED-substituted into a `.bak` copy
# at run time using env vars; the original committed file is restored on EXIT so the
# repo always stays free of real team / provisioning identifiers (see
# scripts/check-no-provisioning-leak.sh — pre-commit guard).
#
# Required env vars:
#   APPLE_TEAM_ID            — Apple Developer team ID (10 chars, e.g. ABC1234567)
#   IOS_PROVISIONING_UUID    — provisioning profile UUID (36 chars) for ai.human.ios
#
# Optional env vars:
#   ARCHIVE_PATH             — where to write the .xcarchive    (default: /tmp/HumaniOS.xcarchive)
#   EXPORT_PATH              — where to export the .ipa         (default: /tmp/export)
#   EXPECTED_BUNDLE_ID       — bundle ID asserted from the IPA  (default: ai.human.ios)
#   EXPECTED_VERSION         — CFBundleShortVersionString       (default: 1.1.0)
#   SKIP_EXPORT              — if "1", skip exportArchive step (used in CI when secrets absent)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXPORT_OPTIONS_SRC="$REPO_ROOT/apps/ios/ExportOptions.plist"
EXPORT_OPTIONS_BAK="$EXPORT_OPTIONS_SRC.bak"

ARCHIVE_PATH="${ARCHIVE_PATH:-/tmp/HumaniOS.xcarchive}"
EXPORT_PATH="${EXPORT_PATH:-/tmp/export}"
EXPECTED_BUNDLE_ID="${EXPECTED_BUNDLE_ID:-ai.human.ios}"
EXPECTED_VERSION="${EXPECTED_VERSION:-1.1.0}"
SKIP_EXPORT="${SKIP_EXPORT:-0}"

cleanup() {
    # Restore the committed ExportOptions.plist (with placeholders) from the .bak copy.
    # This is the guard that keeps real UUIDs out of the working tree even after a run.
    if [ -f "$EXPORT_OPTIONS_BAK" ]; then
        mv -f "$EXPORT_OPTIONS_BAK" "$EXPORT_OPTIONS_SRC"
    fi
}
trap cleanup EXIT INT TERM

require_var() {
    local name="$1"
    local val
    val="$(printenv "$name" || true)"
    if [ -z "$val" ]; then
        echo "ios-archive-export: required env var $name is unset or empty" >&2
        exit 1
    fi
}

# When SKIP_EXPORT=1 we still need placeholder substitution so xcodebuild can parse the
# plist if it walks it; but we tolerate missing real secrets. We use stub values that
# are clearly synthetic (won't accidentally match a real Apple identifier).
if [ "$SKIP_EXPORT" = "1" ]; then
    APPLE_TEAM_ID="${APPLE_TEAM_ID:-STUB000000}"
    IOS_PROVISIONING_UUID="${IOS_PROVISIONING_UUID:-00000000-0000-0000-0000-000000000000}"
else
    require_var APPLE_TEAM_ID
    require_var IOS_PROVISIONING_UUID
fi

# --- Placeholder substitution (silent: avoid leaking IDs into CI logs) -------------
# Wrap the substitution in `set +x` to ensure these values never appear in trace output
# even if the script is invoked under `bash -x` or `xtrace` for debugging.
{
    set +x
    cp "$EXPORT_OPTIONS_SRC" "$EXPORT_OPTIONS_BAK"
    # Substitute into the in-place copy that xcodebuild will read; the .bak (the
    # placeholder-bearing original) is what gets restored by the EXIT trap. We pass the
    # substituted file to xcodebuild; we pass the original (via cleanup) back to disk.
    sed -i.tmp \
        -e "s/__TEAM_ID__/${APPLE_TEAM_ID}/g" \
        -e "s/__PROVISIONING_PROFILE_UUID__/${IOS_PROVISIONING_UUID}/g" \
        "$EXPORT_OPTIONS_SRC"
    rm -f "$EXPORT_OPTIONS_SRC.tmp"
} 2>/dev/null

# Confirm the placeholders are gone from the working copy (sanity), without echoing values.
if grep -q "__TEAM_ID__\|__PROVISIONING_PROFILE_UUID__" "$EXPORT_OPTIONS_SRC"; then
    echo "ios-archive-export: placeholder substitution failed" >&2
    exit 1
fi
echo "ios-archive-export: placeholder substitution OK (values redacted)"

# --- Generate Xcode project --------------------------------------------------------
if ! command -v xcodegen >/dev/null 2>&1; then
    echo "ios-archive-export: xcodegen not installed (brew install xcodegen)" >&2
    exit 1
fi
( cd "$REPO_ROOT/apps/ios" && xcodegen generate )

# --- Archive -----------------------------------------------------------------------
echo "ios-archive-export: archiving HumaniOS -> $ARCHIVE_PATH"
rm -rf "$ARCHIVE_PATH"
( cd "$REPO_ROOT/apps/ios" && \
    xcodebuild \
        -scheme HumaniOS \
        -configuration Release \
        -destination 'generic/platform=iOS' \
        -archivePath "$ARCHIVE_PATH" \
        archive )

if [ ! -d "$ARCHIVE_PATH/Products/Applications/HumaniOS.app" ]; then
    echo "ios-archive-export: expected app not found in archive" >&2
    exit 1
fi

# --- Export (optional in CI without real secrets) ----------------------------------
if [ "$SKIP_EXPORT" = "1" ]; then
    echo "ios-archive-export: SKIP_EXPORT=1 — stopping after archive step"
    echo "ios-archive-export: SUCCESS (archive only)"
    exit 0
fi

rm -rf "$EXPORT_PATH"
mkdir -p "$EXPORT_PATH"
echo "ios-archive-export: exporting App Store IPA -> $EXPORT_PATH"
xcodebuild \
    -exportArchive \
    -archivePath "$ARCHIVE_PATH" \
    -exportOptionsPlist "$EXPORT_OPTIONS_SRC" \
    -exportPath "$EXPORT_PATH"

IPA="$EXPORT_PATH/HumaniOS.ipa"
if [ ! -f "$IPA" ]; then
    echo "ios-archive-export: HumaniOS.ipa not produced at $IPA" >&2
    exit 1
fi

IPA_SIZE="$(wc -c < "$IPA" | tr -d ' ')"
if [ "$IPA_SIZE" -lt 102400 ]; then
    echo "ios-archive-export: HumaniOS.ipa is only $IPA_SIZE bytes (<100 KB); likely corrupt" >&2
    exit 1
fi
echo "ios-archive-export: HumaniOS.ipa = $IPA_SIZE bytes"

# --- plutil assertions on the IPA's Info.plist (AC-14.3.3) -------------------------
TMP_INFO="$(mktemp -t humanios-info.XXXXXX)"
unzip -p "$IPA" "Payload/HumaniOS.app/Info.plist" > "$TMP_INFO"
PLUTIL_OUT="$(plutil -p "$TMP_INFO")"
rm -f "$TMP_INFO"

if ! echo "$PLUTIL_OUT" | grep -q "\"CFBundleIdentifier\" => \"$EXPECTED_BUNDLE_ID\""; then
    echo "ios-archive-export: bundle ID mismatch (expected $EXPECTED_BUNDLE_ID)" >&2
    echo "$PLUTIL_OUT" | grep -i "CFBundleIdentifier" >&2 || true
    exit 1
fi

if ! echo "$PLUTIL_OUT" | grep -q "\"CFBundleShortVersionString\" => \"$EXPECTED_VERSION\""; then
    echo "ios-archive-export: short version mismatch (expected $EXPECTED_VERSION)" >&2
    echo "$PLUTIL_OUT" | grep -i "CFBundleShortVersionString" >&2 || true
    exit 1
fi

echo "ios-archive-export: bundle ID and version assertions PASS"
echo "ios-archive-export: SUCCESS"
