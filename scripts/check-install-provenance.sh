#!/usr/bin/env bash
# check-install-provenance.sh — refuse to install a daemon binary built from a
# commit that is NOT a descendant of the commit the installed binary was built
# from.
#
# Why: on 2026-07-25 a session deployed ecf039650 (hu_graph_ground_compose) at
# 17:43, and at 18:32 a concurrent session installed a binary built from a
# pre-merge tree — silently un-deploying the newer work. The guard-sentinel in
# install-human-daemon.sh checks for specific guard SYMBOLS, but symbol checks
# can't see arbitrary regressions; commit ancestry can. Binaries embed their
# build commit as an "HU_BUILD_SHA=<sha>" literal (cmake/GenGitSha.cmake →
# src/app/version.c), extracted here via `strings`.
#
# Usage: check-install-provenance.sh <candidate-binary> <installed-binary> [repo-root]
#
# Exit codes:
#   0 — OK to install (descendant, same commit, or advisory: SHA unavailable,
#       commit unknown to the repo, or HU_INSTALL_FORCE=1 override)
#   1 — REFUSE: candidate commit is not a descendant of the installed commit
#   2 — usage error
#
# Advisory mode (warn, exit 0) covers: pre-provenance binaries with no embedded
# SHA, "unknown" stamps (non-git build trees), and SHAs the local repo has
# never fetched — so the first rollout of this guard cannot brick installs.

set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 <candidate-binary> <installed-binary> [repo-root]" >&2
    exit 2
fi

CANDIDATE="$1"
INSTALLED="$2"
REPO_ROOT="${3:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# Extract the embedded build SHA from a binary. Dump `strings` into a variable
# first: piping `strings | grep -m1` under pipefail kills strings with SIGPIPE
# on early grep exit and the whole pipeline reads as failed (same trap as the
# guard-sentinel in install-human-daemon.sh). Prints "" when absent.
extract_build_sha() {
    local dump line
    dump="$(strings -a "$1" 2>/dev/null || true)"
    line="$(grep -m1 '^HU_BUILD_SHA=' <<<"$dump" || true)"
    printf '%s\n' "${line#HU_BUILD_SHA=}"
}

advisory() {
    echo "warning: install provenance check is ADVISORY-ONLY this run: $*" >&2
    echo "         (proceeding — ancestry cannot be verified)" >&2
    exit 0
}

if [[ ! -f "$INSTALLED" ]]; then
    echo "==> provenance: no installed binary at $INSTALLED — first install, nothing to compare"
    exit 0
fi

CAND_SHA="$(extract_build_sha "$CANDIDATE")"
INST_SHA="$(extract_build_sha "$INSTALLED")"

[[ "$CAND_SHA" =~ ^[0-9a-f]{40}$ ]] || advisory "candidate binary has no valid embedded SHA ('${CAND_SHA:-none}')"
[[ "$INST_SHA" =~ ^[0-9a-f]{40}$ ]] || advisory "installed binary has no valid embedded SHA ('${INST_SHA:-none}') — predates provenance stamping"

if [[ "$CAND_SHA" == "$INST_SHA" ]]; then
    echo "==> provenance: candidate and installed binaries are both from $CAND_SHA"
    exit 0
fi

git -C "$REPO_ROOT" cat-file -e "$INST_SHA^{commit}" 2>/dev/null ||
    advisory "installed commit $INST_SHA is not in this repo (built elsewhere?)"
git -C "$REPO_ROOT" cat-file -e "$CAND_SHA^{commit}" 2>/dev/null ||
    advisory "candidate commit $CAND_SHA is not in this repo"

if git -C "$REPO_ROOT" merge-base --is-ancestor "$INST_SHA" "$CAND_SHA"; then
    echo "==> provenance: candidate $CAND_SHA is a descendant of installed $INST_SHA"
    exit 0
fi

if [[ "${HU_INSTALL_FORCE:-0}" == "1" ]]; then
    echo "WARNING: HU_INSTALL_FORCE=1 — installing candidate $CAND_SHA even though it is" >&2
    echo "         NOT a descendant of the installed commit $INST_SHA." >&2
    echo "         This is a DELIBERATE ROLLBACK/SIDEGRADE; newer deployed work may be lost." >&2
    exit 0
fi

echo "error: refusing to install — build-provenance regression detected." >&2
echo "       installed binary was built from: $INST_SHA" >&2
echo "       candidate binary was built from: $CAND_SHA" >&2
echo "       The candidate's commit is NOT a descendant of the installed one, so" >&2
echo "       installing it would silently un-deploy newer work (the 2026-07-25" >&2
echo "       clobber class). Merge/rebase onto the deployed commit and rebuild," >&2
echo "       or set HU_INSTALL_FORCE=1 for a deliberate rollback (logged loudly)." >&2
exit 1
