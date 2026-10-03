#!/usr/bin/env bash
# Fetch SOC-2508 (Synthetic Online Conversations, Hugging Face marcodsn/SOC-2508)
# into ~/.human/datasets/soc2508/, pinned to a dataset revision and sha256.
# Used only as a prior for multi-message turns and reply delays.
# License: CC BY 4.0.
set -euo pipefail
# shellcheck source=fetch_lib.sh
source "$(cd "$(dirname "$0")" && pwd)/fetch_lib.sh"

PINNED_REVISION="8a95510c3a404edd428734cf9dbcb1671e609425"
PINNED_SHA256="44c5a797b47ab1db5fcfd0ebf5a1327cd7444ca6536028aadeb2279c1573bf67"
URL="https://huggingface.co/datasets/marcodsn/SOC-2508/resolve/${PINNED_REVISION}/data.jsonl"

hu_fetch_pinned soc2508 "$URL" "$PINNED_SHA256" data.jsonl \
  "marcodsn/SOC-2508@${PINNED_REVISION}" \
  "CC BY 4.0 (dataset card): attribution required; synthetic (Qwen3-235B-A22B-Instruct-2507)"
