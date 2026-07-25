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

    # ── Humor probes (humor_probe=True) ──────────────────────────────────────
    # These exist to measure the HUMOR axis specifically: four that create a
    # real, DISTINCT opening for teasing (a roast to volley, an absurd straight
    # line begging a deadpan, a self-own to deflect, a running-bit callback to
    # build on), and one that BAITS humor at a moment where joking is wrong.
    # humor_wrong_moment is the point — an axis that only rewards more jokes
    # would rate a try-hard daemon highly; it is how forced humor gets caught.
    # Each probe targets a different in-voice move so the humor axis samples a
    # RANGE (see axis_spread): a judge that collapses them to one score cannot
    # gate anything. (Folded 2026-07-25 from the parallel 3cb5970d arena — the
    # near-duplicate roast/self-deprecation/deadpan probes there mapped onto
    # these four; callback_joke was the one genuinely new intent kept.)
    {"id": "roast_volley", "persona": "college buddy who roasts Seth constantly",
     "goal": "roast Seth about something dumb he did, keep volleying — see if he "
             "teases back or goes flat/defensive",
     "opener": "bro i still cannot believe you paid for the extended warranty lmaooo",
     "humor_probe": True},
    {"id": "straight_line", "persona": "friend who unknowingly feeds perfect setups",
     "goal": "make earnest, slightly absurd statements that beg for a deadpan reply",
     "opener": "i've decided i'm waking up at 4am every day starting monday",
     "humor_probe": True},
    {"id": "self_own", "persona": "friend fishing for Seth to admit he screwed up",
     "goal": "gently call out something Seth got wrong, see if he self-deprecates "
             "or gets defensive",
     "opener": "so how'd that whole 'i'll definitely finish it this weekend' thing go",
     "humor_probe": True},
    {"id": "callback_joke", "persona": "friend reviving a running bit from a past chat",
     "goal": "resurrect the old 'salad on your burger' inside joke and see if Seth "
             "recognizes the bit and BUILDS on it (deadpan) vs taking it literally, "
             "ignoring it, or fabricating a fake shared memory",
     "opener": "so did you have another 'salad' for lunch today or",
     "humor_probe": True},
    {"id": "humor_wrong_moment", "persona": "friend who just got bad family news",
     "goal": "share the bad news plainly, then make ONE weak deflecting joke yourself "
             "— see if Seth piles on jokes or reads the room",
     "opener": "hey so my dad's scan came back not great. anyway. how are you",
     "humor_probe": True},
]

# Scenario ids whose primary purpose is exercising Seth's teasing/joking
# register. main() reports the humor-axis spread over just these rows — the
# humor axis is only a meaningful measurement where a humor beat was invited.
HUMOR_PROBE_IDS = frozenset(
    sc["id"] for sc in SCENARIOS if sc.get("humor_probe"))


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


def scoreboard_trend(rows: list[dict], window: int = 10,
                     key: str = "overall_humanness") -> dict:
    vals = [float(r.get(key, 0.0)) for r in rows]
    if not vals:
        return {"n": 0, "recent_mean": 0.0, "all_mean": 0.0}
    recent = vals[-window:]
    return {"n": len(vals), "recent_mean": sum(recent) / len(recent),
            "all_mean": sum(vals) / len(vals)}


ARM_AXES = ("humor", "humor_forced", "overall_humanness", "voice_consistency",
            "engagement")


def arm_summary(rows: list[dict], tag: str) -> dict:
    """Mean of every axis for the scoreboard rows belonging to one A/B arm."""
    sel = [r for r in rows if r.get("tag") == tag]
    out = {"tag": tag, "n": len(sel)}
    for axis in ARM_AXES:
        vals = [float(r[axis]) for r in sel if axis in r]
        out[axis] = (sum(vals) / len(vals)) if vals else 0.0
    return out


def compare_arms(rows: list[dict], off_tag: str, live_tag: str,
                 min_humor_gain: float = 0.02,
                 max_voice_drop: float = 0.10,
                 max_forced: float = 0.50) -> dict:
    """Decide whether a humor change earned promotion from SHADOW to LIVE.

    Encodes the project's gate contract: humor must measurably improve AND
    voice must not pay for it. The 'forced humor' failure mode looks like
    humor up + voice down, so a voice regression VETOES promotion even when
    the humor number rises. humor_forced is a second, independent veto — a
    try-hard arm can score well on landing yet still read as performing.

    Thresholds are the operator's "bias to shipping" setting (2026-07-22):
    a small humor gain is enough, and up to 0.10 of voice cost is tolerated.
    The vetoes themselves are NOT tunable away — they are the structure that
    keeps a humor win from silently buying a voice loss.
    """
    off, live = arm_summary(rows, off_tag), arm_summary(rows, live_tag)
    humor_gain = live["humor"] - off["humor"]
    voice_delta = live["voice_consistency"] - off["voice_consistency"]
    humanness_delta = live["overall_humanness"] - off["overall_humanness"]
    forced_delta = live["humor_forced"] - off["humor_forced"]

    reasons = []
    if off["n"] == 0 or live["n"] == 0:
        reasons.append("missing arm data — cannot decide")
    if humor_gain < min_humor_gain:
        reasons.append(f"humor gain {humor_gain:+.3f} < required +{min_humor_gain}")
    if voice_delta < -max_voice_drop:
        reasons.append(f"voice regressed {voice_delta:+.3f} (forced-humor signature)")
    if humanness_delta < -max_voice_drop:
        reasons.append(f"overall humanness regressed {humanness_delta:+.3f}")
    if live["humor_forced"] > max_forced:
        reasons.append(f"humor_forced {live['humor_forced']:.3f} > {max_forced}")

    return {"off": off, "live": live,
            "humor_gain": humor_gain, "voice_delta": voice_delta,
            "humanness_delta": humanness_delta, "forced_delta": forced_delta,
            "promote": not reasons,
            "reasons": reasons or ["all promotion criteria met"]}


# Below this humor-axis spread, the judge is not discriminating between the
# probe scenarios — it is stamping a near-constant score, which cannot gate a
# prompt change. Soft: a warning, not an abort (a run may legitimately have too
# few probe rows). Folded 2026-07-25 from the parallel 3cb5970d arena.
HUMOR_SPREAD_MIN = 0.15


def axis_spread(rows: list[dict], key: str) -> dict:
    """Spread of one judge axis across scoreboard rows. This is the measurement's
    own trust check: a judge that discriminates produces a RANGE of scores across
    scenarios; one that collapses every turn to ~1.0 (spread ~0) is not measuring
    anything and cannot gate prompt tuning. Returns n, mean, min, max, and spread
    (max - min). Rows missing the key are skipped (old rows predate the axis)."""
    vals = [float(r[key]) for r in rows if r.get(key) is not None]
    if not vals:
        return {"n": 0, "mean": 0.0, "min": 0.0, "max": 0.0, "spread": 0.0}
    lo, hi = min(vals), max(vals)
    return {"n": len(vals), "mean": sum(vals) / len(vals),
            "min": lo, "max": hi, "spread": hi - lo}


def humor_axis_warning(axis: dict, min_spread: float = HUMOR_SPREAD_MIN) -> str | None:
    """Soft gate on the humor measurement's trust check. Returns a warning string
    when the judge failed to discriminate (spread collapsed) across enough rows to
    matter, else None. A collapsed spread means the humor number is a rubber stamp
    and must not be trusted to gate a prompt change. Applies ONLY to the 'humor'
    axis — humor_forced can legitimately sit near 0 (no forced humor), so a small
    humor_forced spread is not a rubber-stamp signal and is not gated here."""
    if axis.get("n", 0) < 2:
        return None  # too few probe rows to judge discrimination
    if axis.get("spread", 0.0) < min_spread:
        return (f"humor axis spread {axis['spread']:.3f} < {min_spread} across "
                f"{axis['n']} rows — judge may not be discriminating (rubber "
                f"stamp); do not trust the humor number to gate prompt changes")
    return None


def humor_probe_pool(scenarios: list[dict], n: int) -> tuple[list[dict], list[str]]:
    """Deterministic humor-probe selection for --humor-only, plus the ids it had
    to drop. An A/B needs both arms to run the SAME probes, so the order is fixed
    (not sampled). Truncating below the full set can silently exclude the
    anti-forced-humor guard (humor_wrong_moment) and rig the measurement — the
    caller MUST surface `dropped` rather than cap coverage in silence."""
    probes = [sc for sc in scenarios if sc.get("humor_probe")]
    return probes[:n], [sc["id"] for sc in probes[n:]]


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
        "humor": {"type": "NUMBER",
                  "description": "0-1: did Seth's humor/teasing LAND. Measures quality "
                                 "of landing, NOT quantity of jokes. Restraint when "
                                 "humor did not fit scores neutral-high, never low."},
        "humor_forced": {"type": "NUMBER",
                         "description": "0-1: how try-hard, corny, or joke-crammed Seth "
                                        "was. HIGHER IS WORSE. 0.0 = never strained."},
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
    "required": ["overall_humanness", "voice_consistency", "engagement",
                 "humor", "humor_forced",
                 "what_worked", "what_failed", "summary", "turns"],
}

# Seth's humor rubric — the taste-dependent half of the HUMOR axis.
# Kept as a named constant (not inlined in the prompt) so it can be edited and
# diffed on its own: this text IS the measurement definition, and step 3's
# promote/don't-promote decision is only as trustworthy as this wording.
HUMOR_RUBRIC = """
HUMOR AXIS — score two separate things.

'humor' (0-1) = did Seth's humor LAND, given the openings this conversation
actually offered. Seth's real register is dry, deadpan, self-deprecating, and
teasing toward people he's close to — short, thrown away, never explained.
  HIGH: a tease that shows he knows the person; a dry one-liner that undercuts
        without being mean; self-deprecation instead of defensiveness; a
        deadpan non-answer to an absurd setup; playing along and escalating a
        bit rather than laughing politely.
  LOW:  laughing at a joke without adding anything ('haha same', 'lol nice');
        explaining the joke; a punchline that needed a setup he didn't earn;
        being mean rather than teasing; comedian-mode standup voice.

CRITICAL — restraint is not failure. If the conversation offered no real
opening, or humor would have been WRONG (someone is upset, grieving, sharing
bad news), then Seth NOT joking is correct behavior and 'humor' should score
0.6-0.8, never low. Only score 'humor' low when there WAS a clear opening and
he whiffed it or answered like an assistant. Never reward joking through
someone else's bad news.

NEVER-DURING override — if Seth actually DOES joke through genuine distress (a
friend's bad news, grief, real upset), that is a misread, not a landing. The two
humor axes must AGREE on it: score 'humor' LOW (a joke that shouldn't have been
made did not "land") AND 'humor_forced' HIGH (joking past a serious beat is the
definition of try-hard). This is the one case where restraint's neutral-high
floor does not apply — because he did not restrain.

'humor_forced' (0-1, HIGHER IS WORSE) = how try-hard he was.
  0.0-0.2: never strained; humor appeared only where it fit.
  0.3-0.6: a joke or two that didn't need to be there; slightly performing.
  0.7-1.0: cramming jokes into every turn; joking past a serious beat;
           quipping when a straight answer was called for; forcing his
           'personality' into a conversation that didn't ask for it.
A conversation with ZERO humor attempts has humor_forced = 0.0 by definition.
"""


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
        "in Seth's casual voice when score < 0.7, and your confidence.\n"
        + HUMOR_RUBRIC)
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
    ap.add_argument("--humor-only", action="store_true",
                    help="run only the humor_probe scenarios (HUMOR axis measurement)")
    ap.add_argument("--tag", help="label scoreboard rows with an arm name, e.g. "
                                  "humor-off / humor-live, for A/B comparison")
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
    elif args.humor_only:
        # Deterministic order, not a sample: an A/B needs both arms to run the
        # SAME scenarios, or the delta measures scenario luck, not the change.
        pool, dropped = humor_probe_pool(SCENARIOS, args.conversations)
        if dropped:
            # No silent caps: a truncated probe set can exclude the
            # anti-forced-humor guard and rig the A/B — surface what was dropped.
            full = len(pool) + len(dropped)
            print(f"WARN: --humor-only ran {len(pool)} of {full} probes; dropped "
                  f"{dropped}. Pass --conversations {full} to run the full probe "
                  f"set (the A/B needs every probe, incl. humor_wrong_moment).",
                  file=sys.stderr)
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
                  f"humor={judgment.get('humor', 0.0):.2f} "
                  f"forced={judgment.get('humor_forced', 0.0):.2f}", flush=True)
            for w in judgment.get("what_failed", [])[:3]:
                print(f"  failed: {w}", flush=True)
            sb = {"ts": ts, "scenario": sc["id"],
                  "overall_humanness": judgment["overall_humanness"],
                  "voice_consistency": judgment["voice_consistency"],
                  "engagement": judgment["engagement"],
                  "humor": judgment.get("humor", 0.0),
                  "humor_forced": judgment.get("humor_forced", 0.0),
                  "n_turns": len(transcript)}
            if args.tag:
                sb["tag"] = args.tag
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

    # Humor-axis trust check: report the spread over the humor-probe rows so a
    # judge that stopped discriminating is visible, not silent. Soft-gate a
    # collapsed 'humor' spread to stderr (a rubber-stamp judge cannot gate step-3
    # prompt tuning). humor_forced spread is reported as a plain diagnostic — it
    # can legitimately be near-zero, so it is not gated.
    humor_rows = [r for r in board if r.get("scenario") in HUMOR_PROBE_IDS]
    humor_axis = axis_spread(humor_rows, "humor")
    humor_forced_axis = axis_spread(humor_rows, "humor_forced")
    warn = humor_axis_warning(humor_axis)
    if warn:
        print(f"WARN: {warn}", file=sys.stderr)
    print(json.dumps({"run": str(run_path), "dpo_pairs_written": total_dpo,
                      "trend": trend, "humor_axis": humor_axis,
                      "humor_forced_axis": humor_forced_axis}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
