#!/usr/bin/env bash
# Lints staged CSS, Astro, and TS files for raw hex color values that should use
# --hu-* design tokens instead. Runs as part of .githooks/pre-commit.
#
# Usage:
#   lint-raw-colors.sh        Check only staged files (default, for pre-commit)
#   lint-raw-colors.sh --all  Check all CSS/TS/Astro files in ui/src and website/src
set -euo pipefail

MODE="staged"
if [ "${1:-}" = "--all" ]; then
  MODE="all"
fi

EXCLUDE='_tokens\.css|design-tokens/|generate-assets|\.svg$|\.json$|docs/tokens\.|DesignTokens\.|design_tokens\.|website/src/pages/index\.astro|ui/index\.html|ui/src/components/hu-chart\.ts'

if [ "$MODE" = "all" ]; then
  FILES=$(find ui/src website/src -type f \( -name '*.css' -o -name '*.ts' -o -name '*.tsx' -o -name '*.astro' -o -name '*.html' \) 2>/dev/null | grep -Ev "$EXCLUDE" || true)
else
  FILES=$(git diff --cached --name-only --diff-filter=ACM -- '*.css' '*.ts' '*.tsx' '*.astro' '*.html' 2>/dev/null | grep -Ev "$EXCLUDE" || true)
fi

if [ -z "$FILES" ]; then
  echo "No files to check."
  exit 0
fi

VIOLATIONS=0

while IFS= read -r file; do
  [ -f "$file" ] || continue

  if [ "$MODE" = "all" ]; then
    content=$(cat "$file" 2>/dev/null) || continue
  else
    content=$(git show ":$file" 2>/dev/null) || continue
  fi

  matches=$(printf '%s' "$content" | grep -nE '#[0-9a-fA-F]{3,8}([^0-9a-zA-Z_-]|$)' \
    | grep -vE '^[0-9]+:[[:space:]]*//' \
    | grep -vE '^[0-9]+:[[:space:]]*\*' \
    | grep -vE '^[0-9]+:[[:space:]]*/\*' \
    | grep -v '@media print' \
    | grep -vE '^[0-9]+:[[:space:]]*--sc(-web)?-[a-z0-9-]+:[[:space:]]*#' || true)

  [ -z "$matches" ] && continue

  while IFS= read -r match; do
    lineno="${match%%:*}"
    line="${match#*:}"

    cleaned=$(printf '%s' "$line" | sed -E \
      -e 's/var\(--[a-zA-Z0-9_-]+,[[:space:]]*#[0-9a-fA-F]{3,8}\)//g' \
      -e 's/linear-gradient\(#[0-9a-fA-F]{3,8}[^)]*\)//g' \
      -e 's/bg-\[#[0-9a-fA-F]{3,8}\]//g')

    if printf '%s' "$cleaned" | grep -qE '#[0-9a-fA-F]{3,8}([^0-9a-zA-Z_-]|$)'; then
      echo "  $file:$lineno: raw hex color -- use a --hu-* token instead"
      echo "    $line"
      VIOLATIONS=$((VIOLATIONS + 1))
    fi
  done <<< "$matches"
done <<< "$FILES"

if [ "$VIOLATIONS" -gt 0 ]; then
  echo ""
  echo "Found $VIOLATIONS raw hex color(s). Use --hu-* or --hu-web-* tokens instead."
  echo "If intentional, add to the allowlist in scripts/lint-raw-colors.sh"
  exit 1
fi

exit 0
