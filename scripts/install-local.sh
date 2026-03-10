#!/usr/bin/env bash
# Install human binary locally with code signing for persistent Full Disk Access.
#
# Usage:
#   ./scripts/install-local.sh              # install from default build dir
#   ./scripts/install-local.sh build-release # install from a specific build dir
#
# On macOS, signs with "Human Local Dev" identity so FDA survives rebuilds.
# Create the identity once in Keychain Access (Certificate Assistant > Create a Certificate)
# or via: security create-identity -s "Human Local Dev" ~/Library/Keychains/login.keychain-db

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-build}"
SRC="${REPO_ROOT}/${BUILD_DIR}/human"
DEST="${HOME}/bin/human"
SIGN_IDENTITY="Human Local Dev"

if [ ! -f "$SRC" ]; then
    echo "error: $SRC not found — build first with: cmake --build ${BUILD_DIR}" >&2
    exit 1
fi

mkdir -p "$(dirname "$DEST")"

# Stop running daemon
if [ -f "${HOME}/.human/human.pid" ]; then
    PID=$(cat "${HOME}/.human/human.pid" 2>/dev/null || true)
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        echo "Stopping running daemon (PID $PID)..."
        kill "$PID" 2>/dev/null || true
        sleep 1
    fi
    rm -f "${HOME}/.human/human.pid"
fi

cp "$SRC" "$DEST"
echo "Installed: $DEST"

# Code-sign on macOS to preserve Full Disk Access across rebuilds
if [ "$(uname)" = "Darwin" ]; then
    if security find-identity -v -p codesigning 2>/dev/null | grep -q "$SIGN_IDENTITY"; then
        codesign --force --sign "$SIGN_IDENTITY" "$DEST" 2>/dev/null
        echo "Signed with \"$SIGN_IDENTITY\" — FDA persists across rebuilds"
    else
        echo "warning: signing identity \"$SIGN_IDENTITY\" not found"
        echo "  FDA will be revoked on next rebuild. Create the identity with:"
        echo "  security create-identity -s \"$SIGN_IDENTITY\" ~/Library/Keychains/login.keychain-db"
    fi
fi

echo "Done. Start with: human service-loop --with-gateway"
