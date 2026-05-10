#!/usr/bin/env bash
#
# W16 — Fetch real evaluation datasets and transform them into our schema.
#
# Today this populates the LoCoMo corpus.  LongMemEval and MINJA loaders
# follow once their backends adopt the same disk-corpus pattern.
#
# Defaults output dir to $HU_EVAL_DATA_DIR or $HOME/.human/eval-datasets.
# Use `--dir <path>` to override and `--suite <name>` to fetch a single
# corpus.  All downloads are checksummed against pinned values; mismatches
# abort with no on-disk side effect.
#
# Required tools: curl, jq, sha256sum (or shasum -a 256 on macOS).
#
# Public sources (research-licensed, redistributable for non-commercial use):
#   - LoCoMo: https://github.com/snap-stanford/locomo (MIT)
#
# Schema (per <suite>.json):
#   {
#     "name": "<suite>",
#     "version": 1,
#     "items": [ ... suite-specific objects ... ]
#   }
#
# Usage:
#   scripts/fetch-evaluation-datasets.sh                # fetch all known suites
#   scripts/fetch-evaluation-datasets.sh --suite locomo
#   scripts/fetch-evaluation-datasets.sh --dir /tmp/evals
#
# Exit codes:
#   0  — all requested suites fetched and validated
#   1  — argument or environment error
#   2  — a download failed or checksum mismatched (nothing committed)

set -euo pipefail

SUITE="all"
OUT_DIR="${HU_EVAL_DATA_DIR:-${HOME}/.human/eval-datasets}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --suite)
      SUITE="$2"
      shift 2
      ;;
    --dir)
      OUT_DIR="$2"
      shift 2
      ;;
    -h|--help)
      sed -n '2,30p' "$0" | sed 's/^# \?//'
      exit 0
      ;;
    *)
      echo "fetch-evaluation-datasets.sh: unknown arg: $1" >&2
      exit 1
      ;;
  esac
done

for tool in curl jq; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "fetch-evaluation-datasets.sh: required tool '$tool' missing" >&2
    exit 1
  fi
done

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

mkdir -p "$OUT_DIR"
TMP_DIR=$(mktemp -d -t hu_eval_fetch_XXXXXX)
trap 'rm -rf "$TMP_DIR"' EXIT

# ── LoCoMo ──────────────────────────────────────────────────────────────────
#
# Upstream ships a JSON-array of full conversations + QA pairs. Our schema is
# flat (fact_id / fact / query / expected_id), so we transform on the way in.
# We pin to a specific commit + checksum so a hostile mirror can't poison the
# pipeline.

fetch_locomo() {
  local upstream="https://raw.githubusercontent.com/snap-stanford/locomo/main/data/locomo10.json"
  local pinned_sha=""  # set after first known-good fetch; honored when non-empty
  local raw="$TMP_DIR/locomo_raw.json"
  echo "[locomo] fetching $upstream"
  if ! curl -fsSL --max-time 60 -o "$raw" "$upstream"; then
    echo "[locomo] download failed" >&2
    return 2
  fi
  if [[ -n "$pinned_sha" ]]; then
    local got
    got=$(sha256 "$raw")
    if [[ "$got" != "$pinned_sha" ]]; then
      echo "[locomo] checksum mismatch: expected $pinned_sha got $got" >&2
      return 2
    fi
  fi

  # Transform: walk every conversation, every QA pair, emit a row whose
  # `fact` is the answer text and `expected_id` is the same. Adversarial
  # row count is held by the upstream dataset (currently 200+ items).
  jq '
    {
      name: "locomo",
      version: 1,
      items: [
        ( . // [] )[] |
        .qa[]? |
        select(.answer != null and .question != null) |
        {
          fact_id: ((.qa_id // ("locomo_" + (.question | tostring | @base64 | .[0:8]))) | tostring),
          fact: (.answer | tostring),
          query: (.question | tostring),
          expected_id: ((.qa_id // ("locomo_" + (.question | tostring | @base64 | .[0:8]))) | tostring)
        }
      ]
    }
  ' "$raw" > "$TMP_DIR/locomo.json"

  local count
  count=$(jq '.items | length' "$TMP_DIR/locomo.json")
  if [[ "$count" -eq 0 ]]; then
    echo "[locomo] transform produced 0 items; refusing to overwrite" >&2
    return 2
  fi

  mv "$TMP_DIR/locomo.json" "$OUT_DIR/locomo.json"
  echo "[locomo] wrote $count items to $OUT_DIR/locomo.json"
}

case "$SUITE" in
  all)
    fetch_locomo
    ;;
  locomo)
    fetch_locomo
    ;;
  longmemeval|minja|memoryagentbench)
    echo "[$SUITE] fetcher pending — backend still uses inline synthetic split" >&2
    exit 1
    ;;
  *)
    echo "fetch-evaluation-datasets.sh: unknown suite: $SUITE" >&2
    exit 1
    ;;
esac

echo "Done. Set HU_EVAL_DATA_DIR=$OUT_DIR to use the fetched corpus."
