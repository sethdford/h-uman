#!/usr/bin/env python3
"""Local PM-Bench-style harness for prospective memory v2 (spec
docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §3, §4.5).

A scripted multi-day scenario set runs against the REAL C functions through
`human prospective probe --full` (hu_prospective_v2_run / _after_delivery):
one fresh fixture DB per scenario (`human prospective init`), each step at a
scripted clock (--now), history passed as a file. The Decide call is the local
model by default (--judge model: the configured provider, GLM on :8741 in
prod); --judge fire|not_now|… makes every call return that word.

Scenario classes (8 situations each): clean_positive, overloaded_positive
(4 pending distractors), silent_negative (resolved earlier in the thread),
cancellation, reschedule (time cue moved; the stale one must not fire, the
new one must), cross_day (keyword cue 5 days after it was noted; time cue due
on day 3).

Reports counts and rates only, no action, cue or conversation text, to
~/.human/logs/pm-bench-local-<ts>.json (0600):
  set_f1                       micro Set-F1 over every judged step's fired set
  false_alarms_per_step        steps with >= 1 fire not expected / judged steps
  silent_negative_false_alarm  steps tagged silent that fired anything / those steps
  cross_day_miss               cross-day expectations not fired / cross-day expectations
  update_miss                  reschedule steps whose fired set was wrong / those steps
  judge_failure_rate           (parse_fail + judge_err) / candidates

Verdict PASS iff set_f1 >= 0.80, silent_negative_false_alarm <= 0.05 and
cross_day_miss <= 0.10 (exit 0); otherwise FAIL (exit 1). Both write the report.
Refuses (exit 2, writes nothing) when the binary is missing, any probe exits
non-zero or breaks the --full contract, nothing was ever judged, or the
scenario set is below its minimums. Exit 3 (writes nothing) when judge
failures exceed 10% of judged items: that run measured the plumbing, not the
policy.
"""
import argparse
import datetime as dt
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import curator_names as cn  # noqa: E402 -- the scripts' shared private 0600 writer

HOME = os.path.expanduser("~")
BASE = 1_790_000_000  # fixed scenario epoch (2026-09-21 UTC); every step is an offset
CONTACT = "+15550100001"
GRACE = 3 * 86400
THRESHOLDS = {"set_f1_min": 0.80, "silent_false_alarm_max": 0.05, "cross_day_miss_max": 0.10,
              "judge_failure_max": 0.10}
MINIMUMS = {"positive_expectations": 20, "silent_steps": 20, "cross_day_expectations": 10}

#
# NOTE: no trailing `$` on the 10 required groups — the real `probe --full`
# header carries a `write_err=N` field after `bytes=N` (measured against
# build/human, HU_STATE_DIR in a temp HOME: "candidates=1 fire=1 ... bytes=94
# write_err=0"). write_err is captured as an 11th, OPTIONAL group: a step
# whose write failed did not complete its measurement (the surfaced text
# never made it out), so it must not be scored as either a correct silence
# or a fire (fix round 1, controller ruling). The fixture `human` in
# tests/test_pm_bench_local.py omits the field entirely; the trailing `?`
# keeps that fixture's output matching the same pattern with write_err=0.
HEADER = re.compile(r"^candidates=(\d+) fire=(\d+) resolved=(\d+) cancel=(\d+) not_now=(\d+) "
                    r"parse_fail=(\d+) judge_err=(\d+) expired=(\d+) capped=(\d+) bytes=(\d+)"
                    r"(?: write_err=(\d+))?")
HEADER_KEYS = ("candidates", "fire", "resolved", "cancel", "not_now", "parse_fail", "judge_err",
               "expired", "capped", "bytes")
ITEM = re.compile(r"^item id=(\d+) "
                  r"verdict=(fire|already_resolved|cancel|not_now|parse_fail|judge_err)$")
DELIVERED = re.compile(r"^surfaced=(\d+) used=(\d+) ignored=(\d+) expired=(\d+)$")

SITUATIONS = [
    {"key": "taco", "cue": "taco place", "action": "ask how the new taco place was",
     "setup": ["them: trying that new taco place friday", "me: nice, report back"],
     "cue_text": "ok the taco place was packed",
     "cue_text2": "the taco place again tonight lol",
     "cue_text3": "still thinking about that taco place",
     "resolved": ["them: taco place was amazing, the al pastor was unreal", "me: told you"],
     "cancel": ["them: the taco place closed down, forget it", "me: noo"],
     "push": "them: we pushed the taco place trip to next week",
     "reply": "wait how was the taco place, worth it?"},
    {"key": "interview", "cue": "interview", "action": "ask how her job interview went",
     "setup": ["them: bank interview on thursday, so nervous", "me: you got this"],
     "cue_text": "interview prep is killing me",
     "cue_text2": "ugh the interview",
     "cue_text3": "interview stuff all day",
     "resolved": ["them: interview went great, they offered me the job!", "me: lets gooo"],
     "cancel": ["them: they canceled the interview, hiring freeze", "me: that sucks"],
     "push": "them: the interview got moved to next tuesday",
     "reply": "how did the interview go??"},
    {"key": "dentist", "cue": "dentist",
     "action": "check if he booked the dentist appointment",
     "setup": ["them: my tooth has been killing me", "me: book the dentist already"],
     "cue_text": "ugh my dentist never calls back",
     "cue_text2": "the dentist thing is annoying",
     "cue_text3": "dentist again today maybe",
     "resolved": ["them: booked the dentist appointment for monday", "me: finally"],
     "cancel": ["them: nvm on the dentist, the tooth stopped hurting", "me: ok good"],
     "push": "them: the dentist moved me to next month",
     "reply": "did you ever get the dentist appointment booked?"},
    {"key": "guitar", "cue": "guitar", "action": "send her the guitar teacher's number",
     "setup": ["them: i want to get back into guitar",
               "me: my old teacher is great, ill send you his number"],
     "cue_text": "practiced guitar for an hour",
     "cue_text2": "my guitar is so out of tune",
     "cue_text3": "guitar question for you",
     "resolved": ["me: sent you the guitar teacher's number", "them: got it thanks"],
     "cancel": ["them: giving up on guitar lessons, dont need the number", "me: fair"],
     "push": "them: guitar lessons can wait till next month",
     "reply": "here's the guitar teacher's number: 555-0100"},
    {"key": "marathon", "cue": "marathon", "action": "ask how the marathon training is going",
     "setup": ["them: signed up for the chicago marathon", "me: that's huge"],
     "cue_text": "marathon playlist ideas?",
     "cue_text2": "my legs are dead from marathon stuff",
     "cue_text3": "marathon is in six weeks",
     "resolved": ["them: marathon training update, ran 18 miles today, all good",
                  "me: beast"],
     "cancel": ["them: pulled out of the marathon, knee is shot", "me: oh no"],
     "push": "them: deferring the marathon to spring",
     "reply": "how's the marathon training going?"},
    {"key": "moving", "cue": "moving", "action": "offer to help with the move on saturday",
     "setup": ["them: moving into the new place saturday", "me: need hands?"],
     "cue_text": "moving boxes everywhere",
     "cue_text2": "moving is the worst",
     "cue_text3": "so much moving stress",
     "resolved": ["me: i'll be there saturday to help with the move", "them: you're the best"],
     "cancel": ["them: moving got canceled, landlord extended our lease", "me: oh nice"],
     "push": "them: moving day got pushed to next weekend",
     "reply": "want help with the move saturday? i can bring the truck"},
    {"key": "lasagna", "cue": "lasagna", "action": "send the lasagna recipe",
     "setup": ["them: that lasagna you made was insane", "me: ill send you the recipe"],
     "cue_text": "craving lasagna again",
     "cue_text2": "lasagna night at our place?",
     "cue_text3": "attempting lasagna this weekend",
     "resolved": ["me: just sent you the lasagna recipe", "them: making it tonight"],
     "cancel": ["them: found a lasagna recipe online, dont worry about it", "me: cool"],
     "push": "them: lasagna attempt postponed, kitchen is torn up",
     "reply": "ok here's the lasagna recipe, go easy on the ricotta"},
    {"key": "biscuit", "cue": "biscuit", "action": "ask how biscuit's vet visit went",
     "setup": ["them: biscuit has a vet visit tomorrow, fingers crossed", "me: poor guy"],
     "cue_text": "biscuit is being so dramatic",
     "cue_text2": "biscuit stole a sock again",
     "cue_text3": "biscuit says hi",
     "resolved": ["them: biscuit's vet visit went fine, just allergies", "me: phew"],
     "cancel": ["them: vet canceled biscuit's appointment, rescheduling someday",
                "me: annoying"],
     "push": "them: biscuit's vet visit moved to next friday",
     "reply": "how'd biscuit's vet visit go?"},
]


class ProbeError(Exception):
    """A probe run that does not honor its contract. Messages never carry text."""


def day(d, h=0, m=0):
    return BASE + d * 86400 + h * 3600 + m * 60


def kw(s, created=None):
    return {"kind": "keyword", "key": s["key"], "cue": s["cue"], "action": s["action"],
            "created": day(0) if created is None else created}


def tm(s, due, key=None, action=None, created=None):
    return {"kind": "time", "key": key or s["key"], "action": action or s["action"], "due": due,
            "created": day(0) if created is None else created}


def step(t, op, text=None, history=(), expect=(), tags=(), add=None):
    return {"t": t, "op": op, "text": text, "history": list(history), "expect": list(expect),
            "tags": list(tags), "add": add}


def clean_positive(s):
    return {"id": f"clean-{s['key']}", "cls": "clean_positive", "intentions": [kw(s)], "steps": [
        step(day(1, 10), "inbound", s["cue_text"], s["setup"], [s["key"]]),
        step(day(1, 10, 5), "deliver", s["reply"]),
        step(day(2, 9), "inbound", s["cue_text2"], s["setup"] + ["me: " + s["reply"]], [],
             ["after_done"]),
    ]}


def overloaded_positive(s, others):
    return {"id": f"overloaded-{s['key']}", "cls": "overloaded_positive",
            "intentions": [kw(s)] + [kw(o) for o in others], "steps": [
                step(day(1, 10), "inbound", s["cue_text"], s["setup"], [s["key"]]),
            ]}


def silent_negative(s):
    hist = s["setup"] + s["resolved"]
    return {"id": f"silent-{s['key']}", "cls": "silent_negative", "intentions": [kw(s)], "steps": [
        step(day(1, 10), "inbound", s["cue_text"], hist, [], ["silent"]),
        step(day(2, 10), "inbound", s["cue_text2"], hist, [], ["silent"]),
        step(day(3, 10), "inbound", s["cue_text3"], hist, [], ["silent"]),
    ]}


def cancellation(s):
    hist = s["setup"] + s["cancel"]
    return {"id": f"cancel-{s['key']}", "cls": "cancellation", "intentions": [kw(s)], "steps": [
        step(day(1, 10), "inbound", s["cue_text"], hist, [], ["silent"]),
        step(day(2, 10), "inbound", s["cue_text2"], hist, [], ["silent"]),
    ]}


def reschedule(s):
    hist = s["setup"] + [s["push"]]
    moved = tm(s, day(4, 9), key=s["key"] + "-v2", action=s["action"] + " after the reschedule",
               created=day(1, 11))
    return {"id": f"reschedule-{s['key']}", "cls": "reschedule",
            "intentions": [tm(s, day(1, 9))], "steps": [
                step(day(1, 10), "tick", None, hist, [], ["silent", "update"]),
                step(day(1, 11), "add", add=moved),
                step(day(4, 10), "tick", None, hist, [s["key"] + "-v2"], ["update"]),
            ]}


def cross_day(s):
    return [
        {"id": f"crossday-kw-{s['key']}", "cls": "cross_day", "intentions": [kw(s)], "steps": [
            step(day(5, 12), "inbound", s["cue_text"], s["setup"], [s["key"]], ["cross_day"]),
        ]},
        {"id": f"crossday-time-{s['key']}", "cls": "cross_day", "intentions": [tm(s, day(3, 9))],
         "steps": [
             step(day(1, 10), "tick", None, s["setup"], [], []),
             step(day(3, 10), "tick", None, s["setup"], [s["key"]], ["cross_day"]),
             step(day(3, 10, 5), "deliver", s["reply"]),
             step(day(3, 15), "tick", None, s["setup"] + ["me: " + s["reply"]], [],
                  ["after_done"]),
         ]},
    ]


def build_scenarios():
    out = []
    n = len(SITUATIONS)
    for i, s in enumerate(SITUATIONS):
        others = [SITUATIONS[(i + k) % n] for k in range(1, 5)]
        out += [clean_positive(s), overloaded_positive(s, others), silent_negative(s),
                cancellation(s), reschedule(s)]
        out += cross_day(s)
    return out


def _cued(cue, text):
    return re.search(r"(?<![a-z0-9])" + re.escape(cue.lower()) + r"(?![a-z0-9])",
                     text.lower()) is not None


def validate(scenarios):
    """-> minimum counts. Raises ValueError for a step that cues an intention it
    does not expect (or does not cue one it expects), or a set below MINIMUMS."""
    counts = {k: 0 for k in MINIMUMS}
    for s in scenarios:
        cues = {i["key"]: i["cue"] for i in s["intentions"] if i["kind"] == "keyword"}
        for st in s["steps"]:
            if st["op"] == "inbound":
                for key, cue in cues.items():
                    hit = _cued(cue, st["text"])
                    quiet = "silent" in st["tags"] or "after_done" in st["tags"]
                    if hit and key not in st["expect"] and not quiet:
                        raise ValueError(f"scenario {s['id']}: a step cues an unexpected intention")
                    if key in st["expect"] and not hit:
                        raise ValueError(f"scenario {s['id']}: an expected intention is not cued")
            if st["op"] in ("inbound", "tick"):
                counts["positive_expectations"] += len(st["expect"])
                counts["silent_steps"] += 1 if "silent" in st["tags"] else 0
                if "cross_day" in st["tags"]:
                    counts["cross_day_expectations"] += len(st["expect"])
    for k, floor in MINIMUMS.items():
        if counts[k] < floor:
            raise ValueError(f"scenario set below its minimums ({k} {counts[k]} < {floor})")
    return counts


def score(results):
    """results: [{"expect": [...], "pred": [...], "tags": [...]}] for judged steps."""
    tp = fp = fn = fa_steps = silent = silent_fa = cd_exp = cd_miss = upd = upd_err = 0
    for r in results:
        e, p, tags = set(r["expect"]), set(r["pred"]), set(r["tags"])
        tp += len(e & p)
        fp += len(p - e)
        fn += len(e - p)
        fa_steps += 1 if p - e else 0
        if "silent" in tags:
            silent += 1
            silent_fa += 1 if p else 0
        if "cross_day" in tags:
            cd_exp += len(e)
            cd_miss += len(e - p)
        if "update" in tags:
            upd += 1
            upd_err += 1 if p != e else 0
    denom = 2 * tp + fp + fn
    counts = {"steps": len(results), "tp": tp, "fp": fp, "fn": fn,
              "steps_with_false_alarm": fa_steps, "silent_steps": silent,
              "silent_false_alarms": silent_fa, "cross_day_expectations": cd_exp,
              "cross_day_misses": cd_miss, "update_steps": upd, "update_errors": upd_err}
    rates = {"set_f1": (2 * tp / denom) if denom else None,
             "false_alarms_per_step": (fa_steps / len(results)) if results else None,
             "silent_negative_false_alarm": (silent_fa / silent) if silent else None,
             "cross_day_miss": (cd_miss / cd_exp) if cd_exp else None,
             "update_miss": (upd_err / upd) if upd else None}
    return counts, rates


def probe(a, args):
    r = subprocess.run([a.human_bin, "prospective"] + args, capture_output=True, text=True,
                       timeout=a.timeout)
    if r.returncode != 0:
        raise ProbeError(f"probe '{args[0]}' exited {r.returncode}")
    return r.stdout


def parse_full(out):
    lines = out.splitlines()
    m = HEADER.match(lines[0]) if lines else None
    if not m:
        raise ProbeError("probe header does not match the --full contract")
    groups = m.groups()
    h = dict(zip(HEADER_KEYS, (int(g) for g in groups[:len(HEADER_KEYS)])))
    write_err_group = groups[len(HEADER_KEYS)]
    h["write_err"] = int(write_err_group) if write_err_group is not None else 0
    items = [(int(im.group(1)), im.group(2)) for im in map(ITEM.match, lines[1:]) if im]
    if len(items) != h["candidates"] or sum(1 for _, v in items if v == "fire") != h["fire"]:
        raise ProbeError("probe items disagree with its header")
    return h, items


def seed(con, it):
    if it["kind"] == "keyword":
        con.execute("INSERT INTO prospective_memories(trigger_type,trigger_value,action,"
                    "contact_id,expires_at,fired,created_at) VALUES('keyword',?,?,?,?,0,?)",
                    (it["cue"], it["action"], CONTACT, it["created"] + 30 * 86400,
                     it["created"]))
    else:
        con.execute("INSERT INTO prospective_memories(trigger_type,trigger_value,action,"
                    "contact_id,expires_at,fired,created_at,cue_kind,due_at,status,source) "
                    "VALUES('time',?,?,?,?,0,?,'time',?,'pending','promise_keeper')",
                    ("bench:" + it["key"], it["action"], CONTACT, it["due"] + GRACE,
                     it["created"], it["due"]))
    con.commit()


def run_scenario(a, scn, scratch, idx):
    db = os.path.join(scratch, f"s{idx}.db")
    if probe(a, ["init", "--db", db]).strip() != "ok":
        raise ProbeError("init did not print ok")
    con = sqlite3.connect(db)
    by_action = {}
    results = []
    judge = {"candidates": 0, "parse_fail": 0, "judge_err": 0, "write_err": 0}
    try:
        for it in scn["intentions"]:
            seed(con, it)
            by_action[it["action"]] = it["key"]
        for k, st in enumerate(scn["steps"]):
            if st["op"] == "add":
                seed(con, st["add"])
                by_action[st["add"]["action"]] = st["add"]["key"]
                continue
            base = ["probe", "--db", db, "--contact", CONTACT, "--now", str(st["t"])]
            if st["op"] == "deliver":
                if not DELIVERED.match(probe(a, base + ["--deliver", st["text"]]).strip()):
                    raise ProbeError("deliver output does not match the contract")
                continue
            hist = os.path.join(scratch, f"s{idx}-h{k}.txt")
            with open(hist, "w") as f:
                f.write("".join(line + "\n" for line in st["history"]))
            args = base + ["--full", "--judge", a.judge, "--history", hist]
            args += ["--inbound", st["text"]] if st["op"] == "inbound" else ["--tick"]
            h, items = parse_full(probe(a, args))
            for key in judge:
                judge[key] += h[key]
            if h["write_err"] > 0:
                # F1R1 (no-number-without-a-measurement): the surfaced write
                # failed, so this step's fire/silence was never actually
                # observed. Counted above toward the judge failure budget
                # (main()'s jfr); excluded here from scoring so it cannot be
                # read as a correct silence or a correct fire.
                continue
            pred = set()
            for item_id, verdict in items:
                row = con.execute("SELECT action FROM prospective_memories WHERE id=?",
                                  (item_id,)).fetchone()
                if not row or row[0] not in by_action:
                    raise ProbeError("probe named an intention the scenario never seeded")
                if verdict == "fire":
                    pred.add(by_action[row[0]])
            results.append({"expect": st["expect"], "pred": sorted(pred), "tags": st["tags"]})
    finally:
        con.close()
    return results, judge


def refuse(msg):
    print(f"refusing: {msg}; nothing written", file=sys.stderr)
    return 2


def write_report(out_dir, stamp, payload):
    """0600 via the scripts' shared private writer (curator_names.write_jsonl_private) --
    see Task 10's scripts/prospective_backfill.py for the sibling caller. One payload,
    one JSON line; the file extension stays .json to match the manifest naming
    convention this harness documents in its own header."""
    path = os.path.join(out_dir, f"pm-bench-local-{stamp}.json")
    return cn.write_jsonl_private(path, [payload])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--human-bin", default=os.path.join(HOME, ".local/bin/human-daemon"))
    ap.add_argument("--judge", default="model",
                    choices=["model", "fire", "already_resolved", "cancel", "not_now"])
    ap.add_argument("--out-dir", default=os.path.join(HOME, ".human/logs"))
    ap.add_argument("--timeout", type=int, default=120)
    a = ap.parse_args(argv)
    if not (os.path.isfile(a.human_bin) and os.access(a.human_bin, os.X_OK)):
        return refuse(f"no executable human binary at {a.human_bin}")
    scenarios = build_scenarios()
    try:
        validate(scenarios)
    except ValueError as e:
        return refuse(str(e))
    scratch = tempfile.mkdtemp(prefix="pm-bench-")
    results = []
    judge = {"candidates": 0, "parse_fail": 0, "judge_err": 0, "write_err": 0}
    try:
        for i, scn in enumerate(scenarios):
            r, j = run_scenario(a, scn, scratch, i)
            results += r
            for k in judge:
                judge[k] += j[k]
    except (ProbeError, OSError, sqlite3.Error, subprocess.TimeoutExpired) as e:
        return refuse(f"scenario run failed ({e.__class__.__name__}: {e})")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    if judge["candidates"] == 0:
        return refuse("no intention was ever judged")
    # write_err joins parse_fail/judge_err in the same failure budget (fix
    # round 1): a step whose surfaced write failed measured the plumbing, not
    # the policy, same as a judge parse failure or a judge error.
    jfr = (judge["parse_fail"] + judge["judge_err"] + judge["write_err"]) / judge["candidates"]
    if jfr > THRESHOLDS["judge_failure_max"]:
        print(f"INCONCLUSIVE: judge failures {jfr:.2f} > {THRESHOLDS['judge_failure_max']}; "
              "nothing written", file=sys.stderr)
        return 3
    counts, rates = score(results)
    rates["judge_failure_rate"] = jfr
    counts.update(judge)
    ok = (rates["set_f1"] is not None and rates["set_f1"] >= THRESHOLDS["set_f1_min"]
          and rates["silent_negative_false_alarm"] is not None
          and rates["silent_negative_false_alarm"] <= THRESHOLDS["silent_false_alarm_max"]
          and rates["cross_day_miss"] is not None
          and rates["cross_day_miss"] <= THRESHOLDS["cross_day_miss_max"])
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    payload = {"schema_version": 1, "measured_at": stamp, "judge": a.judge,
               "scenarios": len(scenarios), "steps": counts["steps"], "counts": counts,
               "rates": rates, "thresholds": THRESHOLDS, "verdict": "PASS" if ok else "FAIL"}
    path = write_report(a.out_dir, stamp, payload)
    def fmt(x):
        return "null" if x is None else f"{x:.3f}"
    print(f"pm_bench_local: set_f1={fmt(rates['set_f1'])} "
          f"silent_fa={fmt(rates['silent_negative_false_alarm'])} "
          f"cross_day_miss={fmt(rates['cross_day_miss'])} verdict={payload['verdict']}")
    print(f"wrote {path}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
