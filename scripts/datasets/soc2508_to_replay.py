#!/usr/bin/env python3
"""SOC-2508 (Synthetic Online Conversations) -> replay-harness turns.

Input: data.jsonl from the Hugging Face dataset marcodsn/SOC-2508 (fetch with
scripts/datasets/fetch_soc2508.sh). Each chat is a list of `chat_parts`; a
part is one sender's burst of one or more messages, with inline tags:
<delay minutes=".." hours=".." days=".."/> (the gap before the burst),
<image>..</image>, <gif>, <audio>, <video>, <file>, <link>, <end/>.

Used ONLY as a prior for multi-message turns and reply delays: each turn
keeps the real next burst as seth_reply_bubbles and, when the source tagged
it, the reply delay as seth_reply_delay_seconds. No memory probes — the chats
are short single sessions. LLM-generated people (Qwen3-235B): never train the
persona on them.
"""
import argparse
import json
import re
import sys
from collections import Counter
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay_common as rc  # noqa: E402

_DELAY = re.compile(r"<delay\b([^>]*)/?>", re.IGNORECASE)
_DELAY_ATTR = re.compile(r"(days|hours|minutes|seconds)\s*=\s*\"(\d+(?:\.\d+)?)\"", re.IGNORECASE)
_DELAY_UNIT = {"days": 86400, "hours": 3600, "minutes": 60, "seconds": 1}
_MEDIA = (("image", "[Photo]"), ("img", "[Photo]"), ("gif", "[GIF]"),
          ("audio", "[Voice Message]"), ("video", "[Video]"), ("file", "[File]"))
_STRIP_TAGS = re.compile(r"</?(?:pause|break|sup|i|b|em|strong)\s*/?>", re.IGNORECASE)
_END = re.compile(r"<end\s*/?>", re.IGNORECASE)


def _delay_seconds(attrs):
    return int(sum(float(v) * _DELAY_UNIT[k.lower()] for k, v in _DELAY_ATTR.findall(attrs)))


def render(message, photo_captions=False):
    """(text, delay_seconds_or_None) for one raw SOC message."""
    delay = None
    for m in _DELAY.finditer(message):
        delay = (delay or 0) + _delay_seconds(m.group(1))
    s = _DELAY.sub(" ", message)
    s = _END.sub(" ", s)
    for tag, label in _MEDIA:
        def rep(m, label=label):
            cap = m.group(1).strip()
            if photo_captions and label == "[Photo]" and cap:
                return f" [Photo: {cap}] "
            return f" {label} "
        s = re.sub(rf"<{tag}>(.*?)(?:</{tag}>|$)", rep, s, flags=re.IGNORECASE | re.DOTALL)
    s = re.sub(r"<link>(.*?)(?:</link>|$)", r" \1 ", s, flags=re.IGNORECASE | re.DOTALL)
    s = _STRIP_TAGS.sub(" ", s)
    return " ".join(s.split()), delay


def _start(chat):
    tail = str(chat.get("chat_id", "")).rsplit("_", 1)[-1]
    epoch = int(tail) if tail.isdigit() else 1754000000
    return datetime.fromtimestamp(epoch, timezone.utc).replace(tzinfo=None)


def convert(chats, seth_persona=2, photo_captions=False, history_limit=0):
    turns = []
    per_part, delays = Counter(), []
    for n, chat in enumerate(chats):
        exp = chat.get("experience") or {}
        seth_id = (exp.get(f"persona{seth_persona}") or {}).get("id")
        cid = str(chat.get("chat_id") or f"chat{n}")
        msgs, dt = [], _start(chat)
        for part in chat.get("chat_parts") or []:
            from_me = part.get("sender") == seth_id
            rendered = [render(m, photo_captions) for m in part.get("messages") or []]
            part_delay = None
            bubbles = []
            for text, d in rendered:
                if d is not None:
                    part_delay = (part_delay or 0) + d
                if text:
                    bubbles.append(text)
            if not bubbles:
                continue
            per_part[str(len(bubbles))] += 1
            if part_delay is not None:
                delays.append(part_delay)
            if msgs:
                gap = part_delay if part_delay is not None else rc.typing_gap_seconds(bubbles[0])
                dt = msgs[-1]["dt"] + timedelta(seconds=max(1, gap))
            for bi, text in enumerate(bubbles):
                if bi:
                    dt = dt + timedelta(seconds=max(2, min(30, len(text) // 6)))
                msgs.append({"from_me": from_me, "text": text, "dt": dt,
                             "delay_s": part_delay if bi == 0 else None})

        def extra(i, j, k, msgs=msgs):
            return {"seth_reply_delay_seconds": msgs[j]["delay_s"]}

        turns += rc.build_turns(msgs, rc.bench_contact_id("soc2508", cid[:40]), f"soc2508:{cid}",
                                history_limit, extra)
    stats = rc.common_stats(turns, [], len(chats))
    delays.sort()
    stats.update({
        "dataset": "soc2508",
        "bubbles_per_part": dict(sorted(per_part.items(), key=lambda kv: int(kv[0]))),
        "delayed_parts": len(delays),
        "delay_seconds_p50": delays[len(delays) // 2] if delays else None,
        "delay_seconds_p90": delays[int(len(delays) * 0.9)] if delays else None,
    })
    return {"turns": turns, "probes": [], "stats": stats}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--input", required=True, help="data.jsonl")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--seth-persona", type=int, choices=(1, 2), default=2)
    ap.add_argument("--photo-captions", action="store_true")
    ap.add_argument("--history-limit", type=int, default=0)
    a = ap.parse_args(argv)
    chats = [json.loads(line) for line in Path(a.input).read_text(encoding="utf-8").splitlines()
             if line.strip()]
    res = convert(chats, a.seth_persona, a.photo_captions, a.history_limit)
    if not res["turns"]:
        print("soc2508_to_replay: 0 turns produced; refusing to write", file=sys.stderr)
        return 2
    print(rc.write_outputs(a.out_dir, res))
    return 0


if __name__ == "__main__":
    sys.exit(main())
