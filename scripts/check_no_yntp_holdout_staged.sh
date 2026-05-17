#!/usr/bin/env bash
# Sprint 11 / US-11.6 — D1 fixture-policy pre-commit guard.
#
# Fails if `yntp_holdout_30.jsonl` (the production-tier private fixture)
# is ever staged for commit. That file MUST stay in
# `~/.human/private/yntp_holdout_30.jsonl` on Seth's machine only.
#
# Wire-up: add to `.githooks/pre-commit` or run from CI on `git diff --cached`.
# Standalone usage: `scripts/check_no_yntp_holdout_staged.sh` (returns 1 if
# the file is staged, 0 otherwise).
set -euo pipefail

# Look at staged files; if `git` isn't available or no commit is in
# progress, this still works as a generic repo scanner.
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  staged="$(git diff --cached --name-only --diff-filter=ACM 2>/dev/null || true)"
  if printf '%s\n' "$staged" | grep -E '(^|/)yntp_holdout_30\.jsonl$' >/dev/null; then
    echo "ERROR: yntp_holdout_30.jsonl is staged for commit."
    echo "       Per sprints/sprint-11/decisions.md D1, this file MUST live"
    echo "       only at ~/.human/private/yntp_holdout_30.jsonl on Seth's"
    echo "       machine. It contains real chat-derived data and must never"
    echo "       enter the repo."
    echo
    echo "       To fix: \`git restore --staged <path>\` and ensure the file"
    echo "       lives under ~/.human/private/."
    exit 1
  fi
fi

# Belt and suspenders: also scan the working tree for any committed copy.
if find . -name 'yntp_holdout_30.jsonl' -not -path './.git/*' -print -quit 2>/dev/null | grep -q .; then
  echo "ERROR: yntp_holdout_30.jsonl found inside the repo working tree:"
  find . -name 'yntp_holdout_30.jsonl' -not -path './.git/*'
  echo "       This file must NOT live in the repo. Move it to"
  echo "       ~/.human/private/yntp_holdout_30.jsonl."
  exit 1
fi

exit 0
