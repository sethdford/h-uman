#!/usr/bin/env python3
"""Inverse-Turing LLM judge tier (contract C4).

For each trial (real_seth vs ai_response, same context) asks
gemini-3.1-pro-preview via Vertex ADC which of A/B is the human-written
reply. A/B order is randomised per trial (seeded on trial index, so the run
is reproducible) and the mapping is recorded per row. Judge accuracy and AUC
(from the judge's stated 1-10 confidence) are computed and written to
~/.human/logs/llm-judge-<date>.json.

Privacy: the output JSON stores ids and votes only -- context/real_seth/
ai_response text is NEVER written to the summary file. It is held in memory
only for the duration of the judge call.

Refuses (exit non-zero, writes nothing) when fewer than --min-trials trials
are available, or when neither GEMINI_API_KEY nor Application Default
Credentials are available (unless HU_JUDGE_FAKE=1, the unit-test escape
hatch -- see test_llm_judge_tier.py).

Request pattern (model, responseSchema, explicit thinkingConfig.thinkingBudget)
follows scripts/eval_blinded_ab.py's judge_gen_config/call_gemini -- gemini-3.x
shares maxOutputTokens between invisible thinking and the visible reply, so an
unset budget can silently truncate/starve the judgment (see that file's
JUDGE_THINKING_BUDGET comment and CLAUDE.md's Gemini 3.x thinking-budget note).
"""
import argparse
import json
import os
import random
import sys
import time
import urllib.parse
import urllib.request

API_KEY = os.environ.get("GEMINI_API_KEY", "")
PROJECT_ID = os.environ.get("GOOGLE_CLOUD_PROJECT", "johnb-2025")
JUDGE_MODEL = "gemini-3.1-pro-preview"
JUDGE_THINKING_BUDGET = 1024
JUDGE_MAX_OUTPUT_TOKENS = 4096

_adc_token_cache = {"token": None, "expires": 0}

_JUDGE_SCHEMA = {
    "type": "object",
    "properties": {
        "choice": {"type": "string", "enum": ["A", "B"]},
        "confidence": {"type": "integer", "minimum": 1, "maximum": 10},
        "reasoning": {"type": "string"},
    },
    "required": ["choice", "confidence", "reasoning"],
    "propertyOrdering": ["choice", "confidence", "reasoning"],
}


def _get_adc_token():
    if _adc_token_cache["token"] and time.time() < _adc_token_cache["expires"] - 60:
        return _adc_token_cache["token"]
    creds_path = os.path.expanduser("~/.config/gcloud/application_default_credentials.json")
    if not os.path.exists(creds_path):
        return None
    with open(creds_path) as f:
        creds = json.load(f)
    payload = urllib.parse.urlencode({
        "client_id": creds["client_id"],
        "client_secret": creds["client_secret"],
        "refresh_token": creds["refresh_token"],
        "grant_type": "refresh_token",
    }).encode()
    req = urllib.request.Request(
        "https://oauth2.googleapis.com/token", data=payload,
        headers={"Content-Type": "application/x-www-form-urlencoded"})
    resp = urllib.request.urlopen(req, timeout=10)
    data = json.loads(resp.read())
    _adc_token_cache["token"] = data["access_token"]
    _adc_token_cache["expires"] = time.time() + data.get("expires_in", 3600)
    return data["access_token"]


def adc_available():
    if API_KEY:
        return True
    try:
        return _get_adc_token() is not None
    except Exception:
        return False


def _gemini_url():
    if API_KEY:
        return f"https://generativelanguage.googleapis.com/v1beta/models/{JUDGE_MODEL}:generateContent?key={API_KEY}"
    return (f"https://aiplatform.googleapis.com/v1/projects/{PROJECT_ID}/locations/global/"
            f"publishers/google/models/{JUDGE_MODEL}:generateContent")


def judge_gen_config(temperature=0.2, response_schema=_JUDGE_SCHEMA):
    cfg = {
        "temperature": temperature,
        "maxOutputTokens": JUDGE_MAX_OUTPUT_TOKENS,
        # gemini-3.x is thinking-enabled by default and shares maxOutputTokens
        # between the invisible thinking and the visible reply -- an unset
        # budget can starve the JSON body. Always set this explicitly.
        "thinkingConfig": {"thinkingBudget": JUDGE_THINKING_BUDGET},
    }
    if response_schema is not None:
        cfg["responseMimeType"] = "application/json"
        cfg["responseSchema"] = response_schema
    return cfg


def call_gemini(prompt, temperature=0.2, timeout_s=30):
    payload = json.dumps({
        "contents": [{"role": "user", "parts": [{"text": prompt}]}],
        "generationConfig": judge_gen_config(temperature),
    }).encode()
    headers = {"Content-Type": "application/json"}
    if not API_KEY:
        token = _get_adc_token()
        if not token:
            raise RuntimeError("No GEMINI_API_KEY and no ADC credentials found")
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(_gemini_url(), data=payload, headers=headers)
    resp = urllib.request.urlopen(req, timeout=timeout_s)
    data = json.loads(resp.read())
    return data["candidates"][0]["content"]["parts"][0]["text"]


def _loads_json_lenient(raw):
    s = raw
    if "```json" in s:
        s = s.split("```json")[1].split("```")[0].strip()
    elif "```" in s:
        s = s.split("```")[1].split("```")[0].strip()
    return json.loads(s)


def build_prompt(context, response_a, response_b):
    return f"""You are a forensic linguistic analyst determining which text message reply was written by a real human and which by an AI impersonating that human.

CONTEXT: A friend sent this message:
"{context}"

Two replies were generated. ONE is from the real human. The OTHER is from an AI trying to impersonate that human. Determine which is human.

REPLY A: "{response_a}"
REPLY B: "{response_b}"

Analyze for: natural language patterns (contractions, fragments, lowercase, typos), emotional authenticity, length appropriateness (humans text brief), AI tells (hedging, over-helpfulness, bullet points, "certainly", excessive empathy), personality/opinions, texting conventions.

Return ONLY valid JSON: {{"choice": "A" or "B", "confidence": 1-10, "reasoning": "brief explanation"}}"""


def assign_order(seed, i):
    """Deterministic per-trial A/B coin. Returns human_is ('A' or 'B') -- which
    slot the REAL reply was placed in. Mirrors eval_blinded_ab.py's
    `random.Random(f"{SEED}:{i}").random() < 0.5` pattern so trial->coin is
    reproducible and independent of run order."""
    coin = random.Random(f"{seed}:{i}").random() < 0.5
    return "A" if coin else "B"


def judge_trial(trial_id, context, real_seth, ai_response, seed, i, judge_fn):
    """Run one trial through judge_fn(context, a, b) -> {"choice","confidence",...}.

    judge_fn is injected so tests can swap in a fake judge with no network
    (see test_llm_judge_tier.py / HU_JUDGE_FAKE=1). Returns a row dict
    containing NO reply text -- only the trial id, the recorded A/B mapping,
    the judge's vote, and its confidence.
    """
    human_is = assign_order(seed, i)
    if human_is == "A":
        a_text, b_text = real_seth, ai_response
    else:
        a_text, b_text = ai_response, real_seth
    judgment = judge_fn(context, a_text, b_text)
    choice = judgment.get("choice")
    confidence = int(judgment.get("confidence", 0))
    correct = (choice == human_is)
    return {
        "id": trial_id,
        "human_is": human_is,
        "judge_choice": choice,
        "confidence": confidence,
        "correct": correct,
    }


def score_for_auc(row):
    """(human_score, ai_score) in [0,1] derived from one judged row.

    conf_norm in [0.1, 1.0] scales how far the judge's stated confidence
    pushes the score away from the 0.5 coin-flip midpoint. When the judge
    picked the real reply, the real reply's score is pushed UP by conf_norm;
    when fooled, it's pushed DOWN by conf_norm. ai_score is the complement,
    so the pair always sums to 1 -- exactly the forced-choice structure of
    the task (the judge chose between exactly two candidates).
    """
    conf_norm = max(1, min(10, row["confidence"])) / 10.0
    human_score = 0.5 + conf_norm * 0.5 if row["correct"] else 0.5 - conf_norm * 0.5
    return human_score, 1.0 - human_score


def compute_auc(pos_scores, neg_scores):
    """Mann-Whitney U AUC: P(a random positive scores higher than a random
    negative), with ties broken by average rank. None if either side is empty.

    This is the textbook U-statistic estimator
    (AUC = (rank_sum(pos) - n1*(n1+1)/2) / (n1*n2)); no sklearn dependency.
    """
    n1, n2 = len(pos_scores), len(neg_scores)
    if n1 == 0 or n2 == 0:
        return None
    combined = sorted([(v, 1) for v in pos_scores] + [(v, 0) for v in neg_scores], key=lambda x: x[0])
    n = len(combined)
    rank_sum_pos = 0.0
    idx = 0
    while idx < n:
        j = idx
        while j < n and combined[j][0] == combined[idx][0]:
            j += 1
        avg_rank = (idx + 1 + j) / 2.0  # 1-indexed rank, averaged across the tie block
        for k in range(idx, j):
            if combined[k][1] == 1:
                rank_sum_pos += avg_rank
        idx = j
    return (rank_sum_pos - n1 * (n1 + 1) / 2.0) / (n1 * n2)


def gemini_judge_fn(context, a_text, b_text):
    raw = call_gemini(build_prompt(context, a_text, b_text))
    return _loads_json_lenient(raw)


def fake_judge_fn(context, a_text, b_text):
    """HU_JUDGE_FAKE=1 escape hatch: no network. Deterministic per-call pick
    driven by a fixed seed derived from the text content, so unit tests can
    assert the exact accuracy/AUC without needing to know assign_order's
    internal coin sequence."""
    r = random.Random(f"fake:{hash((context, a_text, b_text)) & 0xffffffff}")
    choice = "A" if r.random() < float(os.environ.get("HU_JUDGE_FAKE_P_A", "0.5")) else "B"
    confidence = int(os.environ.get("HU_JUDGE_FAKE_CONFIDENCE", "7"))
    return {"choice": choice, "confidence": confidence, "reasoning": "fake"}


def run(trials, seed=1337, min_trials=20, judge_fn=None):
    """Core orchestration, pure enough to unit test: trials in, summary dict +
    rows out. Raises SystemExit (never writes) if len(trials) < min_trials."""
    if len(trials) < min_trials:
        raise SystemExit(f"REFUSING: {len(trials)} trials < --min-trials {min_trials}; nothing written")
    judge_fn = judge_fn or gemini_judge_fn
    rows = []
    for i, t in enumerate(trials):
        tid = t.get("i", f"item_{i:02d}")
        row = judge_trial(tid, t["context"], t["real_seth"], t["ai_response"], seed, i, judge_fn)
        rows.append(row)

    n = len(rows)
    accuracy = sum(1 for r in rows if r["correct"]) / n
    pos_scores, neg_scores = [], []
    for r in rows:
        h, a = score_for_auc(r)
        pos_scores.append(h)
        neg_scores.append(a)
    auc = compute_auc(pos_scores, neg_scores)

    summary = {
        "date": time.strftime("%Y-%m-%d"),
        "judge_model": JUDGE_MODEL,
        "n": n,
        "accuracy": round(accuracy, 3),
        "auc": round(auc, 3) if auc is not None else None,
        "seed": seed,
        "rows": rows,
    }
    return summary


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--trials", default=os.path.expanduser("~/blind_ab_run/classifier_trials.json"))
    ap.add_argument("--out", default=os.path.expanduser("~/.human/logs/llm-judge-%s.json" % time.strftime("%Y-%m-%d")))
    ap.add_argument("--min-trials", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1337)
    a = ap.parse_args()

    fake = os.environ.get("HU_JUDGE_FAKE") == "1"
    if not fake and not adc_available():
        sys.exit("REFUSING: no GEMINI_API_KEY and no Application Default Credentials found; nothing written")

    if not os.path.isfile(a.trials):
        sys.exit(f"REFUSING: trials file not found: {a.trials}; nothing written")
    try:
        data = json.load(open(a.trials))
    except Exception as e:
        sys.exit(f"REFUSING: could not parse {a.trials} ({type(e).__name__}: {e}); nothing written")
    trials = data.get("trials") if isinstance(data, dict) else data
    trials = [t for t in (trials or []) if isinstance(t, dict) and t.get("context") and t.get("real_seth") and t.get("ai_response")]

    judge_fn = fake_judge_fn if fake else gemini_judge_fn
    try:
        summary = run(trials, seed=a.seed, min_trials=a.min_trials, judge_fn=judge_fn)
    except SystemExit as e:
        sys.exit(str(e))

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    json.dump(summary, open(a.out, "w"), indent=2)
    print(json.dumps({k: v for k, v in summary.items() if k != "rows"}, indent=1))
    print(f"n_rows={len(summary['rows'])} wrote {a.out}")


if __name__ == "__main__":
    main()
