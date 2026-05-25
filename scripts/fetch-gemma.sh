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
# Phase 4d (Gemma throughput program) — speculative-decode draft model:
#   scripts/fetch-gemma.sh --draft URL --draft-sha SHA
#                                                   # fetch main + a small draft
#                                                   # alongside (lands at
#                                                   # ~/.human/models/$(basename URL))
#   scripts/fetch-gemma.sh --draft URL --draft-skip-verify
#                                                   # CI / operator-knows-best mode
#
# Pick the draft per the Phase 3b plan: small enough to run on CPU at high
# tok/s (e.g. Gemma-3-270M-it Q4_K_M GGUF) AND same tokenizer family as
# your target. Acceptance rate climbs once Phase 6 trains a persona-
# aligned draft via the same LoRA pipeline used for personalization.
#
# Exit codes:
#   0  success (file present + SHA matches, or --check-only verified)
#   1  download failed (network, HF unreachable, partial transfer)
#   2  SHA mismatch (file present but corrupted — script will NOT delete it)
#   3  --check-only and file missing
#   4  required tool missing (curl + sha256sum/shasum)
#   5  --draft without --draft-sha or --draft-skip-verify

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
# Phase 4d — optional draft for speculative decoding.
DRAFT_URL=""
DRAFT_SHA=""
DRAFT_SKIP_VERIFY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dest)              DEST="${2:?--dest needs a path}"; shift 2 ;;
    --skip-verify)       SKIP_VERIFY=1; shift ;;
    --check-only)        CHECK_ONLY=1; shift ;;
    --draft)             DRAFT_URL="${2:?--draft needs a URL}"; shift 2 ;;
    --draft-sha)         DRAFT_SHA="${2:?--draft-sha needs a hex digest}"; shift 2 ;;
    --draft-skip-verify) DRAFT_SKIP_VERIFY=1; shift ;;
    -h|--help)
      sed -n '2,46p' "$0" | sed 's/^# \?//'
      exit 0
      ;;
    *)
      echo "fetch-gemma: unknown arg: $1" >&2
      exit 4
      ;;
  esac
done

# Phase 4d — if --draft set, require --draft-sha or --draft-skip-verify.
# This is the same SHA-pinning discipline the main model uses; a draft
# without verification is just as silently-corruptible (worse, actually,
# because spec-decode garbage looks like low acceptance rate, not
# garbage tokens).
if [[ -n "${DRAFT_URL}" && -z "${DRAFT_SHA}" && "${DRAFT_SKIP_VERIFY}" -eq 0 ]]; then
  echo "fetch-gemma: --draft requires --draft-sha SHA256 (or --draft-skip-verify for CI)" >&2
  exit 5
fi

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
# Phase 4d — generalized to take expected SHA as arg so it works for
# both main + draft. The pre-Phase-4d single-purpose verify_or_die
# captured MODEL_SHA from the surrounding scope; the new signature
# makes the contract explicit at call time.
verify_or_die() {
  local path="$1"
  local expected_sha="$2"
  local label="$3"  # for log line; e.g. "model" or "draft"
  local actual
  actual="$(sha256_of "${path}")"
  if [[ "${actual}" != "${expected_sha}" ]]; then
    echo "fetch-gemma: ${label} SHA MISMATCH" >&2
    echo "  expected: ${expected_sha}" >&2
    echo "  actual:   ${actual}" >&2
    echo "  file kept at ${path} for forensic inspection (delete manually before retry)" >&2
    exit 2
  fi
  echo "fetch-gemma: ${label} SHA verified ✓ (${actual:0:16}…${actual: -8})"
}

# Phase 4d — generic download+verify, factored out so main + draft share
# one implementation. Returns 0 on success; exits on failure.
fetch_one() {
  local url="$1"
  local dest="$2"
  local expected_sha="$3"     # empty string OR "skip" → no verify
  local label="$4"            # e.g. "model" or "draft"

  mkdir -p "$(dirname "${dest}")"

  if [[ -f "${dest}" ]]; then
    if [[ -z "${expected_sha}" || "${expected_sha}" == "skip" ]]; then
      echo "fetch-gemma: ${label} ${dest} present, no SHA → assuming good"
      return 0
    fi
    echo "fetch-gemma: ${label} ${dest} already present, verifying…"
    verify_or_die "${dest}" "${expected_sha}" "${label}"
    return 0
  fi

  echo "fetch-gemma: downloading ${label}: ${url}"
  echo "fetch-gemma:   ↓ ${dest}"
  if ! curl -L --fail --retry 3 --retry-delay 5 -C - \
          --output "${dest}.partial" "${url}"; then
    echo "fetch-gemma: ${label} download failed; partial file at ${dest}.partial" >&2
    exit 1
  fi
  mv "${dest}.partial" "${dest}"

  if [[ -z "${expected_sha}" || "${expected_sha}" == "skip" ]]; then
    echo "fetch-gemma: ${label} SHA verify skipped — file written to ${dest}"
    return 0
  fi
  verify_or_die "${dest}" "${expected_sha}" "${label}"
  echo "fetch-gemma: ${label} installed at ${dest}"
}

# ── main flow ─────────────────────────────────────────────────────────
# Phase 4d — main model uses the shared fetch_one. The --check-only
# path still short-circuits before download since that's main-only.
if [[ "${CHECK_ONLY}" -eq 1 ]]; then
  if [[ ! -f "${DEST}" ]]; then
    echo "fetch-gemma: --check-only and ${DEST} is missing" >&2
    exit 3
  fi
  verify_or_die "${DEST}" "${MODEL_SHA}" "model"
  exit 0
fi

# Pick the SHA gate ONCE for clarity. "skip" propagates through
# fetch_one and is logged so the operator sees they're in an
# unverified-trust mode.
MAIN_SHA="${MODEL_SHA}"
if [[ "${SKIP_VERIFY}" -eq 1 ]]; then
  MAIN_SHA="skip"
fi
fetch_one "${MODEL_URL}" "${DEST}" "${MAIN_SHA}" "model"

# Phase 4d — optional draft model alongside the main. Same atomic-
# install + SHA discipline. Lands at ~/.human/models/$(basename URL) so
# operators have a predictable path to point HU_LLAMACPP_DRAFT_MODEL at.
if [[ -n "${DRAFT_URL}" ]]; then
  DRAFT_DEST="${HOME}/.human/models/$(basename "${DRAFT_URL%%\?*}")"
  DRAFT_GATE="${DRAFT_SHA}"
  if [[ "${DRAFT_SKIP_VERIFY}" -eq 1 ]]; then
    DRAFT_GATE="skip"
  fi
  fetch_one "${DRAFT_URL}" "${DRAFT_DEST}" "${DRAFT_GATE}" "draft"
  echo
  echo "fetch-gemma: draft ready. To enable spec decode for the llama.cpp"
  echo "             provider, set in your daemon's env:"
  echo "                 HU_LLAMACPP_DRAFT_MODEL=${DRAFT_DEST}"
  echo "             OR add to ~/.human/config.json:"
  echo "                 \"inference\": { \"draft_model\": \"${DRAFT_DEST}\" }"
fi
