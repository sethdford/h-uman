#!/usr/bin/env python3
"""conversation_arena.py — continuous self-play conversations, judged, fed back.

The arena runs multi-turn conversations against ourselves: a Gemini-driven
contact simulator texts the production Seth path (local MLX, real 16KB persona
prompt), and a Gemini judge scores every Seth turn — humanness, voice, AI
tells — and writes an in-voice rewrite for each failure. Three outputs close
the learning loop:

  1. transcripts + judgments  -> ~/.human/logs/arena/run-<ts>.jsonl (audit trail)
  2. scoreboard row per convo -> ~/.human/logs/arena/scoreboard.jsonl (trend)
  3. flagged turns            -> dpo_pairs rows (source='arena') in memory.db,
                                 consumed by the existing nightly DPO/RLAIF loop

Quality bar for (3): judge confidence >= 0.7, margin (1 - score) >= min-margin,
and a non-empty rewrite — synthetic data enters the production learning loop
only when the judge is sure AND can show a better reply. Capped per run.

Ops notes: the local MLX queue is SERIAL and client timeouts don't cancel
generation (2026-07-18 convoy incident) — timeouts here are generous and the
default cadence (launchd, every 4h) keeps arena load a small fraction of the
queue. Judge calls use Vertex ADC (gemini_responseschema_honored pattern).

Stdlib only. Exit 0 on success, 2 on infra-unavailable.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import random
import sqlite3
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
ARENA_DIR = Path("~/.human/logs/arena").expanduser()

GEN_MODEL = "gemini-3.5-flash"          # contact simulator: fast + cheap
JUDGE_MODEL = "gemini-3.1-pro-preview"  # judge: deep read

# ── Scenario library — personas x goals the daemon must survive ──────────────

SCENARIOS = [
    {"id": "pushback_tech", "persona": "old startup friend, opinionated, blunt",
     "goal": "pick a fight about a tech opinion Seth holds and push back twice",
     "opener": "nah man remote work is done, everyone serious is back in office"},
    {"id": "banter_sports", "persona": "college buddy, all banter, never serious",
     "goal": "roast Seth about his team losing, keep it playful",
     "opener": "lmaooo did you even watch that game last night"},
    {"id": "emotional_rough_day", "persona": "close friend going through it",
     "goal": "vent about a brutal work week, see if Seth actually listens",
     "opener": "dude this week is crushing me. i can't keep up anymore"},
    {"id": "test_if_ai", "persona": "suspicious friend who thinks Seth's texts feel off",
     "goal": "subtly probe whether this is really Seth — ask about shared memories, note odd phrasing",
     "opener": "you've been texting weird lately lol. you good?"},
    {"id": "logistics_plan", "persona": "friend organizing a weekend hang",
     "goal": "pin down a day and place, tolerate Seth being vague, push for specifics",
     "opener": "ok so are we doing bbq this weekend or what"},
    {"id": "boredom_loweffort", "persona": "friend killing time, sends low-effort messages",
     "goal": "send short low-content messages ('lol', 'fr', 'idk man') and see if Seth over-replies",
     "opener": "yo"},
    {"id": "share_news", "persona": "friend with big news",
     "goal": "share exciting news, see if Seth matches energy without gushing like an assistant",
     "opener": "DUDE. i got the offer"},
    {"id": "callback_memory", "persona": "friend referencing a previous conversation",
     "goal": "reference something 'we talked about before' that's vague, see if Seth fabricates specifics",
     "opener": "hey did you ever follow up on that thing you mentioned last time"},
    {"id": "late_night_deep", "persona": "friend in a 1am philosophical mood",
     "goal": "go deep about whether any of this matters, see if Seth stays human-sized (no essays)",
     "opener": "you ever think about how weird it is that we just... do this every day"},
    {"id": "double_text_pressure", "persona": "slightly needy friend",
     "goal": "double-text when Seth is brief, fish for reassurance",
     "opener": "hey you seemed off earlier. did i do something?"},

    # ── Teasing / joking register — grounded in seth.json humor.style: dry,
    #    deadpan, self-deprecating; playful roasting; absurd observations; tech
    #    humor; exaggeration. Each opener is a SETUP line that lobs a specific
    #    bait so the judge can score whether Seth volleys in-voice or whiffs.
    {"id": "easy_roast_setup", "register": "humor",
     "persona": "old friend who ribs Seth constantly, dry and merciless",
     "goal": "lob soft insults begging for a comeback (his gym streak, his age, "
             "his music taste); if he volleys back, escalate; corny or earnest "
             "replies should feel like a whiff",
     "opener": "saw your gym check-in streak. real impressive. 0 days"},
    {"id": "callback_joke", "register": "humor",
     "persona": "friend reviving a running bit from a past chat",
     "goal": "resurrect the old 'salad on your burger' inside joke and see if "
             "Seth recognizes the bit and BUILDS on it (deadpan) vs taking it "
             "literally, ignoring it, or fabricating a fake shared memory",
     "opener": "so did you have another 'salad' for lunch today or"},
    {"id": "deadpan_bait", "register": "humor",
     "persona": "earnest friend pushing unsolicited self-improvement advice",
     "goal": "give sincere wellness/productivity advice ('wake up at 5am', "
             "'try meditating') and see if Seth deadpans back instead of "
             "earnestly agreeing like an assistant",
     "opener": "you should really try waking up at 5am, changed my life"},
    {"id": "banter_escalation", "register": "humor",
     "persona": "college buddy who lives for a back-and-forth roast war",
     "goal": "start a playful roast and escalate every turn, testing whether "
             "Seth escalates warmly and holds the bit or folds, over-explains, "
             "or goes earnest and kills it",
     "opener": "be honest, you peaked in like 2009"},
    {"id": "self_deprecation_open", "register": "humor",
     "persona": "friend fishing for details on a big thing that probably went ok-ish",
     "goal": "ask how the demo/pitch went and see if Seth deflects with "
             "self-deprecating humor vs assistant-style earnest positivity or "
             "hollow bragging",
     "opener": "yooo how'd the big demo go"},
]

# Scenario ids whose primary purpose is exercising Seth's teasing/joking
# register. main() reports the humor-axis baseline over just these, since the
# humor axis is only a meaningful measurement where a humor beat was invited.
HUMOR_SCENARIO_IDS = frozenset(
    sc["id"] for sc in SCENARIOS if sc.get("register") == "humor")


def validate_scenario(sc: dict) -> list[str]:
    errs = []
    for k in ("id", "persona", "goal", "opener"):
        if not sc.get(k):
            errs.append(f"scenario {sc.get('id', '?')}: missing {k}")
    return errs


# ── Pure helpers (unit-tested in test_conversation_arena.py) ────────────────

def build_dpo_rows(judgment: dict, transcript: list[dict], min_margin: float = 0.3,
                   cap: int = 4) -> list[dict]:
    """Flagged Seth turns -> dpo_pairs rows. Quality bar: confidence >= 0.7,
    margin = 1 - score >= min_margin, non-empty rewrite, valid seth turn."""
    rows = []
    for t in judgment.get("turns", []):
        idx = t.get("turn_index", -1)
        if not (0 <= idx < len(transcript)) or transcript[idx].get("role") != "seth":
            continue
        score = float(t.get("score", 1.0))
        margin = 1.0 - score
        rewrite = (t.get("better_reply") or "").strip()
        if margin < min_margin or float(t.get("confidence", 0.0)) < 0.7 or not rewrite:
            continue
        context = "\n".join(
            f"{'Them' if m['role'] == 'them' else 'Seth'}: {m['text']}"
            for m in transcript[:idx])
        rows.append({"prompt": context, "chosen": rewrite,
                     "rejected": transcript[idx]["text"],
                     "margin": round(margin, 4), "source": "arena"})
        if len(rows) >= cap:
            break
    return rows


def scoreboard_trend(rows: list[dict], window: int = 10) -> dict:
    vals = [float(r.get("overall_humanness", 0.0)) for r in rows]
    if not vals:
        return {"n": 0, "recent_mean": 0.0, "all_mean": 0.0}
    recent = vals[-window:]
    return {"n": len(vals), "recent_mean": sum(recent) / len(recent),
            "all_mean": sum(vals) / len(vals)}


def axis_spread(rows: list[dict], key: str) -> dict:
    """Spread of one judge axis across scoreboard rows. This is the trust check
    for the humor measurement: a judge that discriminates produces a RANGE of
    scores across scenarios; one that collapses every turn to ~1.0 (spread ~0)
    is not measuring anything and cannot gate step-3 prompt tuning. Returns n,
    mean, min, max, and spread (max - min). Rows missing the key are skipped."""
    vals = [float(r[key]) for r in rows if r.get(key) is not None]
    if not vals:
        return {"n": 0, "mean": 0.0, "min": 0.0, "max": 0.0, "spread": 0.0}
    lo, hi = min(vals), max(vals)
    return {"n": len(vals), "mean": sum(vals) / len(vals),
            "min": lo, "max": hi, "spread": hi - lo}


# ── Generation: Seth side (local MLX, production prompt) ─────────────────────

def production_prompt(human_bin: str, persona: str, channel: str) -> str | None:
    try:
        r = subprocess.run([human_bin, "persona", "show", persona, channel],
                           capture_output=True, text=True, timeout=30)
        if r.returncode == 0 and len(r.stdout) > 500:
            return r.stdout
    except Exception:  # noqa: BLE001
        pass
    return None


def seth_reply(server: str, model: str, system_prompt: str, transcript: list[dict],
               max_tokens: int, timeout: int) -> str:
    messages = [{"role": "system", "content": system_prompt}]
    for m in transcript:
        messages.append({"role": "user" if m["role"] == "them" else "assistant",
                         "content": m["text"]})
    body = json.dumps({"model": model, "messages": messages,
                       "max_tokens": max_tokens, "temperature": 0.7}).encode()
    req = urllib.request.Request(server.rstrip("/") + "/v1/chat/completions",
                                 data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read())["choices"][0]["message"]["content"].strip()


# ── Gemini plumbing (Vertex ADC; ref impl eval_humanness.py) ─────────────────

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
        "client_id": creds["client_id"], "client_secret": creds["client_secret"],
        "refresh_token": creds["refresh_token"], "grant_type": "refresh_token",
    }).encode()
    req = urllib.request.Request("https://oauth2.googleapis.com/token", data=payload,
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    data = json.loads(urllib.request.urlopen(req, timeout=10).read())
    _tok["token"] = data["access_token"]
    _tok["expires"] = time.time() + data.get("expires_in", 3600)
    return _tok["token"]


def gemini(project: str, model: str, prompt: str, schema: dict | None = None,
           temperature: float = 0.7, max_tokens: int = 2048) -> str:
    token = _adc_token()
    if not token:
        raise RuntimeError("no ADC credentials for Gemini judge")
    gen_cfg = {"temperature": temperature, "maxOutputTokens": max_tokens,
               "thinkingConfig": {"thinkingBudget": 0}}
    if schema is not None:
        gen_cfg["responseMimeType"] = "application/json"
        gen_cfg["responseSchema"] = schema
    payload = json.dumps({"contents": [{"role": "user", "parts": [{"text": prompt}]}],
                          "generationConfig": gen_cfg}).encode()
    url = (f"https://aiplatform.googleapis.com/v1/projects/{project}/locations/global/"
           f"publishers/google/models/{model}:generateContent")
    req = urllib.request.Request(url, data=payload, headers={
        "Content-Type": "application/json", "Authorization": f"Bearer {token}"})
    data = json.loads(urllib.request.urlopen(req, timeout=90).read())
    return data["candidates"][0]["content"]["parts"][0]["text"]


def contact_turn(project: str, scenario: dict, transcript: list[dict]) -> str:
    convo = "\n".join(f"{'You' if m['role'] == 'them' else 'Seth'}: {m['text']}"
                      for m in transcript)
    prompt = (
        f"You are role-playing a real person texting their friend Seth on iMessage.\n"
        f"Your persona: {scenario['persona']}\n"
        f"Your goal in this conversation: {scenario['goal']}\n\n"
        f"Conversation so far:\n{convo}\n\n"
        "Write ONLY your next text message. Real texting style: short (usually "
        "under 15 words), casual, lowercase-leaning, no quotation marks, no "
        "narration. Stay in character and keep driving toward your goal. If the "
        "conversation has naturally ended, send a short closer.")
    return gemini(project, GEN_MODEL, prompt, temperature=0.9, max_tokens=256).strip()


JUDGE_SCHEMA = {
    "type": "OBJECT",
    "properties": {
        "overall_humanness": {"type": "NUMBER",
                              "description": "0-1: would a close friend believe this was Seth"},
        "voice_consistency": {"type": "NUMBER"},
        "engagement": {"type": "NUMBER",
                       "description": "0-1: present and responsive without over-serving"},
        "humor": {"type": "NUMBER", "description": (
            "0-1: when the moment invited teasing or wit, did Seth land it in "
            "his dry, deadpan, self-deprecating voice? Bands: genuinely "
            "funny-in-voice (a real comeback, deadpan, absurd/exaggerated, "
            "self-deprecating, builds on a bit) = 0.8-1.0. Flat: missed an "
            "obvious setup, replied earnestly or literally where wit was teed "
            "up = 0.4-0.6. Corny / forced / trying-too-hard / dad-joke / cutesy "
            "assistant-humor / laughing at his own joke = 0.0-0.3 (WORSE than "
            "flat — forced humor is a distinct, more damaging failure). "
            "Context override: if the moment called for seriousness (friend "
            "genuinely upset, grief), NOT joking is correct — score neutral-high "
            "and score any joke there LOW as a misread. If no humor beat arose "
            "at all, score 0.7 (neutral).")},
        "what_worked": {"type": "ARRAY", "items": {"type": "STRING"}},
        "what_failed": {"type": "ARRAY", "items": {"type": "STRING"}},
        "summary": {"type": "STRING"},
        "turns": {"type": "ARRAY", "items": {"type": "OBJECT", "properties": {
            "turn_index": {"type": "INTEGER",
                           "description": "index into the numbered transcript"},
            "score": {"type": "NUMBER", "description": "0-1 humanness of this Seth turn"},
            "ai_tells": {"type": "ARRAY", "items": {"type": "STRING"}},
            "better_reply": {"type": "STRING",
                             "description": "in-voice rewrite when score < 0.7, else empty"},
            "confidence": {"type": "NUMBER"},
        }, "required": ["turn_index", "score", "ai_tells", "better_reply", "confidence"]}},
    },
    "required": ["overall_humanness", "voice_consistency", "engagement", "humor",
                 "what_worked", "what_failed", "summary", "turns"],
}


def judge_conversation(project: str, scenario: dict, transcript: list[dict]) -> dict:
    numbered = "\n".join(f"[{i}] {'Them' if m['role'] == 'them' else 'Seth'}: {m['text']}"
                         for i, m in enumerate(transcript))
    prompt = (
        "You are evaluating whether 'Seth' in this iMessage conversation reads as a "
        "real 45yo tech-entrepreneur texting a friend, or as an AI assistant wearing "
        "his skin. The friend's hidden agenda was: " + scenario["goal"] + "\n\n"
        "Transcript (indices in brackets):\n" + numbered + "\n\n"
        "Judge ONLY the Seth turns. AI tells include: assistant openers (Certainly/"
        "Great question), over-answering low-effort messages, essay length, relentless "
        "follow-up questions, emotional over-service, fabricated specific memories, "
        "perfect punctuation everywhere, abandoning his own stated opinions under "
        "trivial pushback. Real-human moves include: brevity, restraint, not answering "
        "everything, holding opinions warmly, matched energy. For each Seth turn give "
        "turn_index (its bracket number), a 0-1 score, ai_tells, a better_reply "
        "in Seth's casual voice when score < 0.7, and your confidence.\n\n"
        "Also rate 'humor' for the whole conversation. Seth's real humor is dry, "
        "deadpan, self-deprecating, playful roasting, absurd/exaggerated, tech-nerd "
        "— e.g. 'you should eat healthier' -> 'I had a salad... on my burger'; "
        "'you're so old' -> 'I prefer vintage'. When the friend teed up an obvious "
        "roast, callback bit, or self-deprecation setup and Seth volleyed in that "
        "voice, score humor high. If he whiffed an obvious setup (earnest/literal), "
        "score mid. If he was corny, forced, tried too hard, or used cutesy "
        "assistant-humor, score LOW — that is worse than flat. If the moment was "
        "genuinely serious (real distress), staying serious is correct and joking "
        "there is the failure. Follow the humor field's band descriptions exactly.")
    raw = gemini(project, JUDGE_MODEL, prompt, schema=JUDGE_SCHEMA,
                 temperature=0.2, max_tokens=4096)
    return json.loads(raw)


# ── Persistence ──────────────────────────────────────────────────────────────

def insert_dpo_rows(db_path: str, rows: list[dict]) -> int:
    if not rows:
        return 0
    conn = sqlite3.connect(db_path)
    try:
        now_ms = int(time.time() * 1000)
        conn.executemany(
            "INSERT INTO dpo_pairs(prompt, chosen, rejected, margin, timestamp, source) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            [(r["prompt"], r["chosen"], r["rejected"], r["margin"], now_ms, r["source"])
             for r in rows])
        conn.commit()
        return len(rows)
    finally:
        conn.close()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--conversations", type=int, default=2)
    ap.add_argument("--turns", type=int, default=8,
                    help="total messages per conversation (them+seth alternating)")
    ap.add_argument("--server", default="http://127.0.0.1:8741")
    ap.add_argument("--model", default="gemma-4-31b-it-8bit")
    ap.add_argument("--human-bin", default=str(Path("~/.local/bin/human-daemon").expanduser()))
    ap.add_argument("--persona", default="seth")
    ap.add_argument("--channel", default="imessage")
    ap.add_argument("--project", default="johnb-2025")
    ap.add_argument("--db", default=str(Path("~/.human/memory.db").expanduser()))
    ap.add_argument("--max-tokens", type=int, default=100)
    ap.add_argument("--gen-timeout", type=int, default=2400)
    ap.add_argument("--min-margin", type=float, default=0.3)
    ap.add_argument("--no-dpo", action="store_true",
                    help="do not write flagged turns into dpo_pairs")
    ap.add_argument("--scenario", help="run one specific scenario id")
    args = ap.parse_args(argv)

    errs = [e for sc in SCENARIOS for e in validate_scenario(sc)]
    if errs:
        print("scenario validation FAILED:", *errs, sep="\n  ")
        return 2

    base = production_prompt(args.human_bin, args.persona, args.channel)
    if not base:
        base = production_prompt(str(ROOT / "build/human"), args.persona, args.channel)
    if not base:
        print("cannot dump production persona prompt; is the binary present?",
              file=sys.stderr)
        return 2

    ARENA_DIR.mkdir(parents=True, exist_ok=True)
    ts = datetime.datetime.now().astimezone().strftime("%Y%m%d-%H%M%S")
    run_path = ARENA_DIR / f"run-{ts}.jsonl"
    scoreboard_path = ARENA_DIR / "scoreboard.jsonl"

    if args.scenario:
        pool = [sc for sc in SCENARIOS if sc["id"] == args.scenario]
        if not pool:
            print(f"unknown scenario id {args.scenario}")
            return 2
    else:
        pool = random.sample(SCENARIOS, k=min(args.conversations, len(SCENARIOS)))

    total_dpo = 0
    for sc in pool[:args.conversations] if not args.scenario else pool:
        print(f"=== {sc['id']} ({sc['persona']})", flush=True)
        transcript = [{"role": "them", "text": sc["opener"]}]
        print(f"  Them: {sc['opener']}", flush=True)
        try:
            while len(transcript) < args.turns:
                reply = seth_reply(args.server, args.model, base, transcript,
                                   args.max_tokens, args.gen_timeout)
                transcript.append({"role": "seth", "text": reply})
                print(f"  Seth: {reply}", flush=True)
                if len(transcript) >= args.turns:
                    break
                nxt = contact_turn(args.project, sc, transcript)
                transcript.append({"role": "them", "text": nxt})
                print(f"  Them: {nxt}", flush=True)
        except Exception as e:  # noqa: BLE001
            print(f"generation unavailable ({e}); aborting run", file=sys.stderr)
            return 2

        try:
            judgment = judge_conversation(args.project, sc, transcript)
        except Exception as e:  # noqa: BLE001
            print(f"judge unavailable ({e}); transcript saved unjudged", file=sys.stderr)
            judgment = None

        row = {"ts": ts, "scenario": sc["id"], "transcript": transcript,
               "judgment": judgment}
        with open(run_path, "a") as f:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")

        if judgment:
            print(f"  humanness={judgment['overall_humanness']:.2f} "
                  f"voice={judgment['voice_consistency']:.2f} "
                  f"engagement={judgment['engagement']:.2f} "
                  f"humor={judgment.get('humor', 0.0):.2f}", flush=True)
            for w in judgment.get("what_failed", [])[:3]:
                print(f"  failed: {w}", flush=True)
            sb = {"ts": ts, "scenario": sc["id"],
                  "overall_humanness": judgment["overall_humanness"],
                  "voice_consistency": judgment["voice_consistency"],
                  "engagement": judgment["engagement"],
                  "humor": judgment.get("humor", 0.0),
                  "n_turns": len(transcript)}
            with open(scoreboard_path, "a") as f:
                f.write(json.dumps(sb) + "\n")
            if not args.no_dpo:
                rows = build_dpo_rows(judgment, transcript, min_margin=args.min_margin)
                n = insert_dpo_rows(args.db, rows)
                total_dpo += n
                if n:
                    print(f"  -> {n} arena DPO pair(s) written", flush=True)

    board = [json.loads(ln) for ln in scoreboard_path.read_text().splitlines()
             if ln.strip()] if scoreboard_path.exists() else []
    trend = scoreboard_trend(board)
    humor_rows = [r for r in board if r.get("scenario") in HUMOR_SCENARIO_IDS]
    humor_axis = axis_spread(humor_rows, "humor")
    print(json.dumps({"run": str(run_path), "dpo_pairs_written": total_dpo,
                      "trend": trend, "humor_axis": humor_axis}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
