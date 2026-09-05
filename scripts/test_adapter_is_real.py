import json, os, struct, tempfile, numpy as np, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from adapter_is_real import adapter_is_real, MIN_BYTES

def write_st(path, tensors):
    hdr = {}; blobs = []; off = 0
    for k, arr in tensors.items():
        b = arr.astype(np.float32).tobytes(); hdr[k] = {"dtype": "F32", "shape": list(arr.shape), "data_offsets": [off, off + len(b)]}; blobs.append(b); off += len(b)
    h = json.dumps(hdr).encode()
    with open(path, "wb") as f: f.write(struct.pack("<Q", len(h))); f.write(h); f.write(b"".join(blobs))

def test_empty_placeholder_is_not_real():
    d = tempfile.mkdtemp(); write_st(os.path.join(d, "adapters.safetensors"), {})
    ok, why = adapter_is_real(d); assert not ok and "bytes" in why

def test_zero_lora_b_is_not_real_and_nonzero_is_real():
    d = tempfile.mkdtemp(); big = np.zeros((MIN_BYTES // 4 + 8,), np.float32)
    write_st(os.path.join(d, "adapters.safetensors"), {"l.lora_a": big, "l.lora_b": np.zeros((16,), np.float32)})
    ok, why = adapter_is_real(d); assert not ok and "zero" in why
    write_st(os.path.join(d, "adapters.safetensors"), {"l.lora_a": big, "l.lora_b": np.ones((16,), np.float32)})
    ok, why = adapter_is_real(d); assert ok, why

def test_catastrophic_scale_is_not_real():
    d = tempfile.mkdtemp(); big = np.zeros((MIN_BYTES // 4 + 8,), np.float32)
    write_st(os.path.join(d, "adapters.safetensors"), {"l.lora_a": big, "l.lora_b": np.ones((16,), np.float32)})
    json.dump({"lora_parameters": {"scale": 20.0}}, open(os.path.join(d, "adapter_config.json"), "w"))
    ok, why = adapter_is_real(d); assert not ok and "scale" in why
