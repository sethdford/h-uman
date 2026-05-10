#!/usr/bin/env bash
# Memory v2 — local proof bundle (layered stack + gates).
#
# Runs the same checks agents should use before claiming "Memory v2 E2E"
# without paying the full ~9.5k-test wall clock every time:
#   G2  header collision guard
#   G1* `--suite="v2 E2E"` — 13 adversarial scenarios plus the following
#      `run_v2_wiring_e2e_tests` block (U1/U2/D3) in one pass (~20 tests total)
#
# Full repo gate remains: ./build/human_tests && bash scripts/verify-all.sh
#
# Usage:
#   cmake --build build --target human_tests
#   bash scripts/memory-v2-local-proof.sh
#
# Optional:
#   HUMAN_TESTS=/path/to/human_tests bash scripts/memory-v2-local-proof.sh
#   FULL_V2_SUITES=1  # also run each W7–W16 HU_TEST_SUITE block (slower)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

HUMAN_TESTS="${HUMAN_TESTS:-$ROOT/build/human_tests}"
if [[ ! -x "$HUMAN_TESTS" ]]; then
  echo "memory-v2-local-proof: $HUMAN_TESTS is missing or not executable." >&2
  echo "  cmake --build build --target human_tests" >&2
  exit 1
fi

echo "=============================="
echo " Memory v2 — local proof"
echo "=============================="

echo ""
echo "=== G2 — memory v2 header collision guard ==="
bash "$ROOT/scripts/check-memory-v2-header-collision.sh"

echo ""
echo "=== v2 E2E adversarial (13 scenarios) + v2 wiring tail (same suite filter) ==="
"$HUMAN_TESTS" --suite="v2 E2E"

if [[ "${FULL_V2_SUITES:-0}" == "1" ]]; then
  echo ""
  echo "=== FULL_V2_SUITES — per-workstream suite strings (unique substrings) ==="
  for s in "W7 memory facade" "W8 belief layer" "W9 world model" "W10 neural memory" \
           "W11 inline self-RAG" "W12 planner" "W12 verifier loop" "W13 learner" \
           "W14 runners" "W14 scheduler" "W15 cryptographic keystore" "W15 backup-restore" \
           "W16 evaluation"; do
    echo ""
    echo "--- --suite=$s ---"
    "$HUMAN_TESTS" --suite="$s"
  done
fi

echo ""
echo "=============================="
echo " memory-v2-local-proof: OK"
echo " (Set FULL_V2_SUITES=1 for extra W7–W16 suite passes.)"
echo "=============================="
