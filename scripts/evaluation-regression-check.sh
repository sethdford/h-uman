#!/usr/bin/env bash
# scripts/evaluation-regression-check.sh — W16 regression gate wrapper (offline).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BIN="${HUMAN_BIN:-$ROOT/build/human}"
if [ ! -x "$BIN" ]; then
  echo "ERROR: human binary not found at $BIN (set HUMAN_BIN or cmake --build build)" >&2
  exit 1
fi

BASELINE="${EVAL_BASELINE:-$ROOT/docs/evaluation/baseline.json}"
exec "$BIN" evaluation bench --baseline "$BASELINE" --fail-on-regression "$@"
