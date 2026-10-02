#!/usr/bin/env python3
"""LoCoMo -> replay-harness turns + memory probes (docs/guides/memory-benchmarks.md).

Input: locomo10.json from snap-research/locomo (fetch with
scripts/datasets/fetch_locomo.sh). Output, in --out-dir:

  turns.jsonl   one replay turn per contact run Seth answered, with the real
                next Seth run as seth_reply_bubbles
  probes.jsonl  one turn per QA pair: the whole conversation as history, then
                the contact asks the question in texting style; `probe` holds
                the gold answer, the category and where the evidence sits
  stats.json    counts (also printed to stdout)

LoCoMo's numeric categories are mapped to names using the official scorer's
handling (task_eval/evaluation.py): 1 multi-hop (answer split on commas),
2 temporal, 3 open-domain (answer split on ';', first part), 4 single-hop,
5 adversarial (correct = says it was never mentioned).

These are crowd-sourced/LLM-generated people, not Seth: they measure memory
mechanics, never voice. Never train the persona on them.
"""
import argparse
import json
import re
import sys
from datetime import timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay_common as rc  # noqa: E402

CATEGORY = {1: "multi_hop", 2: "temporal", 3: "open_domain", 4: "single_hop", 5: "adversarial"}
SESSION_FMT = "%I:%M %p on %d %B, %Y"
_EVID = re.compile(r"D:?(\d+):(\d+)")


def _sessions(conv):
    nums = sorted(int(m.group(1)) for k in conv
                  for m in [re.fullmatch(r"session_(\d+)", k)] if m)
    for n in nums:
        when = conv.get(f"session_{n}_date_time")
        yield n, when, conv[f"session_{n}"]


def _text(turn, photo_captions):
    text = (turn.get("text") or "").strip()
    if turn.get("img_url") or turn.get("blip_caption"):
        cap = (turn.get("blip_caption") or "").strip()
        tag = f"[Photo: {cap}]" if photo_captions and cap else "[Photo]"
        text = f"{text} {tag}" if text else tag
    return text


def _flatten(sample, seth_speaker, photo_captions):
    conv = sample["conversation"]
    seth = conv["speaker_b"] if seth_speaker == "b" else conv["speaker_a"]
    msgs, index = [], {}
    prev_end = None
    for n, when, turns in _sessions(conv):
        start = rc.parse_dt(when.strip(), SESSION_FMT)
        if prev_end is not None and start <= prev_end:
            start = prev_end + timedelta(seconds=60)
        dt = start
        for t in turns:
            text = _text(t, photo_captions)
            if not text:
                continue
            if msgs:
                dt = max(dt, msgs[-1]["dt"] + timedelta(seconds=1))
            m = _EVID.fullmatch(t.get("dia_id", ""))
            if m:
                index[(int(m.group(1)), int(m.group(2)))] = len(msgs)
            msgs.append({"from_me": t["speaker"] == seth, "text": text, "dt": dt, "session": n})
            dt = dt + timedelta(seconds=rc.typing_gap_seconds(text))
        prev_end = msgs[-1]["dt"] if msgs else prev_end
    return msgs, index


def _gold(qa, cat):
    if cat == 5:
        return []
    ans = str(qa.get("answer", "")).strip()
    if cat == 3:
        ans = ans.split(";")[0].strip()
    return [ans] if ans else []


def convert(samples, seth_speaker="b", window=rc.DAEMON_HISTORY_WINDOW, probe_gap_hours=24,
            photo_captions=False, history_limit=0, per_category=0):
    turns, probes, images = [], [], 0
    for sample in samples:
        sid = str(sample.get("sample_id", f"conv-{len(turns)}"))
        conv = sample["conversation"]
        contact_name = conv["speaker_a"] if seth_speaker == "b" else conv["speaker_b"]
        seth_name = conv["speaker_b"] if seth_speaker == "b" else conv["speaker_a"]
        contact_id = rc.bench_contact_id("locomo", sid)
        msgs, index = _flatten(sample, seth_speaker, photo_captions)
        images += sum(1 for m in msgs if "[Photo" in m["text"])
        turns += rc.build_turns(msgs, contact_id, f"locomo:{sid}", history_limit)

        full = [rc.history_entry(m["from_me"], m["text"], m["dt"]) for m in msgs]
        hist = full[-history_limit:] if history_limit else full
        offset = len(full) - len(hist)
        ask_at = rc.fmt_ts(msgs[-1]["dt"] + timedelta(hours=probe_gap_hours))
        for qi, qa in enumerate(sample.get("qa", [])):
            cat = qa.get("category")
            if cat not in CATEGORY or not qa.get("question"):
                continue
            ev = qa.get("evidence") or []
            full_idx = sorted({index[(int(a), int(b))] for e in ev for a, b in _EVID.findall(e)
                               if (int(a), int(b)) in index})
            idxs = [x - offset for x in full_idx]
            in_hist, in_win = rc.window_flags(idxs, len(hist), window)
            pid = f"locomo:{sid}:probe:{qi:04d}"
            probes.append({
                "id": pid,
                "contact_id": contact_id,
                "inbound_bubbles": [rc.casual_question(qa["question"], contact_name, seth_name, pid)],
                "ts": ask_at,
                "history": hist,
                "probe": {
                    "dataset": "locomo",
                    "sample_id": sid,
                    "category": CATEGORY[cat],
                    "category_raw": cat,
                    "question_original": qa["question"],
                    "gold_answers": _gold(qa, cat),
                    "answer_full": None if cat == 5 else str(qa.get("answer", "")),
                    "adversarial_answer": qa.get("adversarial_answer") if cat == 5 else None,
                    "evidence": ev,
                    "evidence_history_idx": [x for x in idxs if x >= 0],
                    "evidence_in_history": in_hist,
                    "evidence_in_window": in_win,
                    "window": window,
                },
            })
    before = len(probes)
    if per_category:
        probes = rc.sample_per_category(probes, per_category)
    stats = rc.common_stats(turns, probes, len(samples))
    stats.update({"dataset": "locomo", "photo_messages": images, "window": window,
                  "probes_before_sampling": before})
    return {"turns": turns, "probes": probes, "stats": stats}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--input", required=True, help="locomo10.json")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--seth-speaker", choices=("a", "b"), default="b",
                    help="which LoCoMo speaker plays Seth (default b; a is the contact)")
    ap.add_argument("--window", type=int, default=rc.DAEMON_HISTORY_WINDOW,
                    help="history entries the reply path sees (daemon: 25)")
    ap.add_argument("--probe-gap-hours", type=float, default=24.0)
    ap.add_argument("--photo-captions", action="store_true",
                    help="render images as [Photo: caption] instead of [Photo]")
    ap.add_argument("--history-limit", type=int, default=0, help="0 = full history")
    ap.add_argument("--per-category", type=int, default=0,
                    help="keep at most N probes per category (deterministic); 0 = all")
    a = ap.parse_args(argv)
    samples = json.loads(Path(a.input).read_text(encoding="utf-8"))
    res = convert(samples, a.seth_speaker, a.window, a.probe_gap_hours, a.photo_captions,
                  a.history_limit, a.per_category)
    if not res["probes"]:
        print("locomo_to_replay: 0 probes produced; refusing to write", file=sys.stderr)
        return 2
    print(rc.write_outputs(a.out_dir, res))
    return 0


if __name__ == "__main__":
    sys.exit(main())
