#!/usr/bin/env bash
# update-formula-hashes.sh — idempotent updater for Formula/human.rb
#
# Mutates ONLY the `version` field and the three per-arch `sha256` lines
# given a `sha256sums.txt` produced by the release build. All-or-nothing:
# if any of the three required artifact entries is missing from the
# sha256sums file, exits non-zero and leaves the formula UNTOUCHED.
#
# Re-running with identical inputs is a byte-identical no-op.
#
# US-43.1 — Sprint 43 Distribution MVP.
set -euo pipefail
IFS=$'\n\t'

PROG="$(basename "$0")"

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat >&2 <<USAGE
usage: $PROG --formula <path> --version <semver> --sha256sums <path>

Updates Formula/human.rb in place to the given version and per-arch
sha256 hashes read from sha256sums.txt.

Required entries in sha256sums.txt (one per line, "<sha>  <file>"):
  human-macos-aarch64.bin
  human-linux-aarch64.bin
  human-linux-x86_64.bin

Semver format: MAJOR.MINOR.PATCH with optional -prerelease suffix.
All-or-nothing: missing any required entry → exit 1, formula unchanged.
USAGE
    exit 2
}

FORMULA=""
VERSION=""
SHA_FILE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --formula)    FORMULA="${2:-}"; shift 2 ;;
        --version)    VERSION="${2:-}"; shift 2 ;;
        --sha256sums) SHA_FILE="${2:-}"; shift 2 ;;
        -h|--help)    usage ;;
        *)            die "unknown argument: $1" ;;
    esac
done

[ -n "$FORMULA" ]  || { printf 'error: --formula is required\n' >&2; usage; }
[ -n "$VERSION" ]  || { printf 'error: --version is required\n' >&2; usage; }
[ -n "$SHA_FILE" ] || { printf 'error: --sha256sums is required\n' >&2; usage; }

[ -f "$FORMULA" ]  || die "formula not found: $FORMULA"
[ -f "$SHA_FILE" ] || die "sha256sums file not found: $SHA_FILE"
[ -s "$SHA_FILE" ] || die "sha256sums file is empty: $SHA_FILE"

# Semver: MAJOR.MINOR.PATCH with optional -prerelease.
# Accepts: 1.2.3, 0.5.0, 1.2.3-rc.1, 1.0.0-alpha.beta+build (build metadata stripped here is OK)
# Rejects: 1.2, 1.2.3.4, v1.2.3, 1.2.3-, abc.def.ghi
if ! printf '%s' "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z][0-9A-Za-z.-]*)?$'; then
    die "invalid semver: $VERSION"
fi

extract_sha() {
    # Match a line of the form "<64-hex>  <filename>" (or with single space).
    local file="$1"
    local sha
    sha="$(awk -v f="$file" '$2 == f { print $1; exit }' "$SHA_FILE")"
    if [ -z "$sha" ]; then
        return 1
    fi
    if ! printf '%s' "$sha" | grep -Eq '^[0-9a-f]{64}$'; then
        return 1
    fi
    printf '%s' "$sha"
}

# All-or-nothing: collect all three before touching the formula.
SHA_MACOS_ARM64=""
SHA_LINUX_ARM64=""
SHA_LINUX_X86_64=""
MISSING=""

if ! SHA_MACOS_ARM64="$(extract_sha human-macos-aarch64.bin)"; then
    MISSING="$MISSING human-macos-aarch64.bin"
fi
if ! SHA_LINUX_ARM64="$(extract_sha human-linux-aarch64.bin)"; then
    MISSING="$MISSING human-linux-aarch64.bin"
fi
if ! SHA_LINUX_X86_64="$(extract_sha human-linux-x86_64.bin)"; then
    MISSING="$MISSING human-linux-x86_64.bin"
fi

if [ -n "$MISSING" ]; then
    die "missing or malformed sha256 entries for:$MISSING (formula unchanged)"
fi

# Build the new formula contents in a tempfile. We mutate FOUR lines only:
# 1. the `version "..."` line
# 2-4. the three `sha256 "..."` lines, in order: macos-aarch64, linux-aarch64,
#      linux-x86_64. We match by the url= line preceding each sha256.
TMP="$(mktemp "${TMPDIR:-/tmp}/human-formula.XXXXXX")"
trap 'rm -f "$TMP"' EXIT

awk -v ver="$VERSION" \
    -v sha_macos="$SHA_MACOS_ARM64" \
    -v sha_linux_arm="$SHA_LINUX_ARM64" \
    -v sha_linux_x86="$SHA_LINUX_X86_64" '
BEGIN { last_url = "" }
{
    line = $0

    # version field
    if (line ~ /^[ \t]*version[ \t]+"[^"]*"[ \t]*$/) {
        sub(/"[^"]*"/, "\"" ver "\"", line)
        print line
        next
    }

    # Track most recent url= line so we know which sha256 follows.
    if (line ~ /^[ \t]*url[ \t]+"[^"]*"[ \t]*$/) {
        if (line ~ /human-macos-aarch64\.bin/) {
            last_url = "macos_arm64"
            # Rewrite version path in url too.
            sub(/\/v[0-9][0-9A-Za-z.+-]*\//, "/v" ver "/", line)
        } else if (line ~ /human-linux-aarch64\.bin/) {
            last_url = "linux_arm64"
            sub(/\/v[0-9][0-9A-Za-z.+-]*\//, "/v" ver "/", line)
        } else if (line ~ /human-linux-x86_64\.bin/) {
            last_url = "linux_x86_64"
            sub(/\/v[0-9][0-9A-Za-z.+-]*\//, "/v" ver "/", line)
        } else {
            last_url = ""
        }
        print line
        next
    }

    if (line ~ /^[ \t]*sha256[ \t]+"[0-9a-f]+"[ \t]*$/ && last_url != "") {
        if (last_url == "macos_arm64") {
            sub(/"[0-9a-f]+"/, "\"" sha_macos "\"", line)
        } else if (last_url == "linux_arm64") {
            sub(/"[0-9a-f]+"/, "\"" sha_linux_arm "\"", line)
        } else if (last_url == "linux_x86_64") {
            sub(/"[0-9a-f]+"/, "\"" sha_linux_x86 "\"", line)
        }
        last_url = ""
        print line
        next
    }

    print line
}
' "$FORMULA" > "$TMP"

# Validate the tempfile re-parses as Ruby before atomic mv.
if command -v ruby >/dev/null 2>&1; then
    if ! ruby -c "$TMP" >/dev/null 2>&1; then
        die "post-update formula failed Ruby parse check (formula unchanged)"
    fi
fi

# Sanity: the result MUST contain the new version exactly once on a
# version-keyword line, AND all three new shas, AND zero placeholder
# all-zero sha256 entries. If any check fails, leave formula unchanged.
VERSION_HITS=$(grep -cE '^[[:space:]]*version[[:space:]]+"'"$VERSION"'"[[:space:]]*$' "$TMP" || true)
if [ "$VERSION_HITS" -ne 1 ]; then
    die "post-update formula does not contain exactly one version line (formula unchanged)"
fi

for sha in "$SHA_MACOS_ARM64" "$SHA_LINUX_ARM64" "$SHA_LINUX_X86_64"; do
    if ! grep -q "\"$sha\"" "$TMP"; then
        die "post-update formula is missing expected sha (formula unchanged)"
    fi
done

if grep -qE 'sha256[[:space:]]+"0{64}"' "$TMP"; then
    die "post-update formula still contains placeholder sha256 (formula unchanged)"
fi

# Atomic in-place replace.
mv "$TMP" "$FORMULA"
trap - EXIT

printf 'updated %s to version %s\n' "$FORMULA" "$VERSION"
