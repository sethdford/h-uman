#!/usr/bin/env bash
# Lints staged CSS, Astro, and TS files for raw hex color values that should use
# --sc-* design tokens instead. Runs as part of .githooks/pre-commit.
#
# Usage:
#   lint-raw-colors.sh        Check only staged files (default, for pre-commit)
#   lint-raw-colors.sh --all  Check all CSS/TS/Astro files in ui/src and website/src
#
# Allowed patterns:
#   - Token definitions (--sc-*: #hex or --sc-web-*: #hex)
#   - Generated token files (_tokens.css)
#   - Design token source files (design-tokens/)
#   - SVG/asset files and build scripts
#   - Comments
set -euo pipefail

MODE="staged"
if [ "${1:-}" = "--all" ]; then
  MODE="all"
fi

if [ "$MODE" = "all" ]; then
  FILES=$(find ui/src website/src ui/index.html \( -name '*.css' -o -name '*.ts' -o -name '*.tsx' -o -name '*.astro' -o -name '*.html' \) 2>/dev/null || true)
else
  FILES=$(git diff --cached --name-only --diff-filter=ACM -- '*.css' '*.ts' '*.tsx' '*.astro' '*.html' 2>/dev/null || true)
fi

if [ -z "$FILES" ]; then
  echo "No files to check."
  exit 0
fi

VIOLATIONS=0

while IFS= read -r file; do
  case "$file" in
    */_tokens.css|design-tokens/*|*/generate-assets*|*.svg|*.json|docs/tokens.*|*/DesignTokens.*|*/design_tokens.*|website/src/pages/index.astro|ui/index.html) continue ;;
  esac

  if [ "$MODE" = "all" ]; then
    content=$(cat "$file" 2>/dev/null) || continue
  else
    content=$(git show ":$file" 2>/dev/null) || continue
  fi

  lineno=0
  in_print_media=0
  while IFS= read -r line; do
    lineno=$((lineno + 1))

    if echo "$line" | grep -q '@media print'; then
      in_print_media=1
    fi
    if [ "$in_print_media" -eq 1 ] && echo "$line" | grep -q '^}$'; then
      in_print_media=0
      continue
    fi
    [ "$in_print_media" -eq 1 ] && continue

    case "$line" in
      *"//"*|*"/*"*|*"*"*) continue ;;
    esac

    if echo "$line" | grep -qE '^\s*--sc(-web)?-[a-z0-9-]+:\s*#'; then
      continue
    fi

    # Allow hex inside var() fallbacks: var(--sc-xxx, #fff)
    fallback_cleaned=$(echo "$line" | sed -E 's/var\(--sc-[a-zA-Z0-9_-]+,[[:space:]]*#[0-9a-fA-F]{3,8}\)//g')
    # Allow hex inside color-mix() that wraps var() with hex fallback
    fallback_cleaned=$(echo "$fallback_cleaned" | sed -E 's/var\(--[a-zA-Z0-9_-]+,[[:space:]]*#[0-9a-fA-F]{3,8}\)//g')
    # Allow hex inside linear-gradient mask tricks (gradient masking pattern)
    fallback_cleaned=$(echo "$fallback_cleaned" | sed -E 's/linear-gradient\(#[0-9a-fA-F]{3,8}[^)]*\)//g')
    if ! echo "$fallback_cleaned" | grep -qE '#[0-9a-fA-F]{3,8}\b'; then
      continue
    fi

    cleaned=$(echo "$line" | sed -E 's/bg-\[#[0-9a-fA-F]{3,8}\]//g')

    if echo "$cleaned" | grep -qE '#[0-9a-fA-F]{3,8}\b'; then
      echo "  $file:$lineno: raw hex color — use a --sc-* token instead"
      echo "    $line"
      VIOLATIONS=$((VIOLATIONS + 1))
    fi
  done <<< "$content"
done <<< "$FILES"

if [ "$VIOLATIONS" -gt 0 ]; then
  echo ""
  echo "Found $VIOLATIONS raw hex color(s). Use --sc-* or --sc-web-* tokens instead."
  echo "If intentional, add to the allowlist in scripts/lint-raw-colors.sh"
  exit 1
fi

exit 0
