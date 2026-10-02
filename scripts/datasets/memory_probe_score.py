#!/usr/bin/env python3
"""Score memory-probe replies per arm and question category.

    memory_probe_score.py --probes probes.jsonl \\
        --arm base=replay_base.jsonl --arm cand=replay_cand.jsonl [--json report.json]

`--probes` is the probes.jsonl a converter wrote (locomo_to_replay.py,
msc_to_replay.py). Each `--arm` is the JSONL `human replay` wrote for those
probes: one record per turn, joined on `id` (or, failing that, on a 0-based
`line` index into the probes file). The reply is `bubbles` joined by
spaces, else `text`, else `reply`; a record whose `action` is not "text", or
whose reply is empty, counts as no reply.

Scoring is LLM-free and deterministic:
  contains   the normalized gold phrase appears whole in the normalized reply
             (multi-hop: every comma-separated part must appear)
  f1         LoCoMo-style token F1 (a light suffix stemmer stands in for
             Porter, so numbers are comparable within this harness only)
  recall     share of gold content tokens present in the reply — the useful
             one for a chatty reply, where precision is diluted by design
  date       temporal golds with a calendar date match on day+month (+year
             when the reply gives one), across "7 May 2023" / "may 7th" / 5/7/2023
  hit        contains, or recall >= --recall-threshold (temporal golds with a
             date: date or contains); adversarial: the reply abstains or
             corrects ("idk", "never", "that was me") and does not assert the
             adversarial answer
Every probe counts in its denominator: a missing or empty reply is a miss,
reported separately as `missing` / `no_reply`, never dropped.

`--judge-url` adds an optional judge column from a LOCAL OpenAI-compatible
server. Non-loopback URLs are refused: probe history never leaves the machine,
and a cloud judge is never used.
"""
import argparse
import json
import re
import sys
import urllib.parse
import urllib.request
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay_common as rc  # noqa: E402

normalize = rc.normalize
DEFAULT_RECALL_THRESHOLD = 0.6
_STOP = {"i", "you", "he", "she", "it", "we", "they", "is", "are", "was", "were", "be", "to",
         "of", "in", "on", "at", "for", "with", "by", "her", "his", "my", "your", "their",
         "that", "this", "or", "as", "from", "about"}
_ABSTAIN = ("not mentioned", "never mentioned", "didn t mention", "don t know", "dont know",
            "idk", "no idea", "not sure", "don t remember", "dont remember", "can t remember",
            "cant remember", "no clue", "never", "didn t", "didnt", "wasn t", "wasnt",
            "that was me", "not me", "haven t", "no information", "you re thinking of")
_MONTHS = {m: i + 1 for i, m in enumerate(
    ["january", "february", "march", "april", "may", "june", "july", "august", "september",
     "october", "november", "december"])}
_MONTHS.update({k[:3]: v for k, v in list(_MONTHS.items())})
_MONTHS["sept"] = 9
_MON = r"(" + "|".join(sorted(_MONTHS, key=len, reverse=True)) + r")\b\.?"
_ORD = r"(?:st|nd|rd|th)?"


# ── token metrics ────────────────────────────────────────────────────────


def _stem(w):
    for suf, minlen in (("ing", 6), ("ed", 5), ("es", 5), ("s", 4)):
        if w.endswith(suf) and len(w) >= minlen and not w.endswith("ss"):
            return w[: -len(suf)]
    return w


def _tokens(text):
    return [_stem(w) for w in normalize(text).split()]


def token_f1(pred, gold):
    p, g = _tokens(pred), _tokens(gold)
    same = sum((Counter(p) & Counter(g)).values())
    if not p or not g or not same:
        return 0.0
    prec, rec = same / len(p), same / len(g)
    return 2 * prec * rec / (prec + rec)


def token_recall(pred, gold):
    g = _tokens(gold)
    content = [w for w in g if w not in _STOP] or g
    if not content:
        return 0.0
    have = set(_tokens(pred))
    return sum(1 for w in content if w in have) / len(content)


def contains(pred, gold):
    return rc.contains_phrase(normalize(pred), normalize(gold))


# ── dates ────────────────────────────────────────────────────────────────


def extract_dates(text):
    """{(day|None, month|None, year|None)} found in free text."""
    s = str(text).lower()
    out = set()
    for d, m, y in re.findall(rf"\b(\d{{1,2}}){_ORD}\s*(?:of\s+)?{_MON},?\s*(\d{{4}})?", s):
        out.add((int(d), _MONTHS[m.rstrip(".")], int(y) if y else None))
    for m, d, y in re.findall(rf"\b{_MON}\s+(?:the\s+)?(\d{{1,2}}){_ORD}\b(?:,?\s*(\d{{4}}))?", s):
        out.add((int(d), _MONTHS[m.rstrip(".")], int(y) if y else None))
    for mo, d, y in re.findall(r"\b(\d{1,2})/(\d{1,2})/(\d{2,4})\b", s):
        y = int(y) + (2000 if len(y) == 2 else 0)
        out.add((int(d), int(mo), y))  # US month/day/year
    for m, y in re.findall(rf"\b{_MON}\s+(\d{{4}})\b", s):
        out.add((None, _MONTHS[m.rstrip(".")], int(y)))
    for y in re.findall(r"\b((?:19|20)\d{2})\b", s):
        out.add((None, None, int(y)))
    return out


def date_match(pred, gold):
    gold_dates = extract_dates(gold)
    if not gold_dates:
        return None  # nothing to compare: not a date gold
    got = extract_dates(pred)
    for gd, gm, gy in gold_dates:
        for pd, pm, py in got:
            if gd is not None:
                if pd == gd and pm == gm and (py is None or gy is None or py == gy):
                    return True
            elif gm is not None:
                if pm == gm and py == gy:
                    return True
            elif py == gy:
                return True
    return False


# ── one probe ────────────────────────────────────────────────────────────


def _parts(gold, category):
    if category == "multi_hop":
        return [p.strip() for p in gold.split(",") if p.strip()] or [gold]
    return [gold]


def score_probe(probe_rec, reply, recall_threshold=DEFAULT_RECALL_THRESHOLD):
    p = probe_rec["probe"]
    cat = p["category"]
    reply = reply or ""
    res = {"hit": False, "contains": False, "contains_frac": 0.0, "f1": 0.0, "recall": 0.0,
           "date_match": None, "abstained": False, "asserted_adversarial": False}
    if cat == "adversarial":
        nr = normalize(reply)
        res["abstained"] = any(rc.contains_phrase(nr, normalize(m)) for m in _ABSTAIN)
        adv = str(p.get("adversarial_answer") or "")
        # An adversarial answer made only of function words ("That") would
        # match every sentence; it cannot be asserted, only abstained from.
        if adv and any(w not in _STOP for w in _tokens(adv)):
            res["asserted_adversarial"] = (contains(reply, adv)
                                           or token_recall(reply, adv) >= recall_threshold)
        res["hit"] = bool(reply) and res["abstained"] and not res["asserted_adversarial"]
        res["contains"] = res["hit"]
        res["recall"] = 1.0 if res["hit"] else 0.0
        return res
    best = None
    for gold in p.get("gold_answers") or []:
        parts = _parts(str(gold), cat)
        frac = sum(1 for x in parts if contains(reply, x)) / len(parts)
        cand = {
            "contains_frac": frac,
            "contains": frac == 1.0,
            "f1": sum(token_f1(reply, x) for x in parts) / len(parts),
            "recall": sum(token_recall(reply, x) for x in parts) / len(parts),
            "date_match": date_match(reply, gold) if cat == "temporal" else None,
        }
        if cand["date_match"] is not None:
            cand["hit"] = cand["date_match"] or cand["contains"]
        else:
            cand["hit"] = cand["contains"] or cand["recall"] >= recall_threshold
        key = (cand["hit"], cand["f1"], cand["recall"])
        if best is None or key > (best["hit"], best["f1"], best["recall"]):
            best = cand
    if best:
        res.update(best)
    return res


# ── per-arm report ───────────────────────────────────────────────────────


def _reply_of(rec):
    if not rec:
        return None
    if rec.get("action", "text") != "text":
        return None
    b = rec.get("bubbles")
    text = " ".join(x for x in b if x) if isinstance(b, list) else (rec.get("text") or rec.get("reply") or "")
    return text.strip() or None


def _index_outputs(probes, outputs):
    ids = {p["id"]: i for i, p in enumerate(probes)}
    by_idx, extra = {}, 0
    for rec in outputs:
        rid = rec.get("id", rec.get("turn_id"))
        i = ids.get(rid) if rid is not None else None
        if i is None and isinstance(rec.get("line"), int) and 0 <= rec["line"] < len(probes):
            i = rec["line"]
        if i is None:
            extra += 1
        else:
            by_idx[i] = rec
    return by_idx, extra


def _agg(rows, with_judge):
    n = len(rows)
    hits = sum(1 for r in rows if r["hit"])
    out = {"n": n, "hits": hits, "hit_rate": hits / n,
           "contains_rate": sum(1 for r in rows if r["contains"]) / n,
           "mean_f1": sum(r["f1"] for r in rows) / n,
           "mean_recall": sum(r["recall"] for r in rows) / n}
    if with_judge:
        out["judge_rate"] = sum(1 for r in rows if r.get("judge")) / n
    return out


def build_report(probes, arms, judge=None, recall_threshold=DEFAULT_RECALL_THRESHOLD):
    if not probes:
        raise ValueError("no probes: refusing to report rates over an empty denominator")
    report = {"probes": len(probes), "recall_threshold": recall_threshold,
              "judge": bool(judge), "arms": {}}
    for arm, outputs in arms.items():
        by_idx, extra = _index_outputs(probes, outputs)
        rows = []
        missing = no_reply = 0
        for i, pr in enumerate(probes):
            rec = by_idx.get(i)
            if rec is None:
                missing += 1
            reply = _reply_of(rec)
            if rec is not None and reply is None:
                no_reply += 1
            row = score_probe(pr, reply, recall_threshold)
            if judge and reply is not None:
                p = pr["probe"]
                gold = "; ".join(p.get("gold_answers") or []) or "(never mentioned; a correct reply says so)"
                row["judge"] = bool(judge(p["question_original"], gold, reply))
            row["category"] = pr["probe"]["category"]
            row["window"] = "in_window" if pr["probe"].get("evidence_in_window") else "beyond_window"
            row["dataset"] = pr["probe"].get("dataset", "?")
            rows.append(row)
        groups = defaultdict(lambda: defaultdict(list))
        for r in rows:
            groups["by_category"][r["category"]].append(r)
            groups["by_dataset"][r["dataset"]].append(r)
            groups["by_window"][r["window"]].append(r)
        rep = {"overall": _agg(rows, bool(judge)), "answered": len(probes) - missing - no_reply,
               "missing": missing, "no_reply": no_reply, "unmatched_outputs": extra}
        rep["overall"]["adversarial_asserted"] = sum(1 for r in rows if r["asserted_adversarial"])
        for g in ("by_category", "by_dataset"):
            rep[g] = {k: _agg(v, bool(judge)) for k, v in sorted(groups[g].items())}
        rep["by_window"] = {k: {kk: _agg(v, False)[kk] for kk in ("n", "hits", "hit_rate")}
                            for k, v in sorted(groups["by_window"].items())}
        report["arms"][arm] = rep
    return report


# ── optional local judge ─────────────────────────────────────────────────

_LOOPBACK = {"127.0.0.1", "localhost", "::1"}


def check_loopback_url(url):
    u = urllib.parse.urlsplit(url or "")
    if u.scheme not in ("http", "https") or u.username or u.password or u.hostname not in _LOOPBACK:
        raise ValueError(f"judge URL must be loopback (127.0.0.1, localhost or [::1]): {url!r}")
    return url


def make_local_judge(url, model):
    check_loopback_url(url)

    def judge(question, gold, reply):
        body = {
            "model": model, "temperature": 0, "max_tokens": 3,
            "messages": [
                {"role": "system", "content": "You grade memory. Answer only YES or NO."},
                {"role": "user", "content": (
                    f"Question: {question}\nGold answer: {gold}\nReply: {reply}\n"
                    "Does the reply give the gold answer (or, if the gold says it was never "
                    "mentioned, correctly decline)? YES or NO.")},
            ],
        }
        req = urllib.request.Request(url, data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=60) as resp:  # noqa: S310 - loopback only
            out = json.loads(resp.read())
        return str(out["choices"][0]["message"]["content"]).strip().upper().startswith("YES")

    return judge


# ── CLI ──────────────────────────────────────────────────────────────────


def _read_jsonl(path):
    return [json.loads(line) for line in Path(path).read_text(encoding="utf-8").splitlines()
            if line.strip()]


def format_table(report):
    lines = []
    cats = sorted({c for a in report["arms"].values() for c in a["by_category"]})
    head = ["arm", "overall"] + cats + ["in_window", "beyond_window", "missing", "no_reply"]
    lines.append(" | ".join(head))
    lines.append(" | ".join("---" for _ in head))
    for arm, a in report["arms"].items():
        def cell(d):
            return f"{d['hit_rate']:.3f} ({d['hits']}/{d['n']})" if d else "-"
        row = [arm, cell(a["overall"])] + [cell(a["by_category"].get(c)) for c in cats]
        row += [cell(a["by_window"].get("in_window")), cell(a["by_window"].get("beyond_window")),
                str(a["missing"]), str(a["no_reply"])]
        lines.append(" | ".join(row))
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--probes", nargs="+", required=True)
    ap.add_argument("--arm", action="append", required=True, metavar="NAME=PATH")
    ap.add_argument("--json", help="write the full report here")
    ap.add_argument("--recall-threshold", type=float, default=DEFAULT_RECALL_THRESHOLD)
    ap.add_argument("--judge-url", help="LOCAL OpenAI-compatible chat endpoint (loopback only)")
    ap.add_argument("--judge-model", default="local")
    a = ap.parse_args(argv)
    judge = None
    if a.judge_url:
        try:
            judge = make_local_judge(a.judge_url, a.judge_model)
        except ValueError as e:
            print(f"memory_probe_score: {e}", file=sys.stderr)
            return 1
    probes = [p for f in a.probes for p in _read_jsonl(f)]
    arms = {}
    for spec in a.arm:
        name, _, path = spec.partition("=")
        if not name or not path:
            print(f"memory_probe_score: --arm wants NAME=PATH, got {spec!r}", file=sys.stderr)
            return 1
        arms[name] = _read_jsonl(path)
    try:
        report = build_report(probes, arms, judge, a.recall_threshold)
    except ValueError as e:
        print(f"memory_probe_score: {e}", file=sys.stderr)
        return 2
    dead = [n for n, r in report["arms"].items() if r["answered"] == 0]
    if dead:
        print(f"memory_probe_score: arm(s) {dead} answered 0 probes — no measurement; "
              "refusing to write a report", file=sys.stderr)
        return 2
    for n, r in report["arms"].items():
        if r["missing"]:
            print(f"memory_probe_score: arm {n}: {r['missing']} probe(s) have no replay output "
                  "(scored as misses)", file=sys.stderr)
    print(format_table(report))
    if a.json:
        Path(a.json).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
