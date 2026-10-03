#!/bin/bash
# Write Contents/Info.plist for the macOS Human.app bundle (US-C1.1).
# Usage: write-info-plist.sh --app-path <path/Human.app> [--min-macos <ver>]
#
# The version is read from the bundled binary itself (`human --version` prints
# "human v0.5.0 (<sha>)"), so the plist can never disagree with what ships.
# Writes exactly the keys scripts/release/verify-bundle.sh requires, plus
# CFBundleShortVersionString and LSMinimumSystemVersion.
set -euo pipefail

APP_PATH=""
MIN_MACOS="10.15"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --app-path) APP_PATH="$2"; shift 2 ;;
        --min-macos) MIN_MACOS="$2"; shift 2 ;;
        -h | --help)
            echo "Usage: write-info-plist.sh --app-path <path/Human.app> [--min-macos <ver>]"
            exit 0
            ;;
        *) echo "ERROR: unknown argument: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "$APP_PATH" ]]; then
    echo "ERROR: --app-path is required" >&2
    exit 1
fi
BIN="$APP_PATH/Contents/MacOS/human"
if [[ ! -x "$BIN" ]]; then
    echo "ERROR: executable not found at $BIN" >&2
    exit 1
fi

VERSION=$("$BIN" --version 2>/dev/null | sed -nE 's/^human v([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' | head -1)
if [[ -z "$VERSION" ]]; then
    echo "ERROR: could not read a version from '$BIN --version'" >&2
    exit 1
fi

PLIST="$APP_PATH/Contents/Info.plist"
plutil -create xml1 "$PLIST"
plutil -insert CFBundleIdentifier -string "com.h-uman.human" "$PLIST"
plutil -insert CFBundleName -string "Human" "$PLIST"
plutil -insert CFBundleExecutable -string "human" "$PLIST"
plutil -insert CFBundlePackageType -string "APPL" "$PLIST"
plutil -insert CFBundleVersion -string "$VERSION" "$PLIST"
plutil -insert CFBundleShortVersionString -string "$VERSION" "$PLIST"
plutil -insert LSMinimumSystemVersion -string "$MIN_MACOS" "$PLIST"
plutil -lint "$PLIST" >/dev/null
echo "Wrote $PLIST (version $VERSION, macOS >= $MIN_MACOS)"
