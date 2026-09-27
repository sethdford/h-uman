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

def _good_adapter():
    """An adapter comfortably ABOVE MIN_BYTES, so corrupting it exercises the
    completeness check rather than the size floor (which fires first)."""
    d = tempfile.mkdtemp()
    big = np.zeros((MIN_BYTES,), np.float32)          # ~4x MIN_BYTES of payload
    write_st(os.path.join(d, "adapters.safetensors"),
             {"l.lora_a": big, "l.lora_b": np.ones((16,), np.float32)})
    ok, why = adapter_is_real(d); assert ok, why
    return d

def test_truncated_adapter_is_not_real():
    """A trainer killed mid-write leaves a short file whose header still promises
    every tensor. numpy reads a short buffer as a shorter-but-valid array, so the
    non-zero scan alone called this REAL until 2026-09-20."""
    d = _good_adapter(); p = os.path.join(d, "adapters.safetensors")
    before = os.path.getsize(p)
    os.truncate(p, before - 4000)                     # multiple of 4: still valid F32
    assert os.path.getsize(p) > MIN_BYTES, "fixture must stay above the size floor"
    ok, why = adapter_is_real(d)
    assert not ok, "truncated adapter must not pass the guard"
    assert "truncated" in why and "-4000" in why, why

def test_truncated_by_one_element_is_not_real():
    """The smallest truncation that keeps the buffer dtype-aligned."""
    d = _good_adapter(); p = os.path.join(d, "adapters.safetensors")
    os.truncate(p, os.path.getsize(p) - 4)
    ok, why = adapter_is_real(d); assert not ok and "truncated" in why, why

def test_trailing_bytes_is_not_real():
    d = _good_adapter(); p = os.path.join(d, "adapters.safetensors")
    with open(p, "ab") as f: f.write(b"\0" * 64)
    ok, why = adapter_is_real(d)
    assert not ok and "trailing bytes" in why, why

def test_header_shape_disagreeing_with_span_is_not_real():
    """A header whose declared shape does not match its own byte span is
    malformed even when the file length adds up. Built by hand so ONLY the
    shape check can fire."""
    d = tempfile.mkdtemp(); p = os.path.join(d, "adapters.safetensors")
    a = np.zeros((MIN_BYTES,), np.float32); b = np.ones((16,), np.float32)
    hdr = {"l.lora_a": {"dtype": "F32", "shape": [a.size], "data_offsets": [0, a.nbytes]},
           "l.lora_b": {"dtype": "F32", "shape": [999],    # lies: 16 floats are stored
                        "data_offsets": [a.nbytes, a.nbytes + b.nbytes]}}
    h = json.dumps(hdr).encode()
    with open(p, "wb") as f:
        f.write(struct.pack("<Q", len(h))); f.write(h); f.write(a.tobytes()); f.write(b.tobytes())
    ok, why = adapter_is_real(d)
    assert not ok and "needs" in why, why
