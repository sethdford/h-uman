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

# pkgbuild only recognizes a directory as an app bundle when it has an
# Info.plist; without one the component plist below comes back empty.
if [[ ! -f "$APP_PATH/Contents/Info.plist" ]]; then
    echo "ERROR: $APP_PATH has no Contents/Info.plist, so it is not a valid app bundle"
    echo "       (write one with scripts/release/write-info-plist.sh)"
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

# Create component.plist for pkgbuild.
#
# pkgbuild requires a property list whose ROOT is an array of dictionaries,
# one per bundle, with RootRelativeBundlePath relative to --root. A hand-written
# bare <dict> ("Human.app", no array, no plist wrapper) fails with "Component
# property list is not an array of dictionaries" — which is how every macOS
# Release run on main failed. Let pkgbuild describe the staged bundle itself,
# then set the policy keys on that entry.
COMPONENT_PLIST=$(mktemp)
trap "rm -rf '$STAGE' '$COMPONENT_PLIST'" EXIT

pkgbuild --analyze --root "$STAGE" "$COMPONENT_PLIST" >/dev/null
# Not relocatable: install to /Applications, never over a stray copy of the
# app found elsewhere on disk (e.g. a build tree).
plutil -replace 0.BundleIsRelocatable -bool NO "$COMPONENT_PLIST"
plutil -replace 0.BundleIsVersionChecked -bool NO "$COMPONENT_PLIST"
plutil -replace 0.BundleOverwriteAction -string update "$COMPONENT_PLIST"
if [[ "$(plutil -extract 0.RootRelativeBundlePath raw "$COMPONENT_PLIST")" != "Applications/Human.app" ]]; then
    echo "ERROR: pkgbuild --analyze did not find Applications/Human.app in the staging root"
    exit 1
fi

# Build component package
# Named after its identifier because distribution.xml.template's
# <pkg-ref>com.h-uman.human.pkg</pkg-ref> resolves to a FILE of that name under
# --package-path. As "component.pkg" productbuild found nothing to include and
# still exited 0, writing a ~1 KB installer with an empty payload.
COMPONENT_PKG=$(mktemp -d)/com.h-uman.human.pkg
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

# Validate distribution.xml. productbuild has no --validate-only option (the
# check that used to live here always "failed" and only warned), and a
# malformed file is exactly what broke productbuild before, so fail hard.
if ! xmllint --noout "$DIST_XML" 2>/dev/null; then
    echo "ERROR: distribution.xml is not well-formed XML:"
    xmllint --noout "$DIST_XML" 2>&1 | head -5
    exit 1
fi

# Build final distribution package
if [[ "$DRY_RUN" == 1 ]]; then
    echo "DRY RUN: Would run:"
    echo "  productbuild --distribution '$DIST_XML' \\"
    echo "               --package-path '$(dirname "$COMPONENT_PKG")' \\"
    echo "               '$OUTPUT'"
    echo "DRY RUN: Would verify: file '$OUTPUT' (should be xar archive)"
    echo "DRY RUN: Would verify: payload includes Applications/Human.app/Contents/MacOS/human"
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

# Measure the artifact, not the exit code: productbuild exits 0 even when the
# distribution references a package it cannot find, producing an installer
# that installs nothing. Refuse unless the payload really carries the binary.
if ! pkgutil --payload-files "$OUTPUT" 2>/dev/null | grep -qx './Applications/Human.app/Contents/MacOS/human'; then
    echo "ERROR: $OUTPUT does not install Applications/Human.app/Contents/MacOS/human"
    echo "       (payload: $(pkgutil --payload-files "$OUTPUT" 2>/dev/null | wc -l | tr -d ' ') entries)"
    exit 1
fi
SIZE_BYTES=$(stat -f%z "$OUTPUT" 2>/dev/null || stat -c%s "$OUTPUT" 2>/dev/null)
echo "  Output size: $((SIZE_BYTES / 1024)) KB; payload includes Human.app"

# Unsigned is expected until the notarization story (US-C1.x) lands.
if pkgutil --check-signature "$OUTPUT" >/dev/null 2>&1; then
    echo "  Signature:  present"
else
    echo "  Signature:  none (unsigned build)"
fi

echo ""
echo "SUCCESS: .pkg installer built at $OUTPUT"
exit 0
