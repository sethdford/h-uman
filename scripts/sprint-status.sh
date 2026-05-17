#!/usr/bin/env bash
# sprint-status.sh — One-shot view of in-flight sprints + dirty paths.
#
# Sprint 2a's retro called this out: "Read sprints/ for in-flight work
# BEFORE planning. Consider a script that prints all open sprints +
# dirty paths in one shot." The wiped-tree event in Sprint 1 happened
# because the planner did not know that another agent was rewriting
# the same branch.
#
# This script answers four questions a sprint-master needs at planning:
#
#   1. Which sprints/* directories exist and what's their status?
#   2. Which files are currently dirty in the working tree (could
#      collide with implementer wave changes)?
#   3. Which branches look "sprint-shaped" and where do they point?
#   4. Are there detached worktrees that another agent might be using?
#
# Output is plain text intended for a human or another agent to read
# before invoking /scrum or /spec. No side effects. No git mutation.
#
# Usage:
#   bash scripts/sprint-status.sh           # default: full status
#   bash scripts/sprint-status.sh --terse   # summary-only (no per-sprint detail)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TERSE=0
if [[ "${1:-}" == "--terse" ]]; then
  TERSE=1
fi

hr() { printf '%s\n' "------------------------------------------------------------"; }

echo "=========================================================="
echo " sprint-status (h-uman)"
echo " repo: $ROOT"
echo " run:  $(date '+%Y-%m-%d %H:%M:%S %Z')"
echo "=========================================================="

# 1) Sprint directories
echo
echo "## Sprint directories under sprints/"
hr
if [[ -d sprints ]]; then
  found=0
  for d in sprints/sprint-*; do
    [[ -d "$d" ]] || continue
    found=1
    name="${d#sprints/}"
    # Pull status from stories.md frontmatter when present.
    status="(no stories.md)"
    if [[ -f "$d/stories.md" ]]; then
      status="$(awk -F': *' '/^status:/ { gsub(/"/, "", $2); print $2; exit }' "$d/stories.md" 2>/dev/null || true)"
      [[ -z "$status" ]] && status="(stories.md, no status frontmatter)"
    fi
    branch_hint=""
    if [[ -f "$d/stories.md" ]]; then
      branch_hint="$(awk -F': *' '/^branch:/ { gsub(/"/, "", $2); print $2; exit }' "$d/stories.md" 2>/dev/null || true)"
    fi
    has_review="(no)"
    [[ -f "$d/review.md" ]] && has_review="review"
    has_retro=""
    [[ -f "$d/retro.md" ]] && has_retro=" retro"
    has_audit=""
    [[ -f "$d/audit.md" ]] && has_audit=" audit"
    printf '  %-32s status=%-14s artifacts=[%s%s%s]' "$name" "${status:-?}" "$has_review" "$has_retro" "$has_audit"
    [[ -n "$branch_hint" ]] && printf '  branch=%s' "$branch_hint"
    printf '\n'
    if [[ "$TERSE" -eq 0 ]] && [[ -d "$d/evidence" ]]; then
      ev_count=$(find "$d/evidence" -type f 2>/dev/null | wc -l | tr -d ' ')
      [[ "$ev_count" -gt 0 ]] && echo "    evidence files: $ev_count"
    fi
  done
  [[ "$found" -eq 0 ]] && echo "  (none)"
else
  echo "  sprints/ directory not present"
fi

# 2) Dirty working tree
echo
echo "## Working-tree status"
hr
dirty="$(git status --short 2>/dev/null || true)"
if [[ -z "$dirty" ]]; then
  echo "  CLEAN — no unstaged or untracked files"
else
  modified_count=$(printf '%s\n' "$dirty" | grep -c '^ M' || true)
  added_count=$(printf '%s\n' "$dirty" | grep -c '^A ' || true)
  staged_count=$(printf '%s\n' "$dirty" | grep -c '^M ' || true)
  untracked_count=$(printf '%s\n' "$dirty" | grep -c '^?? ' || true)
  total_count=$(printf '%s\n' "$dirty" | wc -l | tr -d ' ')
  echo "  total=$total_count  modified=$modified_count  staged=$staged_count  added=$added_count  untracked=$untracked_count"
  if [[ "$total_count" -gt 5 ]]; then
    echo "  WARNING: >5 dirty files. Sprint Phase 0 rule says PREFER a worktree over a branch."
    echo "    git worktree add ../human-sprint-N sprint-N-<slug>"
  fi
  if [[ "$TERSE" -eq 0 ]]; then
    echo "  files:"
    printf '%s\n' "$dirty" | sed 's/^/    /' | head -40
    if [[ "$total_count" -gt 40 ]]; then
      echo "    ... ($((total_count - 40)) more)"
    fi
  fi
fi

# 3) Sprint-shaped branches
echo
echo "## Sprint-shaped branches"
hr
mapfile -t sprint_branches < <(git for-each-ref --format='%(refname:short)|%(objectname:short)|%(committerdate:short)' refs/heads/sprint-* 2>/dev/null | sort)
if [[ "${#sprint_branches[@]}" -eq 0 ]]; then
  echo "  (none — no refs/heads/sprint-*)"
else
  current="$(git branch --show-current 2>/dev/null || true)"
  for entry in "${sprint_branches[@]}"; do
    branch="${entry%%|*}"
    rest="${entry#*|}"
    when="${rest#*|}"
    marker=" "
    [[ "$branch" == "$current" ]] && marker="*"
    summary="$(git log -1 --format='%s' "$branch" 2>/dev/null | head -c 70 || true)"
    printf '  %s %-40s %s  %s\n' "$marker" "$branch" "$when" "$summary"
  done
fi

# 4) Worktrees
echo
echo "## Active worktrees"
hr
worktrees="$(git worktree list 2>/dev/null || true)"
if [[ -z "$worktrees" ]]; then
  echo "  (no extra worktrees)"
else
  printf '%s\n' "$worktrees" | sed 's/^/  /'
  wt_count=$(printf '%s\n' "$worktrees" | wc -l | tr -d ' ')
  if [[ "$wt_count" -gt 1 ]]; then
    echo
    echo "  NOTE: multiple worktrees detected. Verify your sprint branch isn't"
    echo "        being used by another worktree before dispatching implementers."
  fi
fi

echo
echo "=========================================================="
echo " Recommendation:"
if [[ "${total_count:-0}" -gt 5 ]] || [[ "${#sprint_branches[@]}" -gt 2 ]]; then
  echo "   Concurrent activity is high. Use a dedicated worktree for the"
  echo "   next sprint:"
  echo "     git worktree add ../human-sprint-<N> -b sprint-<N>-<slug>"
else
  echo "   Workspace is calm. A branch-only sprint isolation should be safe."
  echo "     git checkout -b sprint-<N>-<slug>"
fi
echo "=========================================================="
