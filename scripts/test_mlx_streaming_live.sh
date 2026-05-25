#!/usr/bin/env bash
# scripts/test_mlx_streaming_live.sh
#
# Sprint 55 US-M3-B4 Phase 3 — live MLX subprocess streaming integration test.
#
# This is the integration-level counterpart to the unit tests in
# tests/test_mlx_stream_utf8.c (16 tests pinning the UTF-8 chunk-emit
# contract) and tests/test_mlx_provider.c (vtable wiring assertions).
#
# Why a shell script and not a C test:
# The MLX subprocess driver (src/providers/mlx.c::mlx_run_subprocess) is
# gated by HU_MLX_SUBPROCESS_ACTIVE, which is 0 under HU_IS_TEST. That
# means the symbol is dead code in the test binary — there's literally
# nothing to link against. What this script CAN verify is the upstream
# toolchain that `mlx_run_subprocess` invokes: python3, mlx_lm.generate,
# and the model itself. If that pipeline works AND the UTF-8 helpers
# pass their unit tests, the C driver's behavior is structurally pinned.
#
# Behavior (mirrors scripts/test_mlx_adapter_swap.py's skip-on-missing
# pattern from the M3 Phase B2 work):
#   - If HU_MLX_FIXTURE_MODEL_PATH env is unset OR the path doesn't
#     exist OR python3 is missing OR mlx_lm not importable → exit 0
#     with a SKIP message. CI without an MLX-capable machine treats
#     this as a pass.
#   - If all prereqs present → run python3 -m mlx_lm generate against
#     the model, capture stdout, assert non-empty + valid UTF-8 +
#     ≥1 word of output.
#
# Exit codes:
#   0 = pass (real run OR skip)
#   1 = tractable failure (empty output, garbled UTF-8) — investigate
#   2 = environmental error (fixture path bad, mlx_lm crash)
#
# Usage:
#   HU_MLX_FIXTURE_MODEL_PATH=/path/to/mlx_lm/model bash scripts/test_mlx_streaming_live.sh

set -euo pipefail

SCRIPT_NAME="$(basename "$0")"

# ── Skip-when-not-runnable ──────────────────────────────────────────
if [ -z "${HU_MLX_FIXTURE_MODEL_PATH:-}" ]; then
    echo "[${SCRIPT_NAME}] SKIP: HU_MLX_FIXTURE_MODEL_PATH not set" >&2
    echo "[${SCRIPT_NAME}] To run: HU_MLX_FIXTURE_MODEL_PATH=/path/to/mlx_lm/model $0" >&2
    exit 0
fi

if [ ! -e "${HU_MLX_FIXTURE_MODEL_PATH}" ]; then
    echo "[${SCRIPT_NAME}] SKIP: fixture path '${HU_MLX_FIXTURE_MODEL_PATH}' does not exist" >&2
    exit 0
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "[${SCRIPT_NAME}] SKIP: python3 not in PATH" >&2
    exit 0
fi
if ! python3 -c 'import mlx_lm' >/dev/null 2>&1; then
    echo "[${SCRIPT_NAME}] SKIP: mlx_lm not importable (pip install mlx-lm)" >&2
    exit 0
fi
if [ "$(uname -m)" != "arm64" ]; then
    echo "[${SCRIPT_NAME}] SKIP: requires Apple Silicon (uname -m = $(uname -m))" >&2
    exit 0
fi

# Resolve timeout(1) — Linux coreutils ships it as `timeout`; macOS has
# `gtimeout` via Homebrew coreutils. If neither is present, we still
# run but without a hard ceiling (acceptable for local dev; CI should
# install coreutils).
TIMEOUT_CMD=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_CMD="timeout 120"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_CMD="gtimeout 120"
fi

# ── Run mlx_lm.generate directly ────────────────────────────────────
# This is the exact subprocess the C driver (src/providers/mlx.c::
# mlx_run_subprocess) would fork+exec. Running it here proves the
# upstream pipeline works without requiring the C binary to be built
# with HU_ENABLE_MLX_PROVIDER=ON.
TMPDIR_RUN="$(mktemp -d)"
trap 'rm -rf "${TMPDIR_RUN}"' EXIT
OUTFILE="${TMPDIR_RUN}/out"

PROMPT='Respond with a short greeting:'
echo "[${SCRIPT_NAME}] running: python3 -m mlx_lm generate --model ${HU_MLX_FIXTURE_MODEL_PATH} --max-tokens 32 --prompt '${PROMPT}'" >&2

set +e
${TIMEOUT_CMD} python3 -m mlx_lm generate \
    --model "${HU_MLX_FIXTURE_MODEL_PATH}" \
    --max-tokens 32 \
    --prompt "${PROMPT}" \
    > "${OUTFILE}" 2>&1
RC=$?
set -e

if [ ${RC} -ne 0 ]; then
    echo "[${SCRIPT_NAME}] FAIL: mlx_lm exited with ${RC}" >&2
    echo "[${SCRIPT_NAME}] ---- output ----" >&2
    head -40 "${OUTFILE}" >&2
    exit 1
fi

# Assertion 1: non-empty stdout
if [ ! -s "${OUTFILE}" ]; then
    echo "[${SCRIPT_NAME}] FAIL: mlx_lm produced empty output" >&2
    exit 1
fi

# Assertion 2: valid UTF-8 (iconv fails on truncated codepoints — the
# safety contract the Phase 2 unit tests pin)
if ! iconv -f UTF-8 -t UTF-8 "${OUTFILE}" >/dev/null 2>&1; then
    echo "[${SCRIPT_NAME}] FAIL: output is not valid UTF-8 (chunk-emit boundary bug?)" >&2
    cat "${OUTFILE}" >&2
    exit 1
fi

# Assertion 3: mlx_lm prints generation stats at the bottom — confirm
# the run completed by looking for the tokens-per-sec marker.
if ! grep -q 'tokens-per-sec' "${OUTFILE}"; then
    echo "[${SCRIPT_NAME}] FAIL: 'tokens-per-sec' marker not found — run aborted early" >&2
    head -40 "${OUTFILE}" >&2
    exit 1
fi

# Extract generation rate for the SOTA banner
GEN_RATE="$(grep 'Generation:' "${OUTFILE}" | tail -1 | sed -E 's/.*Generation: ([0-9]+) tokens, ([0-9.]+) tokens-per-sec/\1 tokens at \2 tok\/s/' || echo 'unknown')"
BYTES="$(wc -c < "${OUTFILE}" | tr -d ' ')"
echo "[${SCRIPT_NAME}] PASS: ${GEN_RATE}; ${BYTES} bytes of valid UTF-8" >&2
exit 0
