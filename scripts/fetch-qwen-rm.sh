#!/usr/bin/env bash
# Phase 3 Task 8 (RL SOTA) — fetch the Qwen 2.5 0.5B Instruct Q4_K_M GGUF
# and verify its SHA256 before any RM inference test touches it.
#
# Mirrors scripts/fetch-gemma.sh byte-for-byte at the structural level
# (curl + sha256sum/shasum verify + --check-only mode + non-destructive
# on mismatch).
#
# Source: Qwen/Qwen2.5-0.5B-Instruct-GGUF on HuggingFace. SHA pinned
# against scripts/fetch-qwen-rm.sh.sha256.
#
# Usage:
#   scripts/fetch-qwen-rm.sh                          # default path + model
#   scripts/fetch-qwen-rm.sh --dest /tmp/m.gguf       # explicit destination
#   scripts/fetch-qwen-rm.sh --skip-verify            # download w/o SHA check (CI debug only)
#   scripts/fetch-qwen-rm.sh --check-only             # verify existing file, never download
#
# Exit codes:
#   0  success (file present + SHA matches, or --check-only verified)
#   1  download failed (network, HF unreachable, partial transfer)
#   2  SHA mismatch (file present but corrupted — script will NOT delete it)
#   3  --check-only and file missing
#   4  required tool missing (curl + sha256sum/shasum)

set -euo pipefail

# ── pinned model identity ──────────────────────────────────────────────
MODEL_URL="https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf"
MODEL_FILENAME="qwen-2.5-0.5b-instruct-q4_k_m.gguf"

# Read expected SHA from the sidecar file (same directory as this script).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHA_FILE="${SCRIPT_DIR}/fetch-qwen-rm.sh.sha256"
if [[ ! -f "${SHA_FILE}" ]]; then
  echo "fetch-qwen-rm: SHA sidecar missing: ${SHA_FILE}" >&2
  exit 4
fi
MODEL_SHA="$(head -1 "${SHA_FILE}" | awk '{print $1}')"
if [[ -z "${MODEL_SHA}" ]]; then
  echo "fetch-qwen-rm: SHA sidecar is empty" >&2
  exit 4
fi

# ── argument parsing ──────────────────────────────────────────────────
DEFAULT_DEST="${HOME}/.human/models/${MODEL_FILENAME}"
DEST="${DEFAULT_DEST}"
SKIP_VERIFY=0
CHECK_ONLY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dest)        DEST="${2:?--dest needs a path}"; shift 2 ;;
    --skip-verify) SKIP_VERIFY=1; shift ;;
    --check-only)  CHECK_ONLY=1; shift ;;
    -h|--help)
      sed -n '2,30p' "$0" | sed 's/^# \?//'
      exit 0
      ;;
    *)
      echo "fetch-qwen-rm: unknown arg: $1" >&2
      exit 4
      ;;
  esac
done

# ── tool detection ────────────────────────────────────────────────────
if ! command -v curl >/dev/null 2>&1; then
  echo "fetch-qwen-rm: curl is required (install via your package manager)" >&2
  exit 4
fi

sha256_of() {
  local path="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${path}" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "${path}" | awk '{print $1}'
  else
    echo "fetch-qwen-rm: need sha256sum or shasum to verify" >&2
    exit 4
  fi
}

# ── verification helper ───────────────────────────────────────────────
verify_or_die() {
  local path="$1"
  local actual
  actual="$(sha256_of "${path}")"
  if [[ "${actual}" != "${MODEL_SHA}" ]]; then
    echo "fetch-qwen-rm: SHA MISMATCH" >&2
    echo "  expected: ${MODEL_SHA}" >&2
    echo "  actual:   ${actual}" >&2
    echo "  file kept at ${path} for forensic inspection (delete manually before retry)" >&2
    exit 2
  fi
  echo "fetch-qwen-rm: SHA verified (${actual:0:16}...${actual: -8})"
}

# ── main flow ─────────────────────────────────────────────────────────
mkdir -p "$(dirname "${DEST}")"

if [[ -f "${DEST}" ]]; then
  if [[ "${SKIP_VERIFY}" -eq 1 ]]; then
    echo "fetch-qwen-rm: ${DEST} present, --skip-verify -> assuming good"
    exit 0
  fi
  echo "fetch-qwen-rm: ${DEST} already present, verifying..."
  verify_or_die "${DEST}"
  exit 0
fi

if [[ "${CHECK_ONLY}" -eq 1 ]]; then
  echo "fetch-qwen-rm: --check-only and ${DEST} is missing" >&2
  exit 3
fi

echo "fetch-qwen-rm: downloading ${MODEL_URL}"
echo "fetch-qwen-rm:   -> ${DEST}"
if ! curl -L --fail --retry 3 --retry-delay 5 -C - \
        --output "${DEST}.partial" "${MODEL_URL}"; then
  echo "fetch-qwen-rm: download failed; partial file at ${DEST}.partial" >&2
  exit 1
fi

mv "${DEST}.partial" "${DEST}"

if [[ "${SKIP_VERIFY}" -eq 1 ]]; then
  echo "fetch-qwen-rm: --skip-verify (CI debug mode) — NOT verifying SHA"
  echo "fetch-qwen-rm: model written to ${DEST}"
  exit 0
fi

verify_or_die "${DEST}"
echo "fetch-qwen-rm: model installed at ${DEST}"
