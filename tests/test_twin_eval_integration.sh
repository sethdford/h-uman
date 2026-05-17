#!/usr/bin/env bash
# Sprint 11 / US-11.6 AC-11.6.5 — twin-eval round-trip integration test.
#
# Proves the BINDING contract for Wave 2 US-11.7's 4-stage Pareto gate:
#   yntp_eval.py --output X.json | pareto_picker.py --input-schema yntp X.json
# produces a correct gate decision (PROMOTE / DEFER / REJECT).
#
# The two arms below cover both happy path and Sprint 8 broken-adapter
# regression guard (AC-11.6.3 carried through the round-trip):
#
#   Arm A — yntp_good_adapter_log.jsonl: adapter beats base on per-token
#           log-likelihood with zero pad leakage -> PROMOTE (exit 0).
#   Arm B — sprint8_broken_yntp_log.jsonl:  high pad leakage + negative
#           delta -> REJECT (exit 2). This is the regression guard.
#
# Usage:
#   bash tests/test_twin_eval_integration.sh
#
# Exit codes:
#   0 = both arms produced the expected verdict
#   1 = at least one arm produced the wrong verdict

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

YNTP="python3 scripts/yntp_eval.py"
PARETO="python3 scripts/pareto_picker.py"

# Working dir for the temp JSON. Use mktemp so parallel test runs don't
# clobber each other; clean up on exit.
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

assert_verdict() {
  # $1 = arm name, $2 = path to yntp_eval JSON, $3 = expected verdict,
  # $4 = expected pareto_picker exit code.
  local arm="$1"
  local yntp_json="$2"
  local expected_verdict="$3"
  local expected_exit="$4"

  echo "[twin-eval-integration] arm=${arm}: yntp_eval JSON at ${yntp_json}"

  # Run pareto_picker on the YNTP JSON. Capture stdout AND exit code
  # without letting set -e abort the script. The expected exit code is
  # part of the contract (Wave 2 US-11.7 reads it).
  local pareto_out pareto_exit=0
  pareto_out="$(${PARETO} --input-schema yntp "${yntp_json}")" || pareto_exit=$?

  echo "[twin-eval-integration] arm=${arm}: pareto_picker exit=${pareto_exit}"
  printf '%s\n' "${pareto_out}"

  # Verdict assertion: the script prints `VERDICT:     <X>` on its own line.
  # Match anywhere on the line; tolerate variable spacing.
  if ! printf '%s\n' "${pareto_out}" | grep -Eq "^VERDICT:[[:space:]]+${expected_verdict}\$"; then
    echo "[twin-eval-integration] FAIL arm=${arm}: expected VERDICT=${expected_verdict}" >&2
    exit 1
  fi

  if [ "${pareto_exit}" -ne "${expected_exit}" ]; then
    echo "[twin-eval-integration] FAIL arm=${arm}: expected exit=${expected_exit} got=${pareto_exit}" >&2
    exit 1
  fi

  echo "[twin-eval-integration] PASS arm=${arm}: VERDICT=${expected_verdict} exit=${expected_exit}"
}

# ── Arm A — good adapter, synthetic fixture ────────────────────────────
# yntp_eval emits gate_decision=PASS (delta_ll>0, pad_rate=0). Pareto
# then sees delta=+0.435, pad=0 -> PROMOTE (exit 0).
GOOD_JSON="${TMPDIR}/yntp_good.json"
${YNTP} \
  --mock-from-jsonl tests/fixtures/yntp_good_adapter_log.jsonl \
  --output "${GOOD_JSON}" >/dev/null
assert_verdict "good-adapter" "${GOOD_JSON}" "PROMOTE" 0

# ── Arm B — Sprint 8 broken adapter, regression guard ─────────────────
# yntp_eval emits gate_decision=FAIL (pad_rate=1.0 trips the regression
# guard, AC-11.6.3). Pareto sees delta=-2.52, pad=1.0 -> REJECT (exit 2).
# This proves the regression guard survives the round-trip into the
# Wave 2 Pareto stage.
BROKEN_JSON="${TMPDIR}/yntp_broken.json"
# yntp_eval exits 1 on gate FAIL — that's a documented exit code, not an
# error. Capture and ignore (we only care about the JSON content + the
# downstream pareto verdict).
${YNTP} \
  --mock-from-jsonl tests/fixtures/sprint8_broken_yntp_log.jsonl \
  --output "${BROKEN_JSON}" >/dev/null || true

if [ ! -s "${BROKEN_JSON}" ]; then
  echo "[twin-eval-integration] FAIL: arm B produced no JSON output" >&2
  exit 1
fi

assert_verdict "sprint8-broken-adapter" "${BROKEN_JSON}" "REJECT" 2

echo "[twin-eval-integration] PASS: AC-11.6.5 round-trip green on both arms"
exit 0
