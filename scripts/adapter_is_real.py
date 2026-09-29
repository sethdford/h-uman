#!/usr/bin/env python3
"""Is this adapter a REAL trained LoRA, or a no-op / empty placeholder?

Three shapes have shipped as 'success' in this repo: an empty-tensors safetensors
written by a failed run (349 bytes, 2026-09-02), full-size adapters whose lora_b
tensors were all zero (the ORPO zero-gradient bug, 2026-08), and a TRUNCATED
file from a trainer killed mid-write (2026-09-20) — which this guard itself
reported as REAL until the completeness check below was added. All three pass a
file-exists check. This is the check every stage/swap/registration must run.

    adapter_is_real.py <adapter_dir>   -> exit 0 REAL / 1 NOT REAL (prints why)
Reads only the safetensors header + the lora_b tensors; never loads a model.
"""
import json, os, struct, sys

MIN_BYTES = 1_000_000

def safetensors_header(path):
    with open(path, "rb") as f:
        n = struct.unpack("<Q", f.read(8))[0]
        return json.loads(f.read(n)), 8 + n

def adapter_is_real(adapter_dir):
    p = os.path.join(adapter_dir, "adapters.safetensors")
    if not os.path.exists(p):
        return False, "adapters.safetensors missing"
    size = os.path.getsize(p)
    if size < MIN_BYTES:
        return False, f"adapters.safetensors is {size} bytes (< {MIN_BYTES}); empty placeholder"
    hdr, base = safetensors_header(p)
    # Completeness BEFORE contents. A crashed trainer leaves a half-written
    # file whose header still promises every tensor; numpy reads a short buffer
    # as a shorter-but-valid array, so the non-zero scan below reports such a
    # file REAL (verified 2026-09-20: truncating 4000 bytes off a good adapter
    # still printed "REAL: 1/1 lora_b tensors non-zero"). The header's own
    # declared extent is the ground truth for how many bytes should be here.
    spans = [v["data_offsets"] for v in hdr.values()
             if isinstance(v, dict) and isinstance(v.get("data_offsets"), (list, tuple))]
    expected = base + (max(e for _, e in spans) if spans else 0)
    if size != expected:
        kind = "truncated" if size < expected else "has trailing bytes"
        return False, (f"adapters.safetensors {kind}: {size} bytes on disk, header declares "
                       f"{expected} (delta {size - expected:+d}) — an interrupted write, "
                       "not a finished adapter")
    lora_b = {k: v for k, v in hdr.items() if k.endswith("lora_b") and isinstance(v, dict)}
    if not lora_b:
        return False, "no lora_b tensors in header"
    import numpy as np
    nz = 0
    with open(p, "rb") as f:
        for k, v in lora_b.items():
            s, e = v["data_offsets"]
            f.seek(base + s); buf = f.read(e - s)
            dt = {"F32": np.float32, "F16": np.float16, "BF16": np.uint16}[v["dtype"]]
            width = np.dtype(dt).itemsize
            want = width
            for d in v.get("shape", []):
                want *= d
            if (e - s) != want or len(buf) != (e - s):
                return False, (f"{k}: header spans {e - s} bytes, shape {v.get('shape')} needs "
                               f"{want}, read {len(buf)} — malformed or truncated adapter")
            arr = np.frombuffer(buf, dtype=dt)
            if v["dtype"] == "BF16":
                arr = (arr.astype(np.uint32) << 16).view(np.float32)
            if np.abs(arr.astype(np.float32)).max() > 0:
                nz += 1
    if nz == 0:
        return False, f"all {len(lora_b)} lora_b tensors are zero (adapter == base model)"
    cfg = os.path.join(adapter_dir, "adapter_config.json")
    if os.path.exists(cfg):
        scale = json.load(open(cfg)).get("lora_parameters", {}).get("scale")
        if scale is not None and float(scale) > 4.0:
            return False, f"lora_parameters.scale={scale} > 4.0 (catastrophic; see lora-scale-default-or-die)"
    return True, f"{size} bytes, {nz}/{len(lora_b)} lora_b tensors non-zero"

if __name__ == "__main__":
    ok, why = adapter_is_real(sys.argv[1])
    print(("REAL: " if ok else "NOT REAL: ") + why)
    sys.exit(0 if ok else 1)
