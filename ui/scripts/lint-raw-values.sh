#!/usr/bin/env bash
# lint-raw-values.sh — flag design token drift in component code
# Checks for raw hex/rgba colors, hardcoded durations, and raw breakpoints
# that should use --sc-* design tokens instead.

set -euo pipefail

ERRORS=0
SRC="ui/src"

echo "Checking for design token drift in $SRC/..."
echo

# Portable: use find + grep (BSD grep on macOS lacks --include)
FIND_TS="find $SRC -type f -name '*.ts' ! -path '*/node_modules/*' ! -name '*.test.ts' ! -name 'icons.ts'"

# 1. Raw hex colors in CSS template literals (skip SVG fill/stroke, CSS content, and imports)
echo "--- Raw hex colors ---"
while IFS= read -r match; do
  [[ -z "$match" ]] && continue
  # Skip SVG attributes, CSS content/quotes, and import lines
  if echo "$match" | grep -qE '(fill=|stroke=|content:|\"#|import |from )'; then
    continue
  fi
  echo "  $match"
  ERRORS=$((ERRORS + 1))
done < <(eval "$FIND_TS" -exec grep -Hn -E '#[0-9a-fA-F]{3,8}' {} + 2>/dev/null || true)

# 2. Raw rgba() in CSS template literals
echo "--- Raw rgba() colors ---"
while IFS= read -r match; do
  [[ -z "$match" ]] && continue
  if echo "$match" | grep -qE '(color-mix|from |import )'; then
    continue
  fi
  echo "  $match"
  ERRORS=$((ERRORS + 1))
done < <(eval "$FIND_TS" -exec grep -Hn 'rgba(' {} + 2>/dev/null || true)

# 3. Raw breakpoints in @media queries (should use design tokens or shared constants)
echo "--- Raw breakpoints in @media ---"
while IFS= read -r match; do
  [[ -z "$match" ]] && continue
  echo "  $match"
  ERRORS=$((ERRORS + 1))
done < <(eval "$FIND_TS" -exec grep -Hn -E '@media.*[0-9]+px' {} + 2>/dev/null || true)

echo
if [ "$ERRORS" -gt 0 ]; then
  echo "Found $ERRORS design token drift violations."
  echo "Use --sc-* CSS custom properties instead of raw values."
  echo "See docs/design-strategy.md for the full token reference."
  exit 1
else
  echo "No design token drift found."
  exit 0
fi
