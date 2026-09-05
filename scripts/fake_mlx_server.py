#!/usr/bin/env python3
"""Test-only stand-in for mlx-server, shared by the blind-A/B test suites.

Answers the three endpoints the provenance path and the generators touch:
  GET  /v1/adapters/current  -> {"adapter_path", "tensors_loaded"}
  GET  /health               -> {"model", "active_adapter", "adapter_applied", "tensors_loaded"}
  POST /v1/chat/completions  -> one canned choice; increments `calls`

`adapter=None` models a server with no adapter loaded; `tensors=0` with an
adapter named models the 2026-07-26 -> 09-04 lie (adapter applied with
load_weights(strict=False), zero tensors bound). Same shape as the inline
_FakeMlx in blind_ab/test_gen_classifier_trials.py, lifted here so the gate
writer tests do not copy it a third time.

Also carries fake_dump_prompt_head(): a stand-in for tools/dump_prompt_head so
production_system_prompt() can build a >=500-byte head without the C binary.
"""
import http.server
import json
import os
import stat
import threading


class FakeMlx:
    def __init__(self, adapter, tensors, model="fake-base"):
        parent = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):  # quiet
                pass

            def _send(self, obj, code=200):
                body = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path == "/v1/adapters/current":
                    self._send({"adapter_path": parent.adapter, "tensors_loaded": parent.tensors})
                elif self.path == "/health":
                    self._send({"model": parent.model, "active_adapter": parent.adapter,
                                "adapter_applied": bool(parent.adapter),
                                "tensors_loaded": parent.tensors})
                else:
                    self._send({"error": "nope"}, 404)

            def do_POST(self):
                n = int(self.headers.get("Content-Length", "0"))
                self.rfile.read(n)
                parent.calls += 1
                self._send({"model": parent.model,
                            "choices": [{"message": {"content": f"fake reply {parent.calls}"}}]})

        self.adapter, self.tensors, self.model, self.calls = adapter, tensors, model, 0
        self.srv = http.server.HTTPServer(("127.0.0.1", 0), H)
        self.base = f"http://127.0.0.1:{self.srv.server_address[1]}"
        self.url = f"{self.base}/v1/chat/completions"
        self.t = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.t.start()

    def close(self):
        self.srv.shutdown()


def fake_dump_prompt_head(directory, nbytes=800):
    """Write an executable that prints an nbytes head; return its path."""
    path = os.path.join(directory, "dump_prompt_head")
    with open(path, "w") as f:
        f.write("#!/bin/sh\n")
        f.write("printf '%s' '" + ("FAKE PRODUCTION HEAD. " * (nbytes // 22 + 1))[:nbytes] + "'\n")
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return path
