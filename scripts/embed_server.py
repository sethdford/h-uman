#!/usr/bin/env python3
"""Standalone /v1/embeddings server (nomic modernbert via mlx_embeddings).

Production hosts this endpoint INSIDE mlx-server (:8741) so the GPU has one
owner. This standalone copy exists for two reasons: (1) proving the C
semantic-recall path before the production server is restarted, and (2) dev
boxes without the 54 GB model. It loads ~150 MB — an embedding model, not an
LLM — and refuses to start on :8741.

    python3 scripts/embed_server.py --port 8749
"""
import argparse, json, sys, time
from http.server import BaseHTTPRequestHandler, HTTPServer

MODEL_ID = "mlx-community/nomicai-modernbert-embed-base-8bit"
_MODEL = _TOK = None

def _load():
    global _MODEL, _TOK
    if _MODEL is None:
        from mlx_embeddings import load
        _MODEL, _TOK = load(MODEL_ID)

def embed(texts):
    import mlx.core as mx
    _load()
    ins = _TOK.batch_encode_plus(texts, return_tensors="mlx", padding=True, truncation=True, max_length=512)
    out = _MODEL(ins["input_ids"], attention_mask=ins.get("attention_mask"))
    v = out.text_embeds if hasattr(out, "text_embeds") else out
    v = v / mx.linalg.norm(v, axis=-1, keepdims=True)
    mx.eval(v)
    return v.tolist(), int(ins["input_ids"].size)

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _json(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code)
        self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b)))
        self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        if self.path == "/health": return self._json(200, {"status": "ok", "model": MODEL_ID, "loaded": _MODEL is not None})
        self._json(404, {"error": "not found"})
    def do_POST(self):
        if self.path != "/v1/embeddings": return self._json(404, {"error": "not found"})
        try: req = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))).decode("utf-8", "replace"))
        except Exception: return self._json(400, {"error": "invalid JSON"})
        inp = req.get("input"); texts = [inp] if isinstance(inp, str) else inp
        if not isinstance(texts, list) or not texts or not all(isinstance(t, str) and t.strip() for t in texts):
            return self._json(400, {"error": "input must be a non-empty string or list of strings"})
        if len(texts) > 256: return self._json(413, {"error": "max 256 inputs"})
        try: vecs, ntok = embed(texts)
        except Exception as e: return self._json(500, {"error": f"embedding failed: {type(e).__name__}: {e}"})
        self._json(200, {"object": "list", "model": MODEL_ID,
                         "data": [{"object": "embedding", "index": i, "embedding": v} for i, v in enumerate(vecs)],
                         "usage": {"prompt_tokens": ntok, "total_tokens": ntok}})

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--port", type=int, default=8749); ap.add_argument("--host", default="127.0.0.1")
    a = ap.parse_args()
    if a.port == 8741: sys.exit("refusing :8741 — that is the production mlx-server")
    t0 = time.time(); _load(); print(f"[embed-server] {MODEL_ID} loaded in {time.time()-t0:.1f}s, listening on {a.host}:{a.port}", flush=True)
    HTTPServer((a.host, a.port), H).serve_forever()

if __name__ == "__main__": main()
