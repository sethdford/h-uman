#!/usr/bin/env bash
# Spec 2026-05-19 M3 closure / AC-M3-7 / D-M3-8 — fake mlx_lm training shim.
#
# Replaces a real `m3_mlx_lora_bridge.py` invocation in tests so the
# E2E auto-invocation path (Task 16) can run deterministically without
# real GPU work. The shim copies a known-good safetensors fixture in
# place of actually training, producing the same on-disk shape the
# admin-swap endpoint expects.
#
# Contract (matches m3_mlx_lora_bridge.py's exit semantics):
#   --pairs <jsonl>          # required (just for shape parity; not read)
#   --adapter-out <path>     # required: where to write the safetensors
#   --rank <int>             # tolerated (ignored)
#   --iters <int>            # tolerated (ignored)
#   --model <str>            # tolerated (ignored)
#   --learning-rate <float>  # tolerated (ignored)
#   --batch-size <int>       # tolerated (ignored)
#   --check-only             # tolerated (reports as installed)
#
# Exit codes:
#   0 — adapter written successfully
#   2 — input parse failure / required arg missing
#
# This file is intentionally NOT registered in CMakeLists for execution;
# it lives under tests/fixtures/m3/ where the E2E test that needs it
# will reference it by relative path. The HU_IS_TEST C-side guard
# ensures real production code never reaches this shim.

set -euo pipefail

PAIRS=""
ADAPTER_OUT=""
CHECK_ONLY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pairs) PAIRS="$2"; shift 2 ;;
        --adapter-out) ADAPTER_OUT="$2"; shift 2 ;;
        --check-only) CHECK_ONLY=1; shift ;;
        --rank|--iters|--model|--learning-rate|--batch-size)
            shift 2  # consume tolerated flag + value
            ;;
        *)
            # Unknown arg: tolerate but warn so test regressions surface.
            echo "[fake-mlx-lm] note: ignoring unknown arg: $1" >&2
            shift
            ;;
    esac
done

if [[ "$CHECK_ONLY" = "1" ]]; then
    echo "[fake-mlx-lm] check-only OK (fake shim — always 'installed')"
    exit 0
fi

if [[ -z "$ADAPTER_OUT" ]]; then
    echo "[fake-mlx-lm] ERROR: --adapter-out is required" >&2
    exit 2
fi

mkdir -p "$(dirname "$ADAPTER_OUT")"

# Write a minimal safetensors-shaped file:
#   8-byte LE header length + JSON header with a couple of tensor
#   entries + cosmetic body. The header has tensor_count > 0 so the
#   metadata judge in m3_eval_adapter.py treats this as "real".
python3 - "$ADAPTER_OUT" "$PAIRS" <<'PY'
import json, os, struct, sys, time
adapter_out = sys.argv[1]
pairs_path = sys.argv[2] if len(sys.argv) > 2 else ""
pairs_count = 0
if pairs_path and os.path.exists(pairs_path):
    with open(pairs_path) as f:
        pairs_count = sum(1 for line in f if line.strip())

header = {
    "model.layers.0.self_attn.q_proj.lora_a": {
        "dtype": "F32", "shape": [16, 16], "data_offsets": [0, 1024],
    },
    "model.layers.0.self_attn.q_proj.lora_b": {
        "dtype": "F32", "shape": [16, 16], "data_offsets": [1024, 2048],
    },
    "__metadata__": {
        "format": "m3-fake-mlx-train-v1",
        "produced_by": "tests/fixtures/m3/fake_mlx_lm_train.sh",
        "pairs_count": str(pairs_count),
        "produced_at": str(int(time.time())),
    },
}
header_bytes = json.dumps(header).encode("utf-8")
body = b"\0" * 2048
with open(adapter_out, "wb") as f:
    f.write(struct.pack("<Q", len(header_bytes)))
    f.write(header_bytes)
    f.write(body)
print(f"[fake-mlx-lm] wrote {adapter_out} ({os.path.getsize(adapter_out)} bytes)")
PY

exit 0
