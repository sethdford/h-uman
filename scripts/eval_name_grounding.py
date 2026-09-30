#!/usr/bin/env python3
"""Name-grounding E2E measurement (spec 2026-09-29 named-entity-extraction §2, §4.7).

For the last 40 inbound 1:1 texts (14 days, <= 8 per contact), run the REAL
composition -- `human memory ground --full <contact> <text>`, i.e.
hu_graph_ground_compose_turn -- under HU_GRAPH_NAMES=off and =live, both against
the SAME private 0600 copy of graph.db (sqlite3 backup API from a read-only
connection; deleted afterwards, also on failure). Per mode it counts: non-empty
blocks, blocks naming >= 1 typed entity, distinct typed names, bytes. Counts only:
no names, handles or message text reach the output file, stdout or stderr. Each
text goes to the probe as one argv element (no shell).

typed_name_blocks is the probe header's `names=` field (> 0), which the C side
counts over the CONTACT block only; the owner "About you:" block never counts
(an owner fact is not the contact's name). distinct_typed_names reads the block
with the same rule and must agree with the header, or the run refuses.

Refuses (exit 2, writes nothing) when: the human binary is missing or not
executable; chat.db or config.json can't be read; fewer than --n moments exist;
the graph copy fails or has no entities table; ANY probe exits non-zero, times
out, or prints a header/block that doesn't match the `--full` contract. A partial
run is not a measurement.

Scope: this compares off vs live on one graph snapshot. It does NOT prove that
`off` is byte-identical to the pre-change code -- the C golden test pins that.
HU_GRAPH_GROUNDING is set and recorded for the prod picture, but the CLI probe
composes regardless of it (it is the agent loader's injection gate). It counts
composition before the reply-tier gate: prod injects grounding only on some turns,
so N blocks naming someone is not N replies that saw a name. The probe's memory
store is pointed at the scratch dir via HU_MEMORY_SQLITE_PATH, which isolates the
sqlite memory backend only (prod's backend); grounding itself reads only the graph.

The flip gate. `live typed_name_blocks >= 15` alone does not isolate
HU_GRAPH_NAMES: OFF renders the same type suffixes, and with the contact fallback
live a typed top entity fills every lexical miss in both modes. Worse, LIVE can
swap a message-relevant lexical match (a topic) for an unrelated name and still
score a "win". So the moments are compared PAIRED (off[i] vs live[i], same text):

  typed_gained  off names == 0 and live names > 0   (the reader surfaced a name)
  typed_lost    off names > 0 and live names == 0   (a bug: should be 0 by design)
  lexical_lost  off matched > 0 and live matched == 0 (relevance traded for a name)

flip_gate_met is true iff every clause in FLIP_CLAUSES holds: the prod run
parameters (n 40, 14 days, <= 8 per contact) and prod gates, live >= 15/40,
typed_lost == 0, lexical_lost == 0, typed_gained >= 1, and -- spec §2, "rises
from the measured baseline" -- live > the --baseline run's live. Without
--baseline that last clause fails: an unmeasured baseline never passes.
flip_blockers lists the failed clause names. A --baseline file that can't be read
or was produced under different n/days/per_contact/gates is a refusal.
"""
import argparse
import collections
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
import curator_names as cn  # noqa: E402  (0600 writer)
import curator_population as cp  # noqa: E402  (handle validity / normalization / loopback)

HOME = os.path.expanduser("~")
HUMAN_CONFIG = os.path.join(HOME, ".human/config.json")
REACTIONS = range(2000, 4000)
OWNER_LABEL = "About you:"
# Same rule as hu_graph_ground_count_typed_names: a column-0 "- " line ending in a
# name type. Neighbor lines are indented, so they never match.
TYPED_LINE = re.compile(r"^- (.+) \((person|place|organization|event)\)$")
HEADER = re.compile(r"^matched=(\d+) bytes=(\d+) fallback=([01]) self=[01] names=(\d+)$")
MODES = ("off", "live")
TARGET = 15
TARGET_N = 40
PROD_DAYS = 14
PROD_PER_CONTACT = 8
PROD_GATES = {"HU_GRAPH_GROUNDING": "live", "HU_GRAPH_GROUNDING_CONTACT_FALLBACK": "live",
              "HU_GRAPH_GROUNDING_SELF_FACTS": "live"}
# Every clause must hold for flip_gate_met; the failed ones are the flip_blockers.
FLIP_CLAUSES = ("prod_parameters", "prod_gates", "live_typed_target_met", "no_typed_lost",
                "no_lexical_lost", "typed_gained", "rises_from_baseline")

# One `ground --full` answer: header fields plus the block (the block stays in memory).
Probe = collections.namedtuple("Probe", "bytes names matched fallback block")


def _now():
    return dt.datetime.now(dt.timezone.utc)


def load_inbound(chat_db, since):
    """chat.db 1:1 rows per handle, read-only -- the conversation-quality metric's reader
    (group chats are already excluded there: cache_roomnames is empty)."""
    import eval_conversation_quality as cq
    return cq._load_messages(chat_db, since)


def argv_safe(text):
    """Can `text` travel as one argv element? Not with a NUL (C strings end there, and
    subprocess raises ValueError), nor with a lone surrogate the filesystem encoding
    can't carry."""
    if "\x00" in text:
        return False
    try:
        os.fsencode(text)
    except UnicodeError:
        return False
    return True


def sample_moments(per_contact, now, n=TARGET_N, days=14, per_contact_cap=8, exclude=()):
    """[(handle, text)] newest first: inbound, non-reaction, non-empty texts from valid
    1:1 handles within `days`, at most `per_contact_cap` per handle, at most `n`.
    `exclude` (the daemon's loopback/self handle) matches after normalization.
    A text argv can't carry (argv_safe) is skipped BEFORE the cap, so the next eligible
    moment takes its place: the sample still reaches `n` when enough exist, and the
    daemon never grounds such a text as one C string anyway.
    Ties break on (handle, text), so the order is deterministic."""
    cutoff = now - dt.timedelta(days=days)
    excluded = cp.normalize_all(exclude)
    pool = []
    for h, msgs in per_contact.items():
        if not cp.is_valid_handle(h) or cp.normalize_handle(h) in excluded:
            continue
        mine = [(m["t"], (m.get("text") or "").strip()) for m in msgs
                if not m["from_me"] and m["t"] >= cutoff
                and (m.get("atype") or 0) not in REACTIONS and (m.get("text") or "").strip()
                and argv_safe((m.get("text") or "").strip())]
        mine.sort(key=lambda x: (-x[0].timestamp(), x[1]))
        pool.extend((t, h, text) for t, text in mine[:per_contact_cap])
    pool.sort(key=lambda x: (-x[0].timestamp(), x[1], x[2]))
    return [(h, text) for _, h, text in pool[:n]]


def typed_names(block):
    """Typed names in the CONTACT block: stops at the owner "About you:" line."""
    out = []
    for ln in (block or "").split("\n"):  # the C side splits on '\n' only
        if ln == OWNER_LABEL:
            break
        m = TYPED_LINE.match(ln)
        if m:
            out.append(m.group(1))
    return out


def parse_probe(stdout):
    """`ground --full` stdout -> Probe(bytes, names, matched, fallback, block) or None.
    The CLI prints the header, then the block plus one newline when bytes > 0;
    anything else (a plain `ground` header, a "Memory backend: none" line, a
    truncated block) is None."""
    head, sep, block = (stdout or "").partition("\n")
    m = HEADER.match(head)
    if not m or not sep:
        return None
    nbytes = int(m.group(2))
    expect = nbytes + 1 if nbytes else 0
    if len(block.encode("utf-8", "surrogateescape")) != expect:
        return None
    return Probe(nbytes, int(m.group(4)), int(m.group(1)), int(m.group(3)), block)


def probe(human_bin, graph_copy, mode, contact, text, gates, scratch, timeout=60):
    """One `ground --full` call -> Probe or None on any failure."""
    env = {**os.environ, **gates, "HU_GRAPH_DB": graph_copy, "HU_GRAPH_NAMES": mode,
           # grounding reads only the graph; keep the probe off the live memory.db
           "HU_MEMORY_SQLITE_PATH": os.path.join(scratch, "memory.db")}
    try:
        r = subprocess.run([human_bin, "memory", "ground", "--full", contact, text], env=env,
                           capture_output=True, timeout=timeout)
    except (OSError, ValueError, subprocess.TimeoutExpired):  # ValueError: a NUL in argv
        return None  # (sample_moments skips those; this is the backstop -> clean refusal)
    if r.returncode != 0:
        return None
    got = parse_probe(r.stdout.decode("utf-8", "surrogateescape"))
    if got is None or len(typed_names(got.block)) != got.names:
        return None  # our reading of the block must agree with the C count
    return got


def summarize(results):
    """[Probe] -> counts. typed_name_blocks trusts the header."""
    distinct, typed, nonempty, total = set(), 0, 0, 0
    for r in results:
        nonempty += 1 if r.bytes > 0 else 0
        typed += 1 if r.names > 0 else 0
        distinct.update(n.casefold() for n in typed_names(r.block))
        total += r.bytes
    return {"nonempty_blocks": nonempty, "typed_name_blocks": typed,
            "distinct_typed_names": len(distinct), "bytes_total": total}


def summarize_paired(off, live):
    """Per-moment off[i] vs live[i] (same text) -> counts. The first three drive the
    flip gate; nonempty_lost and changed_blocks are diagnostics."""
    if len(off) != len(live):
        raise ValueError("off and live results are not paired")
    c = dict.fromkeys(("typed_gained", "typed_lost", "lexical_lost", "both_typed",
                       "neither_typed", "nonempty_lost", "changed_blocks"), 0)
    for o, v in zip(off, live):
        c["typed_gained"] += o.names == 0 and v.names > 0
        c["typed_lost"] += o.names > 0 and v.names == 0
        c["lexical_lost"] += o.matched > 0 and v.matched == 0
        c["both_typed"] += o.names > 0 and v.names > 0
        c["neither_typed"] += o.names == 0 and v.names == 0
        c["nonempty_lost"] += o.bytes > 0 and v.bytes == 0
        c["changed_blocks"] += o.block != v.block
    return {k: int(n) for k, n in c.items()}


def flip_clauses(n, days, per_contact, gates, modes, paired, baseline):
    """-> {clause: bool} in FLIP_CLAUSES order. `baseline` is None or the counts
    read by load_baseline; without one, rises_from_baseline is False."""
    live = modes["live"]["typed_name_blocks"]
    return {
        "prod_parameters": n == TARGET_N and days == PROD_DAYS
        and per_contact == PROD_PER_CONTACT,
        "prod_gates": gates == PROD_GATES,
        "live_typed_target_met": live >= TARGET,
        "no_typed_lost": paired["typed_lost"] == 0,
        "no_lexical_lost": paired["lexical_lost"] == 0,
        "typed_gained": paired["typed_gained"] >= 1,
        "rises_from_baseline": baseline is not None
        and live > baseline["live_typed_name_blocks"],
    }


def _count(v):
    if isinstance(v, bool) or not isinstance(v, int) or v < 0:
        raise ValueError("not a count")
    return v


def load_baseline(path, n, days, per_contact, gates):
    """An earlier result file -> {"live_typed_name_blocks", "off_typed_name_blocks"}.
    Raises ValueError/OSError when it is unreadable, not a result, or was produced
    under different n/days/per_contact/gates (then the two runs are not comparable)."""
    with open(path) as f:
        b = json.load(f)
    if not isinstance(b, dict) or not isinstance(b.get("modes"), dict):
        raise ValueError("not a name-grounding result")
    for key, want in (("n", n), ("days", days), ("per_contact_cap", per_contact),
                      ("gates", gates)):
        if b.get(key) != want:
            raise ValueError(f"baseline {key} differs from this run")
    try:
        return {f"{m}_typed_name_blocks": _count(b["modes"][m]["typed_name_blocks"])
                for m in ("live", "off")}
    except (KeyError, TypeError) as e:
        raise ValueError("baseline has no typed_name_blocks") from e


def private_copy(src, scratch):
    """Consistent 0600 copy of `src` in `scratch` via the online backup API from a
    read-only connection (includes WAL content; never writes the source). Raises
    sqlite3.Error when the source is missing, unreadable or has no entities table."""
    if not os.path.isfile(src):
        raise sqlite3.OperationalError(f"no graph at {src}")
    path = os.path.join(scratch, "graph.db")
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    os.close(fd)
    s = sqlite3.connect(f"file:{src}?mode=ro", uri=True)
    try:
        d = sqlite3.connect(path)
        try:
            s.backup(d)
            ok = d.execute("select 1 from sqlite_master where type='table' "
                           "and name='entities'").fetchone()
        finally:
            d.close()
    finally:
        s.close()
    if not ok:
        raise sqlite3.DatabaseError("graph copy has no entities table")
    os.chmod(path, 0o600)
    return path


def refuse(msg):
    print(f"refusing: {msg}; nothing written", file=sys.stderr)
    return 2


def _positive(v):
    n = int(v)
    if n < 1:
        raise argparse.ArgumentTypeError("must be >= 1")
    return n


def run_modes(a, moments, gates, scratch):
    """-> {mode: [Probe]} (index-aligned: off[i] and live[i] are the same text), or an
    error string on the first failure.
    Error strings carry the mode and index only, never a handle or a text."""
    copy = private_copy(a.graph_db, scratch)
    results = {mode: [] for mode in MODES}
    for i, (contact, text) in enumerate(moments):
        for mode in MODES:
            r = probe(a.human_bin, copy, mode, contact, text, gates, scratch, a.timeout)
            if r is None:
                return f"{mode} probe {i + 1}/{len(moments)} failed or broke the --full contract"
            results[mode].append(r)
    return results


def main(argv=None):
    ap = argparse.ArgumentParser(description="Name-grounding E2E measurement (off vs live).")
    ap.add_argument("--chat-db", default=os.path.join(HOME, "Library/Messages/chat.db"))
    ap.add_argument("--graph-db", default=os.path.join(HOME, ".human/graph.db"))
    ap.add_argument("--human-bin", default=os.path.join(HOME, ".local/bin/human-daemon"))
    ap.add_argument("--out-dir", default=os.path.join(HOME, ".human/logs"))
    ap.add_argument("--n", type=_positive, default=TARGET_N)
    ap.add_argument("--days", type=_positive, default=14)
    ap.add_argument("--per-contact", type=_positive, default=8)
    ap.add_argument("--timeout", type=_positive, default=60, help="seconds per probe")
    ap.add_argument("--baseline", help="an earlier name-grounding-*.json from the same "
                    "n/days/per-contact/gates; required for flip_gate_met (spec §2)")
    for flag, what in (("--grounding", "HU_GRAPH_GROUNDING"),
                       ("--fallback", "HU_GRAPH_GROUNDING_CONTACT_FALLBACK"),
                       ("--self-facts", "HU_GRAPH_GROUNDING_SELF_FACTS")):
        ap.add_argument(flag, choices=["off", "shadow", "live"], default="live",
                        help=f"{what} for the probe (prod: live)")
    a = ap.parse_args(argv)
    if not (os.path.isfile(a.human_bin) and os.access(a.human_bin, os.X_OK)):
        return refuse(f"human binary not executable ({a.human_bin})")
    now = _now()
    try:
        per_contact = load_inbound(a.chat_db, now - dt.timedelta(days=a.days))
        loopback = cp.load_loopback_handles(HUMAN_CONFIG)
    except (sqlite3.Error, OSError, ValueError) as e:
        return refuse(f"cannot read chat.db or config.json ({type(e).__name__})")
    moments = sample_moments(per_contact, now, a.n, a.days, a.per_contact, loopback)
    if len(moments) < a.n:
        return refuse(f"{len(moments)} moments < {a.n}")
    gates = {"HU_GRAPH_GROUNDING": a.grounding,
             "HU_GRAPH_GROUNDING_CONTACT_FALLBACK": a.fallback,
             "HU_GRAPH_GROUNDING_SELF_FACTS": a.self_facts}
    baseline = None
    if a.baseline:  # checked before any probe: a bad baseline wastes no run
        try:
            baseline = load_baseline(a.baseline, a.n, a.days, a.per_contact, gates)
        except (OSError, ValueError) as e:
            return refuse(f"baseline unusable ({e})")
    scratch = tempfile.mkdtemp(prefix="name-grounding-")  # 0700; holds the copy + its WAL
    try:
        results = run_modes(a, moments, gates, scratch)
    except (sqlite3.Error, OSError) as e:
        return refuse(f"graph copy failed ({e})")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    if isinstance(results, str):
        return refuse(results)
    modes = {m: summarize(results[m]) for m in MODES}
    paired = summarize_paired(results["off"], results["live"])
    clauses = flip_clauses(len(moments), a.days, a.per_contact, gates, modes, paired, baseline)
    result = {"measured_at": now.strftime("%Y-%m-%dT%H:%M:%SZ"), "n": len(moments),
              "days": a.days, "per_contact_cap": a.per_contact, "gates": gates, "modes": modes,
              "paired": paired, "baseline": baseline,
              "target_n": TARGET_N, "target_live_typed_name_blocks": TARGET,
              "flip_clauses": clauses,
              "flip_blockers": [c for c in FLIP_CLAUSES if not clauses[c]],
              "flip_gate_met": all(clauses.values())}
    path = os.path.join(a.out_dir, f"name-grounding-{now.strftime('%Y%m%d-%H%M%S')}.json")
    try:
        cn.write_jsonl_private(path, [result])  # one JSON object, 0600 from creation
    except OSError as e:
        return refuse(f"cannot write {path} ({e})")
    print(f"live typed_name_blocks={modes['live']['typed_name_blocks']}/{len(moments)} "
          f"off={modes['off']['typed_name_blocks']}/{len(moments)} "
          f"flip_gate_met={result['flip_gate_met']} "
          f"blockers={','.join(result['flip_blockers']) or '-'}")
    print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
