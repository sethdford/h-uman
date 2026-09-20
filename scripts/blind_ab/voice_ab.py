#!/usr/bin/env python3
"""voice_ab — the voice twin of rating_drip: paired-clip A/B on Seth's self-chat.

Why: voice replies had NO measurement. Text indistinguishability is at 0.225
(n=40); the voice surface (clone + model + pacing + the transcript-prep layer
wired 2026-09-20) has never been rated by the one person who can judge
"sounds like me". Every promotion (model, speed, prep, a Pro clone) should be
gated on this, the way text promotions gate on blind_ab_gate.json.

What: `gen` renders N pairs of clips from Seth's REAL sent texts (the
transcript is authentically his, so the pair differs ONLY on one voice axis),
through `human voice preview` — the exact daemon pipeline. Axes:

    model  sonic-3.6  vs sonic-3
    speed  0.85       vs 0.95
    prep   on         vs off (--raw)

Side (A/B) is assigned per pair by a seeded RNG; the answer key is private.
`tick` sends one pair at a time to the self-chat (text + clip A + clip B),
harvests the reply from chat.db with rating_drip's parser (A/B + confidence),
re-asks after 24h, skips after 3 asks. `score` prints per-axis preference
with a Wilson 95% CI and writes ~/.human/voice_ab/verdict.json.

Usage:
  python3 voice_ab.py gen --pairs 12 [--seed 7] [--persona seth]
  python3 voice_ab.py tick [--dry-run]
  python3 voice_ab.py status
  python3 voice_ab.py score
"""
import argparse
import json
import math
import os
import random
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rating_drip as rd  # noqa: E402  (harvest/parse/send-hours/imsg live there)

HOME = os.path.expanduser("~")
DIR = os.path.join(HOME, ".human", "voice_ab")
CLIPS = os.path.join(DIR, "clips")
SHEET = os.path.join(DIR, "sheet.json")
ANSWER_KEY = os.path.join(DIR, "answer_key.json")
STATE = os.path.join(DIR, "state.json")
VERDICT = os.path.join(DIR, "verdict.json")
CORPUS = os.path.join(HOME, ".human", "voice_corpus.jsonl")

AXES = {
    "model": ("sonic-3.6", "sonic-3"),
    "speed": ("0.85", "0.95"),
    "prep": ("on", "off"),
}
AXIS_ORDER = ("model", "speed", "prep")
MAX_ASKS = 3
REASK_AFTER_SECS = 24 * 3600


# ── pure helpers (unit-tested) ──────────────────────────────────────────

def usable_texts(lines, lo=60, hi=220):
    """Seth's real sent texts that read well aloud: mid-length, some terminal
    punctuation, no links/phone numbers/codes."""
    out, seen = [], set()
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            t = json.loads(line).get("text", "")
        except (ValueError, AttributeError):
            continue
        t = " ".join(t.split())
        if not (lo <= len(t) <= hi):
            continue
        if not re.search(r"[.!?]", t):
            continue
        if re.search(r"https?://|\d{3}[-.]\d{4}|\b[A-Z0-9]{6,}\b", t):
            continue
        if t in seen:
            continue
        seen.add(t)
        out.append(t)
    return out


def plan_pairs(texts, n_pairs, seed):
    """Deterministic plan: axes round-robin, one text per pair, side assignment
    by seeded RNG so neither variant is always 'A'. Returns (pairs, key)."""
    rng = random.Random(seed)
    texts = list(texts)
    rng.shuffle(texts)
    pairs, key = [], {}
    for i in range(n_pairs):
        axis = AXIS_ORDER[i % len(AXIS_ORDER)]
        v1, v2 = AXES[axis]
        text = texts[i % len(texts)]
        first_is_v1 = rng.random() < 0.5
        a, b = (v1, v2) if first_is_v1 else (v2, v1)
        pid = f"p{i + 1:02d}"
        pairs.append({"id": pid, "axis": axis, "text": text,
                      "A": f"{pid}_A.caf", "B": f"{pid}_B.caf"})
        key[pid] = {"axis": axis, "A": a, "B": b}
    return pairs, key


def preview_argv(human_bin, persona, text, axis, variant, out_path):
    """Exact CLI for one arm. Only the axis under test deviates from the
    persona's live settings, so the pair differs on one thing."""
    argv = [human_bin, "voice", "preview", "--persona", persona, "--text", text,
            "--out", out_path]
    if axis == "model":
        argv += ["--model", variant]
    elif axis == "speed":
        argv += ["--speed", variant]
    elif axis == "prep" and variant == "off":
        argv += ["--raw"]
    return argv


def compose_question(pair, answered, total):
    return (f"[h-uman voice {answered + 1}/{total}] two clips follow, A then B. "
            f"Same words, one difference. Which sounds more like you?\n"
            f"reply A or B (optionally + 1-5 confidence, e.g. \"B 4\")")


def wilson(k, n, z=1.96):
    """95% Wilson interval for k successes in n trials. (lo, hi); (0,1) at n=0."""
    if n == 0:
        return 0.0, 1.0
    p = k / n
    denom = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / denom
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / denom
    return max(0.0, centre - half), min(1.0, centre + half)


def score_sheet(pairs, key):
    """Per axis: how often variant 1 (the candidate) beat variant 2.
    Returns {axis: {"v1","v2","n","v1_wins","p","ci","verdict"}}."""
    out = {}
    for axis in AXIS_ORDER:
        v1, v2 = AXES[axis]
        n = wins = 0
        for p in pairs:
            if p["axis"] != axis or not p.get("choice"):
                continue
            picked = key[p["id"]][p["choice"]]
            n += 1
            wins += 1 if picked == v1 else 0
        lo, hi = wilson(wins, n)
        if n == 0:
            verdict = "unmeasured"
        elif lo > 0.5:
            verdict = f"{v1} preferred"
        elif hi < 0.5:
            verdict = f"{v2} preferred"
        else:
            verdict = "no preference yet"
        out[axis] = {"v1": v1, "v2": v2, "n": n, "v1_wins": wins,
                     "p": (wins / n) if n else None, "ci": [round(lo, 3), round(hi, 3)],
                     "verdict": verdict}
    return out


def next_unanswered(pairs, skipped=None):
    skipped = set(skipped or [])
    for p in pairs:
        if not p.get("choice") and p["id"] not in skipped:
            return p
    return None


# ── io ──────────────────────────────────────────────────────────────────

def load_sheet(path=SHEET):
    with open(path) as f:
        return json.load(f)


def save_sheet(pairs, path=SHEET):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(pairs, f, indent=1)
    os.replace(tmp, path)


def load_state():
    try:
        with open(STATE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {"target": rd.DEFAULT_TARGET, "pending": None, "question_unix": 0,
                "asks": 1, "sent": 0, "answered": 0, "skipped": [], "complete": False}


def save_state(st):
    tmp = STATE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(st, f, indent=1)
    os.replace(tmp, STATE)


def human_bin():
    for c in (os.environ.get("HUMAN_BIN"), shutil.which("human"),
              os.path.join(HOME, ".local", "bin", "human-daemon")):
        if c and os.path.exists(c):
            return c
    return "human"


def send_pair(target, text, clip_a, clip_b, dry_run=False):
    """Text, then clip A, then clip B — three sends, so the bubbles are
    unambiguous on the phone. Each clip is staged as 'Audio Message.caf' in
    its own dir (that is how iMessage renders a CAF as a voice memo)."""
    if dry_run or os.environ.get("HU_IS_TEST"):
        print(f"[dry-run] would send to {target}: {text}\n  A={clip_a}\n  B={clip_b}")
        return True
    if not rd.send_question(target, text):
        return False
    for label, clip in (("A", clip_a), ("B", clip_b)):
        stage = os.path.join(DIR, "stage", label)
        os.makedirs(stage, exist_ok=True)
        staged = os.path.join(stage, "Audio Message.caf")
        shutil.copyfile(clip, staged)
        r = subprocess.run([rd.imsg_bin(), "send", "--to", target, "--text", label,
                            "--file", staged, "--service", "imessage"],
                           capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            print(f"clip {label} send failed: {r.stderr.strip()[:200]}", file=sys.stderr)
            return False
        time.sleep(2)
    return True


# ── commands ────────────────────────────────────────────────────────────

def cmd_gen(a):
    os.makedirs(CLIPS, exist_ok=True)
    with open(a.corpus) as f:
        texts = usable_texts(f)
    if len(texts) < a.pairs:
        print(f"only {len(texts)} usable texts for {a.pairs} pairs", file=sys.stderr)
        return 1
    pairs, key = plan_pairs(texts, a.pairs, a.seed)
    hb = human_bin()
    for p in pairs:
        for side in ("A", "B"):
            out = os.path.join(CLIPS, p[side])
            argv = preview_argv(hb, a.persona, p["text"], p["axis"], key[p["id"]][side], out)
            if a.dry_run:
                print(" ".join(argv))
                continue
            r = subprocess.run(argv, capture_output=True, text=True, timeout=180)
            if r.returncode != 0 or not os.path.exists(out):
                print(f"{p['id']}{side}: preview failed: {r.stderr.strip()[-300:]}",
                      file=sys.stderr)
                return 1
            print(f"{p['id']}{side} ok ({p['axis']}={key[p['id']][side]})")
    if a.dry_run:
        return 0
    if os.path.exists(SHEET):
        stamp = time.strftime("%Y%m%d-%H%M%S")
        arch = os.path.join(DIR, f"rated-{stamp}")
        os.makedirs(arch, exist_ok=True)
        for fn in (SHEET, ANSWER_KEY, STATE):
            if os.path.exists(fn):
                shutil.move(fn, os.path.join(arch, os.path.basename(fn)))
    save_sheet(pairs)
    with open(ANSWER_KEY, "w") as f:
        json.dump(key, f, indent=1)
    st = load_state()
    st.update({"pending": None, "question_unix": 0, "asks": 1, "sent": 0, "answered": 0,
               "skipped": [], "complete": False})
    save_state(st)
    print(f"generated {len(pairs)} pairs -> {SHEET}")
    return 0


def cmd_tick(a, now=None):
    now = now if now is not None else time.time()
    st = load_state()
    pairs = load_sheet()
    total = len(pairs)
    if st.get("pending") and st.get("question_unix"):
        ans = rd.harvest_answer(st["target"], st["question_unix"])
        if ans:
            choice, conf = ans
            for p in pairs:
                if p["id"] == st["pending"]:
                    p["choice"], p["confidence"], p["answered_unix"] = choice, conf, now
            save_sheet(pairs)
            print(f"ingested: {st['pending']} = {choice} (conf {conf})")
            st.update({"pending": None, "question_unix": 0, "asks": 1,
                       "answered": st.get("answered", 0) + 1})
    if next_unanswered(pairs, st.get("skipped")) is None:
        if not st.get("complete"):
            cmd_score(a)
            st["complete"] = True
        save_state(st)
        return 0
    in_hours = rd.within_send_hours(time.localtime(now).tm_hour)
    if st.get("pending"):
        asks = st.get("asks", 1)
        if rd.should_reask(now, st.get("question_unix", 0), asks) and in_hours:
            p = next(x for x in pairs if x["id"] == st["pending"])
            answered = sum(1 for x in pairs if x.get("choice"))
            if send_pair(st["target"], compose_question(p, answered, total),
                         os.path.join(CLIPS, p["A"]), os.path.join(CLIPS, p["B"]), a.dry_run):
                st["asks"], st["question_unix"] = asks + 1, now
                print(f"re-asked {p['id']} (ask {asks + 1}/{MAX_ASKS})")
        elif (now - st.get("question_unix", 0)) >= REASK_AFTER_SECS and asks >= MAX_ASKS:
            st.setdefault("skipped", []).append(st["pending"])
            print(f"{st['pending']} unanswered after {MAX_ASKS} asks — skipping")
            st.update({"pending": None, "question_unix": 0, "asks": 1})
        else:
            print(f"waiting on {st['pending']}")
    elif not in_hours:
        print("outside send hours — skipping")
    else:
        p = next_unanswered(pairs, st.get("skipped"))
        answered = sum(1 for x in pairs if x.get("choice"))
        if send_pair(st["target"], compose_question(p, answered, total),
                     os.path.join(CLIPS, p["A"]), os.path.join(CLIPS, p["B"]), a.dry_run):
            st.update({"pending": p["id"], "question_unix": now, "asks": 1,
                       "sent": st.get("sent", 0) + 1})
            print(f"sent {p['id']} ({answered + 1}/{total})")
    save_state(st)
    return 0


def cmd_status(a):
    pairs = load_sheet()
    st = load_state()
    done = sum(1 for p in pairs if p.get("choice"))
    print(f"{done}/{len(pairs)} rated, pending={st.get('pending')}, "
          f"skipped={st.get('skipped')}, complete={st.get('complete')}")
    return 0


def cmd_score(a):
    pairs = load_sheet()
    with open(ANSWER_KEY) as f:
        key = json.load(f)
    res = score_sheet(pairs, key)
    for axis, r in res.items():
        print(f"{axis:6s} {r['v1']} vs {r['v2']}: {r['v1_wins']}/{r['n']} "
              f"CI={r['ci']} -> {r['verdict']}")
    with open(VERDICT, "w") as f:
        json.dump({"scored_at": int(time.time()), "axes": res}, f, indent=1)
    print(f"-> {VERDICT}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen")
    g.add_argument("--pairs", type=int, default=12)
    g.add_argument("--seed", type=int, default=7)
    g.add_argument("--persona", default="seth")
    g.add_argument("--corpus", default=CORPUS)
    g.add_argument("--dry-run", action="store_true")
    t = sub.add_parser("tick")
    t.add_argument("--dry-run", action="store_true")
    sub.add_parser("status")
    sub.add_parser("score")
    a = ap.parse_args(argv)
    a.dry_run = getattr(a, "dry_run", False)
    return {"gen": cmd_gen, "tick": cmd_tick, "status": cmd_status, "score": cmd_score}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
