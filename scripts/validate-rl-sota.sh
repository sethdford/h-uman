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

# LeakSanitizer gate is FULLY ENABLED. The rl_sota preset is the only CI build
# that runs under Linux ASan+LSan (LeakSanitizer is unsupported on macOS), so it
# is the canonical leak gate for the RL stack. The 1406-allocation backlog was
# driven to zero across three fixes: dpo-bridge :memory: db close, m3 agent
# teardown, and the final residual 58 (reward-model GPT backbone leaked via an
# undertrained 5-seed test whose Linux-FP-divergent assert longjmped past deinit
# — fixed by 200-iter convergence + deinit-before-assert — plus a lora_ab CLI
# dangling-else that skipped freeing strs_before on the promote path).
# Do NOT add `detect_leaks=0` here: fix the leak instead.

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
# Point the baseline gate at the rl_sota binary we just built. Without this it
# defaults to ./build/human, which the rl_sota preset never produces (we build
# into ./build-rl-sota) — so it would trigger a second, default-config rebuild
# of ./build whose stdout is swallowed, and any failure there surfaced only as
# an opaque non-zero exit from this step (the original RL-nightly red).
LORA_BASELINE_BIN="${ROOT}/build-rl-sota/human" bash scripts/check-lora-baseline.sh

if [ "$QUICK" -eq 0 ]; then
    info "Full test suite"
  # Line-buffer (stdbuf -oL) so the "Results:" / "FAIL  (...)" lines flush
  # BEFORE any end-of-process LeakSanitizer abort. A plain pipe is fully
  # buffered on a non-TTY, so SIGABRT at exit discarded the unflushed tail —
  # which is exactly why a CI leak failure used to show the LSan SUMMARY but no
  # Results line, leaving it undiagnosable. Tee the full output to a log so CI
  # can upload it as an artifact, then show the tail. pipefail still propagates
  # a non-zero human_tests exit.
  HU_FULL_SUITE_LOG="${HU_FULL_SUITE_LOG:-${ROOT}/build-rl-sota/full-suite.log}"
  stdbuf -oL -eL ./build-rl-sota/human_tests 2>&1 | tee "${HU_FULL_SUITE_LOG}" | tail -5
fi

info "Demo CLI (HUML wiring, no Gemma)"
OUT="/tmp/human-rl-validate-$$"
# NOTE: no --require-positive-delta here. The HUML backend is the CPU *reference*
# trainer — it computes DPO loss but does NOT emit a fusable LoRA adapter
# (adapter_path stays empty). A positive persona-fidelity delta requires applying
# an adapter in the candidate rollout, which only the MLX backend produces, so a
# positive delta is structurally unavailable on --backend huml. This step asserts
# the closed-loop WIRING + 9 non-stub evidence files instead; the delta gate
# belongs on the MLX path. (Demo now trains on real token-ID synthetic pairs.)
./build-rl-sota/human demo rl-closed-loop \
    --backend huml \
    --reaction-count 50 \
    --out "${OUT}"
for f in manifest.json training_curves.json eval_before.json eval_after.json \
    eval_delta.json delta_responses.md gate_decision.json adversarial_review.md reproduce.sh; do
    test -f "${OUT}/${f}" || fail "demo evidence dir missing ${f}"
    sz=$(wc -c <"${OUT}/${f}" | tr -d ' ')
    test "${sz}" -gt 50 || fail "demo evidence ${f} too small (${sz} bytes)"
done
pass "human demo rl-closed-loop (huml, 9 non-stub files)"

info "DRY_RUN demo script"
DRY_RUN=1 bash scripts/demo-rl-loop.sh
pass "scripts/demo-rl-loop.sh DRY_RUN"

pass "RL SOTA validation complete"
info "Proof index: docs/proof/rl-loop-proof.md"
