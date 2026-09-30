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
composes regardless of it (it is the agent loader's injection gate).
Gate (spec §2): live typed_name_blocks >= 15 of exactly 40 -> live_gate_met.
"""
import argparse
import datetime as dt
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
HEADER = re.compile(r"^matched=(\d+) bytes=(\d+) fallback=[01] self=[01] names=(\d+)$")
MODES = ("off", "live")
TARGET = 15
TARGET_N = 40


def _now():
    return dt.datetime.now(dt.timezone.utc)


def load_inbound(chat_db, since):
    """chat.db 1:1 rows per handle, read-only -- the conversation-quality metric's reader
    (group chats are already excluded there: cache_roomnames is empty)."""
    import eval_conversation_quality as cq
    return cq._load_messages(chat_db, since)


def sample_moments(per_contact, now, n=TARGET_N, days=14, per_contact_cap=8, exclude=()):
    """[(handle, text)] newest first: inbound, non-reaction, non-empty texts from valid
    1:1 handles within `days`, at most `per_contact_cap` per handle, at most `n`.
    `exclude` (the daemon's loopback/self handle) matches after normalization.
    Ties break on (handle, text), so the order is deterministic."""
    cutoff = now - dt.timedelta(days=days)
    excluded = cp.normalize_all(exclude)
    pool = []
    for h, msgs in per_contact.items():
        if not cp.is_valid_handle(h) or cp.normalize_handle(h) in excluded:
            continue
        mine = [(m["t"], (m.get("text") or "").strip()) for m in msgs
                if not m["from_me"] and m["t"] >= cutoff
                and (m.get("atype") or 0) not in REACTIONS and (m.get("text") or "").strip()]
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
    """`ground --full` stdout -> (bytes, names, block) or None. The CLI prints the
    header, then the block plus one newline when bytes > 0; anything else (a plain
    `ground` header, a "Memory backend: none" line, a truncated block) is None."""
    head, sep, block = (stdout or "").partition("\n")
    m = HEADER.match(head)
    if not m or not sep:
        return None
    nbytes, names = int(m.group(2)), int(m.group(3))
    expect = nbytes + 1 if nbytes else 0
    if len(block.encode("utf-8", "surrogateescape")) != expect:
        return None
    return nbytes, names, block


def probe(human_bin, graph_copy, mode, contact, text, gates, scratch, timeout=60):
    """One `ground --full` call -> (bytes, names, block) or None on any failure."""
    env = {**os.environ, **gates, "HU_GRAPH_DB": graph_copy, "HU_GRAPH_NAMES": mode,
           # grounding reads only the graph; keep the probe off the live memory.db
           "HU_MEMORY_SQLITE_PATH": os.path.join(scratch, "memory.db")}
    try:
        r = subprocess.run([human_bin, "memory", "ground", "--full", contact, text], env=env,
                           capture_output=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if r.returncode != 0:
        return None
    got = parse_probe(r.stdout.decode("utf-8", "surrogateescape"))
    if got is None or len(typed_names(got[2])) != got[1]:
        return None  # our reading of the block must agree with the C count
    return got


def summarize(results):
    """[(bytes, names, block)] -> counts. typed_name_blocks trusts the header."""
    distinct, typed, nonempty, total = set(), 0, 0, 0
    for nbytes, names, block in results:
        nonempty += 1 if nbytes > 0 else 0
        typed += 1 if names > 0 else 0
        distinct.update(n.casefold() for n in typed_names(block))
        total += nbytes
    return {"nonempty_blocks": nonempty, "typed_name_blocks": typed,
            "distinct_typed_names": len(distinct), "bytes_total": total}


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
    """-> {mode: [(bytes, names, block)]}, or an error string on the first failure.
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
    result = {"measured_at": now.strftime("%Y-%m-%dT%H:%M:%SZ"), "n": len(moments),
              "days": a.days, "per_contact_cap": a.per_contact, "gates": gates, "modes": modes,
              "target_n": TARGET_N, "target_live_typed_name_blocks": TARGET,
              "live_gate_met": len(moments) == TARGET_N
              and modes["live"]["typed_name_blocks"] >= TARGET}
    path = os.path.join(a.out_dir, f"name-grounding-{now.strftime('%Y%m%d-%H%M%S')}.json")
    try:
        cn.write_jsonl_private(path, [result])  # one JSON object, 0600 from creation
    except OSError as e:
        return refuse(f"cannot write {path} ({e})")
    print(f"live typed_name_blocks={modes['live']['typed_name_blocks']}/{len(moments)} "
          f"off={modes['off']['typed_name_blocks']}/{len(moments)} "
          f"live_gate_met={result['live_gate_met']}")
    print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
