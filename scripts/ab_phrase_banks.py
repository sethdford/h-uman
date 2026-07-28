#!/usr/bin/env python3
"""A/B: do the all-lowercase phrase banks + humor examples drive the product's
lowercase output?

Measured 2026-07-27 on the gateway (product) path, n=50:

    real Seth   lowercase 14%   median 24ch
    product     lowercase 86%   median 42ch

and the Gemini judge named lowercase in 18 of its 33 catches — the single most
cited AI tell. `src/persona/persona.c` tells the model "Normal capitalization
(your phone capitalizes for you)", and `~/.human/personas/seth.json`
anti_patterns says "All lowercase texting like a teenager. Use normal
capitalization." The model does it anyway.

The remaining lowercase-biased inputs that actually reach the product's prompt
are lexical, not instructional:

    ~/.human/phrase_banks.json    100% lowercase (32/32 entries: yeah, oh,
                                  sure, ok, hey, just, and, ...)
    seth.json humor.examples      5/5 lowercase

Exemplars beating an abstract instruction is the obvious reading. This script
tests it.

HOW IT STAYS SAFE
-----------------
Both variables are LIVE state that the running daemon reads at startup, so
this does NOT restart the live daemon or edit ~/.human. Each arm runs its own
isolated instance:

  * `human gateway --with-agent` — the real hu_agent_turn + persona pipeline,
    the same path `eval_blinded_ab.py --gateway` measures, but gateway-ONLY:
    no service loop, no channel polling, nothing can be texted.
  * its own $HOME, a symlink farm to ~/.human where only phrase_banks.json and
    config.json are real files.
  * its own gateway port via that sandboxed config, so it cannot bind or be
    mistaken for the live daemon's.
  * personas via HU_PERSONA_DIR pointing at a copy.

INTERNAL CONTROL
----------------
Arm A must reproduce ~86% lowercase. If it does not, the isolated instance
differs from the measured path and the comparison is void — say so rather than
reporting the delta.

Usage:
    python3 scripts/ab_phrase_banks.py             # 16 prompts/arm
    AB_N=24 python3 scripts/ab_phrase_banks.py
    python3 scripts/test_ab_phrase_banks.py        # unit tests, no model needed

Requires local serving on :8741. If it is down, every trial returns
"(error: ... code=7 ...)" and the run reports NO USABLE OUTPUT rather than a
number — check `launchctl list | grep mlx-server` first (absent means the job
was unloaded, commonly to free memory for a training run).
"""
import copy
import json
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eval_blinded_ab import build_gateway_messages  # same shape the gate sends

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
HU_BIN = os.path.join(REPO, "build", "human")
GT_PATH = os.path.join(REPO, "data", "imessage", "ground_truth.jsonl")
REAL_HOME = os.path.expanduser("~/.human")
DEFAULT_N = 16
SEED = 1337
TURN_TIMEOUT_S = 240
BANK_NAMES = ("fillers", "starters", "backchannels", "farewells")

# (label, empty the phrase banks?, strip humor examples?, gateway port)
ARMS = (
    ("A-control", False, False, 3011),
    ("B-treatment", True, True, 3012),
)


# ---- pure helpers (unit-tested; see scripts/test_ab_phrase_banks.py) --------

def sandbox_config(config, port):
    """Copy of `config` with the gateway port moved off the live daemon's.

    Deep-copied: the caller reads the REAL ~/.human/config.json and must not
    have it mutated under them.
    """
    out = copy.deepcopy(config) if config else {}
    out.setdefault("gateway", {})["port"] = port
    return out


def emptied_phrase_banks():
    """The treatment arm's phrase_banks.json — every bank present but empty.

    Empty-but-present rather than absent: a missing file is the documented
    pre-mining state and takes a different code path in
    `hu_conversation_phrase_banks_load`, which would confound the arm.
    """
    return {"imessage": {name: [] for name in BANK_NAMES},
            "_meta": {"ab": "emptied by scripts/ab_phrase_banks.py"}}


def persona_without_humor_examples(persona):
    """Copy of the persona with humor.examples emptied. Non-mutating."""
    out = copy.deepcopy(persona)
    humor = out.get("humor")
    if isinstance(humor, dict) and "examples" in humor:
        humor["examples"] = []
    return out


def sample_pairs(rows, n, seed=SEED):
    """Deterministic sample of usable ground-truth pairs.

    Pairs without a thread are skipped: the human reply being compared against
    was written with the thread visible, so a context-free prompt measures an
    asymmetry the harness invented (see get_ai_response_cli in
    eval_blinded_ab.py).
    """
    import random
    usable = [r for r in rows
              if (r.get("context_turns") or []) and (r.get("incoming") or "").strip()]
    random.Random(seed).shuffle(usable)
    return usable[:n]


def style_stats(texts):
    """Lowercase-start rate, median length, and dropped count. None if empty.

    Sentinels like "(error: ...)" / "(no choices)" are failures, not short
    replies — counting them as data would drag the measured style toward
    whatever the failure text happens to look like.
    """
    usable = [t.strip() for t in texts
              if t and not t.strip().startswith("(")]
    if not usable:
        return None
    lower = 0
    for s in usable:
        m = re.search(r"[A-Za-z]", s)
        if m and s[m.start()].islower():
            lower += 1
    return {"n": len(usable),
            "dropped": len(texts) - len(usable),
            "lower": lower / len(usable),
            "median_len": statistics.median([len(s) for s in usable])}


# ---- isolated instance -----------------------------------------------------

def build_sandbox(work_dir, label, strip_banks, strip_humor, port):
    root = os.path.join(work_dir, f"gwsb-{label}")
    shutil.rmtree(root, ignore_errors=True)
    home = os.path.join(root, ".human")
    os.makedirs(home)
    for entry in os.listdir(REAL_HOME):
        if entry in ("phrase_banks.json", "personas", "config.json"):
            continue
        os.symlink(os.path.join(REAL_HOME, entry), os.path.join(home, entry))

    with open(os.path.join(REAL_HOME, "config.json")) as f:
        cfg = json.load(f)
    with open(os.path.join(home, "config.json"), "w") as f:
        json.dump(sandbox_config(cfg, port), f, indent=1)

    banks_dst = os.path.join(home, "phrase_banks.json")
    if strip_banks:
        with open(banks_dst, "w") as f:
            json.dump(emptied_phrase_banks(), f, indent=1)
    else:
        os.symlink(os.path.join(REAL_HOME, "phrase_banks.json"), banks_dst)

    personas = os.path.join(root, "personas")
    shutil.copytree(os.path.join(REAL_HOME, "personas"), personas)
    if strip_humor:
        seth = os.path.join(personas, "seth.json")
        with open(seth) as f:
            persona = json.load(f)
        with open(seth, "w") as f:
            json.dump(persona_without_humor_examples(persona), f, indent=1)
    return root, personas


def start_gateway(root, personas, port, log_path):
    env = {**os.environ, "HOME": root, "HU_PERSONA_DIR": personas}
    log = open(log_path, "w")
    proc = subprocess.Popen([HU_BIN, "gateway", "--with-agent"],
                            stdout=log, stderr=subprocess.STDOUT,
                            env=env, preexec_fn=os.setsid)
    for _ in range(60):
        time.sleep(1)
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{port}/api/status",
                                   timeout=3).read()
            return proc
        except Exception:
            if proc.poll() is not None:
                raise RuntimeError(f"gateway exited early — see {log_path}")
    stop_gateway(proc)
    raise RuntimeError(f"gateway never bound :{port} — see {log_path}")


def stop_gateway(proc):
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except (ProcessLookupError, PermissionError):
        pass
    time.sleep(2)


def generate(port, pair):
    body = json.dumps({
        "model": "default",
        "messages": build_gateway_messages(pair["incoming"], pair.get("context_turns")),
    }).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}/v1/chat/completions",
                                 data=body,
                                 headers={"Content-Type": "application/json"})
    try:
        data = json.loads(urllib.request.urlopen(req, timeout=TURN_TIMEOUT_S).read())
        choices = data.get("choices", [])
        return choices[0]["message"]["content"] if choices else "(no choices)"
    except Exception as e:
        return f"(error: {e})"


# ---- driver ----------------------------------------------------------------

def main():
    work = os.environ.get("AB_WORK_DIR") or os.path.join(REPO, "data", "ab_phrase_banks")
    os.makedirs(work, exist_ok=True)
    n = int(os.environ.get("AB_N", DEFAULT_N))

    with open(GT_PATH) as f:
        rows = [json.loads(line) for line in f if line.strip()]
    pairs = sample_pairs(rows, n)
    if not pairs:
        print("no usable ground-truth pairs", file=sys.stderr)
        return 2

    print(f"phrase-bank A/B on the GATEWAY (product) path — {len(pairs)} prompts/arm",
          flush=True)
    results = {}
    for label, strip_banks, strip_humor, port in ARMS:
        root, personas = build_sandbox(work, label, strip_banks, strip_humor, port)
        log_path = os.path.join(work, f"gateway-{label}.log")
        print(f"\n=== ARM {label} (banks_emptied={strip_banks} "
              f"humor_stripped={strip_humor}) :{port}", flush=True)
        proc = start_gateway(root, personas, port, log_path)
        outputs = []
        try:
            for i, pair in enumerate(pairs):
                t0 = time.time()
                reply = generate(port, pair)
                outputs.append(reply)
                print(f"  [{i + 1:2}/{len(pairs)}] {time.time() - t0:5.1f}s {reply[:88]!r}",
                      flush=True)
        finally:
            stop_gateway(proc)
        results[label] = {"outputs": outputs, "stats": style_stats(outputs)}

    print("\n" + "=" * 68)
    for label, arm in results.items():
        s = arm["stats"]
        if not s:
            print(f"{label:12} NO USABLE OUTPUT — is :8741 serving? "
                  f"(launchctl list | grep mlx-server)")
            continue
        print(f"{label:12} n={s['n']:2} dropped={s['dropped']}  "
              f"lowercase {s['lower']:5.0%}  median {s['median_len']:.0f}ch")
    print("\nreference  real Seth              lowercase  14%   median 24ch")
    print("reference  product (gateway n=50) lowercase  86%   median 42ch")

    control = results.get("A-control", {}).get("stats")
    if control and not (0.70 <= control["lower"] <= 1.0):
        print(f"\nWARNING: control arm measured {control['lower']:.0%} lowercase, not "
              "~86%. The isolated instance does not reproduce the measured path; "
              "treat the delta as VOID rather than reporting it.")

    out_path = os.path.join(work, "result.json")
    with open(out_path, "w") as f:
        json.dump(results, f, indent=1)
    print(f"\nraw -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
