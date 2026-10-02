#!/usr/bin/env bash
# Fetch LoCoMo (Snap Research, ACL 2024) into ~/.human/datasets/locomo/,
# pinned to an upstream commit and sha256. See docs/guides/memory-benchmarks.md.
# License: CC BY-NC 4.0 — internal evaluation only; do not redistribute.
set -euo pipefail
# shellcheck source=fetch_lib.sh
source "$(cd "$(dirname "$0")" && pwd)/fetch_lib.sh"

PINNED_REVISION="3eb6f2c585f5e1699204e3c3bdf7adc5c28cb376"
PINNED_SHA256="79fa87e90f04081343b8c8debecb80a9a6842b76a7aa537dc9fdf651ea698ff4"
URL="https://raw.githubusercontent.com/snap-research/locomo/${PINNED_REVISION}/data/locomo10.json"

hu_fetch_pinned locomo "$URL" "$PINNED_SHA256" locomo10.json \
  "snap-research/locomo@${PINNED_REVISION}" \
  "CC BY-NC 4.0 (snap-research/locomo LICENSE.txt): non-commercial, internal evaluation only"
