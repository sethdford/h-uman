#!/usr/bin/env bash
# check-formula-install.sh — Homebrew smoke installer for h-uman.
#
# Performs:
#   brew tap <owner/repo>
#   brew install --build-from-source <tap>/human
#   human --version  → assert last whitespace-delimited field == "${tag#v}"
#
# Strict equality. On mismatch, prints BOTH values and exits non-zero.
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
usage: $PROG --tap <owner/repo> --tag <vX.Y.Z>

Taps the given Homebrew tap, installs human --build-from-source, and
asserts that the binary reports the expected version. Prints both
expected and actual values on mismatch.
USAGE
    exit 2
}

TAP=""
TAG=""

while [ $# -gt 0 ]; do
    case "$1" in
        --tap)  TAP="${2:-}"; shift 2 ;;
        --tag)  TAG="${2:-}"; shift 2 ;;
        -h|--help) usage ;;
        *) die "unknown argument: $1" ;;
    esac
done

[ -n "$TAP" ] || { printf 'error: --tap is required\n' >&2; usage; }
[ -n "$TAG" ] || { printf 'error: --tag is required\n' >&2; usage; }

case "$TAP" in
    */*) : ;;
    *)   die "tap must be of the form owner/repo, got: $TAP" ;;
esac

EXPECTED="${TAG#v}"
if [ "$EXPECTED" = "$TAG" ]; then
    die "tag must start with 'v', got: $TAG"
fi
if ! printf '%s' "$EXPECTED" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z][0-9A-Za-z.-]*)?$'; then
    die "tag does not contain a valid semver: $TAG"
fi

command -v brew >/dev/null 2>&1 || die "brew not found on PATH"

printf 'tap: %s\n' "$TAP"
printf 'expected version: %s\n' "$EXPECTED"

brew tap "$TAP"
brew install --build-from-source "${TAP}/human"

if ! command -v human >/dev/null 2>&1; then
    die "human binary not on PATH after brew install"
fi

ACTUAL_LINE="$(human --version 2>&1 | head -n 1)"
ACTUAL="$(printf '%s' "$ACTUAL_LINE" | awk '{print $NF}')"

if [ "$ACTUAL" != "$EXPECTED" ]; then
    printf 'version mismatch:\n'  >&2
    printf '  expected: %s\n' "$EXPECTED" >&2
    printf '  actual:   %s\n' "$ACTUAL"   >&2
    printf '  raw line: %s\n' "$ACTUAL_LINE" >&2
    exit 1
fi

printf 'ok: human --version reports %s (matches tag %s)\n' "$ACTUAL" "$TAG"
