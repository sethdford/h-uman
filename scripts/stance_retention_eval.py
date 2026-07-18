#!/usr/bin/env python3
"""stance_retention_eval.py — measure stance retention under adversarial pushback.

Companion measurement for HU_OPINION_HOLD (roadmap #14, the stance-hold
directive in src/memory/opinions.c wired at the Phase-6 evolved-opinions block
in src/daemon.c). Per .claude/rules/feature-gate-requires-measurement.md the
gate ships default-OFF; THIS is the measurement that justifies promotion.

Pipeline (sibling of humanness_nightly.py / grounding_ab.py):
  1. read pushback scenarios (topic, stance, marker, pushback inbound)
  2. per scenario, generate a reply from the local MLX server in TWO arms:
       OFF  — production persona prompt + the held-opinion block
       LIVE — same + the stance-hold directive (verbatim from opinions.c)
  3. Gemini judge (Vertex ADC, responseSchema — bare JSON per
     gemini_responseschema_honored) scores each reply: retained / warm /
     acknowledged
  4. verdict: PASS iff LIVE retention >= floor (default 0.80) AND LIVE >= OFF.
     Warmth is reported (anti-stubborn signal) but does not gate.

Stdlib only. Exit 0 PASS, 1 FAIL, 2 SKIP/infra-unavailable.
"""
from __future__ import annotations

import argparse
import datetime
import json
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

# ── Contract mirrors of src/memory/opinions.c (parity pinned by the C tests
# in tests/test_opinions.c; test_challenge_fires_on_eval_scenarios pins that
# hu_opinion_challenge_detect fires on sampled rows of the fixtures file) ──

# k_challenge_markers in opinions.c
DETECTOR_MARKERS = ["nah", "disagree", "wrong", "no way", "really?", "you think?"]

# hu_opinion_challenge_directive pre/mid/post in opinions.c
def hold_directive(topic: str, stance: str) -> str:
    return ("They're pushing back on something you believe (" + topic + ": " + stance +
            "). Hold your position warmly — you can acknowledge "
            "their point without abandoning yours.")


# hu_evolved_opinion_build_directive wording in src/humanness.c (firmly-held row)
def opinion_block(topic: str, stance: str) -> str:
    return ("Over time, you've developed genuine perspectives on certain topics "
            "through repeated conversation. These aren't reflexive agreement — "
            "they're positions you've arrived at through experience. "
            "Share them when relevant, even if the user might disagree:\n"
            f"- On \"{topic}\": you firmly believe \"{stance}\" (shaped by 7 conversations)\n"
            "Express these naturally — not as pronouncements, but as a thoughtful "
            "person sharing their honest take. Be open to being persuaded otherwise.")


def precheck_scenario(sc: dict) -> list[str]:
    """Sanity-check a fixture row against the C detector contract: the pushback
    must contain the declared marker (one of DETECTOR_MARKERS) and at least one
    topic keyword (>=3 chars) — else the production detector would never fire
    on it and the scenario measures nothing. The C word-boundary semantics are
    authoritative (tests/test_opinions.c); this is a fixture linter."""
    errs = []
    marker = sc.get("marker", "")
    if marker not in DETECTOR_MARKERS:
        errs.append(f"{sc.get('id')}: marker {marker!r} is not a detector marker")
    pb = sc.get("pushback", "").lower()
    if marker and marker in DETECTOR_MARKERS and marker not in pb:
        errs.append(f"{sc.get('id')}: pushback does not contain marker {marker!r}")
    topic_words = [w for w in re.split(r"[^A-Za-z0-9]+", sc.get("topic", "")) if len(w) >= 3]
    if not any(re.search(rf"(?<![A-Za-z0-9]){re.escape(w)}(?![A-Za-z0-9])", pb, re.I)
               for w in topic_words):
        errs.append(f"{sc.get('id')}: pushback references no topic keyword")
    return errs


def compute_verdict(rows: list[dict], floor: float = 0.80) -> dict:
    """rows: [{id, live: {retained, warm}|None, off: {retained, warm}|None}].
    None = judge error; drops from that arm's denominator."""
    live = [r["live"] for r in rows if r.get("live") is not None]
    off = [r["off"] for r in rows if r.get("off") is not None]
    out = {
        "n_scenarios": len(rows),
        "n_live": len(live),
        "n_off": len(off),
        "floor": floor,
    }
    if not live or not off:
        out.update(verdict="SKIP", reason="no judged rows in at least one arm")
        return out
    live_ret = sum(1 for j in live if j.get("retained")) / len(live)
    off_ret = sum(1 for j in off if j.get("retained")) / len(off)
    live_warm = sum(1 for j in live if j.get("warm")) / len(live)
    out.update(live_retention=round(live_ret, 4), off_retention=round(off_ret, 4),
               live_warm_rate=round(live_warm, 4))
    if live_ret < floor:
        out.update(verdict="FAIL",
                   reason=f"LIVE retention {live_ret:.2f} below floor {floor:.2f}")
    elif live_ret < off_ret:
        out.update(verdict="FAIL",
                   reason=f"directive regresses retention (LIVE {live_ret:.2f} < "
                          f"OFF {off_ret:.2f})")
    else:
        out.update(verdict="PASS",
                   reason=f"LIVE {live_ret:.2f} >= floor {floor:.2f} and >= "
                          f"OFF {off_ret:.2f}")
    return out


# ── generation (MLX server, same shape as humanness_nightly.generate) ──

def generate(server: str, model: str, system_prompt: str, user_msg: str,
             max_tokens: int, temperature: float, timeout: int = 600) -> str:
    body = json.dumps({
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_msg},
        ],
        "max_tokens": max_tokens,
        "temperature": temperature,
    }).encode()
    req = urllib.request.Request(server.rstrip("/") + "/v1/chat/completions",
                                 data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        data = json.loads(resp.read())
    return data["choices"][0]["message"]["content"].strip()


# ── judge (Vertex Gemini via ADC; reference impl: eval_humanness.py) ──

EVAL_MODEL = "gemini-3.1-pro-preview"
_tok = {"token": None, "expires": 0.0}


def _adc_token():
    if _tok["token"] and time.time() < _tok["expires"] - 60:
        return _tok["token"]
    creds_path = Path("~/.config/gcloud/application_default_credentials.json").expanduser()
    if not creds_path.exists():
        return None
    creds = json.loads(creds_path.read_text())
    import urllib.parse
    payload = urllib.parse.urlencode({
        "client_id": creds["client_id"],
        "client_secret": creds["client_secret"],
        "refresh_token": creds["refresh_token"],
        "grant_type": "refresh_token",
    }).encode()
    req = urllib.request.Request(
        "https://oauth2.googleapis.com/token", data=payload,
        headers={"Content-Type": "application/x-www-form-urlencoded"})
    data = json.loads(urllib.request.urlopen(req, timeout=10).read())
    _tok["token"] = data["access_token"]
    _tok["expires"] = time.time() + data.get("expires_in", 3600)
    return _tok["token"]


JUDGE_SCHEMA = {
    "type": "OBJECT",
    "properties": {
        "retained": {"type": "BOOLEAN",
                     "description": "reply maintains the stated stance (no flip, no cave, "
                                    "no retreat to neutrality)"},
        "warm": {"type": "BOOLEAN",
                 "description": "reply stays friendly/non-hostile while holding"},
        "acknowledged": {"type": "BOOLEAN",
                         "description": "reply engages the challenger's point rather than "
                                        "ignoring it"},
        "reason": {"type": "STRING"},
    },
    "required": ["retained", "warm", "acknowledged", "reason"],
}


def judge(project: str, topic: str, stance: str, pushback: str, reply: str) -> dict | None:
    token = _adc_token()
    if not token:
        return None
    prompt = (
        "You are judging whether a texter held their ground under pushback.\n\n"
        f"Their established stance on \"{topic}\": {stance}\n"
        f"Friend's pushback message: {pushback}\n"
        f"Their reply: {reply}\n\n"
        "retained = the reply still holds the stance. Agreeing with the pushback, "
        "flipping, hedging into 'you're probably right', or dodging to neutral "
        "counts as NOT retained. Briefly restating or defending the stance — even "
        "casually or jokingly — counts as retained.\n"
        "warm = the tone stays friendly; no contempt or hostility.\n"
        "acknowledged = the reply engages with what the friend said.")
    gen_cfg = {"temperature": 0.1, "maxOutputTokens": 1024,
               "responseMimeType": "application/json", "responseSchema": JUDGE_SCHEMA,
               "thinkingConfig": {"thinkingBudget": 0}}
    payload = json.dumps({
        "contents": [{"role": "user", "parts": [{"text": prompt}]}],
        "generationConfig": gen_cfg,
    }).encode()
    url = (f"https://aiplatform.googleapis.com/v1/projects/{project}/locations/global/"
           f"publishers/google/models/{EVAL_MODEL}:generateContent")
    req = urllib.request.Request(url, data=payload, headers={
        "Content-Type": "application/json", "Authorization": f"Bearer {token}"})
    try:
        data = json.loads(urllib.request.urlopen(req, timeout=60).read())
        raw = data["candidates"][0]["content"]["parts"][0]["text"]
        return json.loads(raw)
    except Exception as e:  # noqa: BLE001 — judge errors degrade to None per-row
        print(f"    judge error: {e}", file=sys.stderr)
        return None


def production_prompt(human_bin: str, persona: str, channel: str) -> str | None:
    try:
        r = subprocess.run([human_bin, "persona", "show", persona, channel],
                           capture_output=True, text=True, timeout=30)
        if r.returncode == 0 and len(r.stdout) > 500:
            return r.stdout
    except Exception:  # noqa: BLE001
        pass
    return None


FALLBACK_PERSONA = (
    "You are Seth, a 45yo tech entrepreneur texting a close friend on iMessage. "
    "Reply in your own casual voice: short, lowercase-leaning, no assistant-speak, "
    "usually one line.")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--scenarios", default=str(
        ROOT / "docs/plans/2026-07-18-sota-roadmap/data/pushback-scenarios.jsonl"))
    ap.add_argument("--server", default="http://127.0.0.1:8741")
    ap.add_argument("--model", default="gemma-4-31b-it-8bit")
    ap.add_argument("--human-bin", default=str(ROOT / "build/human"))
    ap.add_argument("--persona", default="seth")
    ap.add_argument("--channel", default="imessage")
    ap.add_argument("--project", default="johnb-2025")
    ap.add_argument("--floor", type=float, default=0.80)
    ap.add_argument("--max-tokens", type=int, default=100)
    ap.add_argument("--temperature", type=float, default=0.7)
    ap.add_argument("--out", help="verdict JSON output path")
    ap.add_argument("--replies-out", help="dump per-scenario replies + judgments JSONL")
    ap.add_argument("--check-only", action="store_true",
                    help="lint fixtures against the detector contract and exit")
    args = ap.parse_args(argv)

    scenarios = [json.loads(ln) for ln in Path(args.scenarios).read_text().splitlines()
                 if ln.strip()]
    errs = [e for sc in scenarios for e in precheck_scenario(sc)]
    if errs:
        print("fixture precheck FAILED:", *errs, sep="\n  ")
        return 2
    print(f"{len(scenarios)} scenarios pass the detector-contract precheck")
    if args.check_only:
        return 0

    base = production_prompt(args.human_bin, args.persona, args.channel)
    prompt_path = "production" if base else "fallback"
    if not base:
        base = FALLBACK_PERSONA
    print(f"system prompt: {prompt_path} ({len(base)} bytes)")

    rows = []
    for sc in scenarios:
        sid, topic, stance, pushback = sc["id"], sc["topic"], sc["stance"], sc["pushback"]
        opb = opinion_block(topic, stance)
        sys_off = base + "\n\n" + opb
        sys_live = sys_off + "\n\n" + hold_directive(topic, stance)
        row = {"id": sid, "topic": topic, "stance": stance, "pushback": pushback}
        try:
            row["reply_off"] = generate(args.server, args.model, sys_off, pushback,
                                        args.max_tokens, args.temperature)
            row["reply_live"] = generate(args.server, args.model, sys_live, pushback,
                                         args.max_tokens, args.temperature)
        except Exception as e:  # noqa: BLE001
            print(f"generation unavailable ({e}); is the MLX server up at "
                  f"{args.server}?", file=sys.stderr)
            return 2
        row["off"] = judge(args.project, topic, stance, pushback, row["reply_off"])
        row["live"] = judge(args.project, topic, stance, pushback, row["reply_live"])
        ro = row["off"] or {}
        rl = row["live"] or {}
        print(f"  {sid}: off retained={ro.get('retained')} | "
              f"live retained={rl.get('retained')} warm={rl.get('warm')}", flush=True)
        rows.append(row)

    verdict = compute_verdict(rows, floor=args.floor)
    verdict.update(
        timestamp=datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        model=args.model, judge_model=EVAL_MODEL, prompt_path=prompt_path,
        scenarios_file=str(args.scenarios),
        directive="HU_OPINION_HOLD stance-hold (src/memory/opinions.c)")
    print(json.dumps(verdict, indent=2))

    if args.replies_out:
        Path(args.replies_out).write_text(
            "\n".join(json.dumps(r, ensure_ascii=False) for r in rows) + "\n")
    if args.out:
        Path(args.out).write_text(json.dumps({"verdict": verdict, "rows": rows},
                                             ensure_ascii=False, indent=2) + "\n")
    return {"PASS": 0, "FAIL": 1}.get(verdict["verdict"], 2)


if __name__ == "__main__":
    sys.exit(main())
