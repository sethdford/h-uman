#!/usr/bin/env python3
"""Turn Seth's "gold" moments into on-policy preference pairs: for each moment
where only Seth answered an inbound message, ask the serving model what h-uman
would have said there (rendered with the production prompt) and pair Seth's
real reply (chosen) with that sample (rejected).

Why (2026-09-26): the depth-upweighted SFT candidate read LESS like Seth than
the serving adapter (authorship twin 0.565 vs 0.597; strangers 0.64), and
natural same-moment pairs are rare (8 in 60 days, below the trainer's 200
floor). Seth's own replies are plentiful (216 gold moments). Pairing each
with a fresh policy sample is the standard on-policy recipe, but only if the
sample is what h-uman would actually send: a hand-written or full-head prompt
would teach the model to beat a strawman.

So, in order:
  1. Guards: refuse (exit 2, write nothing) inside the 02-05 retrain window,
     when the model server is down, or when the installed daemon lacks
     `reply-prompt`. One request in flight, X-HU-Priority: batch.
  2. Prompts come from `human reply-prompt` (src/agent/reply_prompt.c) under
     the HU_* env of the service plist, so gates match production. Values are
     never printed; secret-looking keys are dropped.
  3. Fidelity check before anything is written: for moments where h-uman
     really replied under the CURRENT policy (sent after the later of the
     installed daemon's and the persona file's mtime), sample a reply for the
     same context and compare one-word share and median length with what
     h-uman actually sent. Too few moments or a mismatch -> refuse, unless
     --skip-validation (recorded in the manifest as validated=false).
  4. Write {"prompt","chosen","rejected"} JSONL plus a text-free manifest.

The sample is an approximation of production (see reply_prompt.h for what the
offline prompt omits); the manifest says so.
"""
import argparse
import datetime as dt
import json
import os
import plistlib
import re
import statistics
import subprocess
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import eval_conversation_quality as cq  # noqa: E402
from merge_seth_preference_sources import render_prompt  # noqa: E402
from mine_onpolicy_pairs import iter_moments  # noqa: E402

RETRAIN_HOURS = range(2, 5)       # nightly retrain stops :8741 (window 02-05)
MIN_VALIDATE = 20                 # current-policy h-uman replies needed to trust samples
# Fidelity tolerances (judgment, recorded in the manifest): the sample must
# reproduce h-uman's real terseness and length closely enough that "Seth beats
# the sample" means "Seth beats h-uman".
MAX_ONE_WORD_GAP = 0.15
MEDIAN_RATIO = (0.67, 1.5)
SECRET_HINTS = ("KEY", "TOKEN", "SECRET", "PASSWORD", "CREDENTIAL")
THINK = re.compile(r"<think>.*?</think>", re.S)


def service_env(plist_path):
    """HU_* variables from the daemon's LaunchAgent, minus secret-looking keys."""
    try:
        with open(plist_path, "rb") as f:
            env = plistlib.load(f).get("EnvironmentVariables", {})
    except (OSError, plistlib.InvalidFileException):
        return {}
    return {k: str(v) for k, v in env.items()
            if k.startswith("HU_") and not any(h in k.upper() for h in SECRET_HINTS)}


def strip_thinking(text):
    return THINK.sub("", text or "").strip()


def one_word_share(texts):
    return sum(1 for t in texts if len(t.split()) <= 1) / len(texts)


def fidelity(real, sampled):
    """Compare h-uman's real replies with samples for the same moments.
    Returns (ok, stats); never ok on n < MIN_VALIDATE."""
    n = min(len(real), len(sampled))
    stats = {"n": n}
    if n < MIN_VALIDATE:
        stats["reason"] = f"only {n} current-policy h-uman replies (need {MIN_VALIDATE})"
        return False, stats
    real, sampled = real[:n], sampled[:n]
    r1, s1 = one_word_share(real), one_word_share(sampled)
    rm = statistics.median(len(t.encode()) for t in real)
    sm = statistics.median(len(t.encode()) for t in sampled)
    ratio = sm / rm if rm else float("inf")
    stats.update({"real_one_word": round(r1, 3), "sample_one_word": round(s1, 3),
                  "real_median_bytes": rm, "sample_median_bytes": sm,
                  "median_ratio": round(ratio, 3)})
    ok = abs(r1 - s1) <= MAX_ONE_WORD_GAP and MEDIAN_RATIO[0] <= ratio <= MEDIAN_RATIO[1]
    if not ok:
        stats["reason"] = "samples do not reproduce h-uman's real replies"
    return ok, stats


def trailing_inbound(turns):
    """The inbound batch being answered: trailing user turns, joined."""
    tail = []
    for t in reversed(turns):
        if t["role"] != "user":
            break
        tail.append(t["content"])
    return "\n".join(reversed(tail))


class Sampler:
    def __init__(self, human_bin, server, env, stage, temperature, max_tokens, timeout):
        self.human_bin, self.server, self.env = human_bin, server.rstrip("/"), env
        self.stage, self.temperature = stage, temperature
        self.max_tokens, self.timeout = max_tokens, timeout

    def system_prompt(self, contact, incoming):
        r = subprocess.run([self.human_bin, "reply-prompt", "--contact", contact,
                            "--incoming", incoming, "--stage", self.stage],
                           capture_output=True, text=True, env=self.env, timeout=60)
        return r.stdout if r.returncode == 0 and r.stdout.strip() else None

    def sample(self, contact, turns):
        system = self.system_prompt(contact, trailing_inbound(turns))
        if not system:
            return None
        messages = [{"role": "system", "content": system}] + turns
        body = json.dumps({"model": "default", "messages": messages,
                           "temperature": self.temperature,
                           "max_tokens": self.max_tokens}).encode()
        req = urllib.request.Request(self.server + "/v1/chat/completions", data=body,
                                     headers={"Content-Type": "application/json",
                                              "X-HU-Priority": "batch"})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                text = json.loads(resp.read())["choices"][0]["message"]["content"]
        except (urllib.error.URLError, OSError, KeyError, ValueError):
            return None
        return strip_thinking(text) or None


def server_up(server):
    try:
        with urllib.request.urlopen(server.rstrip("/") + "/health", timeout=5):
            return True
    except (urllib.error.URLError, OSError):
        return False


def supports_reply_prompt(human_bin):
    try:
        r = subprocess.run([human_bin, "reply-prompt", "--help"], capture_output=True,
                           text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return False
    # A binary that predates the command prints "Unknown command: reply-prompt".
    return "Usage: human reply-prompt" in (r.stdout + r.stderr)


def collect(chat_path, mem_path, since, policy_since):
    """(gold moments, current-policy h-uman moments), each with its contact."""
    att = cq.attribute(chat_path, mem_path, since)
    gold, huuman = [], []
    for contact, timeline in att["timelines"].items():
        for m in iter_moments(timeline, att["labels"]):
            if m["kind"] == "gold":
                gold.append((contact, m))
            elif m["kind"] == "huuman" and m["t"] >= policy_since:
                huuman.append((contact, m))
    return gold, huuman


def refuse(msg):
    print(f"REFUSE — {msg} (nothing written)")
    return 2


def main(argv=None, now=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--memory-db", default=os.path.expanduser("~/.human/memory.db"))
    ap.add_argument("--persona", default=os.path.expanduser("~/.human/personas/seth.json"))
    ap.add_argument("--human-bin", default=os.path.expanduser("~/.local/bin/human-daemon"))
    ap.add_argument("--plist", default=os.path.expanduser(
        "~/Library/LaunchAgents/ai.human.service-loop.plist"))
    ap.add_argument("--server", default="http://127.0.0.1:8741")
    ap.add_argument("--days", type=int, default=60)
    ap.add_argument("--stage", default="trusted", choices=["new", "familiar", "trusted", "deep"])
    ap.add_argument("--temperature", type=float, default=0.7)
    ap.add_argument("--max-tokens", type=int, default=300)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--limit", type=int, default=0, help="max gold moments to sample (0 = all)")
    ap.add_argument("--skip-validation", action="store_true")
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    now = now or dt.datetime.now().astimezone()

    if now.hour in RETRAIN_HOURS:
        return refuse(f"{now:%H:%M} is inside the 02-05 retrain window")
    if not server_up(a.server):
        return refuse(f"model server {a.server} is not answering /health")
    if not supports_reply_prompt(a.human_bin):
        return refuse(f"{a.human_bin} has no `reply-prompt` command (deploy first)")

    stamps = [os.path.getmtime(p) for p in (a.human_bin, a.persona) if os.path.exists(p)]
    policy_since = dt.datetime.fromtimestamp(max(stamps), dt.timezone.utc)
    since = now.astimezone(dt.timezone.utc) - dt.timedelta(days=a.days)
    gold, current = collect(a.chat_db, a.memory_db, since, policy_since)
    env = dict(os.environ)
    gated = service_env(a.plist)
    env.update(gated)
    sampler = Sampler(a.human_bin, a.server, env, a.stage, a.temperature, a.max_tokens,
                      a.timeout)

    real, sampled = [], []
    for contact, m in current:
        s = sampler.sample(contact, m["turns"])
        if s:
            real.append("\n".join(m["huuman"]))
            sampled.append(s)
    ok, fstats = fidelity(real, sampled)
    print(f"fidelity: {json.dumps(fstats)}")
    if not ok and not a.skip_validation:
        return refuse(f"fidelity check failed: {fstats.get('reason')}")

    pairs, failed = [], 0
    todo = gold[: a.limit] if a.limit else gold
    for contact, m in todo:
        chosen = "\n".join(m["seth"])
        rejected = sampler.sample(contact, m["turns"])
        prompt = render_prompt(m["turns"])
        if not rejected or not prompt:
            failed += 1
            continue
        if rejected.strip() == chosen.strip():
            continue
        pairs.append({"prompt": prompt, "chosen": chosen, "rejected": rejected})
    print(f"gold moments={len(todo)} pairs={len(pairs)} sample_failures={failed}")
    if not pairs:
        return refuse("no pairs produced")

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    tmp = a.out + ".tmp"
    with open(tmp, "w") as f:
        for p in pairs:
            f.write(json.dumps(p, ensure_ascii=False) + "\n")
    os.replace(tmp, a.out)
    manifest = {"generated_at": now.isoformat(), "pairs": len(pairs), "gold_moments": len(todo),
                "sample_failures": failed, "validated": ok, "fidelity": fstats,
                "policy_since": policy_since.isoformat(), "stage": a.stage,
                "temperature": a.temperature, "server": a.server,
                "service_env_keys": sorted(gated),
                "rejected_source": "offline approximation of the production prompt "
                                   "(human reply-prompt; no memory/graph/trust/tapback context)"}
    with open(a.out + ".manifest.json", "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"wrote {len(pairs)} pairs -> {a.out} (validated={ok})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
