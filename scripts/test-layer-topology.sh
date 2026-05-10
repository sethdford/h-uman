#!/usr/bin/env bash
# test-layer-topology.sh — adversarial self-test for check-layer-topology.sh.
#
# Verifies that the topology check catches the kinds of violations it claims
# to prevent. Runs in three phases:
#
#   1. Baseline: assert the current tree passes (no false positives).
#   2. Inject:   create a deliberate L0 -> L4 violation in a temp file under
#                src/memory/, run the check, assert it fails.
#   3. Restore:  delete the temp file and re-run; assert clean.
#
# This script is exercised by the W7-W16 quality ceremony (`scripts/verify-all.sh`)
# and ensures the topology check stays load-bearing — i.e. silently passing
# is a real signal, not a stale one.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

CHECK="$SCRIPT_DIR/check-layer-topology.sh"
CANARY="src/memory/_topology_canary.c"

cleanup() {
    rm -f "$CANARY"
}
trap cleanup EXIT INT TERM

# Phase 1 — baseline.
echo "[1/3] Baseline: current tree must pass the topology check."
if ! bash "$CHECK" >/dev/null 2>&1; then
    echo "FAIL: baseline tree has cross-layer violations (cannot self-test)."
    bash "$CHECK" || true
    exit 2
fi
echo "      OK"

# Phase 2 — inject a real violation.
# A file under src/memory/graph_*.c is L0; including a L4 ml header is a
# textbook upward dependency. The check MUST flag it.
echo "[2/3] Inject: planting an L0 file that includes an L4 header."
cat > "$CANARY" <<'EOF'
/* TOPOLOGY-CANARY: deliberate cross-layer violation used by
 * test-layer-topology.sh to verify the check still catches things.
 * If the topology check passes with this file present, the check is
 * broken. */
#include "human/ml/learner.h"
EOF
# Place in src/memory/ so it classifies as L0 (graph substrate prefix).
# Rename so the SOURCE_RULES regex picks it up: matches ^src/memory/(graph|...)
# We need a name that classifies. Move under src/memory/graph_canary.c.
mv "$CANARY" src/memory/graph_canary.c
CANARY="src/memory/graph_canary.c"

if bash "$CHECK" >/dev/null 2>&1; then
    echo "FAIL: topology check did NOT detect the deliberate violation."
    cat "$CANARY"
    exit 1
fi
echo "      OK (violation correctly rejected)"

# Phase 3 — remove and re-verify clean.
rm -f "$CANARY"
echo "[3/3] Restore: tree clean again."
if ! bash "$CHECK" >/dev/null 2>&1; then
    echo "FAIL: tree dirty after canary cleanup (this should not happen)."
    bash "$CHECK" || true
    exit 1
fi
echo "      OK"

echo
echo "Layer topology self-test passed: check is load-bearing."
exit 0
