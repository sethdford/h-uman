#!/usr/bin/env python3
"""HU_SPONTANEITY promotion check: does the daemon fire extras as often as Seth does?

Measures, under the SAME eligibility rules the daemon applies
(src/context/conversation.c, parsed from source so they cannot drift):

  Seth:   per reply turn in his own DMs (chat.db, read-only), how often he
          double-texted an afterthought, tapped back his own message, or sent a
          GIF -- counted only when the turn was eligible for that extra.
  Daemon: per reactive turn, how often an extra fired (the one
          "[HU_SPONTANEITY live|shadow] turn=1 ... chosen=<kind>" line per turn).

Then a two-proportion z-test per kind. Exit 0 = PASS (every kind consistent at
95%), 1 = FAIL (some kind differs), 2 = INCONCLUSIVE (too few turns for the
normal approximation: n*p < 5). Prints aggregate counts only -- never message
text or handles.

`--emit-learned` prints the per-kind <kind>_rate / <kind>_n fields (Seth's
P(extra | eligible) and the eligible count) the daemon reads from the `global`
block of <persona>.learned-style.json, plus double_text_gap_s. It writes
nothing; the learned-style learner owns that file.

`--self-test` runs against a synthetic in-memory chat.db and log.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import sqlite3
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CONV_C = REPO / "src" / "context" / "conversation.c"
APPLE_EPOCH = 978307200
KINDS = ("double_text", "self_reaction", "gif")

# ── eligibility rules, read from the C source ────────────────────────────────

_LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')


def _literals(text: str) -> list[str]:
    return [m.group(1).replace('\\"', '"').replace("\\\\", "\\") for m in _LIT.finditer(text)]


def _body(src: str, signature: str) -> str:
    i = src.index(signature)
    j = src.index("\n}\n", i)
    return src[i:j]


def load_rules(conv_c: Path = CONV_C) -> dict:
    src = conv_c.read_text(encoding="utf-8")
    fw = src[src.index("DEFAULT_FAREWELL_PHRASES"):]
    farewell = _literals(fw[: fw.index("};")])
    selfr = _body(src, "hu_reaction_type_t hu_conversation_self_reaction_kind(")
    gif = _body(src, "bool hu_conversation_should_send_gif(")
    split = gif.index("bool gif_worthy")
    end = gif.index("if (!gif_worthy)")
    return {
        "farewell": [p for p in farewell if p],
        "self_reaction": [p for p in _literals(selfr) if p],
        "gif_exclude": [p for p in _literals(gif[:split]) if p],
        "gif_worthy": [p for p in _literals(gif[split:end]) if p],
    }


def _has(text: str, needles: list[str]) -> bool:
    t = text.lower()
    return any(n.lower() in t for n in needles)


def dt_eligible(rules, reply: str, hour: int, last4_from_me: int) -> bool:
    if not reply or hour >= 23 or hour < 5 or _has(reply, rules["farewell"]):
        return False
    return last4_from_me < 3


def sr_eligible(rules, reply: str) -> bool:
    return bool(reply) and _has(reply, rules["self_reaction"])


def gif_eligible(rules, inbound: str) -> bool:
    return bool(inbound) and not _has(inbound, rules["gif_exclude"]) and _has(
        inbound, rules["gif_worthy"])


# ── chat.db (read-only) ──────────────────────────────────────────────────────


def decode_body(text, blob) -> str:
    """chat.db keeps ~98% of bodies only in attributedBody (typedstream)."""
    if text:
        return text
    if not blob:
        return ""
    i = blob.find(b"NSString")
    if i < 0:
        return ""
    s = blob[i + 8 + 5:]
    if not s:
        return ""
    if s[0] == 0x81:
        n, off = int.from_bytes(s[1:3], "little"), 3
    else:
        n, off = s[0], 1
    return s[off:off + n].decode("utf-8", "replace")


def seth_turns(db: sqlite3.Connection, since_unix: int, rules, afterthought_gap_s: float):
    """Yield one dict per Seth reply turn in a 1:1 chat."""
    rows = db.execute(
        """
        SELECT cmj.chat_id, m.ROWID, m.guid, m.text, m.attributedBody, m.is_from_me,
               m.date / 1000000000 + ?2, m.associated_message_type,
               m.associated_message_guid,
               EXISTS (SELECT 1 FROM message_attachment_join maj
                       JOIN attachment a ON a.ROWID = maj.attachment_id
                       WHERE maj.message_id = m.ROWID AND a.mime_type = 'image/gif')
        FROM message m
        JOIN chat_message_join cmj ON cmj.message_id = m.ROWID
        JOIN chat c ON c.ROWID = cmj.chat_id
        WHERE c.style = 45 AND m.date > (?1 - ?2) * 1000000000
        ORDER BY cmj.chat_id, m.date
        """, (since_unix, APPLE_EPOCH)).fetchall()
    chats: dict[int, list] = {}
    for r in rows:
        chats.setdefault(r[0], []).append(r)
    for msgs in chats.values():
        plain = [m for m in msgs if m[7] == 0]
        own_taps = {}
        for m in msgs:
            if m[5] == 1 and 2000 <= (m[7] or 0) <= 2006 and m[8]:
                tgt = m[8].split("/", 1)[-1] if "/" in m[8] else m[8].removeprefix("bp:")
                own_taps[tgt] = True
        i = 0
        while i < len(plain):
            if plain[i][5] == 1 or i + 1 >= len(plain) or plain[i + 1][5] != 1:
                i += 1
                continue
            inbound = decode_body(plain[i][3], plain[i][4])
            j = i + 1
            run = []
            while j < len(plain) and plain[j][5] == 1:
                run.append(plain[j])
                j += 1
            reply = run[0]
            history = plain[max(0, i - 3):i + 1]
            last4_from_me = sum(1 for h in history if h[5] == 1)
            hour = time.localtime(reply[6]).tm_hour
            reply_text = decode_body(reply[3], reply[4])
            gaps = [b[6] - a[6] for a, b in zip(run, run[1:])]
            dt_gap = next((g for g in gaps if g >= afterthought_gap_s), None)
            yield {
                "dt_elig": dt_eligible(rules, reply_text, hour, last4_from_me),
                "dt_fired": dt_gap is not None,
                "dt_gap": dt_gap,
                "sr_elig": sr_eligible(rules, reply_text),
                "sr_fired": any(own_taps.get(m[2]) for m in run),
                "gif_elig": gif_eligible(rules, inbound),
                "gif_fired": any(m[9] for m in run),
            }
            i = j


def seth_rates(turns) -> dict:
    out = {k: {"turns": 0, "eligible": 0, "fired_eligible": 0} for k in KINDS}
    gaps = []
    for t in turns:
        for k, p in zip(KINDS, ("dt", "sr", "gif")):
            out[k]["turns"] += 1
            if t[p + "_elig"]:
                out[k]["eligible"] += 1
                if t[p + "_fired"]:
                    out[k]["fired_eligible"] += 1
                    if k == "double_text" and t["dt_gap"] is not None:
                        gaps.append(t["dt_gap"])
    gaps.sort()
    out["double_text_gap_s"] = gaps[len(gaps) // 2] if gaps else None
    return out


# ── daemon log ───────────────────────────────────────────────────────────────

_TURN = re.compile(r"\[HU_SPONTANEITY (live|shadow)\] turn=1 .*chosen=(\w+)")


def daemon_rates(lines, mode: str) -> dict:
    out = {"turns": 0, **{k: 0 for k in KINDS}}
    for line in lines:
        m = _TURN.search(line)
        if not m or m.group(1) != mode:
            continue
        out["turns"] += 1
        if m.group(2) in KINDS:
            out[m.group(2)] += 1
    return out


# ── verdict ──────────────────────────────────────────────────────────────────


def compare(seth: dict, daemon: dict) -> tuple[int, dict]:
    verdicts, worst = {}, 0
    for k in KINDS:
        n1, x1 = seth[k]["turns"], seth[k]["fired_eligible"]
        n2, x2 = daemon["turns"], daemon[k]
        pooled = (x1 + x2) / (n1 + n2) if n1 + n2 else 0.0
        if n1 == 0 or n2 == 0 or min(n1, n2) * pooled < 5 or min(n1, n2) * (1 - pooled) < 5:
            verdicts[k] = {"verdict": "INCONCLUSIVE", "seth": [x1, n1], "daemon": [x2, n2]}
            worst = max(worst, 2)
            continue
        se = math.sqrt(pooled * (1 - pooled) * (1 / n1 + 1 / n2))
        z = (x2 / n2 - x1 / n1) / se if se else 0.0
        ok = abs(z) <= 1.96
        verdicts[k] = {"verdict": "PASS" if ok else "FAIL", "z": round(z, 2),
                       "seth_rate": round(x1 / n1, 4), "daemon_rate": round(x2 / n2, 4),
                       "seth": [x1, n1], "daemon": [x2, n2]}
        if not ok:
            worst = 1 if worst != 2 else worst
    return worst, verdicts


# ── self-test ────────────────────────────────────────────────────────────────


def _self_test() -> int:
    rules = load_rules()
    assert "?" in rules["gif_exclude"] and "lol" in rules["gif_worthy"], rules
    assert rules["farewell"] and "lol" in rules["self_reaction"], rules
    db = sqlite3.connect(":memory:")
    db.executescript("""
        CREATE TABLE chat (ROWID INTEGER PRIMARY KEY, guid TEXT, style INTEGER);
        CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER);
        CREATE TABLE attachment (ROWID INTEGER PRIMARY KEY, mime_type TEXT);
        CREATE TABLE message_attachment_join (message_id INTEGER, attachment_id INTEGER);
        CREATE TABLE message (ROWID INTEGER PRIMARY KEY, guid TEXT, text TEXT,
          attributedBody BLOB, is_from_me INTEGER, date INTEGER,
          associated_message_type INTEGER DEFAULT 0, associated_message_guid TEXT);
        INSERT INTO chat VALUES (1, 'any;-;+15550009999', 45);
        INSERT INTO attachment VALUES (1, 'image/gif');
    """)
    base = int(time.mktime(time.strptime("2026-09-01 12:00", "%Y-%m-%d %H:%M")))

    def msg(rowid, guid, text, me, t, amt=0, aguid=None):
        db.execute("INSERT INTO message VALUES (?,?,?,?,?,?,?,?)",
                   (rowid, guid, text, None, me, (t - APPLE_EPOCH) * 1000000000, amt, aguid))
        db.execute("INSERT INTO chat_message_join VALUES (1, ?)", (rowid,))

    # turn 1: eligible double-text, afterthought 120 s later
    msg(1, "a", "how was it", 0, base)
    msg(2, "b", "pretty good honestly", 1, base + 30)
    msg(3, "c", "oh and the food was great", 1, base + 150)
    # turn 2: lol reply he taps himself; gif-worthy inbound, he sends a gif
    msg(4, "d", "lol that party was wild", 0, base + 600)
    msg(5, "e", "lol right", 1, base + 630)
    msg(6, "f", None, 1, base + 640)
    db.execute("INSERT INTO message_attachment_join VALUES (6, 1)")
    msg(7, "g", None, 1, base + 645, 2003, "p:0/e")
    # turn 3: question inbound (not gif-eligible), farewell reply (not dt-eligible)
    msg(8, "h", "you coming?", 0, base + 900)
    msg(9, "i", "ok bye", 1, base + 930)
    s = seth_rates(seth_turns(db, base - 10, rules, 60.0))
    assert s["double_text"] == {"turns": 3, "eligible": 2, "fired_eligible": 1}, s
    assert s["self_reaction"] == {"turns": 3, "eligible": 1, "fired_eligible": 1}, s
    assert s["gif"] == {"turns": 3, "eligible": 1, "fired_eligible": 1}, s
    assert s["double_text_gap_s"] == 120, s
    lines = ["x [HU_SPONTANEITY live] turn=1 dt=1/0.5/1 sr=0/-1/0 gif=0/-1/0 chosen=double_text "
             "fired=1", "x [HU_SPONTANEITY live] turn=1 dt=0/-1/0 chosen=none fired=0",
             "x [HU_SPONTANEITY shadow] turn=1 chosen=gif fired=0"]
    d = daemon_rates(lines, "live")
    assert d == {"turns": 2, "double_text": 1, "self_reaction": 0, "gif": 0}, d
    code, _ = compare(s, d)
    assert code == 2  # far too few turns: refuses to call it
    big_s = {k: {"turns": 1000, "eligible": 500, "fired_eligible": 100} for k in KINDS}
    same = {"turns": 1000, **{k: 105 for k in KINDS}}
    off = {"turns": 1000, **{k: 300 for k in KINDS}}
    assert compare(big_s, same)[0] == 0
    assert compare(big_s, off)[0] == 1  # a 3x over-firing daemon FAILS
    print("self-test OK")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--chatdb", default=os.environ.get(
        "HU_CHATDB", str(Path.home() / "Library/Messages/chat.db")))
    ap.add_argument("--log", default=str(Path(os.environ.get(
        "HU_STATE_DIR", Path.home() / ".human")) / "logs/service-loop-error.log"))
    ap.add_argument("--days", type=int, default=180)
    ap.add_argument("--mode", choices=("live", "shadow"), default="live")
    ap.add_argument("--afterthought-gap-s", type=float, default=60.0,
                    help="an own message this long after the previous one in the same reply "
                         "run counts as an afterthought (the daemon's FU-1 window)")
    ap.add_argument("--emit-learned", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return _self_test()
    rules = load_rules()
    db = sqlite3.connect(f"file:{a.chatdb}?mode=ro", uri=True)
    seth = seth_rates(seth_turns(db, int(time.time()) - a.days * 86400, rules,
                                 a.afterthought_gap_s))
    if a.emit_learned:
        learned = {}
        for k in KINDS:
            e = seth[k]["eligible"]
            if e:
                learned[f"{k}_rate"] = round(seth[k]["fired_eligible"] / e, 4)
                learned[f"{k}_n"] = e
        if seth["double_text_gap_s"] is not None:
            learned["double_text_gap_s"] = seth["double_text_gap_s"]
        print(json.dumps({"global": learned}, indent=2))
        return 0
    try:
        lines = Path(a.log).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as e:
        print(f"INCONCLUSIVE: cannot read daemon log ({e.strerror})")
        return 2
    code, verdicts = compare(seth, daemon_rates(lines, a.mode))
    print(json.dumps({"mode": a.mode, "verdicts": verdicts,
                      "result": ["PASS", "FAIL", "INCONCLUSIVE"][code]}, indent=2))
    return code


if __name__ == "__main__":
    sys.exit(main())
