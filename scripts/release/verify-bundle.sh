#!/bin/bash

# Verify macOS app bundle structure (US-C1.1)
# Usage: ./scripts/release/verify-bundle.sh [--app-path <path>]
# Defaults to build/Human.app or /Applications/Human.app if built

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Default app paths
APP_PATH="${1:-build/Human.app}"
if [[ ! -d "$APP_PATH" ]]; then
    # Try /Applications as fallback
    APP_PATH="/Applications/Human.app"
fi

# Ensure we're on macOS
if [[ ! "$OSTYPE" == "darwin"* ]]; then
    echo "ERROR: verify-bundle.sh only runs on macOS"
    exit 1
fi

# Check if app exists
if [[ ! -d "$APP_PATH" ]]; then
    echo "ERROR: Bundle not found at $APP_PATH"
    exit 1
fi

echo "Verifying macOS app bundle: $APP_PATH"

# Check directory structure
echo "  ✓ Checking directory structure..."
[[ -d "$APP_PATH/Contents" ]] || { echo "    ERROR: Contents directory missing"; exit 1; }
[[ -d "$APP_PATH/Contents/MacOS" ]] || { echo "    ERROR: MacOS directory missing"; exit 1; }
[[ -d "$APP_PATH/Contents/Resources" ]] || { echo "    ERROR: Resources directory missing"; exit 1; }
echo "    OK: Directory structure valid"

# Check Info.plist exists and is valid XML
echo "  ✓ Checking Info.plist..."
[[ -f "$APP_PATH/Contents/Info.plist" ]] || { echo "    ERROR: Info.plist missing"; exit 1; }

# Validate plist syntax
if ! plutil -lint "$APP_PATH/Contents/Info.plist" >/dev/null 2>&1; then
    echo "    ERROR: Info.plist is not valid XML"
    exit 1
fi
echo "    OK: Info.plist is valid"

# Verify required keys in plist
echo "  ✓ Checking Info.plist required keys..."
required_keys=(
    "CFBundleIdentifier"
    "CFBundleName"
    "CFBundleVersion"
    "CFBundleExecutable"
    "CFBundlePackageType"
)

for key in "${required_keys[@]}"; do
    if ! plutil -p "$APP_PATH/Contents/Info.plist" 2>/dev/null | grep -q "$key"; then
        echo "    ERROR: Missing required key: $key"
        exit 1
    fi
done
echo "    OK: All required keys present"

# Verify CFBundlePackageType is APPL
echo "  ✓ Checking CFBundlePackageType..."
pkg_type=$(plutil -p "$APP_PATH/Contents/Info.plist" 2>/dev/null | grep "CFBundlePackageType" | sed 's/.*=> //' | tr -d '"' || true)
if [[ "$pkg_type" != "APPL" ]]; then
    echo "    ERROR: CFBundlePackageType should be APPL, got: $pkg_type"
    exit 1
fi
echo "    OK: CFBundlePackageType is APPL"

# Verify CFBundleExecutable is 'human'
echo "  ✓ Checking CFBundleExecutable..."
exec_name=$(plutil -p "$APP_PATH/Contents/Info.plist" 2>/dev/null | grep "CFBundleExecutable" | sed 's/.*=> //' | tr -d '"' || true)
if [[ "$exec_name" != "human" ]]; then
    echo "    ERROR: CFBundleExecutable should be 'human', got: $exec_name"
    exit 1
fi
echo "    OK: CFBundleExecutable is 'human'"

# Check daemon binary
echo "  ✓ Checking daemon binary..."
[[ -f "$APP_PATH/Contents/MacOS/human" ]] || { echo "    ERROR: Daemon binary missing"; exit 1; }
[[ -x "$APP_PATH/Contents/MacOS/human" ]] || { echo "    ERROR: Daemon binary not executable"; exit 1; }
echo "    OK: Daemon binary present and executable"

# Verify no rpath entries (except ASan which is allowed in debug builds)
echo "  ✓ Checking for rpath entries..."
rpath_count=$(otool -L "$APP_PATH/Contents/MacOS/human" 2>/dev/null | grep -c "@rpath" || echo "0")
if [[ "$rpath_count" -gt 1 ]]; then
    echo "    ERROR: Binary contains multiple @rpath entries (should use system libraries)"
    otool -L "$APP_PATH/Contents/MacOS/human"
    exit 1
fi
if [[ "$rpath_count" -eq 1 ]]; then
    # Allow ASan dynamic library in debug builds
    if otool -L "$APP_PATH/Contents/MacOS/human" | grep -q "@rpath/libclang_rt.asan"; then
        echo "    OK: Only ASan @rpath entry found (debug build)"
    else
        echo "    ERROR: Binary contains unexpected @rpath entry"
        exit 1
    fi
else
    echo "    OK: No @rpath entries detected (release build)"
fi

echo ""
echo "SUCCESS: macOS app bundle is valid and ready for packaging"
exit 0
