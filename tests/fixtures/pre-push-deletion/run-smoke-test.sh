#!/usr/bin/env bash
# run-smoke-test.sh — Smoke-test for the deletion-only short-circuit in
# .githooks/pre-push (decision logic in .githooks/lib/push-refs.sh).
#
# git feeds pre-push one line per ref: <local ref> <local sha> <remote ref>
# <remote sha>. A deletion has an all-zero local sha. Only a push whose refs
# are ALL deletions may skip the build and gates.
#
# Two layers, because each can pass while the other is broken:
#   decision — prepush_deletion_only against synthetic stdin
#   wiring   — the real hook, run from an empty scratch dir. With no
#              CMakeLists.txt the non-skip path prints "No build system
#              found" and exits 0 without building, so the two paths are
#              told apart by their output, not their exit code.
#
# Cases (both layers):
#   deletion-sha1    — one ref, 40 zeros                  → skip
#   deletion-sha256  — one ref, 64 zeros                  → skip
#   deletion-multi   — two refs, both deletions           → skip
#   mixed            — a deletion plus a real branch push → run
#   normal           — one real branch push               → run
#   empty            — no refs at all                     → run
#   blank-line       — a single blank line (not a ref)    → run
#
# Exit codes:
#   0  — every case produced the expected result
#   1  — one or more cases failed
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
HOOK="$REPO_ROOT/.githooks/pre-push"
LIB="$REPO_ROOT/.githooks/lib/push-refs.sh"

# shellcheck source=../../../.githooks/lib/push-refs.sh
. "$LIB"

Z40=0000000000000000000000000000000000000000
Z64=0000000000000000000000000000000000000000000000000000000000000000
SHA_A=4f1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f7a8b9c
SHA_B=9a8b7c6d5e4f3a2b1c0d9e8f7a6b5c4d3e2f1a0b

DEL1="(delete) $Z40 refs/heads/merged-branch $SHA_A"
DEL2="(delete) $Z40 refs/heads/other-branch $SHA_B"
DEL256="(delete) $Z64 refs/heads/merged-branch ${SHA_A}${SHA_A:0:24}"
PUSH="refs/heads/feature $SHA_A refs/heads/feature $SHA_B"

SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/pre-push-deletion.XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT

FAIL=0

# expect <case> <skip|run> <stdin>
expect() {
    local name="$1" want="$2" input="$3" got out rc=0

    if printf '%s' "$input" | prepush_deletion_only; then got=skip; else got=run; fi
    if [ "$got" = "$want" ]; then
        echo "PASS  decision $name → $got"
    else
        echo "FAIL  decision $name → $got (expected $want)" >&2
        FAIL=1
    fi

    out=$(cd "$SCRATCH" && printf '%s' "$input" | sh "$HOOK" 2>&1) || rc=$?
    case "$out" in
        *"deletion-only push; skipping"*) got=skip ;;
        *"No build system found"*)        got=run ;;
        *)                                got="unrecognised" ;;
    esac
    if [ "$got" = "$want" ] && [ "$rc" -eq 0 ]; then
        echo "PASS  hook     $name → $got"
    else
        echo "FAIL  hook     $name → $got, exit $rc (expected $want, exit 0)" >&2
        printf '%s\n' "$out" | sed 's/^/      /' >&2
        FAIL=1
    fi
}

expect deletion-sha1   skip "$DEL1"$'\n'
expect deletion-sha256 skip "$DEL256"$'\n'
expect deletion-multi  skip "$DEL1"$'\n'"$DEL2"$'\n'
expect mixed           run  "$DEL1"$'\n'"$PUSH"$'\n'
expect normal          run  "$PUSH"$'\n'
expect empty           run  ""
expect blank-line      run  $'\n'

if [ $FAIL -gt 0 ]; then
    exit 1
fi

echo "OK  pre-push deletion-only smoke test passed"
exit 0
