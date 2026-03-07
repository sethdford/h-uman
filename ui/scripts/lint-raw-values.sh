#!/usr/bin/env bash
# lint-raw-values.sh — flag design token drift in component code
# Checks for: raw hex/rgba colors, raw breakpoints, raw pixel spacing/radii,
# raw durations, and emoji-as-icons — all should use --sc-* design tokens.

set -euo pipefail

ERRORS=0
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$SCRIPT_DIR/.." && pwd)/src"

# Accept optional file list as arguments; if provided, lint only those files
if [ $# -gt 0 ]; then
  FILES=("$@")
  echo "Checking for design token drift in ${#FILES[@]} file(s)..."
else
  FILES=()
  echo "Checking for design token drift in $SRC/..."
fi
echo

find_ts() {
  if [ ${#FILES[@]} -gt 0 ]; then
    printf '%s\n' "${FILES[@]}" | xargs grep -l '.' 2>/dev/null | while IFS= read -r f; do
      echo "$f"
    done
  else
    find "$SRC" -type f -name '*.ts' ! -path '*/node_modules/*' ! -name '*.test.ts' ! -name 'icons.ts' "$@"
  fi
}

grep_ts() {
  local pattern="$1"
  if [ ${#FILES[@]} -gt 0 ]; then
    grep -Hn -E "$pattern" "${FILES[@]}" 2>/dev/null || true
  else
    find "$SRC" -type f -name '*.ts' ! -path '*/node_modules/*' ! -name '*.test.ts' ! -name 'icons.ts' -exec grep -Hn -E "$pattern" {} + 2>/dev/null || true
  fi
}

# 1. Raw hex colors in CSS template literals (skip SVG fill/stroke, CSS content, and imports)
echo "--- Raw hex colors ---"
while IFS= read -r match; do
  [[ -z "$match" ]] && continue
  # Allow explicit lint overrides via /* sc-lint-ok */ comment
  if echo "$match" | grep -qF 'sc-lint-ok'; then
    continue
  fi
  # Skip SVG attributes, CSS content/quotes, import lines, and JS private fields (#identifier)
  # (#identifier has letters outside hex range g-z or underscore)
  if echo "$match" | grep -qE '(fill=|stroke=|content:|\"#|import |from |#.*[g-zG-Z_])'; then
    continue
  fi
  echo "  $match"
  ERRORS=$((ERRORS + 1))
done < <(grep_ts '#[0-9a-fA-F]{3,8}')

# 2. Raw rgba() in CSS template literals
echo "--- Raw rgba() colors ---"
while IFS= read -r match; do
  [[ -z "$match" ]] && continue
  # Allow explicit lint overrides via /* sc-lint-ok */ comment
  if echo "$match" | grep -qF 'sc-lint-ok'; then
    continue
  fi
  if echo "$match" | grep -qE '(color-mix|from |import )'; then
    continue
  fi
  echo "  $match"
  ERRORS=$((ERRORS + 1))
done < <(grep_ts 'rgba\(')

# 3. Raw breakpoints in @media queries (skip lines with /* --sc-breakpoint-* */ comments)
echo "--- Raw breakpoints in @media ---"
while IFS= read -r match; do
  [[ -z "$match" ]] && continue
  # Allow explicit lint overrides via /* sc-lint-ok */ comment
  if echo "$match" | grep -qF 'sc-lint-ok'; then
    continue
  fi
  if echo "$match" | grep -qE '/\*.*--sc-breakpoint'; then
    continue
  fi
  echo "  $match"
  ERRORS=$((ERRORS + 1))
done < <(grep_ts '@media.*[0-9]+px')

# 4. Raw pixel spacing (padding, margin, gap, top, bottom, left, right, inset)
echo "--- Raw pixel spacing ---"
while IFS= read -r match; do
  [[ -z "$match" ]] && continue
  # Allow explicit lint overrides via /* sc-lint-ok */ comment
  if echo "$match" | grep -qF 'sc-lint-ok'; then
    continue
  fi
  # Skip 1px (hairline borders), border-width, outline-offset, box-shadow
  if echo "$match" | grep -qE '(1px|border-width|outline-offset|box-shadow)'; then
    continue
  fi
  # Skip minmax(, @media, and var(--sc-*, Npx) fallbacks
  if echo "$match" | grep -qE '(minmax\(|@media|, [0-9]+px\))'; then
    continue
  fi
  # Skip border-* (border-left, border-right, etc.) — those are border-width, not spacing
  if echo "$match" | grep -qE 'border-(left|right|top|bottom):'; then
    continue
  fi
  echo "  $match"
  ERRORS=$((ERRORS + 1))
done < <(grep_ts '(padding|margin|gap|top|bottom|left|right|inset):[^;]*[0-9]+px')

# 5. Raw pixel radii (border-radius with raw px)
echo "--- Raw pixel radii ---"
while IFS= read -r match; do
  [[ -z "$match" ]] && continue
  # Allow explicit lint overrides via /* sc-lint-ok */ comment
  if echo "$match" | grep -qF 'sc-lint-ok'; then
    continue
  fi
  if echo "$match" | grep -qE 'var\(--sc-radius'; then
    continue
  fi
  echo "  $match"
  ERRORS=$((ERRORS + 1))
done < <(grep_ts 'border-radius:[^;]*[0-9]+px')

# 6. Raw durations (transition/animation with ms or s)
echo "--- Raw durations ---"
while IFS= read -r match; do
  [[ -z "$match" ]] && continue
  if echo "$match" | grep -qE 'prefers-reduced-motion'; then
    continue
  fi
  if echo "$match" | grep -qE 'var\(--sc-duration-'; then
    continue
  fi
  if echo "$match" | grep -qE '(0s|0ms)\s*[,;)]'; then
    continue
  fi
  if echo "$match" | grep -qE 'animation-delay'; then
    continue
  fi
  # Allow explicit lint overrides via /* sc-lint-ok */ comment
  if echo "$match" | grep -qF 'sc-lint-ok'; then
    continue
  fi
  echo "  $match"
  ERRORS=$((ERRORS + 1))
done < <(grep_ts '(transition|animation):[^;]*([1-9][0-9]*ms|[0-9]+\.[0-9]+s|[1-9][0-9]*s)')

# 7. Emoji as icons (common emoji in .ts files, exclude test files and comments)
echo "--- Emoji as icons ---"
while IFS= read -r match; do
  [[ -z "$match" ]] && continue
  # Allow explicit lint overrides via /* sc-lint-ok */ comment
  if echo "$match" | grep -qF 'sc-lint-ok'; then
    continue
  fi
  # Skip comment-only lines
  if echo "$match" | grep -qE '^\s*(//|/\*|\*)'; then
    continue
  fi
  echo "  $match"
  ERRORS=$((ERRORS + 1))
done < <(grep_ts '[👍👎❤️📋🔖⚠️💬🔧⚡⚙]')

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
