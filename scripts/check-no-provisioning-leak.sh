#!/usr/bin/env bash
# check-no-provisioning-leak.sh — fail nonzero if apps/ios/ExportOptions.plist contains
# a real-looking provisioning profile UUID or team ID instead of the placeholder tokens.
#
# Why: real Apple provisioning UUIDs are 36-char dash-separated hex strings (e.g.
# 12345678-1234-1234-1234-123456789012). A real Apple Team ID is a 10-char uppercase
# alphanumeric string. Either committed to this repo would tie the project to a
# specific Apple developer account. The ExportOptions.plist SHIPS WITH placeholder
# tokens (`__PROVISIONING_PROFILE_UUID__`, `__TEAM_ID__`); the wrapper script
# `scripts/ios-archive-export.sh` sed-substitutes them at run time and restores the
# committed file from `.bak` on EXIT.
#
# This guard runs in pre-commit and CI. Exit 0 if clean. Exit 1 with a clear message
# if a UUID-shaped or team-ID-shaped value was committed.
#
# Usage:
#   scripts/check-no-provisioning-leak.sh              # checks apps/ios/ExportOptions.plist
#   scripts/check-no-provisioning-leak.sh path/to/file # checks a specific file
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="${1:-$REPO_ROOT/apps/ios/ExportOptions.plist}"

if [ ! -f "$TARGET" ]; then
    echo "check-no-provisioning-leak: file not found: $TARGET" >&2
    exit 1
fi

# 1. Placeholder must be present — otherwise something stripped it.
if ! grep -q "__PROVISIONING_PROFILE_UUID__" "$TARGET"; then
    echo "check-no-provisioning-leak: placeholder __PROVISIONING_PROFILE_UUID__ MISSING from $TARGET" >&2
    echo "  -> placeholder must be present; the wrapper script substitutes it at runtime" >&2
    exit 1
fi
if ! grep -q "__TEAM_ID__" "$TARGET"; then
    echo "check-no-provisioning-leak: placeholder __TEAM_ID__ MISSING from $TARGET" >&2
    echo "  -> placeholder must be present; the wrapper script substitutes it at runtime" >&2
    exit 1
fi

# 2. A 36-char UUID pattern (8-4-4-4-12 hex) must NOT appear in the file.
#    grep -E with the canonical UUID regex; case-insensitive for hex.
UUID_RE='[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}'
if grep -Eq "$UUID_RE" "$TARGET"; then
    echo "check-no-provisioning-leak: provisioning UUID pattern detected in $TARGET" >&2
    echo "  -> never commit a real UUID; use the __PROVISIONING_PROFILE_UUID__ placeholder" >&2
    echo "  -> if scripts/ios-archive-export.sh just ran, the EXIT trap should have" >&2
    echo "     restored the placeholder from .bak. Check that trap is firing." >&2
    grep -nE "$UUID_RE" "$TARGET" >&2 || true
    exit 1
fi

# 3. A 10-char uppercase alphanumeric "team ID" pattern must NOT appear as the value
#    of the <key>teamID</key> entry. Real Apple Team IDs are exactly 10 chars,
#    uppercase A-Z + 0-9. We look for a string that follows the teamID key.
#    NOTE: the placeholder __TEAM_ID__ is 10 chars but contains underscores, so it
#    cannot match the uppercase-alphanumeric regex below.
TEAM_RE='<string>[A-Z0-9]{10}</string>'
# Find the teamID line and the next <string>...</string>; if it's [A-Z0-9]{10}, leak.
if awk '
    /<key>teamID<\/key>/ { found_key = 1; next }
    found_key && /<string>[A-Z0-9]{10}<\/string>/ { print; found = 1; exit }
    found_key && /<string>/ { found_key = 0 }
    END { exit (found ? 0 : 1) }
' "$TARGET" > /dev/null; then
    echo "check-no-provisioning-leak: real-looking Apple Team ID detected in $TARGET" >&2
    echo "  -> use the __TEAM_ID__ placeholder; the wrapper script substitutes at runtime" >&2
    exit 1
fi

echo "check-no-provisioning-leak: OK ($TARGET clean — placeholders intact, no UUID, no team ID)"
