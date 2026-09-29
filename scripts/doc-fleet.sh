#!/usr/bin/env bash
# Doc fleet: aggregated documentation validation for CI and local verify-all.
# Runs standards index, drift, terminology, docs frontmatter, human-skills frontmatter,
# and repo-wide Markdown relative links (set DOC_FLEET_LINKS_FAST=1 to limit links to default roots only).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

EXIT_CODE=0

run() {
  local name="$1"
  shift
  echo ""
  echo "=== doc-fleet: $name ==="
  if "$@"; then
    echo "--- OK: $name"
  else
    echo "--- FAIL: $name"
    EXIT_CODE=1
  fi
}

echo "=============================="
echo " doc-fleet (documentation checks)"
echo "=============================="

run "standards doc index" bash scripts/check-doc-index.sh
run "standards drift" bash scripts/check-standards-drift.sh
run "terminology" bash scripts/check-terminology.sh
run "docs frontmatter" bash scripts/check-docs-frontmatter.sh
run "human-skills frontmatter" bash scripts/check-human-skills-frontmatter.sh
run "metrics drift" bash scripts/check-metrics-drift.sh
run "footprint claims" python3 scripts/footprint.py check
if [ "${DOC_FLEET_LINKS_FAST:-0}" = "1" ]; then
  run "markdown relative links" bash scripts/check-docs-relative-links.sh
else
  run "markdown relative links (repo-wide)" env MARKDOWN_LINK_SCAN_ALL=1 bash scripts/check-docs-relative-links.sh
fi

echo ""
echo "=============================="
if [ "$EXIT_CODE" -eq 0 ]; then
  echo " doc-fleet: all checks passed"
else
  echo " doc-fleet: one or more checks failed"
fi
echo "=============================="

exit "$EXIT_CODE"
