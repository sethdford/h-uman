#!/usr/bin/env python3
"""Drive the real-turn replay harness: snapshot the state, run every arm.

  snapshot  copy the daemon's state into the run dir, read-only from the
            source: config.json (cloud API keys dropped), personas/,
            contacts/, the small top-level *.json files, and memory.db /
            graph.db / cognition.db through SQLite's online backup from a
            mode=ro connection — a consistent copy even while the daemon
            writes. The source is never opened for writing.
  run       one `human replay` process per arm, strictly one after another,
            each on a FRESH copy of the snapshot (an arm cannot see another
            arm's writes) with env = base env + the arm's overrides.

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
    return copied


def base_env_from_plist(path):
    out = subprocess.run(["plutil", "-extract", "EnvironmentVariables", "json", "-o", "-", path],
                         capture_output=True, text=True, check=True).stdout
    return {k: v for k, v in json.loads(out).items() if k.startswith("HU_")}


def arm_env(parent, base, overrides, state_dir):
    """The arm's process env: parent minus every HU_* var, plus base, plus the
    arm, plus the isolation and network-fence variables. Overrides win."""
    env = {k: v for k, v in parent.items() if not k.startswith("HU_")}
    env.update(base)
    env.update(overrides)
    env["HU_STATE_DIR"] = state_dir
    env["HU_MEMORY_SQLITE_PATH"] = os.path.join(state_dir, "memory.db")
    for k in ("ALL_PROXY", "all_proxy", "HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy"):
        env[k] = DEAD_PROXY
    env["NO_PROXY"] = env["no_proxy"] = "127.0.0.1,localhost,::1"
    env.pop("HU_IS_TEST", None)
    env.pop("CI", None)
    return env


def count_lines(path):
    if not os.path.exists(path):
        return 0
    with open(path) as f:
        return sum(1 for line in f if line.strip())


def run_arm(a, run_dir, name, overrides, base_env, n_turns):
    """One arm; returns its summary dict. Never raises on a failed arm."""
    arm_state = os.path.join(run_dir, "state", name)
    if os.path.exists(arm_state):
        shutil.rmtree(arm_state)
    shutil.copytree(os.path.join(run_dir, "state", "base"), arm_state)
    os.chmod(arm_state, 0o700)
    out_dir = os.path.join(run_dir, "out")
    logs = os.path.join(run_dir, "logs")
    make_private_dir(out_dir)
    make_private_dir(logs)
    out_path = os.path.join(out_dir, f"{name}.jsonl")
    cmd = [a.human, "replay", "--in", os.path.join(run_dir, "turns.jsonl"), "--out", out_path,
           "--arm", name, "--endpoint", a.endpoint, "--delay-ms", str(a.delay_ms),
           "--seed", str(a.seed)]
    if a.model:
        cmd += ["--model", a.model]
    if a.temperature is not None:
        cmd += ["--temperature", str(a.temperature)]
    if a.limit:
        cmd += ["--limit", str(a.limit)]
    if a.dump_requests:
        dump = os.path.join(run_dir, "requests")
        make_private_dir(dump)
        cmd += ["--dump-requests", dump]
    log_path = os.path.join(logs, f"{name}.log")
    fd = os.open(log_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as log:
        rc = subprocess.run(cmd, env=arm_env(os.environ, base_env, overrides, arm_state),
                            cwd=arm_state, stdout=log, stderr=subprocess.STDOUT).returncode
    want = min(n_turns, a.limit) if a.limit else n_turns
    got = count_lines(out_path)
    errors = 0
    if got:
        with open(out_path) as f:
            errors = sum(1 for line in f if line.strip() and json.loads(line)["action"] == "error")
    complete = rc == 0 and got == want and errors == 0
    return {"arm": name, "overrides": overrides, "exit": rc, "rows": got, "expected": want,
            "errors": errors, "complete": complete}


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
    if not os.path.isdir(os.path.join(run_dir, "state", "base")):
        print("refusing: no snapshot (run `replay_driver.py snapshot` first)", file=sys.stderr)
        return 2
    base_env = base_env_from_plist(a.base_env_plist) if a.base_env_plist else {}
    n_turns = count_lines(os.path.join(run_dir, "turns.jsonl"))
    summaries = []
    for name, overrides in arms:
        s = run_arm(a, run_dir, name, overrides, base_env, n_turns)
        summaries.append(s)
        print(f"arm {name}: rows {s['rows']}/{s['expected']} errors {s['errors']} "
              f"exit {s['exit']} {'complete' if s['complete'] else 'INCOMPLETE'}")
    manifest = {"endpoint": a.endpoint, "model": a.model, "temperature": a.temperature,
                "seed": a.seed, "base_env": base_env, "arms": summaries}
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
    a = ap.parse_args(argv)
    try:
        return cmd_snapshot(a) if a.cmd == "snapshot" else cmd_run(a)
    except (ValueError, RuntimeError) as e:
        print(f"refusing: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
