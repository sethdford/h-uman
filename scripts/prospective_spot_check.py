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
         already_resolved/not_now/cancel), redacts context/reminder text with
         blind_ab's email/phone/contact-name scrub (name list resolved from the
         local macOS AddressBook, never over the network), shuffles the rows
         with a seeded RNG, and renumbers ids AFTER the shuffle (q001..) so id
         order, id format, and context length leak nothing about which class
         a row belongs to. Writes into --out-dir (0700):
           rating_sheet.csv  id, context (last 6 turns before the item's
                             timestamp, text only), reminder, answer (blank)
           answer_key.json   {id: "fire" | "hold"}, plus "_seed"/"_fire_count"/
                             "_hold_count" metadata -- private; never opened
                             while rating, never shipped inside the sheet. The
                             seed and the per-class counts live ONLY here:
                             either one alongside the rater-facing README
                             would let a rater replay the Fisher-Yates shuffle
                             and deanonymize every row (see the comment at the
                             write site).
           README.md         the question, the total row count, and the
                             scoring command -- no seed, no fire/hold split
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
import secrets
import sqlite3
import sys
from datetime import datetime, timezone
from io import StringIO

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "blind_ab"))

import curator_names as cn  # noqa: E402
import prospective_shadow_report as psr  # noqa: E402
# Same email/phone/name scrub -- and the same local-AddressBook-only, no-network
# resolver -- as the rest of blind_ab (make_rating_sheet.main calls both the same
# way; see resolve_contact_name_tokens' own docstring for the hermetic escape hatch).
from make_rating_sheet import redact, resolve_contact_name_tokens  # noqa: E402

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

    # Resolved once per build, at CLI-invocation time -- never from a test, per
    # resolve_contact_name_tokens' own contract. Every redact() call below MUST
    # receive it: omitting it is the privacy regression this dispatch fixed
    # (a seeded "call John Smith back" surviving verbatim into the sheet).
    name_tokens = resolve_contact_name_tokens()

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
                sheet_rows.append({"id": rid,
                                   "context": redact(context(con, contact, ts), name_tokens),
                                   "reminder": redact(action, name_tokens), "answer": ""})
                key[rid] = label
    finally:
        con.close()

    if a.seed is None:  # still replayable: the drawn seed is recorded in the private key
        a.seed = secrets.randbits(63)
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
    # answer_key.json's non-underscore entries are a pure {id: "fire"|"hold"} map
    # (score_sheet strips "_"-prefixed keys before reading it as that map).
    #
    # BLINDNESS: the shuffle seed and the per-class counts live ONLY here, never in
    # README.md. The public build() code always lays fire rows before hold rows
    # pre-shuffle (see the loop above), and Fisher-Yates' permutation is a pure
    # function of (seed, length) -- random.Random(seed).shuffle(list(range(n))). A
    # rater who has the seed AND the fire/hold split can replay that permutation and
    # invert it to recover which pre-shuffle slot (fire-block or hold-block) every
    # shuffled row came from, deanonymizing the whole sheet without ever opening this
    # file. Keeping both facts out of every rater-facing artifact (README.md, the
    # CSV itself) is the fix; this file is 0600 and the rater is told not to open it.
    key_out = dict(key)
    key_out["_seed"] = a.seed
    key_out["_fire_count"] = len(fire)
    key_out["_hold_count"] = len(hold)
    cn.write_text_private(os.path.join(a.out_dir, "answer_key.json"),
                          json.dumps(key_out, indent=2, sort_keys=True))
    readme = f"""# Prospective reminder spot check -- {a.since} to {a.until}

{len(sheet_rows)} rows, shuffled. Each row is a real `context` (the last few
turns before the reminder would have fired) and the candidate `reminder` text.
The shuffle seed and the fire/hold split are recorded ONLY in answer_key.json
(private) -- never here, and never derivable from this file.

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
