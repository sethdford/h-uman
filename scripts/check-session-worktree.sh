#!/usr/bin/env bash
# check-session-worktree.sh — warn when committing to main in the SHARED checkout
# while other Claude sessions have live worktrees.
#
# Why: worktrees are already created per session (.claude/worktrees/<name>), but
# sessions routinely abandon them and commit in the shared main checkout instead.
# When two do it at once they collide: HEAD moves mid-operation, the working tree
# fills with another session's edits, and one session can accidentally push
# another's in-flight commits. See .claude/rules/session-worktree-isolation.md.
#
# Advisory by default (always exits 0) — a hard gate would block legitimate
# urgent main-branch fixes. Set HU_WORKTREE_STRICT=1 to make it fail instead.
set -uo pipefail

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)" || exit 0

# A linked worktree has --git-dir (.git/worktrees/<name>) != --git-common-dir (.git).
# In the shared main checkout they resolve to the same directory.
gd=$(git rev-parse --git-dir 2>/dev/null) || exit 0
cd_=$(git rev-parse --git-common-dir 2>/dev/null) || exit 0
gd_abs=$(cd "$gd" 2>/dev/null && pwd) || exit 0
cd_abs=$(cd "$cd_" 2>/dev/null && pwd) || exit 0
[ "$gd_abs" = "$cd_abs" ] || exit 0   # in a worktree already — nothing to say

branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "")
[ "$branch" = "main" ] || exit 0      # feature branch in main checkout is fine

# Count session worktrees whose HEAD is NOT yet on origin/main — i.e. live work
# someone could still be committing to.
live=0
live_names=""
while read -r _ wt; do
    case "$wt" in */.claude/worktrees/*) ;; *) continue ;; esac
    sha=$(git -C "$wt" rev-parse HEAD 2>/dev/null) || continue
    if ! git merge-base --is-ancestor "$sha" origin/main 2>/dev/null; then
        live=$((live + 1))
        live_names="${live_names}    - $(basename "$wt")
"
    fi
done < <(git worktree list --porcelain 2>/dev/null | grep '^worktree ' || true)

[ "$live" -gt 0 ] || exit 0

printf '\n'
printf 'NOTE: committing to main in the SHARED checkout while %d session worktree(s)\n' "$live"
printf '      have unmerged work. Concurrent commits here collide (HEAD moves under\n'
printf '      you; pushes race; you can pick up another session'"'"'s commits by accident).\n'
printf '%s' "$live_names"
printf '      Prefer working in your own worktree (EnterWorktree), then merging.\n'
printf '      See .claude/rules/session-worktree-isolation.md\n\n'

if [ "${HU_WORKTREE_STRICT:-0}" = "1" ]; then
    echo "HU_WORKTREE_STRICT=1 — refusing the commit." >&2
    exit 1
fi
exit 0
