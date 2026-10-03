#!/usr/bin/env python3
"""Hermetic tests for reply_latency_report.py: synthetic service-log lines only."""
import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import reply_latency_report as rlr  # noqa: E402

H = "+15550000001"
LOG = f"""2026-10-02T10:00:00 INFO  [imessage] incoming handle={H} len=12
2026-10-02T10:00:01 INFO  [human] director delay: 8000 ms (read after 1500ms)
2026-10-02T10:00:02 INFO  [human] calling agent turn for {H}...
2026-10-02T10:00:03 INFO  [http] POST http://127.0.0.1:8741/v1/embeddings (body_len=40)
2026-10-02T10:00:05 INFO  [http] POST http://127.0.0.1:8741/v1/chat/completions (body_len=900)
2026-10-02T10:00:20 INFO  [http] POST http://127.0.0.1:8741/v1/chat/completions (body_len=9000)
2026-10-02T10:00:30 INFO  [human] agent turn result: err=success response_len=31 for {H}
2026-10-02T10:00:31 INFO  [http] POST https://aiplatform.googleapis.com/v1/x:generateContent (body_len=10)
2026-10-02T10:00:33 INFO  [human] imessage_dispatch: flat send
2026-10-02T10:00:40 INFO  [post_send_defer] [HU_POST_SEND_DEFER shadow] jobs=3 extract=1 embed=2 ran=0 inline_full=0 facts=2 facts_literal=1 flush_ms=0
2026-10-02T10:01:00 INFO  [human] processing batch: hello there
"""


class ReportTest(unittest.TestCase):
    def setUp(self):
        self.ev = rlr.parse(io.StringIO(LOG))

    def test_processing_is_total_minus_director_delay(self):
        rep = rlr.summarize(self.ev)
        self.assertEqual(rep["sent"], 1)
        self.assertEqual(rep["total_s"]["p50"], 33.0)
        self.assertEqual(rep["delay_s"]["p50"], 8.0)
        self.assertEqual(rep["processing_s"]["p50"], 25.0)
        self.assertEqual(rep["turn_s"]["p50"], 28.0)
        self.assertEqual(rep["tail_s"]["p50"], 3.0)

    def test_counts_calls_per_kind_between_inbound_and_send(self):
        calls = rlr.summarize(self.ev)["calls_per_turn"]
        self.assertEqual(calls["local_chat"]["p50"], 2)
        self.assertEqual(calls["local_emb"]["p50"], 1)
        self.assertEqual(calls["cloud"]["p50"], 1)

    def test_sums_post_send_defer_lines(self):
        psd = rlr.summarize(self.ev)["post_send_defer"]
        self.assertEqual((psd["flush_lines"], psd["extract"], psd["embed"]), (1, 1, 2))
        self.assertEqual((psd["facts"], psd["facts_literal"]), (2, 1))

    def test_output_carries_no_handle_or_text(self):
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write(LOG)
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(rlr.main(["--log", f.name]), 0)
            with redirect_stdout(io.StringIO()):
                self.assertEqual(rlr.main(["--log", f.name, "--json"]), 0)
        finally:
            os.unlink(f.name)
        out = buf.getvalue()
        self.assertIn("processing_s", out)
        self.assertNotIn(H, out)
        self.assertNotIn("hello", out)

    def test_no_sent_turn_is_not_a_measurement(self):
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write(LOG.replace("imessage_dispatch: flat send", "pre-send abort: real user"))
        try:
            with redirect_stdout(io.StringIO()):
                self.assertEqual(rlr.main(["--log", f.name]), 1)
        finally:
            os.unlink(f.name)


if __name__ == "__main__":
    unittest.main()
