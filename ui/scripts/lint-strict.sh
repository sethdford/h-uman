#!/usr/bin/env bash
# lint-strict.sh — check for quality regressions beyond standard lint
set -euo pipefail

cd "$(dirname "$0")/.."
FAIL=0

# Check for TypeScript `any` in production code
ANY_HITS=$(grep -rn --include='*.ts' --exclude='*.test.ts' -E ': any\b|as any\b' src/ 2>/dev/null || true)
if [ -n "$ANY_HITS" ]; then
  echo "FAIL: Found TypeScript 'any' usages in production code:"
  echo "$ANY_HITS"
  FAIL=1
fi

# Check for console.log/debug in production code (console.warn/error allowed for diagnostics)
CONSOLE_HITS=$(grep -rn --include='*.ts' --exclude='*.test.ts' --exclude='demo-gateway.ts' -E 'console\.(log|debug)\(' src/ 2>/dev/null || true)
if [ -n "$CONSOLE_HITS" ]; then
  echo "FAIL: Found console.log/debug statements in production code:"
  echo "$CONSOLE_HITS"
  FAIL=1
fi

# Report component usage (informational)
TOTAL=0
USED=0
for f in src/components/hu-*.ts; do
  [ -f "$f" ] || continue
  echo "$f" | grep -q test && continue
  TOTAL=$((TOTAL + 1))
  name=$(basename "$f" .ts)
  if grep -rql "$name" src/views/ src/app.ts 2>/dev/null; then
    USED=$((USED + 1))
  fi
done
UNUSED=$((TOTAL - USED))
echo "INFO: Components: $TOTAL total, $USED used in views/app, $UNUSED library-only"

# Report token usage (informational)
DEFINED=$(grep -c '^\s*--hu-' src/styles/_tokens.css 2>/dev/null || echo 0)
REFERENCED=$(grep -roh --include='*.ts' --include='*.css' 'var(--hu-[a-z0-9_-]*' src/ 2>/dev/null | sed 's/var(//' | sort -u | wc -l | tr -d ' ')
echo "INFO: CSS tokens: $DEFINED lines, $REFERENCED unique vars referenced"

if [ "$FAIL" -gt 0 ]; then
  exit 1
fi

echo "lint:strict passed"
