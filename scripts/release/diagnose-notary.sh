#!/bin/bash
set -euo pipefail

##
# diagnose-notary.sh — Translate Apple's notarization failures into human-readable fixes.
#
# Usage:
#   ./scripts/release/diagnose-notary.sh --log /path/to/notary.json
#   ./scripts/release/diagnose-notary.sh --submission-id <id>
#   ./scripts/release/diagnose-notary.sh --help
#
# Exit code: number of issues found (0 = clean, >0 = issue count)
##

show_help() {
    cat <<'EOF'
diagnose-notary.sh — Translate Apple notarization failures into actionable fixes

Usage:
  ./scripts/release/diagnose-notary.sh --log <path>         Read notary log JSON from file
  ./scripts/release/diagnose-notary.sh --submission-id <id>  Fetch notary log from Apple
  ./scripts/release/diagnose-notary.sh --help                Show this help text

Exit code: number of issues found (0 = clean)

Examples:
  # Local JSON file (e.g., from xcrun notarytool log)
  ./scripts/release/diagnose-notary.sh --log /tmp/notary.json

  # Fetch from Apple (requires xcrun notarytool)
  ./scripts/release/diagnose-notary.sh --submission-id abcd-1234-efgh-5678

EOF
}

log_file=""
submission_id=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --log)
            log_file="$2"
            shift 2
            ;;
        --submission-id)
            submission_id="$2"
            shift 2
            ;;
        --help|-h)
            show_help
            exit 0
            ;;
        *)
            echo "Error: unknown option '$1'" >&2
            show_help >&2
            exit 1
            ;;
    esac
done

if [[ -z "$log_file" ]] && [[ -z "$submission_id" ]]; then
    echo "Error: --log or --submission-id required" >&2
    show_help >&2
    exit 1
fi

if [[ -n "$submission_id" ]]; then
    if ! command -v xcrun &>/dev/null; then
        echo "Error: xcrun not found; install Xcode or set --log <path>" >&2
        exit 1
    fi
    log_file=$(mktemp)
    trap "rm -f '$log_file'" EXIT
    if ! xcrun notarytool log "$submission_id" --output-file "$log_file" 2>/dev/null; then
        echo "Error: failed to fetch notary log for submission '$submission_id'" >&2
        exit 1
    fi
fi

if [[ ! -f "$log_file" ]]; then
    echo "Error: log file not found: $log_file" >&2
    exit 1
fi

# Count issues by counting "issues" arrays
# Note: grep returns 1 when no matches found, so we need to handle that
issue_count=$(grep -o '"severity"' "$log_file" 2>/dev/null | wc -l || true)
issue_count="${issue_count// /}"  # Remove leading/trailing spaces

if [[ "$issue_count" -eq 0 ]]; then
    echo "Notarization: CLEAN (no issues)" >&2
    exit 0
fi

# Read entire file into memory for pattern matching
json_content=$(cat "$log_file")

# Extract messages and translate them
exit_code=0

# Use grep to find all "message" lines
while IFS= read -r msg_line; do
    # Extract the message text after "message": "
    if [[ "$msg_line" =~ \"message\"[[:space:]]*:[[:space:]]*\"([^\"]+)\" ]]; then
        message="${BASH_REMATCH[1]}"

        # Translate known patterns
        if [[ "$message" =~ "SDK older" ]]; then
            echo "ISSUE: Code built with old SDK"
            echo "FIX: rebuild with newer macOS SDK (10.15+)"
            echo "COMMAND: cmake --preset release && cmake --build build"
            echo ""
            ((exit_code++))
        elif [[ "$message" =~ "timestamp" ]]; then
            echo "ISSUE: Missing code signature timestamp"
            echo "FIX: add --timestamp flag when signing"
            echo "COMMAND: codesign -s <cert-id> --timestamp --options runtime <app>"
            echo ""
            ((exit_code++))
        elif [[ "$message" =~ "hardened runtime" ]]; then
            echo "ISSUE: Missing hardened runtime entitlement"
            echo "FIX: enable hardened runtime when signing"
            echo "COMMAND: codesign -s <cert-id> --options runtime <app>"
            echo ""
            ((exit_code++))
        elif [[ "$message" =~ "entitlement" ]]; then
            echo "ISSUE: Unsupported or restricted entitlement"
            echo "FIX: remove sensitive entitlements (keychain, hardware access)"
            echo "COMMAND: Review entitlements.plist; remove restricted keys"
            echo ""
            ((exit_code++))
        elif [[ "$message" =~ "CFBundleVersion" ]]; then
            echo "ISSUE: Missing or invalid CFBundleVersion"
            echo "FIX: set CFBundleVersion in Info.plist"
            echo "COMMAND: Update Info.plist with valid version (e.g., '1.0.0')"
            echo ""
            ((exit_code++))
        elif [[ "$message" =~ "certificate" ]] || [[ "$message" =~ "cert" ]] || [[ "$message" =~ "signature" ]]; then
            echo "ISSUE: Code signature or certificate invalid"
            echo "FIX: check certificate is valid and code is properly signed"
            echo "COMMAND: codesign -v --deep <app>"
            echo ""
            ((exit_code++))
        else
            echo "ISSUE: Notarization rejected (unknown reason)"
            echo "MESSAGE: $message"
            echo "FIX: See Apple's notarization guide: https://developer.apple.com/documentation/security/notarizing_macos_software_before_distribution"
            echo ""
            ((exit_code++))
        fi
    fi
done < <(grep '"message"' "$log_file")

exit "$exit_code"
