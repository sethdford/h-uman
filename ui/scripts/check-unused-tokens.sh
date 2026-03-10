#!/usr/bin/env bash
# check-unused-tokens.sh — detect design tokens defined but never referenced
set -euo pipefail

TOKENS_FILE="src/styles/_tokens.css"
SRC_DIR="src"
UNUSED=0

if [ ! -f "$TOKENS_FILE" ]; then
  echo "Token file not found: $TOKENS_FILE"
  exit 1
fi

# Extract all --hu-* custom property definitions from _tokens.css
DEFINED=$(grep -oE '\-\-hu-[a-zA-Z0-9_-]+' "$TOKENS_FILE" | sort -u)
TOTAL=$(echo "$DEFINED" | wc -l | tr -d ' ')

echo "Checking $TOTAL defined tokens for usage in $SRC_DIR/..."
echo ""

for token in $DEFINED; do
  # Search all TS, CSS files in src/ (excluding _tokens.css itself)
  # Use -e to avoid tokens like --hu-* being parsed as grep options
  REFS=$(find "$SRC_DIR" \( -name "*.ts" -o -name "*.css" \) ! -path "*_tokens.css" -print0 | xargs -0 grep -l -e "$token" 2>/dev/null | wc -l | tr -d ' ') || REFS=0
  if [ "$REFS" -eq 0 ]; then
    echo "  UNUSED: $token"
    UNUSED=$((UNUSED + 1))
  fi
done

echo ""
echo "Summary: $UNUSED unused tokens out of $TOTAL defined"

if [ "$UNUSED" -gt 0 ]; then
  echo "Consider removing unused tokens from design-tokens/*.tokens.json"
fi
