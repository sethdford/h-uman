#!/usr/bin/env python3
"""Dry-run fixtures for the cut-over kit: everything fake, nothing real.

  build DIR --port N   a fixture chat.db (synthetic 1:1 threads dated in the
                       last 30 days), a fixture state dir (config.json on the
                       fake server, empty memory.db), a fixture service plist
                       and a LoCoMo-shaped probes.jsonl. No real message, no
                       real contact (handles are 555 numbers / .invalid).
  serve --port-file F  one loopback OpenAI-compatible server for the whole
                       pipeline: director decisions, replies (deterministic in
                       the request, so different gate env -> different reply)
                       and judge verdicts. Writes its port to F, serves until
                       killed.

Used by `run_cutover.sh --dry-run` and tests/test_cutover_dryrun.py.
"""
import argparse
import hashlib
import http.server
import json
import os
import plistlib
import sqlite3
import sys
import time

APPLE_EPOCH = 978307200

INBOUND = ["you around tonight?", "did you see the game", "lol that's wild",
           "can you grab milk on the way", "how was the trip?", "ok sounds good",
           "what time works for you?", "miss you guys", "running late sorry",
           "did mom call you?", "that photo is amazing", "want to get lunch friday?"]
SETH = [["yeah", "what time"], ["ha yes"], ["lol"], ["yep"], ["so good, tell you later"],
        ["👍"], ["7 works"], ["miss you too"], ["no worries"], ["not yet"], ["thanks!"],
        ["friday works"]]
REPLIES = ["yeah for sure", "haha no way", "ok cool, see you then", "lol yes",
           "not sure, we'll see", "sounds good to me and", "7pm works", "miss you too!!",
           "no worries at all", "yep just talked to her", "right?? so good", "friday's good"]


def _ns(epoch):
    return (int(epoch) - APPLE_EPOCH) * 1_000_000_000


def build_chat_db(path, now=None, contacts=8, turns_per_contact=4):
    """1:1 threads, newest within the last 30 days. Each turn: 1-2 inbound
    bubbles, then Seth's text reply (or, every 5th turn, a tapback)."""
    now = int(now or time.time())
    con = sqlite3.connect(path)
    con.executescript("""
        CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT);
        CREATE TABLE chat (ROWID INTEGER PRIMARY KEY);
        CREATE TABLE chat_handle_join (chat_id INTEGER, handle_id INTEGER);
        CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER);
        CREATE TABLE message (ROWID INTEGER PRIMARY KEY, guid TEXT, text TEXT,
            attributedBody BLOB, handle_id INTEGER, date INTEGER, is_from_me INTEGER,
            item_type INTEGER DEFAULT 0, associated_message_type INTEGER DEFAULT 0,
            associated_message_guid TEXT);
    """)
    rowid = 0
    k = 0
    for c in range(1, contacts + 1):
        con.execute("INSERT INTO handle VALUES (?,?)", (c, f"+1555000{c:04d}"))
        con.execute("INSERT INTO chat VALUES (?)", (c,))
        con.execute("INSERT INTO chat_handle_join VALUES (?,?)", (c, c))
        t = now - 20 * 86400 + c * 3600
        for _ in range(turns_per_contact):
            i = k % len(INBOUND)
            msgs = [(INBOUND[i], 0, 0, None)]
            if k % 3 == 0:
                msgs.insert(0, ("hey", 0, 0, None))
            if k % 5 == 4:
                msgs.append((None, 1, 2001, None))  # Seth's tapback on the last inbound
            else:
                msgs += [(b, 1, 0, None) for b in SETH[i]]
            last_inbound_guid = None
            for text, me, amt, _ in msgs:
                rowid += 1
                t += 40
                guid = f"g{rowid}"
                aguid = f"p:0/{last_inbound_guid}" if amt else None
                con.execute("INSERT INTO message VALUES (?,?,?,?,?,?,?,0,?,?)",
                            (rowid, guid, text, None, c, _ns(t), me, amt, aguid))
                con.execute("INSERT INTO chat_message_join VALUES (?,?)", (c, rowid))
                if not me:
                    last_inbound_guid = guid
            t += 6 * 3600
            k += 1
    con.commit()
    con.close()


def build_state(state_dir, port):
    os.makedirs(os.path.join(state_dir, "personas"), exist_ok=True)
    with open(os.path.join(state_dir, "config.json"), "w") as f:
        json.dump({"default_provider": "mlx_local", "default_model": "fake-model",
                   "providers": [{"name": "mlx_local", "api_key": "local-test-key",
                                  "base_url": f"http://127.0.0.1:{port}/v1"}],
                   "memory": {"backend": "sqlite"},
                   "channels": {"imessage": {"daemon": {"llm_decides": True}}}}, f)
    con = sqlite3.connect(os.path.join(state_dir, "memory.db"))
    con.execute("CREATE TABLE outbound_sends (contact TEXT, sent_at_ms INTEGER, text TEXT)")
    con.commit()
    con.close()


def build_plist(path):
    with open(path, "wb") as f:
        plistlib.dump({"Label": "ai.human.service-loop.fixture",
                       "EnvironmentVariables": {"HU_TERSENESS": "live",
                                                "HU_LENGTH_POLICY": "shadow"}}, f)


def build_probes(path, n=10):
    """LoCoMo-replay-shaped probes about a fictional .invalid contact."""
    cats = ["single_hop", "multi_hop", "temporal", "open_domain", "adversarial"]
    golds = ["Biscuit", "Lisbon, pottery", "7 May 2023", "Yes", "Biscuit"]
    with open(path, "w") as f:
        for i in range(n):
            c = i % len(cats)
            f.write(json.dumps({
                "id": f"locomo:fx:probe:{i:04d}", "contact_id": "locomo.fx@bench.invalid",
                "ts": f"2023-05-{21 + (i % 5):02d} 14:12:00",
                "inbound_bubbles": ["wait what's your dog's name again?"],
                "history": [{"from_me": True, "text": "my dog Biscuit says hi",
                             "ts": "2023-05-08 13:56:00"},
                            {"from_me": False, "text": "ha cute", "ts": "2023-05-08 13:57:00"}],
                "seth_action": "text", "seth_reply_bubbles": ["biscuit"],
                "probe": {"dataset": "locomo", "category": cats[c], "gold_answers": [golds[c]],
                          "question_original": "What is the name of Rory's dog?",
                          "adversarial_answer": "Biscuit" if cats[c] == "adversarial" else None,
                          "evidence_in_window": True, "window": 25}}) + "\n")


def build(d, port):
    os.makedirs(d, exist_ok=True)
    build_chat_db(os.path.join(d, "chat.db"))
    build_state(os.path.join(d, "state_src"), port)
    build_plist(os.path.join(d, "ai.human.service-loop.plist"))
    build_probes(os.path.join(d, "probes.jsonl"))


def _pick(seq, *parts):
    h = hashlib.sha256("|".join(parts).encode()).digest()
    return seq[h[0] % len(seq)], h


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        req = json.loads(self.rfile.read(n) or b"{}")
        msgs = req.get("messages") or [{}]
        last = msgs[-1].get("content") or ""
        if isinstance(last, list):
            last = " ".join(p.get("text", "") for p in last if isinstance(p, dict))
        whole = json.dumps(msgs, sort_keys=True)
        if "Exactly one reply was written by the REAL person" in last:
            real, _ = _pick(["A", "B", "A"], whole)
            content = json.dumps({"real": real, "opinion": 3, "memory": 3, "reasoning": 3,
                                  "lexical": 3, "tone": 3, "syntax": 3})
        elif "New message from them" in last or req.get("cutover_role") == "director":
            action, _ = _pick(["text", "text", "text", "text", "tapback"], whole)
            content = json.dumps({"action": action, "delay_s": 1, "reaction": "like",
                                  "burst": False, "direction": "keep it short"})
        else:
            content, _ = _pick(REPLIES, whole)
        body = json.dumps({"id": "x", "object": "chat.completion", "model": req.get("model"),
                           "choices": [{"index": 0, "finish_reason": "stop",
                                        "message": {"role": "assistant", "content": content}}],
                           "usage": {"prompt_tokens": 1, "completion_tokens": 1,
                                     "total_tokens": 2}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # /api/tags and /v1/models probes
        body = json.dumps({"models": [{"name": "fake-judge"}], "data": [{"id": "fake-model"}]})
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(body.encode())


def serve(port_file, port=0):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    tmp = port_file + ".tmp"
    with open(tmp, "w") as f:
        f.write(str(srv.server_address[1]))
    os.replace(tmp, port_file)
    srv.serve_forever()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("dir")
    b.add_argument("--port", type=int, required=True)
    s = sub.add_parser("serve")
    s.add_argument("--port-file", required=True)
    a = ap.parse_args(argv)
    if a.cmd == "build":
        build(a.dir, a.port)
    else:
        serve(a.port_file)
    return 0


if __name__ == "__main__":
    sys.exit(main())
