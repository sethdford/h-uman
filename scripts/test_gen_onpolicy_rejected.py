#!/usr/bin/env python3
"""Tests for gen_onpolicy_rejected.py — hermetic: synthetic chat.db/memory.db,
a fake `human-daemon` script and a fake model server on 127.0.0.1:0."""
import datetime as dt
import http.server
import json
import os
import plistlib
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_onpolicy_rejected as gen  # noqa: E402
from test_eval_conversation_quality import Fixture, MIN, T0  # noqa: E402

NOW = (T0 + dt.timedelta(days=2)).astimezone()


class FakeServer:
    def __init__(self, reply):
        self.reply, self.requests = reply, []
        outer = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b"ok")

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                outer.requests.append({"body": body,
                                       "priority": self.headers.get("X-HU-Priority")})
                data = json.dumps({"choices": [{"message": {"content": outer.reply}}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(data)

        self.httpd = http.server.HTTPServer(("127.0.0.1", 0), H)
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()


class TestPure(unittest.TestCase):
    def test_strip_thinking(self):
        self.assertEqual(gen.strip_thinking("<think>plan it</think>\nNah"), "Nah")

    def test_service_env_keeps_hu_gates_and_drops_secrets(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "svc.plist")
            with open(p, "wb") as f:
                plistlib.dump({"EnvironmentVariables": {
                    "HU_WARMTH_TONE_VOCAB": "live", "HU_API_TOKEN": "x", "PATH": "/bin"}}, f)
            self.assertEqual(gen.service_env(p), {"HU_WARMTH_TONE_VOCAB": "live"})

    def test_trailing_inbound_joins_the_last_user_turns(self):
        turns = [{"role": "assistant", "content": "hey"},
                 {"role": "user", "content": "Heyo"}, {"role": "user", "content": "you up?"}]
        self.assertEqual(gen.trailing_inbound(turns), "Heyo\nyou up?")

    def test_fidelity_needs_enough_moments_and_a_close_match(self):
        ok, s = gen.fidelity(["Nah"] * 19, ["Nah"] * 19)
        self.assertFalse(ok)
        self.assertIn("need", s["reason"])
        ok, _ = gen.fidelity(["Nah"] * 20, ["Yeah"] * 20)
        self.assertTrue(ok)
        ok, s = gen.fidelity(["Nah"] * 20, ["that sounds really fun, send pics"] * 20)
        self.assertFalse(ok)


class TestMain(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = self.tmp.name
        self.fx = Fixture(d)
        self.out = os.path.join(d, "pairs.jsonl")
        self.persona = os.path.join(d, "seth.json")
        with open(self.persona, "w") as f:
            json.dump({"name": "seth", "contacts": {"+1": {"name": "L"}}}, f)
        self.plist = os.path.join(d, "svc.plist")
        with open(self.plist, "wb") as f:
            plistlib.dump({"EnvironmentVariables": {"HU_WARMTH_TONE_VOCAB": "live",
                                                    "HU_API_TOKEN": "hidden"}}, f)
        self.bin = os.path.join(d, "human-daemon")
        self.write_bin(supports=True)
        self.server = None

    def tearDown(self):
        if self.server:
            self.server.close()
        self.tmp.cleanup()

    def write_bin(self, supports):
        with open(self.bin, "w") as f:
            if supports:
                f.write('#!/bin/bash\n'
                        'if [ "$2" = "--help" ]; then echo "Usage: human reply-prompt" >&2; exit 1; fi\n'
                        'echo "SYSTEM tone=$HU_WARMTH_TONE_VOCAB token=${HU_API_TOKEN:-none}"\n')
            else:
                f.write('#!/bin/bash\necho "Unknown command: $1"\n')
        os.chmod(self.bin, 0o755)

    def set_policy_before_fixture(self):
        old = (T0 - dt.timedelta(days=1)).timestamp()
        for p in (self.bin, self.persona):
            os.utime(p, (old, old))

    def huuman(self, secs, text):
        self.fx.outbound("+1", secs, text, self.fx.max_rowid())
        self.fx.msg("+1", secs, text, True)

    def build(self, n_current=20, current_text="Nah", n_gold=3):
        t = 0
        for _ in range(n_current):
            self.fx.msg("+1", t, "Heyo", False)
            self.huuman(t + 30, current_text)
            t += 20 * MIN
        for i in range(n_gold):
            self.fx.msg("+1", t, "how was your day?", False)
            self.fx.msg("+1", t + 30, f"long one, tell you tonight {i}", True)
            t += 20 * MIN
        self.fx.close()

    def run_main(self, *extra, reply="<think>short</think>Nah", now=NOW, server=True):
        url = "http://127.0.0.1:9"
        if server:
            self.server = FakeServer(reply)
            url = self.server.url
        return gen.main(["--chat-db", self.fx.chat_path, "--memory-db", self.fx.mem_path,
                         "--persona", self.persona, "--human-bin", self.bin,
                         "--plist", self.plist, "--server", url, "--out", self.out, *extra],
                        now=now)

    def test_validated_run_writes_pairs_with_production_env(self):
        self.build()
        self.set_policy_before_fixture()
        self.assertEqual(self.run_main(), 0)
        rows = [json.loads(line) for line in open(self.out)]
        self.assertEqual(len(rows), 3)
        self.assertTrue(all(r["rejected"] == "Nah" for r in rows))
        self.assertTrue(all(r["chosen"].startswith("long one") for r in rows))
        req = self.server.requests[-1]
        self.assertEqual(req["priority"], "batch")
        system = req["body"]["messages"][0]["content"]
        self.assertIn("tone=live", system)       # plist gate reached the prompt
        self.assertIn("token=none", system)      # secret-looking key was dropped
        m = json.load(open(self.out + ".manifest.json"))
        self.assertTrue(m["validated"])
        self.assertEqual(m["service_env_keys"], ["HU_WARMTH_TONE_VOCAB"])
        self.assertNotIn("long one", json.dumps(m))

    def test_refuses_when_too_few_current_policy_replies(self):
        self.build()  # binary/persona mtimes are "now": no h-uman reply is current
        self.assertEqual(self.run_main(), 2)
        self.assertFalse(os.path.exists(self.out))

    def test_skip_validation_is_recorded(self):
        self.build()
        self.assertEqual(self.run_main("--skip-validation"), 0)
        self.assertFalse(json.load(open(self.out + ".manifest.json"))["validated"])

    def test_refuses_when_samples_do_not_match_real_replies(self):
        self.build()
        self.set_policy_before_fixture()
        self.assertEqual(self.run_main(reply="that sounds amazing, tell me everything tonight"), 2)
        self.assertFalse(os.path.exists(self.out))

    def test_refuses_in_retrain_window(self):
        self.build()
        self.assertEqual(self.run_main(now=NOW.replace(hour=3)), 2)
        self.assertFalse(os.path.exists(self.out))

    def test_refuses_when_server_down(self):
        self.build()
        self.assertEqual(self.run_main(server=False), 2)

    def test_refuses_when_binary_lacks_reply_prompt(self):
        self.build()
        self.write_bin(supports=False)
        self.assertEqual(self.run_main(), 2)
        self.assertFalse(os.path.exists(self.out))


if __name__ == "__main__":
    unittest.main()
