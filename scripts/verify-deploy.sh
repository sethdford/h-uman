#!/usr/bin/env bash
# verify-deploy.sh — post-deploy verification with the DESCENDANT contract.
#
# In a multi-session environment, sibling sessions legitimately redeploy the
# daemon within minutes of each other. A deploy-verification contract that
# asserts "installed SHA == my commit" false-FAILs the moment a sibling lands
# a descendant on top (observed 2026-07-27: verifier flagged cc81f5f1c as a
# regression when it was e6b6af640 -> 78a059e67 -> cc81f5f1c forward motion).
# The correct invariant is:
#
#   installed SHA is my commit OR a descendant of it, AND is on origin/main
#
# Usage:
#   scripts/verify-deploy.sh <my-commit> [-m "marker string"]...
#
#   <my-commit>   the commit whose content must be in the running binary
#   -m STRING     optional: assert STRING appears in the installed binary
#                 (use for a distinctive literal your change introduced)
#
# Exit 0 with "RESULT_deploy_verify=PASS" only if ALL checks hold.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${HU_INSTALL_BIN:-$HOME/.local/bin/human-daemon}"
SERVICE="ai.human.service-loop"

fail() { echo "FAIL: $*" >&2; echo "RESULT_deploy_verify=FAIL"; exit 1; }

[ $# -ge 1 ] || fail "usage: verify-deploy.sh <my-commit> [-m marker]..."
MY_COMMIT="$1"; shift
MARKERS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -m) [ $# -ge 2 ] || fail "-m requires an argument"; MARKERS+=("$2"); shift 2 ;;
        *) fail "unknown argument: $1" ;;
    esac
done

[ -x "$BIN" ] || fail "installed binary not found/executable: $BIN"
git -C "$REPO" rev-parse --verify -q "${MY_COMMIT}^{commit}" >/dev/null \
    || fail "my-commit '$MY_COMMIT' is not a commit in $REPO"

# 1. Extract the embedded build SHA (stamped by cmake/GenGitSha.cmake).
INSTALLED_SHA="$(strings -a "$BIN" | sed -n 's/^HU_BUILD_SHA=//p' | head -1)"
[ -n "$INSTALLED_SHA" ] || fail "no HU_BUILD_SHA stamp in $BIN (pre-guard binary?)"
git -C "$REPO" cat-file -e "$INSTALLED_SHA" 2>/dev/null \
    || { git -C "$REPO" fetch origin -q || true; git -C "$REPO" cat-file -e "$INSTALLED_SHA" 2>/dev/null \
    || fail "installed SHA $INSTALLED_SHA unknown to this repo (fetch didn't help)"; }

# 2. THE CONTRACT: installed is my commit or a descendant of it...
git -C "$REPO" merge-base --is-ancestor "$MY_COMMIT" "$INSTALLED_SHA" \
    || fail "installed $INSTALLED_SHA does NOT contain $MY_COMMIT — rollback or stale binary"
# ...and installed is itself published on origin/main (not a stray local build).
git -C "$REPO" merge-base --is-ancestor "$INSTALLED_SHA" origin/main \
    || fail "installed $INSTALLED_SHA is not on origin/main — deployed from an unpublished branch"
echo "ok: installed $INSTALLED_SHA contains $MY_COMMIT and is on origin/main"

# 3. Optional content markers introduced by my change.
for m in "${MARKERS[@]:-}"; do
    [ -z "$m" ] && continue
    n="$(strings -a "$BIN" | grep -cF "$m" || true)"
    [ "$n" -ge 1 ] || fail "marker not in installed binary: '$m'"
    echo "ok: marker present (x$n): '$m'"
done

# 4. The launchd service is actually running (a binary on disk isn't a deploy).
PID="$(launchctl print "gui/$(id -u)/$SERVICE" 2>/dev/null | sed -n 's/^[[:space:]]*pid = \([0-9]*\)$/\1/p' | head -1)"
[ -n "$PID" ] || fail "service $SERVICE has no running pid"
echo "ok: $SERVICE running (pid $PID)"

# 5. Doctor is clean.
DOCTOR="$("$BIN" doctor imessage 2>&1 | grep 'Summary:' || true)"
echo "$DOCTOR" | grep -q ' 0 errors' || fail "doctor not clean: ${DOCTOR:-no Summary line}"
echo "ok: $DOCTOR"

echo "RESULT_deploy_verify=PASS"
