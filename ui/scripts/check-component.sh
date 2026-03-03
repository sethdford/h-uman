#!/usr/bin/env bash
# Usage: bash scripts/check-component.sh src/components/sc-button.ts
set -euo pipefail

cd "$(dirname "$0")/.."

FILE="${1:?Usage: check-component.sh <component-file>}"

if [ ! -f "$FILE" ]; then
  echo "Error: File not found: $FILE"
  exit 1
fi

NAME=$(basename "$FILE" .ts)
ERRORS=0

check() {
  local desc="$1"
  local pattern="$2"
  if ! grep -qE "$pattern" "$FILE"; then
    echo "FAIL: $desc"
    ERRORS=$((ERRORS + 1))
  else
    echo "PASS: $desc"
  fi
}

echo "=== Component Checklist: $NAME ==="
echo ""

# 1. Must use @customElement decorator
check "Uses @customElement decorator" "@customElement"

# 2. Must extend LitElement
check "Extends LitElement" "extends LitElement"

# 3. Must have static styles (static styles or static override styles)
check "Has static styles" "static\s+(override\s+)?styles"

# 4. Must use --sc- tokens (at least one)
check "Uses --sc-* design tokens" "var\(--sc-"

# 5. Must NOT have hardcoded hex colors
if grep -qE '#[0-9a-fA-F]{3,8}' "$FILE" 2>/dev/null; then
  # Filter out template literal backticks and common exceptions
  HEX_COUNT=$(grep -cE '#[0-9a-fA-F]{3,8}' "$FILE" 2>/dev/null || true)
  echo "WARN: Found $HEX_COUNT possible hardcoded hex colors — use --sc-* tokens"
else
  echo "PASS: No hardcoded hex colors"
fi

# 6. Check test coverage
TESTS_FILE="src/components/components.test.ts"
if [ -f "$TESTS_FILE" ]; then
  if grep -q "$NAME" "$TESTS_FILE"; then
    echo "PASS: Has test in components.test.ts"
  else
    echo "FAIL: No test found in components.test.ts for $NAME"
    ERRORS=$((ERRORS + 1))
  fi
else
  echo "FAIL: components.test.ts not found"
  ERRORS=$((ERRORS + 1))
fi

# 7. Check catalog entry
CATALOG="catalog.html"
if [ -f "$CATALOG" ]; then
  if grep -q "<$NAME" "$CATALOG" || grep -q "$NAME" "$CATALOG"; then
    echo "PASS: Has entry in catalog.html"
  else
    echo "FAIL: No entry in catalog.html for $NAME"
    ERRORS=$((ERRORS + 1))
  fi
else
  echo "FAIL: catalog.html not found"
  ERRORS=$((ERRORS + 1))
fi

# 8. ARIA check
if grep -qE 'role=|aria-' "$FILE"; then
  echo "PASS: Has ARIA attributes"
else
  echo "WARN: No ARIA attributes found — verify accessibility"
fi

# 9. Focus ring check
if grep -qE 'focus-visible|:focus' "$FILE"; then
  echo "PASS: Has focus styles"
else
  echo "WARN: No :focus-visible styles found"
fi

echo ""
if [ "$ERRORS" -gt 0 ]; then
  echo "$ERRORS check(s) failed"
  exit 1
else
  echo "All checks passed"
fi
