#!/usr/bin/env python3
"""Simulated blind-A/B raters — a research-grounded LLM-judge PROXY for the human run.

Reads triples.json ({id, context, seth_reply, huuman_reply}) and scores each pair
with a debiased Gemini judge panel that role-plays 3 rater personas (close friend
who knows Seth / skeptical stranger / linguist). Per pair it measures TWO axes:

  - DETECTION (Turing): can the panel pick Seth's REAL reply out of {real, h-uman}?
    detect_rate = P(panel picks the real one). 0.5 = indistinguishable (the goal).
  - ENDEARING PREFERENCE: which reply is warmer / more loveable / in-voice?
    endearing_pref = P(panel finds h-uman's reply MORE endearing than Seth's).
    "Better than human" = detect_rate ~0.5 AND endearing_pref > 0.5.

Debiasing (per MT-Bench / Zheng 2023 + PersonaEval 2508.10014):
  - position-swap: judge each pair in BOTH orders (A,B) and (B,A); a persona that
    flips its pick across orders is position-confused -> counts as chance (0.5).
  - rubric-anchored scoring to curb verbosity bias.
  - confidence-weighted aggregation across personas.

HONEST CAVEAT (printed in the report): LLM persona-raters reach ~69% accuracy vs
real humans' ~90.8% on persona discrimination (PersonaEval). So this proxy is
OPTIMISTIC + miscalibrated — a fast iteration signal, NOT the activation gate.
The human blind-A/B remains the gold standard.

Privacy: triples contain real private messages. Read from a non-repo path
(~/blind_ab_run). Never git-add the data. Only the aggregate result (no message
text) is shareable.

Usage:
  simulate_raters.py ~/blind_ab_run/triples.json --out ~/blind_ab_run/sim-results.json
  simulate_raters.py ... --limit 3        # quick smoke test
  simulate_raters.py ... --workers 6
"""
import argparse, json, os, random, sys, urllib.parse, urllib.request
from concurrent.futures import ThreadPoolExecutor

PROJECT_ID = os.environ.get("GOOGLE_CLOUD_PROJECT", "johnb-2025")
MODEL = "gemini-3.5-flash"
_URL = (f"https://aiplatform.googleapis.com/v1/projects/{PROJECT_ID}/locations/global/"
        f"publishers/google/models/{MODEL}:generateContent")

PERSONAS = {
    "close_friend": "You have known Seth for years over text. You can feel when a reply "
                    "isn't quite him — his terse lowercase style, dry humor, the way he "
                    "skips punctuation. Pick the reply that sounds like the REAL Seth.",
    "skeptical_stranger": "You are suspicious of AI chat. You hunt for AI tells: openers like "
                          "'Certainly!'/'Happy to help', over-hedging, perfect punctuation, "
                          "too-long or too-polished, canned empathy ('that sounds really hard'), "
                          "restating context. Pick the reply written by a real human.",
    "linguist": "You analyze register and rhythm. Real casual texting is brief, lowercase, "
                "uses lol/idk/omw, drops punctuation, has typos, trails off with '...'. "
                "AI text is uniform-case, complete, grammatical. Pick the genuinely human reply.",
}

# Top AI-tells (from rubric research: Markowitz 2024; sycophancy: Bashir 2024).
# IMPORTANT iMessage calibration: iOS AUTO-CAPITALIZES the first letter and auto-punctuates,
# so proper case + periods are NORMAL for real human iMessages. Do NOT use casing/punctuation
# as a humanness signal — it inverts the judgment on this platform.
RUBRIC = (
    "PLATFORM NOTE: these are iMessages. iOS auto-capitalizes sentences and auto-inserts "
    "punctuation, so capital letters and periods are NORMAL in real human texts here — DO NOT "
    "treat capitalization or punctuation as an AI tell, and do NOT treat all-lowercase as more "
    "human. Judge on CONTENT, not casing.\n"
    "AI TELLS (flag any you see): formal openers (Certainly!/Happy to help/Great question); "
    "one long block vs a short natural reply; hedging cluster (I'd say/arguably/somewhat); "
    "canned empathy (that sounds hard/you're valid); restating context already shared; "
    "generic non-specific phrasing; over-explaining. "
    "ENDEARING = warm WITHOUT sycophancy: specific callbacks, playful teasing, brevity that "
    "assumes comfort, mild vulnerability, reciprocity. NOT endearing = empty validation, "
    "overpraise, canned 'I'm here for you', never disagreeing."
)

SCHEMA = {
    "type": "object",
    "properties": {
        "raters": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "persona": {"type": "string"},
                    "human_choice": {"type": "string", "enum": ["A", "B"]},
                    "confidence": {"type": "integer"},
                    "humanness_A": {"type": "integer"}, "humanness_B": {"type": "integer"},
                    "endearing_A": {"type": "integer"}, "endearing_B": {"type": "integer"},
                    "voice_A": {"type": "integer"}, "voice_B": {"type": "integer"},
                    "ai_tells": {"type": "array", "items": {"type": "string"}},
                },
                "required": ["persona", "human_choice", "confidence",
                             "humanness_A", "humanness_B", "endearing_A", "endearing_B",
                             "voice_A", "voice_B"],
            },
        }
    },
    "required": ["raters"],
}


def _token():
    p = os.path.expanduser("~/.config/gcloud/application_default_credentials.json")
    with open(p) as f:
        c = json.load(f)
    body = urllib.parse.urlencode({
        "client_id": c["client_id"], "client_secret": c["client_secret"],
        "refresh_token": c["refresh_token"], "grant_type": "refresh_token",
    }).encode()
    req = urllib.request.Request("https://oauth2.googleapis.com/token", data=body,
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    return json.loads(urllib.request.urlopen(req, timeout=15).read())["access_token"]


_TOK = {"t": None}


def _call(prompt):
    if _TOK["t"] is None:
        _TOK["t"] = _token()
    body = json.dumps({
        "contents": [{"role": "user", "parts": [{"text": prompt}]}],
        "generationConfig": {
            "temperature": 0.4, "maxOutputTokens": 1200,
            "thinkingConfig": {"thinkingBudget": 0},   # CLAUDE.md gotcha: 0 or empty replies
            "responseMimeType": "application/json", "responseSchema": SCHEMA,
        },
    }).encode()
    req = urllib.request.Request(_URL, data=body, headers={
        "Authorization": f"Bearer {_TOK['t']}", "Content-Type": "application/json"})
    for attempt in range(3):
        try:
            r = json.loads(urllib.request.urlopen(req, timeout=40).read())
            txt = r["candidates"][0]["content"]["parts"][0]["text"]
            return json.loads(txt)
        except Exception as e:
            if attempt == 2:
                raise
            _TOK["t"] = _token()  # refresh + retry
    return None


def _prompt(context, reply_A, reply_B):
    persona_block = "\n".join(f"- {k}: {v}" for k, v in PERSONAS.items())
    return (
        "Two people replied to the same text message. One reply (A or B) is from a REAL human "
        "(Seth); the other was generated by an AI imitating him. For EACH of the three raters "
        "below, decide which reply is the REAL human, give confidence 0-100, and score BOTH "
        "replies 1-10 on humanness (real-texting feel), endearing (warm/loveable, not "
        "sycophantic), and voice (sounds like a specific real person).\n\n"
        f"{RUBRIC}\n\nRATERS:\n{persona_block}\n\n"
        f"CONTEXT (the message they're replying to):\n{context}\n\n"
        f"REPLY A:\n{reply_A}\n\nREPLY B:\n{reply_B}\n\n"
        "Return JSON with one entry per rater (persona name exactly as listed)."
    )


def _persona_axis(rater, real_is):
    """Return (picked_real:bool, conf, endear_real, endear_huuman, human_real, voice_real)."""
    pick = rater["human_choice"]
    picked_real = (pick == real_is)
    er = rater[f"endearing_{real_is}"]
    eh = rater[f"endearing_{'B' if real_is=='A' else 'A'}"]
    hr = rater[f"humanness_{real_is}"]
    vr = rater[f"voice_{real_is}"]
    return picked_real, rater["confidence"], er, eh, hr, vr


def judge_pair(triple):
    """Debiased panel verdict for one triple. Returns dict of aggregates + tells."""
    real, huuman = triple["seth_reply"], triple["huuman_reply"]
    ctx = triple["context"]
    # two orders: real as A, then real as B
    out = {"id": triple["id"], "tells": []}
    per_persona = {p: {"picks": [], "conf": [], "er": [], "eh": [], "hr": [], "vr": []}
                   for p in PERSONAS}
    for real_is, (a, b) in (("A", (real, huuman)), ("B", (huuman, real))):
        resp = _call(_prompt(ctx, a, b))
        if not resp:
            continue
        for rater in resp.get("raters", []):
            name = rater.get("persona", "").strip().lower().replace(" ", "_")
            name = next((p for p in PERSONAS if p in name or name in p), None)
            if not name:
                continue
            try:
                picked, conf, er, eh, hr, vr = _persona_axis(rater, real_is)
            except (KeyError, TypeError):
                continue
            per_persona[name]["picks"].append(picked)
            per_persona[name]["conf"].append(conf)
            per_persona[name]["er"].append(er); per_persona[name]["eh"].append(eh)
            per_persona[name]["hr"].append(hr); per_persona[name]["vr"].append(vr)
            out["tells"] += [t for t in (rater.get("ai_tells") or []) if not picked]

    # per-persona detection with position-swap debias: flips across orders => chance
    detect_votes, weights, endear_real, endear_huuman, human_real, voice_real = [], [], [], [], [], []
    for p, d in per_persona.items():
        if not d["picks"]:
            continue
        if len(d["picks"]) == 2 and d["picks"][0] != d["picks"][1]:
            p_real = 0.5  # position-confused
        else:
            p_real = sum(1 for x in d["picks"] if x) / len(d["picks"])
        detect_votes.append(p_real)
        weights.append(max(1, sum(d["conf"]) / len(d["conf"])))
        endear_real.append(sum(d["er"]) / len(d["er"]))
        endear_huuman.append(sum(d["eh"]) / len(d["eh"]))
        human_real.append(sum(d["hr"]) / len(d["hr"]))
        voice_real.append(sum(d["vr"]) / len(d["vr"]))
    if not detect_votes:
        return None
    tw = sum(weights)
    out["p_detect_real"] = sum(v * w for v, w in zip(detect_votes, weights)) / tw
    out["endear_real"] = sum(endear_real) / len(endear_real)
    out["endear_huuman"] = sum(endear_huuman) / len(endear_huuman)
    out["human_real"] = sum(human_real) / len(human_real)
    out["voice_real"] = sum(voice_real) / len(voice_real)
    out["huuman_more_endearing"] = out["endear_huuman"] > out["endear_real"]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("triples")
    ap.add_argument("--out", default=os.path.expanduser("~/blind_ab_run/sim-results.json"))
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--workers", type=int, default=6)
    a = ap.parse_args()

    triples = json.load(open(os.path.expanduser(a.triples)))
    triples = [t for t in triples if t.get("huuman_reply")]  # skip empties
    if a.limit:
        triples = triples[:a.limit]
    print(f"judging {len(triples)} pairs with {MODEL} (3 personas x 2 orders each)...",
          file=sys.stderr)

    results = []
    with ThreadPoolExecutor(max_workers=a.workers) as ex:
        for r in ex.map(lambda t: _safe(judge_pair, t), triples):
            if r:
                results.append(r)
                print(".", end="", flush=True, file=sys.stderr)
    print("", file=sys.stderr)
    if not results:
        print("no results (check ADC: gcloud auth application-default login)", file=sys.stderr)
        sys.exit(1)

    n = len(results)
    detect = sum(r["p_detect_real"] for r in results) / n
    # Wilson-ish stderr on detection
    import math
    se = math.sqrt(detect * (1 - detect) / n)
    endear_pref = sum(1 for r in results if r["huuman_more_endearing"]) / n
    seth_endear = sum(r["endear_real"] for r in results) / n
    huuman_endear = sum(r["endear_huuman"] for r in results) / n
    seth_human = sum(r["human_real"] for r in results) / n
    seth_voice = sum(r["voice_real"] for r in results) / n
    from collections import Counter
    tells = Counter(t.strip().lower()[:60] for r in results for t in r["tells"])

    distinguishability = abs(detect - 0.5)
    if distinguishability <= 0.15:
        verdict = "INDISTINGUISHABLE (judge near chance)"
    elif detect < 0.5:
        verdict = "DISTINGUISHABLE — judge reliably prefers h-uman (likely too polished/verbose vs Seth's real terseness, OR genuinely warmer; LLM judges under-rate authentic terseness)"
    else:
        verdict = "DISTINGUISHABLE — judge spots h-uman as AI"
    out = {
        "model": MODEL, "n_pairs": n,
        "detect_rate": round(detect, 3), "detect_stderr": round(se, 3),
        "distinguishability_abs": round(distinguishability, 3),
        "indistinguishable_proxy": distinguishability <= 0.15,
        "verdict": verdict,
        "endearing_pref_huuman": round(endear_pref, 3),
        "axes": {"seth_endearing": round(seth_endear, 2),
                 "huuman_endearing": round(huuman_endear, 2),
                 "seth_humanness": round(seth_human, 2),
                 "seth_voice": round(seth_voice, 2)},
        "top_ai_tells": tells.most_common(8),
        "caveat": "LLM-judge proxy. PersonaEval: LLM persona-raters ~69% vs humans ~90.8% on "
                  "persona discrimination -> this detect_rate is OPTIMISTIC. Dev signal only; "
                  "the human blind-A/B remains the activation gate.",
    }
    json.dump(out, open(os.path.expanduser(a.out), "w"), indent=2)

    print("\n================ SIMULATED BLIND-A/B (proxy) ================")
    print(f"pairs judged:            {n}")
    print(f"DETECT RATE:             {detect:.3f} ± {se:.3f}   (0.5 = indistinguishable; "
          f"|Δ0.5|={distinguishability:.3f})")
    print(f"VERDICT:                 {verdict}")
    print(f"endearing-pref (h-uman): {endear_pref:.3f}   (>0.5 = h-uman warmer than real Seth)")
    print(f"endearing  Seth {seth_endear:.2f} vs h-uman {huuman_endear:.2f}  |  "
          f"Seth humanness {seth_human:.2f}  voice {seth_voice:.2f}")
    print("top AI-tells the panel flagged in h-uman replies:")
    for t, c in tells.most_common(8):
        print(f"   {c:2d}x  {t}")
    print("CAVEAT: proxy only (LLM raters ~69% vs humans ~90.8%); human run is the gate.")
    print(f"wrote {a.out}")


def _safe(fn, t):
    try:
        return fn(t)
    except Exception as e:
        print(f"\n  pair {t.get('id')} failed: {e}", file=sys.stderr)
        return None


if __name__ == "__main__":
    main()
