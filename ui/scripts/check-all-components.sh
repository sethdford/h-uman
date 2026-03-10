#!/usr/bin/env bash
# Runs check-component.sh against every hu-*.ts component.
# Exit code is non-zero if any component fails.
set -euo pipefail

cd "$(dirname "$0")/.."

SCRIPT="scripts/check-component.sh"
FAILURES=0
TOTAL=0

for file in src/components/hu-*.ts; do
  # Skip test files and type definition files
  case "$file" in
    *.test.ts|*.d.ts) continue ;;
  esac

  TOTAL=$((TOTAL + 1))
  echo ""
  if ! bash "$SCRIPT" "$file"; then
    FAILURES=$((FAILURES + 1))
  fi
done

echo ""
echo "================================"
echo "Components checked: $TOTAL"
echo "Failures: $FAILURES"
echo "================================"

if [ "$FAILURES" -gt 0 ]; then
  exit 1
fi
