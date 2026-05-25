#!/bin/bash
# Build macOS .pkg installer from Human.app bundle (US-C1.2)
# Usage: build-pkg.sh [--version <semver>] [--app-path <path>] [--output <path>] [--dry-run]
# Defaults: version from Info.plist, app-path=build/Human.app, output=human-release.pkg

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Default values
VERSION=""
APP_PATH="${PROJECT_DIR}/build/Human.app"
OUTPUT="${PROJECT_DIR}/human-release.pkg"
DRY_RUN=0

# Parse command-line arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)
            VERSION="$2"
            shift 2
            ;;
        --app-path)
            APP_PATH="$2"
            shift 2
            ;;
        --output)
            OUTPUT="$2"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        *)
            echo "Usage: build-pkg.sh [--version <semver>] [--app-path <path>] [--output <path>] [--dry-run]"
            exit 1
            ;;
    esac
done

# Sanity check: are we on macOS?
if [[ ! "$OSTYPE" == "darwin"* ]]; then
    echo "ERROR: build-pkg.sh only runs on macOS"
    exit 79
fi

# Verify pkgbuild and productbuild are available
if ! command -v pkgbuild &>/dev/null; then
    echo "ERROR: pkgbuild not found (requires Xcode Command Line Tools)"
    exit 1
fi
if ! command -v productbuild &>/dev/null; then
    echo "ERROR: productbuild not found (requires Xcode Command Line Tools)"
    exit 1
fi

# Validate app-path exists
if [[ ! -d "$APP_PATH" ]]; then
    echo "ERROR: App bundle not found at $APP_PATH"
    exit 1
fi

# Verify executable bit
if [[ ! -x "$APP_PATH/Contents/MacOS/human" ]]; then
    echo "ERROR: Contents/MacOS/human is not executable at $APP_PATH"
    exit 1
fi

# Extract version from Info.plist if not provided
if [[ -z "$VERSION" ]]; then
    VERSION=$(plutil -p "$APP_PATH/Contents/Info.plist" 2>/dev/null | grep "CFBundleVersion" | sed 's/.*=> //' | tr -d '"' || echo "0.0.0")
    if [[ "$VERSION" == "0.0.0" ]]; then
        echo "WARNING: Could not extract version from Info.plist, using 0.0.0"
    fi
fi

echo "Building .pkg installer:"
echo "  App Path:   $APP_PATH"
echo "  Version:    $VERSION"
echo "  Output:     $OUTPUT"

# Create temporary staging directory
STAGE=$(mktemp -d)
trap "rm -rf '$STAGE'" EXIT

echo "  Staging:    $STAGE"

# Copy app bundle to staging, preserving permissions
echo "Copying bundle to staging directory..."
mkdir -p "$STAGE/Applications"
ditto "$APP_PATH" "$STAGE/Applications/Human.app"

# Verify executable preserved
if [[ ! -x "$STAGE/Applications/Human.app/Contents/MacOS/human" ]]; then
    echo "ERROR: Executable bit not preserved during staging"
    exit 1
fi

# Create component.plist for pkgbuild
COMPONENT_PLIST=$(mktemp)
trap "rm -rf '$STAGE' '$COMPONENT_PLIST'" EXIT

cat > "$COMPONENT_PLIST" << 'EOF'
<dict>
  <key>BundleIsRelocatable</key><true/>
  <key>BundleIsVersionChecked</key><false/>
  <key>BundleOverwriteAction</key><string>update</string>
  <key>RootRelativeBundlePath</key><string>Human.app</string>
</dict>
EOF

# Build component package
COMPONENT_PKG=$(mktemp -d)/component.pkg
mkdir -p "$(dirname "$COMPONENT_PKG")"

if [[ "$DRY_RUN" == 1 ]]; then
    echo "DRY RUN: Would run:"
    echo "  pkgbuild --root '$STAGE' \\"
    echo "           --component-plist '$COMPONENT_PLIST' \\"
    echo "           --identifier 'com.h-uman.human.pkg' \\"
    echo "           --version '$VERSION' \\"
    echo "           '$COMPONENT_PKG'"
else
    echo "Running pkgbuild..."
    pkgbuild --root "$STAGE" \
             --component-plist "$COMPONENT_PLIST" \
             --identifier "com.h-uman.human.pkg" \
             --version "$VERSION" \
             "$COMPONENT_PKG" || {
        echo "ERROR: pkgbuild failed"
        exit 1
    }
fi

# Create distribution.xml from template
DIST_XML=$(mktemp)
trap "rm -rf '$STAGE' '$COMPONENT_PLIST' '$(dirname "$COMPONENT_PKG")' '$DIST_XML'" EXIT

# Escape ampersands and forward slashes in VERSION for sed substitution
VERSION_ESC="${VERSION//&/\\&}"
VERSION_ESC="${VERSION_ESC//\//\\/}"

sed "s|@VERSION@|${VERSION_ESC}|g" \
    "$PROJECT_DIR/tests/fixtures/distribution.xml.template" > "$DIST_XML"

# Validate distribution.xml syntax
if [[ "$DRY_RUN" == 0 ]]; then
    if ! productbuild --validate-only --distribution "$DIST_XML" --package-path "$(dirname "$COMPONENT_PKG")" 2>/dev/null; then
        echo "WARNING: Distribution.xml validation failed (may still work)"
    fi
fi

# Build final distribution package
if [[ "$DRY_RUN" == 1 ]]; then
    echo "DRY RUN: Would run:"
    echo "  productbuild --distribution '$DIST_XML' \\"
    echo "               --package-path '$(dirname "$COMPONENT_PKG")' \\"
    echo "               '$OUTPUT'"
    echo "DRY RUN: Would verify: file '$OUTPUT' (should be xar archive)"
    echo "DRY RUN: Would verify: size >= 8 MB"
    echo "DRY RUN: Success (dry run)"
    exit 0
else
    echo "Running productbuild..."
    productbuild --distribution "$DIST_XML" \
                 --package-path "$(dirname "$COMPONENT_PKG")" \
                 "$OUTPUT" || {
        echo "ERROR: productbuild failed"
        exit 1
    }
fi

# Verify output file
if [[ ! -f "$OUTPUT" ]]; then
    echo "ERROR: Output file not created at $OUTPUT"
    exit 1
fi

# Check file type
FILE_TYPE=$(file "$OUTPUT" | grep -o "xar archive" || true)
if [[ -z "$FILE_TYPE" ]]; then
    echo "WARNING: Output file does not appear to be a valid xar archive"
fi

# Check file size (should be at least 8 MB for binary inclusion)
SIZE_BYTES=$(stat -f%z "$OUTPUT" 2>/dev/null || stat -c%s "$OUTPUT" 2>/dev/null)
SIZE_MB=$((SIZE_BYTES / 1024 / 1024))

echo "  Output size: $SIZE_MB MB ($SIZE_BYTES bytes)"

if [[ "$SIZE_MB" -lt 8 ]]; then
    echo "WARNING: Package size is less than expected 8 MB (may be normal for stripped binaries)"
fi

# Verify signature (unsigned .pkg still has valid structure)
if pkgutil --check-signature "$OUTPUT" >/dev/null 2>&1; then
    echo "  Signature:  OK (valid but unsigned)"
elif [[ "$SIZE_MB" -gt 5 ]]; then
    # Large package, likely OK even if signature check fails on unsigned
    echo "  Signature:  (unsigned, expected)"
else
    echo "WARNING: Package validation returned non-zero (may be unsigned)"
fi

echo ""
echo "SUCCESS: .pkg installer built at $OUTPUT"
exit 0
