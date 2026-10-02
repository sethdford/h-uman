#!/usr/bin/env bash
# Fetch Multi-Session Chat (Xu et al. 2022, ParlAI) into ~/.human/datasets/msc/
# and extract only msc_dialogue/session_5 (the episodes that carry all five
# sessions). The sha256 is the one ParlAI's own parlai/tasks/msc/build.py pins.
# See docs/guides/memory-benchmarks.md for the license note.
set -euo pipefail
# shellcheck source=fetch_lib.sh
source "$(cd "$(dirname "$0")" && pwd)/fetch_lib.sh"

PINNED_REVISION="msc_v0.1"
PINNED_SHA256="e640e37cf4317cd09fc02a4cd57ef130a185f23635f4003b0cee341ffcb45e60"
URL="https://dl.fbaipublicfiles.com/parlai/msc/${PINNED_REVISION}.tar.gz"

hu_fetch_pinned msc "$URL" "$PINNED_SHA256" "${PINNED_REVISION}.tar.gz" \
  "parlai ${PINNED_REVISION} (sha256 pinned by facebookresearch/ParlAI parlai/tasks/msc/build.py)" \
  "no dataset-specific license file; distributed by ParlAI (MIT-licensed repo): research/internal evaluation"

marker="$HU_DATASET_DIR/session_5/.extracted-from"
if [[ -f "$marker" && "$(cat "$marker")" == "${HU_FETCH_TEST_SHA256:-$PINNED_SHA256}" ]]; then
  echo "[msc] session_5 already extracted"
  exit 0
fi
tmpdir="$(mktemp -d "$HU_DATASET_DIR/.extract.XXXXXX")"
trap 'rm -rf "$tmpdir"' EXIT
tar -xzf "$HU_DATASET_FILE" -C "$tmpdir" msc/msc_dialogue/session_5
rm -rf "$HU_DATASET_DIR/session_5"
mv "$tmpdir/msc/msc_dialogue/session_5" "$HU_DATASET_DIR/session_5"
echo "${HU_FETCH_TEST_SHA256:-$PINNED_SHA256}" >"$marker"
echo "[msc] extracted session_5 -> $HU_DATASET_DIR/session_5"
