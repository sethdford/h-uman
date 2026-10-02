#!/usr/bin/env python3
"""Drive the real-turn replay harness: snapshot the state, run every arm.

  snapshot  copy the daemon's state into the run dir, read-only from the
            source: config.json (cloud API keys dropped), personas/,
            contacts/, the small top-level *.json files, and memory.db /
            graph.db / cognition.db: each db and its WAL are byte-copied and
            the copy is checked and backed up — the source is never opened
            (even a mode=ro connection creates -shm/-wal files next to it).
  run       turn by turn, oldest first: clone the snapshot (copy-on-write),
            delete every row written after the turn (TIME_FILTERS), then for
            each arm clone that again and run one `human replay` process on
            it — so every arm sees the same pristine, time-cut state for every
            turn and no turn sees another's writes. env = base env + the arm's
            overrides, with HOME, HU_STATE_DIR and TMPDIR inside the turn's
            scratch dir; on macOS the process runs under sandbox-exec, which
            denies any write outside that dir and any non-loopback network.

Arms: --arm NAME:ENV=VAL,ENV=VAL (repeatable). `--arm off:` is an arm with no
overrides. The base env is empty (every inherited HU_* var is dropped) unless
--base-env-plist names a launchd plist, whose HU_* EnvironmentVariables then
apply to every arm: replaying "production plus one gate" means passing the
service-loop plist.

Everything is written under the private run dir (0700 dirs, 0600 files).
Model calls go to a loopback endpoint only; the process env also points every
libcurl proxy variable at a dead loopback port.

Runbook: docs/guides/replay-harness.md.
"""
import argparse
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from replay_export_turns import make_private_dir, run_dir_for  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SQLITE_DBS = ("memory.db", "graph.db", "cognition.db")
STATE_DIRS = ("personas", "contacts")
SMALL_JSON_MAX = 1 << 20
DEAD_PROXY = "http://127.0.0.1:9"
LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}


def is_loopback(url):
    try:
        u = urllib.parse.urlsplit(url)
    except ValueError:
        return False
    if u.scheme not in ("http", "https") or u.username or u.password:
        return False
    return (u.hostname or "").lower() in LOOPBACK_HOSTS


def parse_arm(spec):
    """'name:K=V,K=V' -> (name, {K: V}). Raises ValueError."""
    name, sep, rest = spec.partition(":")
    if not sep or not name or not name.replace("-", "").replace("_", "").isalnum():
        raise ValueError(f"bad arm {spec!r}: want NAME:ENV=VAL,ENV=VAL")
    env = {}
    for part in filter(None, rest.split(",")):
        k, eq, v = part.partition("=")
        if not eq or not k.startswith("HU_"):
            raise ValueError(f"bad arm override {part!r}: want HU_NAME=value")
        env[k] = v
    return name, env


def _private_copy(src, dst):
    shutil.copyfile(src, dst)
    os.chmod(dst, 0o600)


def _backup_sqlite(src, dst, attempts=3):
    """A consistent copy of a live SQLite db without touching the source.

    Even a mode=ro connection creates the -shm/-wal sidecars next to a WAL db
    that has none, so the source is never opened: its main file and WAL are
    byte-copied into a scratch dir beside dst, the scratch copy is checked
    (PRAGMA quick_check) and backed up into dst. A copy torn by a concurrent
    checkpoint fails the check and is retried."""
    scratch = dst + ".scratch"
    for _ in range(attempts):
        shutil.rmtree(scratch, ignore_errors=True)
        make_private_dir(scratch)
        tmp = os.path.join(scratch, "db")
        shutil.copyfile(src, tmp)
        if os.path.exists(src + "-wal"):
            shutil.copyfile(src + "-wal", tmp + "-wal")
        try:
            s = sqlite3.connect(tmp)
            try:
                if s.execute("PRAGMA quick_check").fetchone()[0] != "ok":
                    continue
                fd = os.open(dst, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
                os.close(fd)
                d = sqlite3.connect(dst)
                try:
                    s.backup(d)
                finally:
                    d.close()
            finally:
                s.close()
            os.chmod(dst, 0o600)
            return
        except sqlite3.DatabaseError:
            continue
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
    raise RuntimeError(f"could not take a consistent copy of {os.path.basename(src)}")


def _sanitized_config(src_path):
    """config.json with api_key removed from every provider whose base_url is
    not loopback: the replay never talks to them, so the copy never holds them."""
    with open(src_path) as f:
        cfg = json.load(f)
    provs = cfg.get("providers")
    items = provs.values() if isinstance(provs, dict) else (provs or [])
    for p in items:
        if isinstance(p, dict) and not is_loopback(p.get("base_url") or ""):
            p.pop("api_key", None)
    return cfg


def snapshot(state_src, base_dir):
    """Copy the replay's state from state_src into base_dir. Read-only on src."""
    make_private_dir(base_dir)
    copied = []
    cfg_src = os.path.join(state_src, "config.json")
    if os.path.isfile(cfg_src):
        dst = os.path.join(base_dir, "config.json")
        fd = os.open(dst, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            json.dump(_sanitized_config(cfg_src), f, indent=2)
        copied.append("config.json")
    for d in STATE_DIRS:
        src = os.path.join(state_src, d)
        if os.path.isdir(src):
            dst = os.path.join(base_dir, d)
            shutil.copytree(src, dst, dirs_exist_ok=True, symlinks=False)
            for root, dirs, files in os.walk(dst):
                os.chmod(root, 0o700)
                for fn in files:
                    os.chmod(os.path.join(root, fn), 0o600)
            copied.append(d + "/")
    for fn in sorted(os.listdir(state_src)):
        p = os.path.join(state_src, fn)
        if (fn.endswith(".json") and fn != "config.json" and os.path.isfile(p)
                and os.path.getsize(p) <= SMALL_JSON_MAX):
            _private_copy(p, os.path.join(base_dir, fn))
            copied.append(fn)
    for db in SQLITE_DBS:
        src = os.path.join(state_src, db)
        if os.path.isfile(src):
            _backup_sqlite(src, os.path.join(base_dir, db))
            copied.append(db)
    if "memory.db" not in copied:
        raise RuntimeError("no memory.db in the state source")
    # Writers that append under the state dir expect these to exist, as they
    # do in ~/.human; without them their output is silently dropped.
    for d in ("training-data", "logs"):
        make_private_dir(os.path.join(base_dir, d))
    return copied


def base_env_from_plist(path):
    out = subprocess.run(["plutil", "-extract", "EnvironmentVariables", "json", "-o", "-", path],
                         capture_output=True, text=True, check=True).stdout
    return {k: v for k, v in json.loads(out).items() if k.startswith("HU_")}


# Rows written after a turn are removed from that turn's copy, so the replay
# only knows what the daemon could have known then. (db, table, column, unit):
# unit "s" / "ms" = unix seconds / milliseconds, "text" = an SQLite datetime.
# ms columns keep rows stamped with uptime (< 1e12): their time is unknown.
TIME_FILTERS = (
    ("memory.db", "memories", "created_at", "text"),
    ("memory.db", "embeddings", "created_at", "text"),
    ("memory.db", "messages", "created_at", "text"),
    ("memory.db", "contact_insights", "created_at_ms", "ms"),
    ("memory.db", "episodes", "created_at", "s"),
    ("memory.db", "commitments", "created_at", "s"),
    ("memory.db", "prospective_memories", "created_at", "s"),
    ("memory.db", "temporal_events", "extracted_at", "s"),
    ("memory.db", "emotional_moments", "created_at", "s"),
    ("memory.db", "inside_jokes", "created_at", "s"),
    ("memory.db", "micro_moments", "created_at", "s"),
    ("memory.db", "mood_log", "set_at", "s"),
    ("memory.db", "contact_mood_log", "created_at", "s"),
    ("memory.db", "growth_milestones", "created_at", "s"),
    ("memory.db", "pattern_observations", "observed_at", "s"),
    ("memory.db", "outbound_sends", "sent_at_ms", "ms"),
    ("graph.db", "entities", "first_seen", "ms"),
    ("graph.db", "relations", "first_seen", "ms"),
    ("graph.db", "community_summaries", "generated_at", "ms"),
    ("graph.db", "negative_memory", "created_at", "ms"),
    ("graph.db", "hyperedges", "created_at", "ms"),
)
UPTIME_STAMP_MAX_MS = 10 ** 12


def filter_to_cutoff(state_dir, cutoff_s):
    """Delete every TIME_FILTERS row newer than cutoff_s in state_dir's dbs.
    Returns {"deleted": n, "absent": [...], "errors": [...]}; an error leaves
    that table unfiltered and is reported, never hidden."""
    report = {"deleted": 0, "absent": [], "errors": []}
    by_db = {}
    for db, table, col, unit in TIME_FILTERS:
        by_db.setdefault(db, []).append((table, col, unit))
    for db, specs in by_db.items():
        path = os.path.join(state_dir, db)
        if not os.path.isfile(path):
            report["absent"].extend(f"{db}:{t}" for t, _, _ in specs)
            continue
        con = sqlite3.connect(path)
        try:
            for table, col, unit in specs:
                cols = [r[0] for r in con.execute(
                    "SELECT name FROM pragma_table_info(?)", (table,))]
                if col not in cols:
                    report["absent"].append(f"{db}:{table}")
                    continue
                if unit == "text":
                    sql = (f'DELETE FROM "{table}" WHERE julianday("{col}") >= '
                           "julianday(?, 'unixepoch')")
                    arg = int(cutoff_s)
                elif unit == "ms":
                    sql = f'DELETE FROM "{table}" WHERE "{col}" >= ? AND "{col}" >= ?'
                    arg = None
                else:
                    sql = f'DELETE FROM "{table}" WHERE "{col}" >= ?'
                    arg = int(cutoff_s)
                try:
                    if unit == "ms":
                        cur = con.execute(sql, (int(cutoff_s * 1000), UPTIME_STAMP_MAX_MS))
                    else:
                        cur = con.execute(sql, (arg,))
                    report["deleted"] += max(cur.rowcount, 0)
                except sqlite3.DatabaseError as e:
                    report["errors"].append(f"{db}:{table}: {type(e).__name__}")
            con.commit()
        finally:
            con.close()
    return report


def clone_tree(src, dst):
    """Copy-on-write clone (APFS clonefile via `cp -c`) when available."""
    if sys.platform == "darwin":
        r = subprocess.run(["cp", "-cR", src, dst], capture_output=True)
        if r.returncode == 0:
            return
        shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst)


SANDBOX_EXEC = "/usr/bin/sandbox-exec"


def sandbox_profile(write_root):
    """macOS sandbox profile: writes only under write_root (and /dev), network
    only to loopback (and unix sockets, e.g. DNS)."""
    root = os.path.realpath(write_root).replace('"', "")
    return ("(version 1)\n(allow default)\n"
            f'(deny file-write* (require-not (require-any (subpath "{root}") (subpath "/dev"))))\n'
            "(deny network-outbound (require-not (require-any (remote ip \"localhost:*\") "
            "(remote unix-socket))))\n")


def want_sandbox(mode):
    if mode == "off":
        return False
    have = os.path.exists(SANDBOX_EXEC)
    if mode == "on" and not have:
        raise RuntimeError("--sandbox on needs macOS sandbox-exec")
    return have


def arm_env(parent, base, overrides, state_dir):
    """The arm's process env: parent minus every HU_* var, plus base, plus the
    arm, plus isolation: HOME, HU_STATE_DIR and TMPDIR all inside the turn's
    scratch dir (so even a $HOME-built path lands there), and every libcurl
    proxy variable at a dead loopback port. Overrides win."""
    env = {k: v for k, v in parent.items() if not k.startswith("HU_")}
    env.update(base)
    env.update(overrides)
    env["HU_STATE_DIR"] = state_dir
    env["HU_MEMORY_SQLITE_PATH"] = os.path.join(state_dir, "memory.db")
    env["HOME"] = os.path.join(state_dir, "home")
    env["TMPDIR"] = os.path.join(state_dir, "tmp")
    for k in ("ALL_PROXY", "all_proxy", "HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy"):
        env[k] = DEAD_PROXY
    env["NO_PROXY"] = env["no_proxy"] = "127.0.0.1,localhost,::1"
    for k in ("HU_IS_TEST", "CI", "HU_CHATDB", "HUMAN_LOG"):
        env.pop(k, None)
    return env


def count_lines(path):
    if not os.path.exists(path):
        return 0
    with open(path) as f:
        return sum(1 for line in f if line.strip())


def load_turns(path, limit):
    with open(path) as f:
        turns = [json.loads(line) for line in f if line.strip()]
    turns.sort(key=lambda t: t["ts"])
    return turns[:limit] if limit else turns


def read_row(path, turn_id):
    """The one result row of a turn run, or None when missing or malformed."""
    try:
        with open(path) as f:
            lines = [line for line in f if line.strip()]
        row = json.loads(lines[0]) if len(lines) == 1 else None
    except (OSError, ValueError):
        return None
    if not isinstance(row, dict) or row.get("id") != turn_id or "action" not in row:
        return None
    return row


def run_turn_arm(a, turn_base, arm_state, turn, name, overrides, base_env, log, sandbox):
    """One turn on one arm, on a fresh clone of the turn's filtered state.
    Returns (row or None, exit code)."""
    clone_tree(turn_base, arm_state)
    os.chmod(arm_state, 0o700)
    for d in ("home", "tmp"):
        make_private_dir(os.path.join(arm_state, d))
    tin = os.path.join(arm_state, "turn.jsonl")
    with open(tin, "w") as f:
        f.write(json.dumps(turn, ensure_ascii=False) + "\n")
    tout = os.path.join(arm_state, "row.jsonl")
    cmd = [a.human, "replay", "--in", tin, "--out", tout, "--arm", name, "--endpoint",
           a.endpoint, "--delay-ms", "0", "--seed", str(a.seed)]
    if a.model:
        cmd += ["--model", a.model]
    if a.temperature is not None:
        cmd += ["--temperature", str(a.temperature)]
    if a.dump_requests:
        make_private_dir(os.path.join(arm_state, "requests"))
        cmd += ["--dump-requests", os.path.join(arm_state, "requests")]
    if sandbox:
        prof = os.path.join(arm_state, "tmp", "replay.sb")
        with open(prof, "w") as f:
            f.write(sandbox_profile(arm_state))
        cmd = [SANDBOX_EXEC, "-f", prof] + cmd
    rc = subprocess.run(cmd, env=arm_env(os.environ, base_env, overrides, arm_state),
                        cwd=arm_state, stdout=log, stderr=subprocess.STDOUT).returncode
    row = read_row(tout, turn["id"])
    if a.dump_requests:
        dump = os.path.join(os.path.dirname(os.path.dirname(arm_state)), "requests")
        make_private_dir(dump)
        src = os.path.join(arm_state, "requests")
        for fn in os.listdir(src):
            shutil.move(os.path.join(src, fn), os.path.join(dump, fn))
    return row, rc


def cmd_snapshot(a):
    run_dir = run_dir_for(a.name, a.run_root)
    if not os.path.isfile(os.path.join(run_dir, "turns.jsonl")):
        print("refusing: run dir has no turns.jsonl (run replay_export_turns.py first)",
              file=sys.stderr)
        return 2
    copied = snapshot(os.path.expanduser(a.state_src), os.path.join(run_dir, "state", "base"))
    print(f"snapshot: {len(copied)} items -> {run_dir}/state/base")
    return 0


def cmd_run(a):
    if not is_loopback(a.endpoint):
        print("refusing: --endpoint must be loopback (the replay is local-only)", file=sys.stderr)
        return 2
    try:
        arms = [parse_arm(s) for s in a.arm]
    except ValueError as e:
        print(f"refusing: {e}", file=sys.stderr)
        return 2
    if len({n for n, _ in arms}) != len(arms) or not arms:
        print("refusing: need at least one arm, with distinct names", file=sys.stderr)
        return 2
    run_dir = run_dir_for(a.name, a.run_root)
    base = os.path.join(run_dir, "state", "base")
    if not os.path.isdir(base):
        print("refusing: no snapshot (run `replay_driver.py snapshot` first)", file=sys.stderr)
        return 2
    sandbox = want_sandbox(a.sandbox)
    base_env = base_env_from_plist(a.base_env_plist) if a.base_env_plist else {}
    turns = load_turns(os.path.join(run_dir, "turns.jsonl"), a.limit)
    out_dir, logs, work = (os.path.join(run_dir, d) for d in ("out", "logs", "work"))
    for d in (out_dir, logs, work):
        make_private_dir(d)
    stats = {n: {"arm": n, "overrides": o, "rows": 0, "expected": len(turns), "errors": 0,
                 "malformed": 0, "failed_exits": 0} for n, o in arms}
    filt = {"deleted": 0, "errors": [], "absent": set()}
    outs = {}
    for n, _ in arms:
        fd = os.open(os.path.join(out_dir, f"{n}.jsonl"), os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
                     0o600)
        outs[n] = os.fdopen(fd, "w")
    log_fd = os.open(os.path.join(logs, "replay.log"), os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
                     0o600)
    try:
        with os.fdopen(log_fd, "w") as log:
            for k, turn in enumerate(turns):
                # Turn-major: every arm replays this turn from the same pristine,
                # time-filtered state before the next turn starts.
                turn_base = os.path.join(work, "turn")
                shutil.rmtree(turn_base, ignore_errors=True)
                clone_tree(base, turn_base)
                rep = filter_to_cutoff(turn_base, turn["ts"])
                filt["deleted"] += rep["deleted"]
                filt["errors"] += rep["errors"]
                filt["absent"].update(rep["absent"])
                for n, overrides in arms:
                    if k > 0 and a.delay_ms > 0:
                        time.sleep(a.delay_ms / 1000.0)
                    arm_state = os.path.join(work, f"{n}-{turn['id']}" if a.keep_state else n)
                    shutil.rmtree(arm_state, ignore_errors=True)
                    log.write(f"== turn {turn['id']} arm {n}\n")
                    log.flush()
                    row, rc = run_turn_arm(a, turn_base, arm_state, turn, n, overrides, base_env,
                                           log, sandbox)
                    st = stats[n]
                    if rc != 0:
                        st["failed_exits"] += 1
                    if row is None:
                        st["malformed"] += 1
                    else:
                        st["rows"] += 1
                        st["errors"] += row["action"] == "error"
                        outs[n].write(json.dumps(row, ensure_ascii=False) + "\n")
                        outs[n].flush()
                    if not a.keep_state:
                        shutil.rmtree(arm_state, ignore_errors=True)
                shutil.rmtree(turn_base, ignore_errors=True)
    finally:
        for f in outs.values():
            f.close()
    summaries = []
    for n, _ in arms:
        st = stats[n]
        st["complete"] = (st["rows"] == st["expected"] and st["errors"] == 0
                          and st["malformed"] == 0 and st["failed_exits"] == 0)
        summaries.append(st)
        print(f"arm {n}: rows {st['rows']}/{st['expected']} errors {st['errors']} "
              f"malformed {st['malformed']} failed exits {st['failed_exits']} "
              f"{'complete' if st['complete'] else 'INCOMPLETE'}")
    if filt["errors"]:
        print(f"WARNING: time filter failed on {len(filt['errors'])} table runs "
              f"(those rows stay): {sorted(set(filt['errors']))}", file=sys.stderr)
    manifest = {"endpoint": a.endpoint, "model": a.model, "temperature": a.temperature,
                "seed": a.seed, "sandbox": sandbox, "base_env": base_env,
                "time_filter": {"rows_deleted": filt["deleted"],
                                "errors": sorted(set(filt["errors"])),
                                "absent": sorted(filt["absent"])},
                "turn_ids": [t["id"] for t in turns], "arms": summaries}
    fd = os.open(os.path.join(run_dir, "manifest.json"), os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
                 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(manifest, f, indent=2)
    return 0 if all(s["complete"] for s in summaries) else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("snapshot")
    s.add_argument("--name", required=True)
    s.add_argument("--run-root", default="~/blind_ab_run")
    s.add_argument("--state-src", default="~/.human")
    r = sub.add_parser("run")
    r.add_argument("--name", required=True)
    r.add_argument("--run-root", default="~/blind_ab_run")
    r.add_argument("--arm", action="append", default=[], required=True)
    r.add_argument("--human", default=os.path.join(REPO_ROOT, "build", "human"))
    r.add_argument("--endpoint", default="http://127.0.0.1:8741/v1")
    r.add_argument("--model", default=None)
    r.add_argument("--temperature", type=float, default=None)
    r.add_argument("--delay-ms", type=int, default=2000)
    r.add_argument("--limit", type=int, default=0)
    r.add_argument("--seed", type=int, default=1)
    r.add_argument("--base-env-plist", default=None)
    r.add_argument("--dump-requests", action="store_true")
    r.add_argument("--sandbox", choices=("auto", "on", "off"), default="auto",
                   help="macOS sandbox-exec: writes only in the turn's scratch dir, "
                        "network only to loopback (auto = on when available)")
    r.add_argument("--keep-state", action="store_true",
                   help="keep every turn's scratch state as work/<arm>-<turn id> (debugging)")
    a = ap.parse_args(argv)
    try:
        return cmd_snapshot(a) if a.cmd == "snapshot" else cmd_run(a)
    except (ValueError, RuntimeError) as e:
        print(f"refusing: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
