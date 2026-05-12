#!/usr/bin/env bash
# Phase 1 (RL SOTA) — fetch the Gemma 3 IT 4B Q4_K_M GGUF and verify
# its SHA256 before any test or runner script touches it.
#
# Why a script (not a CMake hook): models are 2.4 GB. We never want to
# pay download cost during build, never want to commit the file, and
# the SHA verification has to be unconditional — a corrupted GGUF can
# silently produce garbage tokens that look like real model output.
#
# Source: ggml-org/gemma-3-4b-it-GGUF (the canonical llama.cpp-side
# distribution; SHA pinned 2026-05-11). If the upstream rotates, this
# script fails noisily — re-pin SHA + URL together, never just one.
#
# Usage:
#   scripts/fetch-gemma.sh                          # default path + model
#   scripts/fetch-gemma.sh --dest /tmp/m.gguf       # explicit destination
#   scripts/fetch-gemma.sh --skip-verify            # download w/o SHA check (CI debug only)
#   scripts/fetch-gemma.sh --check-only             # verify existing file, never download
#
# Exit codes:
#   0  success (file present + SHA matches, or --check-only verified)
#   1  download failed (network, HF unreachable, partial transfer)
#   2  SHA mismatch (file present but corrupted — script will NOT delete it)
#   3  --check-only and file missing
#   4  required tool missing (curl + sha256sum/shasum)

set -euo pipefail

# ── pinned model identity ──────────────────────────────────────────────
MODEL_URL="https://huggingface.co/ggml-org/gemma-3-4b-it-GGUF/resolve/main/gemma-3-4b-it-Q4_K_M.gguf"
MODEL_SHA="882e8d2db44dc554fb0ea5077cb7e4bc49e7342a1f0da57901c0802ea21a0863"
MODEL_SIZE_BYTES=2489757856  # 2.32 GiB; fits within HF's free LFS quota
MODEL_FILENAME="gemma-3-4b-it-Q4_K_M.gguf"

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
      echo "fetch-gemma: unknown arg: $1" >&2
      exit 4
      ;;
  esac
done

# ── tool detection ────────────────────────────────────────────────────
if ! command -v curl >/dev/null 2>&1; then
  echo "fetch-gemma: curl is required (install via your package manager)" >&2
  exit 4
fi

# macOS ships shasum, Linux ships sha256sum — both work, normalize here.
sha256_of() {
  local path="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${path}" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "${path}" | awk '{print $1}'
  else
    echo "fetch-gemma: need sha256sum or shasum to verify" >&2
    exit 4
  fi
}

# ── verification helper ───────────────────────────────────────────────
verify_or_die() {
  local path="$1"
  local actual
  actual="$(sha256_of "${path}")"
  if [[ "${actual}" != "${MODEL_SHA}" ]]; then
    echo "fetch-gemma: SHA MISMATCH" >&2
    echo "  expected: ${MODEL_SHA}" >&2
    echo "  actual:   ${actual}" >&2
    echo "  file kept at ${path} for forensic inspection (delete manually before retry)" >&2
    exit 2
  fi
  echo "fetch-gemma: SHA verified ✓ (${actual:0:16}…${actual: -8})"
}

# ── main flow ─────────────────────────────────────────────────────────
mkdir -p "$(dirname "${DEST}")"

if [[ -f "${DEST}" ]]; then
  if [[ "${SKIP_VERIFY}" -eq 1 ]]; then
    echo "fetch-gemma: ${DEST} present, --skip-verify → assuming good"
    exit 0
  fi
  echo "fetch-gemma: ${DEST} already present, verifying…"
  verify_or_die "${DEST}"
  exit 0
fi

if [[ "${CHECK_ONLY}" -eq 1 ]]; then
  echo "fetch-gemma: --check-only and ${DEST} is missing" >&2
  exit 3
fi

# Download with resume support (-C -). HF redirects to CloudFront so -L
# is mandatory. --fail turns 4xx/5xx into a non-zero exit so we don't
# silently accept an HTML error page as a GGUF.
echo "fetch-gemma: downloading ${MODEL_URL}"
echo "fetch-gemma:   ↓ ${DEST} (~$(( MODEL_SIZE_BYTES / 1024 / 1024 )) MiB)"
if ! curl -L --fail --retry 3 --retry-delay 5 -C - \
        --output "${DEST}.partial" "${MODEL_URL}"; then
  echo "fetch-gemma: download failed; partial file at ${DEST}.partial" >&2
  exit 1
fi

# Atomic install: only rename to the final path once the byte stream
# completed. A crashed curl leaves .partial behind for resume; it never
# pollutes the install path.
mv "${DEST}.partial" "${DEST}"

if [[ "${SKIP_VERIFY}" -eq 1 ]]; then
  echo "fetch-gemma: --skip-verify (CI debug mode) — NOT verifying SHA"
  echo "fetch-gemma: model written to ${DEST}"
  exit 0
fi

verify_or_die "${DEST}"
echo "fetch-gemma: model installed at ${DEST}"
