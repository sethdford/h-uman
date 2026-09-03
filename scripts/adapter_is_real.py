#!/usr/bin/env python3
"""Is this adapter a REAL trained LoRA, or a no-op / empty placeholder?

Two shapes have shipped as 'success' in this repo: an empty-tensors safetensors
written by a failed run (349 bytes, 2026-09-02), and full-size adapters whose
lora_b tensors were all zero (the ORPO zero-gradient bug, 2026-08). Both pass a
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
