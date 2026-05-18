#!/usr/bin/env bash
# Sprint 7 / US-7.4 AC-7.4.3 — assert check-lora-ab.sh emits a JSON
# measurement line carrying BOTH `delta` and `size_mb` keys.
#
# This is a SCHEMA test, not a quality gate: the JSON line must exist
# and parse. The actual delta and size_mb values are validated by the
# underlying gate (`scripts/check-lora-ab.sh`) and by the Python tests
# in `tests/test_finetune_gemma_modules.py`.
#
# Usage:
#   bash tests/test_check_lora_ab_json_schema.sh
#
# Requires:
#   - jq if available (preferred); falls back to grep+python if not.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

OUTPUT="$(bash scripts/check-lora-ab.sh)"
EXIT_CODE=$?
if [ "$EXIT_CODE" -ne 0 ]; then
  echo "[schema-test] FAIL: check-lora-ab.sh exited $EXIT_CODE" >&2
  printf '%s\n' "$OUTPUT" >&2
  exit 1
fi

# Pick out the JSON line. The script emits exactly one line that starts
# with `{` and ends with `}`. Use awk for portability (grep -P is not
# available on macOS by default).
JSON_LINE="$(printf '%s\n' "$OUTPUT" | awk '/^\{.*\}$/ { print; exit }')"
if [ -z "$JSON_LINE" ]; then
  echo "[schema-test] FAIL: no JSON line in check-lora-ab.sh output" >&2
  printf '%s\n' "$OUTPUT" >&2
  exit 1
fi

# Validate via jq if available; otherwise via python3 (always present
# on the dev machine and in CI).
if command -v jq >/dev/null 2>&1; then
  # `has("size_mb")` is the idiomatic jq way to check key presence
  # (regardless of whether the value is null). `.delta != null`
  # rejects a missing or null delta (AC-7.4.3 says delta must exist).
  if ! jq -e '.delta != null and has("size_mb")' <<< "$JSON_LINE" >/dev/null; then
    echo "[schema-test] FAIL: JSON missing delta or size_mb key: $JSON_LINE" >&2
    exit 1
  fi
else
  python3 - "$JSON_LINE" <<'PY'
import json, sys
line = sys.argv[1]
obj = json.loads(line)
assert obj.get("delta") is not None, f"delta missing or null: {obj}"
assert "size_mb" in obj, f"size_mb key missing: {obj}"
PY
fi

echo "[schema-test] PASS: $JSON_LINE"

# ── --judgment SKIP path must NOT parse as PASS ───────────────────────
# US-7.6 decision D3: when no NLL backend is registered, the script
# emits a visible SKIP line that downstream automation must not treat
# as success. We only check the SKIP appears; we do NOT require a JSON
# line in the judgment path (judgment is informational text).
JUDGMENT_OUTPUT="$(bash scripts/check-lora-ab.sh --judgment 2>&1 || true)"
if ! printf '%s\n' "$JUDGMENT_OUTPUT" | grep -q "\[lora-ab\] judgment:"; then
  echo "[schema-test] FAIL: --judgment produced no [lora-ab] judgment: line" >&2
  printf '%s\n' "$JUDGMENT_OUTPUT" >&2
  exit 1
fi
# The line must be SKIP-or-PASS, never silently absent. SKIP is the
# dormant-by-design state for Sprint 7.
JUDGMENT_LINE="$(printf '%s\n' "$JUDGMENT_OUTPUT" | grep '\[lora-ab\] judgment:' | head -n1)"
case "$JUDGMENT_LINE" in
  *SKIP*|*PASS*)
    echo "[schema-test] judgment line OK: $JUDGMENT_LINE"
    ;;
  *)
    echo "[schema-test] FAIL: unexpected judgment line shape: $JUDGMENT_LINE" >&2
    exit 1
    ;;
esac

exit 0
