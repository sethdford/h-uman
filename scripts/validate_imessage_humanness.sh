#!/usr/bin/env bash
# validate_imessage_humanness.sh — Build, run conversation/proof C tests, validate eval JSON.
# Optional: run LLM-judged suite (needs provider in ~/.human/config.json).
#
# Usage:
#   ./scripts/validate_imessage_humanness.sh
#   BUILD=build-test ./scripts/validate_imessage_humanness.sh
#   RUN_EVAL=1 ./scripts/validate_imessage_humanness.sh   # also: human eval run …
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
cd "$ROOT"

BUILD="${BUILD:-build}"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
HUMAN="$ROOT/$BUILD/human"
TESTS="$ROOT/$BUILD/human_tests"

echo "==> cmake build ($BUILD): human + human_tests"
cmake --build "$BUILD" -j"$JOBS" --target human human_tests

echo "==> C tests: conversation + e2e_conversation + prove_e2e"
"$TESTS" --suite=conversation
"$TESTS" --suite=e2e_conversation
"$TESTS" --suite=prove_e2e

echo "==> eval validate (all JSON in eval_suites/)"
"$HUMAN" eval validate "$ROOT/eval_suites"

if [ "${RUN_EVAL:-0}" = 1 ]; then
  echo "==> human eval run imessage_humanness.json (LLM; uses ~/.human config)"
  "$HUMAN" eval run "$ROOT/eval_suites/imessage_humanness.json" | head -c 8000
  echo ""
fi

echo "Done. Tip: RUN_EVAL=1 to exercise the new judge suite against your default provider."
