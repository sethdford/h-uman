#!/usr/bin/env python3
"""Blind spot check of SHADOW would-fires -- the spec §3 promotion bar for
HU_PROSPECTIVE=live (docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md
§3, §6 step 4): "30 SHADOW would-fires spot-checked blind ... precision >= 0.8".

The existing rating-sheet flow (scripts/build_rule_preference_sheet.py ->
scripts/blind_ab/score_preference.py) asks an A-vs-B preference; precision
needs a yes/no verdict per item instead. This script builds the same kind of
local sheet for that question, mixing SHADOW would-fires with an equal number
of held items (already_resolved / not_now / cancel) so the rater cannot tell
which is which, then scores precision on the would-fires only.

  build  Reads the dated "prospective [time ]shadow item: id=N verdict=V"
         log lines (src/daemon/daemon_prospective.c
         hu_daemon_prospective_log_counts) via prospective_shadow_report's
         own parser -- not a second regex -- collapses repeated lines for the
         same id to its most recent verdict, and further collapses to one row
         per (contact, action) intention (several trigger rows can share one;
         see prospective_shadow_report.intentions). Takes the most recent 30
         would-fires (verdict "fire") and up to 30 held items (verdict in
         already_resolved/not_now/cancel), shuffles them with a seeded RNG
         recorded in the answer key, and renumbers ids AFTER the shuffle
         (q001..) so id order, id format, and context length leak nothing
         about which class a row belongs to. Writes into --out-dir (0700):
           rating_sheet.csv  id, context (last 6 turns before the item's
                             timestamp, text only), reminder, answer (blank)
           answer_key.json   {id: "fire" | "hold"} -- private; never opened
                             while rating, never shipped inside the sheet
           README.md         the question and the scoring command
         Refuses (exit 2, writes nothing) when: --out-dir already exists, is
         a symlink, or resolves inside this git repo tree (so the sheet --
         real conversation lines -- can never be `git add`ed); the log or
         memory.db is missing; the window holds a malformed shadow log line;
         or fewer than 30 would-fires survive dedup in the window.
  score  precision = yes / answered among would-fires; pass iff precision
         >= 0.8 on >= 30 answered would-fires (exit 2 otherwise, printing the
         result first). An answer that is not exactly "yes" or "no"
         (case-insensitive, trimmed) counts as UNANSWERED, never as "no". The
         held rows' yes-rate is reported too, to expose a rater who says yes
         to everything -- it plays no part in pass/fail.

The sheet holds real conversation lines. It is written 0700/0600 and never
leaves this machine: no cloud call, no network call, anywhere in this script.
"""
import argparse
import csv
import json
import os
import random
import sqlite3
import sys
from datetime import datetime, timezone
from io import StringIO

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "blind_ab"))

import curator_names as cn  # noqa: E402
import prospective_shadow_report as psr  # noqa: E402
from make_rating_sheet import redact  # noqa: E402 -- same email/phone/name scrub as the rest of blind_ab

HOME = os.path.expanduser("~")
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MIN_FIRE = 30
PRECISION_BAR = 0.8
HOLD_VERDICTS = ("already_resolved", "not_now", "cancel")
SHEET_COLUMNS = ["id", "context", "reminder", "answer"]
QUESTION = ("Seth is about to reply to this conversation. Would it be natural and welcome "
            "for him to bring up the reminder now? Answer yes or no.")


def refuse(msg):
    print(f"refusing: {msg}; nothing written", file=sys.stderr)
    return 2


def _is_in_repo_tree(path):
    """True if `path` resolves (symlinks included) inside this checkout --
    the sheet holds real conversation lines and must never be `git add`able."""
    try:
        real = os.path.realpath(path)
        real_root = os.path.realpath(REPO_ROOT)
    except OSError:
        return False
    return real == real_root or real.startswith(real_root + os.sep)


def unsafe_out_dir_reason(out_dir):
    """-> a refusal reason string, or None if --out-dir is safe to create."""
    if os.path.islink(out_dir):
        return f"{out_dir} is a symlink"
    if os.path.exists(out_dir):
        return f"{out_dir} already exists"
    if _is_in_repo_tree(out_dir):
        return f"{out_dir} resolves inside the git repo tree ({REPO_ROOT}) -- would be git-addable"
    return None


def context(con, contact, ts, n=6):
    """Last `n` messages for `contact` at or before `ts` (epoch, UTC), oldest
    first, text only. created_at is stored as UTC text 'YYYY-MM-DD HH:MM:SS'
    (see prospective_shadow_report.inbound_by_contact for the same
    epoch<->text convention)."""
    cutoff = datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    rows = con.execute(
        "SELECT role, content FROM messages WHERE session_id=? AND created_at <= ? "
        "ORDER BY created_at DESC, id DESC LIMIT ?", (contact, cutoff, n)).fetchall()
    return "\n".join(f"{'them' if role == 'user' else 'me'}: {content}"
                     for role, content in reversed(rows))


def collapse_to_intentions(rows, meta):
    """rows: psr.read_log's row list. meta: {id: (contact, action)} from
    psr.intentions(). -> {(contact, action): (ts, verdict, id)}, collapsed in
    two steps so an intention that would-fires on several passes -- or whose
    several keyword trigger rows share one (contact, action) -- is ONE row:

      1. per DB id, the most recent "item" line wins (later lines overwrite);
      2. per (contact, action) intention, the most recent id (by that id's
         own most-recent ts) wins.
    """
    by_id = {}
    for r in rows:
        if r["kind"] != "item":
            continue
        kv = r["kv"]
        iid = int(kv["id"])
        if iid not in meta:
            continue  # item line for a row psr.intentions() doesn't know -- skip, don't guess
        by_id[iid] = (r["ts"], kv["verdict"])  # later lines overwrite: latest wins

    by_intention = {}
    for iid, (ts, verdict) in by_id.items():
        key = meta[iid]
        prev = by_intention.get(key)
        if prev is None or ts > prev[0]:
            by_intention[key] = (ts, verdict, iid)
    return by_intention


def build(a):
    reason = unsafe_out_dir_reason(a.out_dir)
    if reason:
        return refuse(reason)
    if not os.path.isfile(a.log):
        return refuse(f"no log at {a.log}")
    if not os.path.isfile(a.memory_db):
        return refuse(f"no database at {a.memory_db}")

    since, until = psr.parse_day(a.since), psr.parse_day(a.until)
    rows, malformed = psr.read_log(a.log, since, until)
    if malformed:
        return refuse(f"{malformed} malformed shadow log line(s) in the window")

    con = sqlite3.connect(f"file:{a.memory_db}?mode=ro", uri=True)
    try:
        meta = psr.intentions(con)
        by_intention = collapse_to_intentions(rows, meta)

        fire, hold = [], []
        for key, (ts, verdict, iid) in by_intention.items():
            if verdict == "fire":
                fire.append((ts, iid, key))
            elif verdict in HOLD_VERDICTS:
                hold.append((ts, iid, key))
            # parse_fail / judge_err: not a real decision to rate -- excluded

        if len(fire) < MIN_FIRE:
            return refuse(f"only {len(fire)} would-fire intention(s) in the window "
                          f"(need {MIN_FIRE})")
        fire.sort(key=lambda t: -t[0])
        hold.sort(key=lambda t: -t[0])
        fire, hold = fire[:MIN_FIRE], hold[:MIN_FIRE]  # most recent MIN_FIRE of each

        sheet_rows, key = [], {}
        for label, group in (("fire", fire), ("hold", hold)):
            for ts, iid, (contact, action) in group:
                rid = f"r{len(sheet_rows) + 1:03d}"  # placeholder, renumbered below
                # Aliasing (privacy ruling): the sheet's columns are fixed to
                # id/context/reminder/answer -- no "contact" column exists to leak a
                # raw handle through. redact() is the same email/phone/name scrub
                # blind_ab's sheets use, applied here in case the contact's own
                # number, email, or name appears INSIDE the conversation text itself.
                sheet_rows.append({"id": rid, "context": redact(context(con, contact, ts)),
                                   "reminder": redact(action), "answer": ""})
                key[rid] = label
    finally:
        con.close()

    rng = random.Random(a.seed)
    rng.shuffle(sheet_rows)
    renumbered_key = {}
    for n, row in enumerate(sheet_rows, 1):  # renumber AFTER the shuffle: id order, id
        old = row["id"]                      # format and position leak nothing about class
        row["id"] = f"q{n:03d}"
        renumbered_key[row["id"]] = key[old]
    key = renumbered_key

    os.makedirs(a.out_dir, mode=0o700)
    os.chmod(a.out_dir, 0o700)  # belt-and-suspenders against a umask that stripped bits

    buf = StringIO()
    w = csv.DictWriter(buf, fieldnames=SHEET_COLUMNS)
    w.writeheader()
    w.writerows(sheet_rows)
    cn.write_text_private(os.path.join(a.out_dir, "rating_sheet.csv"), buf.getvalue())
    # answer_key.json is a pure {id: "fire"|"hold"} map -- scorers and raters treat it
    # as exactly that shape. The shuffle seed is recorded in README.md instead (also
    # private, also never shipped in the rating sheet itself) so the shuffle stays
    # reproducible without adding a non-id/non-label value to this file.
    cn.write_text_private(os.path.join(a.out_dir, "answer_key.json"),
                          json.dumps(key, indent=2, sort_keys=True))
    readme = f"""# Prospective reminder spot check -- {a.since} to {a.until}

{len(sheet_rows)} rows ({len(fire)} SHADOW would-fires, {len(hold)} held items --
already_resolved/not_now/cancel -- mixed in blind). Each row is a real `context`
(the last few turns before the reminder would have fired) and the candidate
`reminder` text. Shuffled with seed {a.seed!r} (reproducible from the same log +
memory.db window).

> {QUESTION}

Fill the `answer` column of rating_sheet.csv with exactly `yes` or `no` per row.
Do not open `answer_key.json` until you are done -- it holds which rows are the
real would-fires.

Score (from the repo root):
```
python3 scripts/prospective_spot_check.py score --out-dir {a.out_dir}
```
precision = yes / answered among the would-fire rows; the spec §3 promotion bar
is precision >= {PRECISION_BAR} on >= {MIN_FIRE} answered would-fires. The held
rows' yes-rate is reported alongside but does not gate pass/fail -- it is there
to flag a rater who answered yes to everything.
"""
    cn.write_text_private(os.path.join(a.out_dir, "README.md"), readme)
    print(f"wrote {len(sheet_rows)} rows ({len(fire)} fire, {len(hold)} hold) to {a.out_dir}")
    return 0


def score_sheet(out_dir):
    with open(os.path.join(out_dir, "answer_key.json")) as f:
        key = {k: v for k, v in json.load(f).items() if not k.startswith("_")}
    res = {"fire_rated": 0, "fire_yes": 0, "hold_rated": 0, "hold_yes": 0}
    with open(os.path.join(out_dir, "rating_sheet.csv"), newline="") as f:
        for row in csv.DictReader(f):
            label = key.get(row["id"])
            if label is None:
                continue
            ans = (row.get("answer") or "").strip().lower()
            if ans not in ("yes", "no"):
                continue  # anything else (blank, "maybe", typo) is UNANSWERED, never "no"
            res[f"{label}_rated"] += 1
            if ans == "yes":
                res[f"{label}_yes"] += 1
    res["precision"] = (res["fire_yes"] / res["fire_rated"]) if res["fire_rated"] else None
    res["hold_yes_rate"] = (res["hold_yes"] / res["hold_rated"]) if res["hold_rated"] else None
    res["pass"] = bool(res["fire_rated"] >= MIN_FIRE and res["precision"] is not None and
                       res["precision"] >= PRECISION_BAR)
    return res


def score(a):
    res = score_sheet(a.out_dir)
    print(json.dumps(res))
    if res["fire_rated"] < MIN_FIRE:
        return refuse(f"only {res['fire_rated']} would-fires answered (need {MIN_FIRE})")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build")
    b.add_argument("--log", default=os.path.join(HOME, ".human/logs/service-loop-error.log"))
    b.add_argument("--memory-db", default=os.path.join(HOME, ".human/memory.db"))
    b.add_argument("--since", required=True, help="YYYY-MM-DD (UTC midnight), inclusive")
    b.add_argument("--until", required=True, help="YYYY-MM-DD (UTC midnight), exclusive")
    b.add_argument("--out-dir", required=True)
    b.add_argument("--seed", type=int, default=None)

    s = sub.add_parser("score")
    s.add_argument("--out-dir", required=True)

    a = ap.parse_args(argv)
    if a.cmd == "build":
        return build(a)
    return score(a)


if __name__ == "__main__":
    sys.exit(main())
