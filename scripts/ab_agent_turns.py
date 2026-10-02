#!/usr/bin/env python3
"""Offline A/B through the real agent turn, never sending anything.

Each turn is `human agent --once -m <inbound> --contact <handle> --channel
imessage` (the production hu_agent_turn: memory, recall planner,
tree-of-thought, the persona and style card) run with the service loop's
non-secret HU_* env, in an isolated copy of the state dir that is restored
from a pristine snapshot before EVERY turn. Without that reset, arm B
restored arm A's reply as session history, the model repeated it and the
server's echo guard blanked it (2026-10-02: 3 of the first 9 turns empty).

Contexts are real inbound messages from chat.db (1:1, not the owner's own
numbers, not channels.imessage.exclude_from), with Seth's own next reply when
he wrote one (the daemon's sends are excluded by its own records).

  scripts/ab_agent_turns.py setup                  # snapshot config/personas/memory.db
  scripts/ab_agent_turns.py run planner --n 40     # arms: see ARMS
  scripts/ab_agent_turns_judge.py <work>/ab_planner.jsonl off live

The CLI agent prints its reply and sends nothing (checked 2026-10-02: no
outgoing chat.db row during 60 test turns). It still loads the local model on
:8741, which serves live replies too, and the nightly retrain stops that
server: run outside 03:00-04:15, and expect rc!=0 rows if you don't (they are
retried on the next run).
"""
import argparse, json, os, plistlib, random, shutil, sqlite3, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

OWNER_DEFAULT = ("+18012017497", "sethdouglasford@gmail.com", "+14845661687")
STATE_HOME = os.path.expanduser("~/.human")

# name -> [(arm, config file in the work state, extra env)]
ARMS = {
    "tot": [("off", "config.tot_off.json", {}), ("on", "config.tot_on.json", {})],
    "planner": [("off", "config.json", {"HU_RECALL_PLANNER_LLM": "off"}),
                ("live", "config.json", {"HU_RECALL_PLANNER_LLM": "live"})],
    "beat": [("off", "config.json", {"HU_STYLE_SECOND_BEAT": "off"}),
             ("live", "config.json", {"HU_STYLE_SECOND_BEAT": "live"})],
}


def default_work():
    return os.path.join(STATE_HOME, "benchmarks", "ab-agent-turns")


def owner_handles(persona="seth"):
    try:
        contacts = json.load(open(os.path.join(STATE_HOME, "personas", f"{persona}.json"))).get("contacts") or {}
        found = {h for h, c in contacts.items() if isinstance(c, dict) and c.get("relationship") == "test"}
        return found or set(OWNER_DEFAULT)
    except (OSError, ValueError):
        return set(OWNER_DEFAULT)


def excluded_handles():
    try:
        c = json.load(open(os.path.join(STATE_HOME, "config.json")))
        return set(c.get("channels", {}).get("imessage", {}).get("exclude_from") or [])
    except (OSError, ValueError):
        return set()


def keep_context(kind, text, has_seth_reply):
    """Which inbound messages each A/B is about. Pure."""
    if not text or "￼" in text or "http" in text:
        return False
    if kind == "tot":
        return len(text) > 200  # tree-of-thought only runs above 200 chars
    if kind == "planner":
        return len(text.split()) > 12 and len(text) <= 400 and has_seth_reply
    if kind == "beat":
        return 8 <= len(text) <= 160 and has_seth_reply
    raise ValueError(kind)


def contexts(kind, n, seed=7, days=150):
    import extract_imessage_pairs as e
    from blind_ab.imessage_text import msg_text

    is_daemon = e.daemon_send_predicate(e.load_daemon_records())
    db = sqlite3.connect("file:" + os.path.expanduser("~/Library/Messages/chat.db") + "?mode=ro", uri=True)
    rows = db.execute(f"""
      SELECT c.chat_identifier, m.date/1000000000+978307200 ts, m.is_from_me, m.text, m.attributedBody,
             COALESCE(m.associated_message_type,0)
      FROM message m JOIN chat_message_join j ON j.message_id=m.ROWID JOIN chat c ON c.ROWID=j.chat_id
      WHERE c.style=45 AND ts > CAST(strftime('%s','now','-{int(days)} days') AS INTEGER)
      ORDER BY c.chat_identifier, ts""").fetchall()
    skip = owner_handles() | excluded_handles()
    out = []
    for i, r in enumerate(rows):
        if r[0] in skip or r[2] or r[5]:
            continue
        txt = (msg_text(r[3], r[4]) or "").strip()
        nxt = rows[i + 1] if i + 1 < len(rows) and rows[i + 1][0] == r[0] else None
        seth = None
        if nxt and nxt[2] and not nxt[5]:
            t2 = (msg_text(nxt[3], nxt[4]) or "").strip()
            if t2 and not is_daemon(t2, nxt[1]) and nxt[1] - r[1] < 6 * 3600:
                seth = t2
        if keep_context(kind, txt, bool(seth)):
            out.append({"id": f"{r[0]}:{int(r[1])}", "contact": r[0], "inbound": txt, "seth": seth})
    random.Random(seed).shuffle(out)
    return out[:n]


def prod_env(state):
    pl = plistlib.load(open(os.path.expanduser("~/Library/LaunchAgents/ai.human.service-loop.plist"), "rb"))
    env = {k: v for k, v in pl.get("EnvironmentVariables", {}).items()
           if k.startswith("HU_") and not any(s in k for s in ("KEY", "TOKEN", "SECRET"))}
    env.update({"HU_STATE_DIR": state, "HU_MEMORY_SQLITE_PATH": os.path.join(state, "memory.db"),
                "HU_SEMANTIC_EMBED_URL": "http://127.0.0.1:8741"})
    return env


def setup(work, card=None):
    """Pristine snapshot: config (+ tree-of-thought on/off variants), personas,
    an online-backup copy of memory.db. `card` replaces the style card."""
    pristine = os.path.join(work, "pristine")
    shutil.rmtree(pristine, ignore_errors=True)
    os.makedirs(pristine)
    cfg = json.load(open(os.path.join(STATE_HOME, "config.json")))
    json.dump(cfg, open(os.path.join(pristine, "config.json"), "w"), indent=1)
    for name, tot in (("config.tot_off.json", False), ("config.tot_on.json", True)):
        cfg.setdefault("agent", {})["tree_of_thought"] = tot
        json.dump(cfg, open(os.path.join(pristine, name), "w"), indent=1)
    shutil.copytree(os.path.join(STATE_HOME, "personas"), os.path.join(pristine, "personas"))
    if card:
        shutil.copy(card, os.path.join(pristine, "personas", "seth.style-card.json"))
    src = sqlite3.connect(os.path.join(STATE_HOME, "memory.db"))
    dst = sqlite3.connect(os.path.join(pristine, "memory.db"))
    src.backup(dst)
    dst.close()
    src.close()
    return pristine


def reset_state(work):
    """Every turn starts from the snapshot (APFS clones make it instant)."""
    pristine, state = os.path.join(work, "pristine"), os.path.join(work, "state")
    shutil.rmtree(state, ignore_errors=True)
    os.makedirs(state)
    for name in os.listdir(pristine):
        src, dst = os.path.join(pristine, name), os.path.join(state, name)
        if os.path.isdir(src):
            shutil.copytree(src, dst)
        elif subprocess.run(["cp", "-c", src, dst]).returncode != 0:
            shutil.copy2(src, dst)  # not APFS: a real copy
    return state


def run_turn(work, binp, cfg, contact, msg, extra_env):
    state = reset_state(work)
    env = {**os.environ, **prod_env(state), **extra_env}
    t0 = time.time()
    p = subprocess.run([binp, "agent", "--config", os.path.join(state, cfg), "--contact", contact,
                        "--channel", "imessage", "--once", "-m", msg],
                       capture_output=True, env=env, timeout=600)
    return p.stdout.decode("utf-8", "replace").strip(), round(time.time() - t0, 1), p.returncode


def settled(path):
    """(id, arm) pairs already measured: a row that failed (rc != 0 or an
    empty reply) is retried on the next run."""
    done = set()
    if os.path.exists(path):
        for line in open(path):
            r = json.loads(line)
            if r.get("rc") == 0 and r.get("reply", "").strip():
                done.add((r["id"], r["arm"]))
    return done


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", default=default_work())
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("setup")
    s.add_argument("--card", help="style card to use instead of the live one")
    r = sub.add_parser("run")
    r.add_argument("name", choices=list(ARMS))
    r.add_argument("--n", type=int, default=30)
    r.add_argument("--bin", default=os.path.join(REPO, "build-prod", "human"))
    a = ap.parse_args(argv)
    os.makedirs(a.work, exist_ok=True)
    if a.cmd == "setup":
        print("pristine snapshot:", setup(a.work, a.card))
        return 0
    if not os.path.isdir(os.path.join(a.work, "pristine")):
        sys.exit("REFUSING: run `setup` first")
    path = os.path.join(a.work, f"ab_{a.name}.jsonl")
    done = settled(path)
    ctxs = contexts(a.name, a.n)
    print(f"{len(ctxs)} contexts -> {path}", flush=True)
    for k, c in enumerate(ctxs):
        arms = ARMS[a.name] if k % 2 == 0 else list(reversed(ARMS[a.name]))  # alternate order
        for arm, cfg, extra in arms:
            if (c["id"], arm) in done:
                continue
            reply, secs, rc = run_turn(a.work, a.bin, cfg, c["contact"], c["inbound"], extra)
            with open(path, "a") as f:
                f.write(json.dumps({**c, "arm": arm, "reply": reply, "secs": secs, "rc": rc}) + "\n")
            print(f"[{k + 1}/{len(ctxs)}] {arm:4} {secs:5.1f}s rc={rc} {reply[:70]!r}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
