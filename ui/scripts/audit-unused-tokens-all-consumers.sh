#!/usr/bin/env bash
# Comprehensive design token audit across ALL consumers
# ui, website, docs, design-tokens, apps (Flutter/Swift/Kotlin use different names)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOKENS_FILE="$ROOT/ui/src/styles/_tokens.css"

if [ ! -f "$TOKENS_FILE" ]; then
  echo "Token file not found: $TOKENS_FILE"
  exit 1
fi

# Extract all --hu-* custom property definitions (unique, from LHS of : only)
DEFINED=$(grep -oE '\-\-hu-[a-zA-Z0-9_-]+' "$TOKENS_FILE" | sort -u)
TOTAL=$(echo "$DEFINED" | wc -l | tr -d ' ')

echo "=== Design Token Audit: $TOTAL tokens from _tokens.css ==="
echo ""

# Build list of tokens used as VALUES in other tokens (indirect use)
echo "Building indirect-use map (tokens referenced in other token values)..."
INDIRECT=""
for token in $DEFINED; do
  # Check if this token appears in any token VALUE (RHS) - e.g. var(--hu-accent-subtle)
  if grep -q "$token" "$TOKENS_FILE" 2>/dev/null; then
    # Count occurrences: if it appears more than once, it's used (definition + reference)
    # Or if it appears in a line that's not a definition of itself
    REFS=$(grep -c "$token" "$TOKENS_FILE" 2>/dev/null || echo 0)
    if [ "$REFS" -gt 1 ]; then
      INDIRECT="$INDIRECT $token"
    else
      # Check if it appears in a var() on a different line (value of another token)
      if grep -v "^[[:space:]]*$token" "$TOKENS_FILE" | grep -q "$token"; then
        INDIRECT="$INDIRECT $token"
      fi
    fi
  fi
done

UNUSED_LIST=""
UNUSED_COUNT=0

for token in $DEFINED; do
  # Skip if used indirectly (referenced in another token's value)
  if echo "$INDIRECT" | grep -qw "$token" 2>/dev/null; then
    continue
  fi

  # Search ui/src (excluding _tokens.css)
  UI=$(find "$ROOT/ui/src" \( -name "*.ts" -o -name "*.css" \) ! -path "*_tokens.css" ! -path "*node_modules*" -print0 2>/dev/null | xargs -0 grep -l -e "$token" 2>/dev/null | wc -l | tr -d ' ') || UI=0

  # Search website/ (exclude node_modules, dist, _tokens.css)
  WEB=$(find "$ROOT/website" \( -name "*.css" -o -name "*.astro" -o -name "*.mdx" -o -name "*.ts" -o -name "*.tsx" \) ! -path "*_tokens*" ! -path "*node_modules*" ! -path "*dist*" ! -path "*/.astro/*" -print0 2>/dev/null | xargs -0 grep -l -e "$token" 2>/dev/null | wc -l | tr -d ' ') || WEB=0

  # Search docs/
  DOCS=$(find "$ROOT/docs" \( -name "*.md" -o -name "*.mdx" -o -name "*.json" -o -name "*.ts" \) -print0 2>/dev/null | xargs -0 grep -l -e "$token" 2>/dev/null | wc -l | tr -d ' ') || DOCS=0

  # Search design-tokens/ (build scripts, tests)
  DT=$(find "$ROOT/design-tokens" \( -name "*.ts" -o -name "*.json" -o -name "*.sh" \) -print0 2>/dev/null | xargs -0 grep -l -e "$token" 2>/dev/null | wc -l | tr -d ' ') || DT=0

  TOTAL_REFS=$((UI + WEB + DOCS + DT))
  if [ "$TOTAL_REFS" -eq 0 ]; then
    echo "UNUSED: $token"
    UNUSED_LIST="$UNUSED_LIST $token"
    UNUSED_COUNT=$((UNUSED_COUNT + 1))
  fi
done

echo ""
echo "=== Summary ==="
echo "Total tokens: $TOTAL"
echo "Truly unused (no refs in ui, website, docs, design-tokens): $UNUSED_COUNT"
echo ""
echo "Note: Tokens used as values in other tokens (e.g. var(--hu-x) in --hu-y) are excluded."
echo "Note: C (design_tokens.h) and apps (Flutter/Swift/Kotlin) use generated outputs, not CSS var names."
