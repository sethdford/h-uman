#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

# Build for production
npm run build --silent

# Measure ENTRY POINT size only (files referenced in index.html).
# Lazy-loaded chunks (Shiki languages, KaTeX, view chunks) are excluded
# because they don't affect initial page load.
TOTAL=0
ENTRY_FILES=$(grep -oE '(src|href)="/assets/[^"]*"' dist/index.html \
  | sed 's/.*"\/assets\//dist\/assets\//' | sed 's/"$//')
for f in $ENTRY_FILES; do
  [ -f "$f" ] || continue
  SIZE=$(wc -c < "$f")
  echo "  $f: $((SIZE / 1024)) KB"
  TOTAL=$((TOTAL + SIZE))
done

KB=$((TOTAL / 1024))
BUDGET=350
PCT=$((KB * 100 / BUDGET))

echo "Entry bundle: ${KB} KB / ${BUDGET} KB (${PCT}%)"
if [ "$KB" -gt "$BUDGET" ]; then
  echo "OVER BUDGET by $((KB - BUDGET)) KB"
  exit 1
fi
echo "Within budget"
