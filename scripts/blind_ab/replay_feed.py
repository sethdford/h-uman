#!/usr/bin/env python3
"""Turn replay output into blind A/B inputs and per-arm stats vs Seth.

Reads <run>/turns.jsonl (Seth's real responses) and <run>/out/<arm>.jsonl
(`human replay` rows), then writes, per arm, under <run>/feed/<arm>/:

  triples.json   [{id, context, seth_reply, huuman_reply}] — the input of
                 make_rating_sheet.py (and so synthetic_judge.py / score.py).
                 Only turns where BOTH sides answered in text are kept; the
                 tapback/silence/dropped turns are counted in the stats instead.
  rating_sheet.csv + answer_key.json   (with --sheets: make_rating_sheet.py)

and <run>/feed/stats.json: per arm AND for Seth's real replies on the same
turns — reply length distribution, bubbles per reply, tapback share, silence
and drop share, question rate, truncated-fragment rate, deflection rate. Plus,
per arm, how many turns sent a reply request byte-identical to the first arm's
(`same_request_as_<arm>`): an arm identical on every turn did not reach the
prompt at all, and its numbers say nothing about its gate.

Refuses (exit 1, nothing written) when an arm's output is short of the turns
file or holds error rows — a partial arm is not a measurement.

Prints numbers only, never message text. Runbook: docs/guides/replay-harness.md.
"""
import argparse
import json
import math
import os
import re
import statistics
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from replay_export_turns import make_private_dir, run_dir_for  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# ── heuristics (pinned by test_replay_feed.py) ─────────────────────────

# Words an unpunctuated reply essentially never ends on: articles,
# coordinating conjunctions, a bare "to", possessives, "i", "just", "very",
# "gonna"/"gotta" — and "lock", a truncation seen in real replies ("gonna
# lock" cut from "gonna lock in for tomorrow"). Auxiliaries ("yeah it is",
# "i can") and stranded prepositions ("who are you with", "i'm in") are
# complete in texting, so they are NOT here.
DANGLING = frozenset("""
a an the and but or because cause cuz if than to
my your our their his her its i just very gonna gotta lock
""".split())
# "to" completes these: "what are you up to", "i have to", "i used to".
TO_OK_AFTER = frozenset("up used want have need got going supposed able hope plan try".split())
TERMINAL = ".!?…)\"'”’*"
_WORD = re.compile(r"[a-z']+")


def is_fragment(reply):
    """True for a reply cut off mid-clause: no terminal punctuation (or emoji)
    at the end AND the last word is a function word nothing ends on, or the
    text ends on a dangling comma/colon/dash."""
    s = (reply or "").rstrip()
    if not s:
        return False
    if s[-1] in ",:;-–—":
        return True
    if s[-1] in TERMINAL or ord(s[-1]) > 0x2000:  # punctuation or emoji/symbol
        return False
    words = _WORD.findall(s.lower())
    if not words or words[-1] not in DANGLING:
        return False
    return not (words[-1] == "to" and len(words) > 1 and words[-2] in TO_OK_AFTER)


DEFLECT_PHRASES = (
    "idk", "i dont know", "i don't know", "not sure", "no idea", "we'll see", "well see",
    "hard to say", "it depends", "depends", "let me think", "let me check",
    "i'll get back to you", "ill get back to you", "get back to you", "maybe later",
    "can't really say", "cant really say", "whatever you think", "up to you",
)
_STOP = frozenset("""
the a an and or but so to of in on at for with from about is are was were be been am
do does did have has had i you he she it we they me my your our their this that what
when where who how why are you u ur r just like ok okay yeah yes no not
""".split())
# A reply that opens with one of these, or carries a number or a clock time,
# is answering — "yeah what time?" answers "you coming?" before it asks.
ANSWER_OPENERS = frozenset("""
yes yeah yea yep yup ya ye no nah nope sure ok okay k kk def definitely absolutely
totally of course probably prob already tonight today tomorrow tmrw now soon later
""".split())
CONTENT_MIN_LEN = 4  # content words: >= 4 letters and not in _STOP
_NUMBERISH = re.compile(r"\b\d+(:\d\d)?\s*(am|pm)?\b|\b(noon|midnight)\b")


def _has_phrase(text, phrase):
    return re.search(r"(?<![a-z0-9])" + re.escape(phrase) + r"(?![a-z0-9])", text) is not None


def _content_words(text):
    return {w for w in _WORD.findall((text or "").lower())
            if len(w) >= CONTENT_MIN_LEN and w not in _STOP}


def asked_question(inbound):
    return "?" in (inbound or "")


def answers(inbound, reply):
    """The reply engages the question: it shares a content word with it,
    opens with an answer word, or carries a number / clock time."""
    r = (reply or "").strip().lower()
    words = _WORD.findall(r)
    if words and words[0] in ANSWER_OPENERS:
        return True
    if _NUMBERISH.search(r):
        return True
    return bool(_content_words(inbound) & _content_words(r))


def is_deflection(inbound, reply):
    """Defined only for a reply to a question. True when the reply does NOT
    answer (answers() is false) AND it either uses a stock non-answer phrase
    (word-bounded) or is only a question back."""
    r = (reply or "").strip().lower()
    if not r or not asked_question(inbound) or answers(inbound, r):
        return False
    if any(_has_phrase(r, p) for p in DEFLECT_PHRASES):
        return True
    return r.endswith("?") and not re.search(r"[.!]\s", r)


def ks_two_sample(xs, ys):
    """Two-sample Kolmogorov-Smirnov: (D, asymptotic p). None when a side is
    empty. p uses the Kolmogorov series with the Stephens small-n correction;
    with heavy ties (lengths are integers) it is conservative."""
    if not xs or not ys:
        return None
    a, b = sorted(xs), sorted(ys)
    n, m = len(a), len(b)
    i = j = 0
    d = 0.0
    while i < n and j < m:
        v = min(a[i], b[j])
        while i < n and a[i] == v:
            i += 1
        while j < m and b[j] == v:
            j += 1
        d = max(d, abs(i / n - j / m))
    en = math.sqrt(n * m / (n + m))
    lam = (en + 0.12 + 0.11 / en) * d
    if lam < 1e-9:
        return d, 1.0
    p = 2.0 * sum((-1) ** (k - 1) * math.exp(-2.0 * k * k * lam * lam) for k in range(1, 101))
    return d, min(1.0, max(0.0, p))


def p90(values):
    """Nearest-rank 90th percentile: the ceil(0.9 n)-th smallest value."""
    v = sorted(values)
    return v[max(0, math.ceil(0.9 * len(v)) - 1)] if v else None


def has_question(reply):
    return "?" in (reply or "")


# ── loading ────────────────────────────────────────────────────────────

def load_jsonl(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def joined(bubbles):
    return "\n".join(b for b in bubbles if b)


def summarize(rows):
    """rows: [{action, reply, inbound, bubbles}] -> stats dict (n = denominator)."""
    n = len(rows)
    if n == 0:
        return {"n": 0}
    texts = [r for r in rows if r["action"] == "text" and r["reply"]]
    lens = [len(r["reply"]) for r in texts]
    nt = len(texts)

    def share(k):
        return sum(1 for r in rows if r["action"] == k) / n

    def rate(pred):
        return (sum(1 for r in texts if pred(r)) / nt) if nt else None

    asked = [r for r in texts if asked_question(r["inbound"])]

    return {
        "n": n,
        "text_n": nt,
        "tapback_share": share("tapback"),
        "silence_share": share("silence"),
        "dropped_share": share("dropped"),
        "len_mean": statistics.fmean(lens) if lens else None,
        "len_median": statistics.median(lens) if lens else None,
        "len_p90": p90(lens),
        "bubbles_mean": statistics.fmean(r["bubbles"] for r in texts) if texts else None,
        "question_rate": rate(lambda r: has_question(r["reply"])),
        "fragment_rate": rate(lambda r: any(is_fragment(b) for b in r["bubble_list"])),
        "deflection_n": len(asked),
        "deflection_rate": (sum(1 for r in asked if is_deflection(r["inbound"], r["reply"]))
                            / len(asked)) if asked else None,
        "lengths": lens,
    }


def seth_rows(turns):
    return [{"action": t.get("seth_action", "text"), "reply": joined(t["seth_reply_bubbles"]),
             "bubble_list": t["seth_reply_bubbles"], "bubbles": len(t["seth_reply_bubbles"]),
             "inbound": joined(t["inbound_bubbles"])} for t in turns]


def arm_rows(turns_by_id, results):
    rows = []
    for r in results:
        t = turns_by_id[r["id"]]
        rows.append({"action": r["action"], "reply": joined(r["bubbles"]),
                     "bubble_list": r["bubbles"], "bubbles": r["bubble_count"],
                     "inbound": joined(t["inbound_bubbles"])})
    return rows


def triples_for(turns_by_id, results):
    out = []
    for r in results:
        t = turns_by_id[r["id"]]
        seth = joined(t["seth_reply_bubbles"])
        mine = joined(r["bubbles"])
        if r["action"] == "text" and mine and t.get("seth_action", "text") == "text" and seth:
            out.append({"id": r["id"], "context": joined(t["inbound_bubbles"]),
                        "seth_reply": seth, "huuman_reply": mine})
    return out


def arm_order(run_dir, out_dir):
    """The driver's arm order (manifest.json), so the first --arm is the
    baseline every other arm is compared with; else alphabetical."""
    present = {f[:-6] for f in os.listdir(out_dir) if f.endswith(".jsonl")}
    try:
        with open(os.path.join(run_dir, "manifest.json")) as f:
            ordered = [x["arm"] for x in json.load(f)["arms"] if x["arm"] in present]
    except (OSError, ValueError, KeyError):
        ordered = []
    return ordered + sorted(present - set(ordered))


def partial_reasons(run_dir, by_id, results):
    """Why these arms are not one complete measurement (empty when they are):
    the driver's manifest missing or marking an arm INCOMPLETE, an arm absent
    from the manifest, or arms covering different turn sets."""
    out = []
    try:
        with open(os.path.join(run_dir, "manifest.json")) as f:
            manifest = json.load(f)
        flags = {x["arm"]: x.get("complete") for x in manifest["arms"]}
    except (OSError, ValueError, KeyError):
        return ["no readable manifest.json from replay_driver.py run"]
    for arm in results:
        if arm not in flags:
            out.append(f"arm {arm} is not in the manifest")
        elif flags[arm] is not True:
            out.append(f"arm {arm} is marked INCOMPLETE in the manifest")
    sets = {arm: frozenset(r["id"] for r in rows) for arm, rows in results.items()}
    if len(set(sets.values())) > 1:
        sizes = ", ".join(f"{arm}={len(v)}" for arm, v in sets.items())
        out.append(f"arms cover different turn sets ({sizes})")
    return out


def write_private_json(path, obj):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(obj, f, indent=2, ensure_ascii=False)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--name", required=True)
    ap.add_argument("--run-root", default="~/blind_ab_run")
    ap.add_argument("--arm", action="append", default=[],
                    help="arms to feed (default: every out/*.jsonl)")
    ap.add_argument("--sheets", action="store_true", help="also run make_rating_sheet.py")
    ap.add_argument("--allow-partial", action="store_true",
                    help="feed INCOMPLETE arms on the turns every arm finished (exploration only)")
    a = ap.parse_args(argv)
    try:
        run_dir = run_dir_for(a.name, a.run_root)
    except ValueError as e:
        print(f"refusing: {e}", file=sys.stderr)
        return 2
    turns = load_jsonl(os.path.join(run_dir, "turns.jsonl"))
    by_id = {t["id"]: t for t in turns}
    out_dir = os.path.join(run_dir, "out")
    arms = a.arm or arm_order(run_dir, out_dir)
    if not arms:
        print("refusing: no arm output", file=sys.stderr)
        return 1
    results = {arm: load_jsonl(os.path.join(out_dir, f"{arm}.jsonl")) for arm in arms}
    why = partial_reasons(run_dir, by_id, results)
    if why and not a.allow_partial:
        for w in why:
            print(f"refusing: {w}", file=sys.stderr)
        print("refusing: a partial arm is not a measurement (--allow-partial to compare the "
              "turns every arm finished)", file=sys.stderr)
        return 1
    for w in why:
        print(f"WARNING (--allow-partial): {w}", file=sys.stderr)
    for arm, rows in results.items():
        bad = sum(1 for r in rows if r["action"] == "error")
        unknown = sum(1 for r in rows if r["id"] not in by_id)
        if bad or unknown or len(rows) == 0 or len({r["id"] for r in rows}) != len(rows):
            print(f"refusing: arm {arm} has {len(rows)} rows, {bad} errors, {unknown} unknown ids",
                  file=sys.stderr)
            return 1
    common = set.intersection(*({r["id"] for r in rows} for rows in results.values()))
    turns_c = [t for t in turns if t["id"] in common]
    stats = {"turns": len(turns_c), "seth": summarize(seth_rows(turns_c)), "arms": {}}
    first = arms[0]
    first_fp = {r["id"]: r.get("reply_fp") for r in results[first]}
    feed_root = os.path.join(run_dir, "feed")
    make_private_dir(feed_root)
    for arm in arms:
        rows = [r for r in results[arm] if r["id"] in common]
        s = summarize(arm_rows(by_id, rows))
        ks = ks_two_sample(s.get("lengths") or [], stats["seth"].get("lengths") or [])
        s["ks_len_vs_seth"] = None if ks is None else {"D": ks[0], "p": ks[1]}
        if arm != first:
            both = [r for r in rows if r.get("reply_fp") and first_fp.get(r["id"])]
            s[f"compared_with_{first}"] = len(both)
            s[f"same_request_as_{first}"] = sum(1 for r in both if r["reply_fp"] == first_fp[r["id"]])
        stats["arms"][arm] = s
        arm_dir = os.path.join(feed_root, arm)
        make_private_dir(arm_dir)
        triples = triples_for(by_id, rows)
        write_private_json(os.path.join(arm_dir, "triples.json"), triples)
        if a.sheets and triples:
            subprocess.run([sys.executable, os.path.join(HERE, "make_rating_sheet.py"),
                            os.path.join(arm_dir, "triples.json"), "--out-dir", arm_dir],
                           check=True, stdout=subprocess.DEVNULL)
            for fn in ("rating_sheet.csv", "answer_key.json"):
                if os.path.exists(os.path.join(arm_dir, fn)):
                    os.chmod(os.path.join(arm_dir, fn), 0o600)
    write_private_json(os.path.join(feed_root, "stats.json"), stats)
    cols = ("n", "tapback_share", "len_median", "len_p90", "bubbles_mean", "question_rate",
            "fragment_rate", "deflection_rate", "ks_D", "ks_p")
    for s in stats["arms"].values():
        if s.get("ks_len_vs_seth"):
            s["ks_D"], s["ks_p"] = s["ks_len_vs_seth"]["D"], s["ks_len_vs_seth"]["p"]
    print("who".ljust(16) + "".join(c[:12].rjust(13) for c in cols))
    for who, s in [("seth", stats["seth"])] + list(stats["arms"].items()):
        cells = []
        for c in cols:
            v = s.get(c)
            cells.append(("-" if v is None else (f"{v:.3f}" if isinstance(v, float) else str(v)))
                         .rjust(13))
        print(who[:15].ljust(16) + "".join(cells))
    for arm in arms[1:]:
        same = stats["arms"][arm].get(f"same_request_as_{first}", 0)
        compared = stats["arms"][arm].get(f"compared_with_{first}", 0)
        if compared > 0 and same == compared:
            print(f"WARNING: arm {arm} sent byte-identical reply requests to {first} on every "
                  f"turn — its gate never reached the prompt", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
