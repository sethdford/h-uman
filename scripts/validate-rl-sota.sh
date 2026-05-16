#!/usr/bin/env bash
# Validate the full RL SOTA stack (Phases 0–6) locally.
# Usage: bash scripts/validate-rl-sota.sh [--quick]
set -euo pipefail

QUICK=0
if [ "${1:-}" = "--quick" ]; then
    QUICK=1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

pass() { printf '\033[0;32mPASS\033[0m  %s\n' "$1"; }
fail() { printf '\033[0;31mFAIL\033[0m  %s\n' "$1"; exit 1; }
info() { printf '      %s\n' "$1"; }

info "=== RL SOTA validation (rl_sota preset) ==="

cmake --preset rl_sota
cmake --build --preset rl_sota -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

export HU_E2E_TMP_ROOT="${ROOT}/build-rl-sota/tests/_tmp"
mkdir -p "${HU_E2E_TMP_ROOT}/proofs"

info "E2E closed-loop suite"
./build-rl-sota/human_tests --suite=E2E-closed-loop

info "Phase 5 eval suites"
for s in bootstrap eval_judge_external leaderboard eval-gate; do
    ./build-rl-sota/human_tests --suite="${s}" >/dev/null
done
pass "Phase 5 eval suites"

info "v1 fidelity + lora baseline"
./build-rl-sota/human_tests --suite=personal-model-fidelity-v2 >/dev/null
bash scripts/check-lora-baseline.sh

if [ "$QUICK" -eq 0 ]; then
    info "Full test suite"
  ./build-rl-sota/human_tests | tail -3
fi

info "Demo CLI (HUML wiring, no Gemma)"
OUT="/tmp/human-rl-validate-$$"
./build-rl-sota/human demo rl-closed-loop \
    --backend huml \
    --reaction-count 50 \
    --out "${OUT}" \
    --require-positive-delta
test -f "${OUT}/manifest.json" || fail "demo evidence dir missing manifest.json"
test -f "${OUT}/gate_decision.json" || fail "demo evidence dir missing gate_decision.json"
pass "human demo rl-closed-loop (huml)"

info "DRY_RUN demo script"
DRY_RUN=1 bash scripts/demo-rl-loop.sh
pass "scripts/demo-rl-loop.sh DRY_RUN"

pass "RL SOTA validation complete"
info "Proof index: docs/proof/rl-loop-proof.md"
