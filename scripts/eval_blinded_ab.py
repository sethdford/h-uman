#!/usr/bin/env python3
"""
Blinded A/B Evaluation: AI vs Real Seth

Takes ground truth pairs (real incoming + real Seth reply), sends the same
incoming through the AI daemon, then presents both to an independent LLM
judge in randomized order. The judge picks which response sounds more human.

Usage:
  python3 scripts/eval_blinded_ab.py                      # CLI mode
  python3 scripts/eval_blinded_ab.py --gateway             # Live daemon mode
  python3 scripts/eval_blinded_ab.py --gateway --synthetic  # Include synthetic scenarios
"""

import json
import os
import random
import subprocess
import tempfile
import sys
import time
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

API_KEY = os.environ.get("GEMINI_API_KEY", "")
PROJECT_ID = os.environ.get("GOOGLE_CLOUD_PROJECT", "johnb-2025")
EVAL_MODEL = "gemini-3.1-pro-preview"

_adc_token_cache = {"token": None, "expires": 0}

def _get_adc_token():
    """Get an OAuth2 access token from Application Default Credentials."""
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
    req = urllib.request.Request("https://oauth2.googleapis.com/token",
                                 data=payload, headers={"Content-Type": "application/x-www-form-urlencoded"})
    resp = urllib.request.urlopen(req, timeout=10)
    data = json.loads(resp.read())
    _adc_token_cache["token"] = data["access_token"]
    _adc_token_cache["expires"] = time.time() + data.get("expires_in", 3600)
    return data["access_token"]


def _gemini_url():
    if API_KEY:
        return f"https://generativelanguage.googleapis.com/v1beta/models/{EVAL_MODEL}:generateContent?key={API_KEY}"
    return (f"https://aiplatform.googleapis.com/v1/projects/{PROJECT_ID}/locations/global/"
            f"publishers/google/models/{EVAL_MODEL}:generateContent")

# Resolved after gateway_url_from_config/_load_human_config are defined, so the
# configured daemon port wins over the legacy hardcoded :3002.
GATEWAY_URL = None
MLX_URL = os.environ.get("MLX_URL", "http://127.0.0.1:8741/v1/chat/completions")
MLX_TIMEOUT_S = 120
USE_GATEWAY = "--gateway" in sys.argv
USE_SYNTHETIC = "--synthetic" in sys.argv
USE_MLX = "--mlx" in sys.argv
USE_GATE = "--gate" in sys.argv
GATE_DRY_RUN = "--gate-dry-run" in sys.argv
# Advisory Binoculars AI-tell metric appended to the results JSON after the
# run (docs/research/2026-07-25-binoculars-discriminator.md). Measurement-side
# only: never feeds the gate, never changes the exit code. ~12 min GPU.
USE_BINOCULARS = "--binoculars" in sys.argv
MAX_TRIALS = 50
# Gemini judge runs concurrently in a thread pool while MLX generation (serial,
# model_lock-bound) continues — see the main loop. Default kept modest to stay
# gentle on the judge API; raise with --judge-workers=N.
JUDGE_WORKERS = 6
# Seeding the A/B coin per pair index makes the human/AI assignment reproducible
# AND order-independent (safe under concurrent judging). Override with --seed=N.
SEED = 1337

for arg in sys.argv:
    if arg.startswith("--max-trials="):
        MAX_TRIALS = int(arg.split("=")[1])
    elif arg.startswith("--judge-workers="):
        JUDGE_WORKERS = max(1, int(arg.split("=")[1]))
    elif arg.startswith("--seed="):
        SEED = int(arg.split("=")[1])

import blind_ab_gate as _gate
_GATE_PATH = os.environ.get("HU_BLIND_AB_GATE_PATH", _gate.GATE_PATH)


def _git_commit():
    """Get the current git commit SHA."""
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True).strip()
    except Exception:
        return None


def _load_baseline():
    """Load the previous fool_rate from the gate file."""
    try:
        with open(_GATE_PATH) as f:
            return {"fool_rate": json.load(f)["proxy"].get("fool_rate")}
    except Exception:
        return None

HU_BIN = "hu"
GT_PATH = os.path.join(os.path.dirname(__file__), "..", "data", "imessage", "ground_truth.jsonl")
RESULTS_PATH = os.path.join(os.path.dirname(__file__), "..", "data", "eval_blinded_ab.json")

SETH_SYSTEM_PROMPT = (
    "You are Seth Ford, 45, texting on iMessage. Chief Architect at Vanguard. "
    "Live alone with your cat in King of Prussia, PA. From Afton, Wyoming. "
    "Three kids (Annette, Emerson, Edison) who don't live with you. "
    "Speak Japanese, lived in Japan (lost home in 2011 tsunami). "
    "23 years at Fidelity before this. Build AI runtimes as side projects.\n\n"
    "Style: casual, warm, direct. Short messages. Lowercase. "
    # Measured over 1,789 of Seth's own decoded iMessages (2026-07-26): tbh
    # 0.34%, idk 0.56%, hru 0.17%, gonna 0.34%. Instructing the model to
    # "abbreviate (gonna, tbh, idk, hru)" drove tbh to 71% of replies — a
    # 213x amplification and the single clearest AI tell in the corpus.
    "Emoji rare. Strong opinions. Dry humor. "
    "Do NOT use texting abbreviations (tbh, ngl, idk, hru, imo) \u2014 he almost never does."
)

SYNTHETIC_SCENARIOS = [
    {"incoming": "hey whats up", "seth_reply": "not much just chilling. you?"},
    {"incoming": "want to grab dinner tonight?", "seth_reply": "yeah im down. what are you thinking"},
    {"incoming": "I got the job!!", "seth_reply": "LETS GO!! dude thats amazing congrats"},
    {"incoming": "I'm so stressed about this deadline", "seth_reply": "ugh I feel you. whats the deadline for?"},
    {"incoming": "did you see that game last night", "seth_reply": "no I missed it. was it good?"},
    {"incoming": "can you help me move this weekend", "seth_reply": "yeah I can probably do Saturday morning. what time?"},
    {"incoming": "I've been thinking about switching careers", "seth_reply": "oh for real? what are you thinking about doing"},
    {"incoming": "lol remember that time we got lost in SF", "seth_reply": "hahaha dude yes. that uber driver was SO mad"},
    {"incoming": "happy birthday!!", "seth_reply": "thanks!! 🙏"},
    {"incoming": "sorry I've been MIA lately", "seth_reply": "no worries at all. everything ok?"},
]


# Structured output schema for the blinded A/B judge (see eval_humanness.py
# reference pattern). Constrains the model to bare JSON, removing the
# fragile ```json fence-strip.
_BLINDED_AB_JUDGE_SCHEMA = {
    "type": "object",
    "properties": {
        "choice": {"type": "string", "enum": ["A", "B"]},
        "confidence": {"type": "integer", "minimum": 1, "maximum": 10},
        "reasoning": {"type": "string"},
        "a_analysis": {"type": "string"},
        "b_analysis": {"type": "string"},
    },
    "required": ["choice", "confidence", "reasoning", "a_analysis", "b_analysis"],
    "propertyOrdering": ["choice", "confidence", "reasoning", "a_analysis", "b_analysis"],
}


# Judge thinking budget. gemini-3.x is thinking-enabled by default and shares
# maxOutputTokens between the invisible thinking and the visible reply, so an
# unset budget lets a hard judgment starve its own JSON body — the response is
# truncated mid-string and json.loads raises "Unterminated string". On
# 2026-07-27 that silently dropped 18/50 trials, and the loss is BIASED: the
# judge thinks longest on the closest calls, so the surviving sample is
# skewed toward easy ones. Budget it explicitly and leave the body room.
JUDGE_THINKING_BUDGET = 1024
JUDGE_MAX_OUTPUT_TOKENS = 4096


def judge_gen_config(temperature, response_schema=None):
    cfg = {
        "temperature": temperature,
        "maxOutputTokens": JUDGE_MAX_OUTPUT_TOKENS,
        "thinkingConfig": {"thinkingBudget": JUDGE_THINKING_BUDGET},
    }
    if response_schema is not None:
        cfg["responseMimeType"] = "application/json"
        cfg["responseSchema"] = response_schema
    return cfg


def call_gemini(prompt, temperature=0.3, response_schema=None):
    gen_cfg = judge_gen_config(temperature, response_schema)
    payload = json.dumps({
        "contents": [{"role": "user", "parts": [{"text": prompt}]}],
        "generationConfig": gen_cfg,
    }).encode()
    headers = {"Content-Type": "application/json"}
    if not API_KEY:
        token = _get_adc_token()
        if not token:
            raise RuntimeError("No GEMINI_API_KEY and no ADC credentials found")
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(_gemini_url(), data=payload, headers=headers)
    resp = urllib.request.urlopen(req, timeout=30)
    data = json.loads(resp.read())
    return data["candidates"][0]["content"]["parts"][0]["text"]


def run_mode(use_gateway, use_mlx):
    """Which generator produced the replies — for the results JSON.

    Precedence MUST mirror get_ai_response(): gateway, then mlx, then cli.
    This was `"gateway" if USE_GATEWAY else "cli"`, which labelled every
    --mlx run (i.e. every nightly run) as "cli". Downstream readers were
    told the C pipeline generated replies that raw MLX generated.
    """
    if use_gateway:
        return "gateway"
    if use_mlx:
        return "mlx"
    return "cli"


def build_mlx_messages(system_prompt, message, context_turns=None):
    """Chat messages for the MLX path, including the thread.

    Seth's turns are `assistant`, everyone else's are `user`. Omitting the
    thread is not a neutral simplification: the real reply this is scored
    against was written with the thread visible, so a context-free model
    reply loses on "lack of conversational memory" — an asymmetry the
    harness invented. See get_ai_response_cli's docstring; that fix landed
    on the CLI path only while the nightly runs --mlx.

    Malformed turns are skipped, not fatal: ground truth is machine-extracted
    and one bad row must not abort a 45-trial run.
    """
    msgs = [{"role": "system", "content": system_prompt}]
    for t in (context_turns or []):
        if not isinstance(t, dict):
            continue
        text = (t.get("text") or "").strip()
        if not text:
            continue
        role = "assistant" if t.get("from") == "seth" else "user"
        msgs.append({"role": role, "content": text})
    msgs.append({"role": "user", "content": message})
    return msgs


def get_ai_response(message, context_turns=None):
    if USE_GATEWAY:
        return get_ai_response_gateway(message, context_turns=context_turns)
    if USE_MLX:
        return get_ai_response_mlx(message, context_turns=context_turns)
    return get_ai_response_cli(message, context_turns=context_turns)


def get_ai_response_mlx(message, context_turns=None):
    """Get AI response from the local MLX server with Seth persona."""
    try:
        payload = json.dumps({
            "messages": build_mlx_messages(SETH_SYSTEM_PROMPT, message, context_turns),
            "max_tokens": 200,
            "temperature": 0.7,
        }).encode()
        req = urllib.request.Request(
            MLX_URL, data=payload,
            headers={"Content-Type": "application/json"}, method="POST",
        )
        # 120s: a 31B generation under GPU contention regularly exceeds 30s;
        # 2026-07-11 a 30s timeout abandoned 50/50 requests into the
        # single-threaded server's queue (each still generated, then
        # BrokenPipe'd) — degrading live serving for the drain duration.
        resp = urllib.request.urlopen(req, timeout=MLX_TIMEOUT_S)
        data = json.loads(resp.read())
        choices = data.get("choices", [])
        if choices:
            return choices[0].get("message", {}).get("content", "(empty)")
        return "(no choices)"
    except Exception as e:
        return f"(error: {e})"


def get_ai_response_cli(message, context_turns=None):
    """Generate via the real C pipeline.

    context_turns (from ground truth) is seeded into agent->history via
    --history-file. Omitting it makes this measurement meaningless: the human
    reply being compared against was written with the thread visible, so a
    context-free model reply loses on "lack of conversational memory" — an
    asymmetry the harness invented, not a property of the model. Measured
    2026-07-26: 0/9 fooled, with the judge citing exactly that. A/B on the same
    prompt: without history "5pm works for me. Please confirm..."; with the
    3-turn thread, "I'll be there at 7pm."
    """
    hist_path = None
    try:
        env = {**os.environ, "PATH": os.path.expanduser("~/bin") + ":" + os.environ.get("PATH", "")}
        cmd = [HU_BIN, "agent", "-m", message]
        if context_turns:
            # A file, not argv: message bodies contain quotes/newlines/emoji.
            fd, hist_path = tempfile.mkstemp(prefix="blindab-hist-", suffix=".jsonl")
            with os.fdopen(fd, "w") as hf:
                for t in context_turns:
                    txt = (t or {}).get("text") or ""
                    if not txt.strip():
                        continue
                    frm = "seth" if (t or {}).get("from") == "seth" else "them"
                    hf.write(json.dumps({"from": frm, "text": txt}) + "\n")
            cmd += ["--history-file", hist_path]
        # 30s -> 180s: a 106B MoE on a serial queue routinely exceeds 30s, and a
        # timeout here silently degrades the trial into "(error: ...)".
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=180, env=env)
        import re
        output = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', result.stdout)
        output = re.sub(r'\x1b\[\?25[hl]', '', output)
        lines = [l.strip() for l in output.strip().split("\n") if l.strip() and l.strip() != "Goodbye."]
        return " ".join(lines) if lines else "(empty)"
    except Exception as e:
        return f"(error: {e})"
    finally:
        if hist_path:
            try:
                os.unlink(hist_path)
            except OSError:
                pass


def build_gateway_messages(message, context_turns=None):
    """Chat messages for the gateway (product) path.

    Deliberately NO system prompt. The gateway runs the real agent turn, which
    builds the persona itself from ~/.human/personas + src/persona. Injecting a
    harness-authored persona here would reintroduce the exact artifact class
    this path exists to remove: every style claim the harness asserts becomes
    an AI tell the moment it is wrong, and it has been wrong twice
    ("Lowercase." ~10x, "Abbreviate (gonna, tbh, idk, hru)" ~200x on tbh).

    hu_openai_compat_handle_chat_completions clears agent history and repopulates
    it from this array, mapping every message but the trailing user turn into
    history — so the thread arrives natively, no C-side change needed.
    """
    msgs = []
    for t in (context_turns or []):
        if not isinstance(t, dict):
            continue
        text = (t.get("text") or "").strip()
        if not text:
            continue
        msgs.append({"role": "assistant" if t.get("from") == "seth" else "user",
                     "content": text})
    msgs.append({"role": "user", "content": message})
    return msgs


def gateway_url_from_config(config, env_url=None, default_port=3002):
    """Resolve the gateway base URL from ~/.human/config.json.

    The default was hardcoded to :3002 while the daemon binds the configured
    port (3006 as of 2026-07-27), so --gateway failed connection-refused before
    it generated anything.
    """
    if env_url:
        return env_url
    port = default_port
    try:
        cfg_port = (config or {}).get("gateway", {}).get("port")
        if isinstance(cfg_port, int) and cfg_port > 0:
            port = cfg_port
    except AttributeError:
        pass
    return f"http://127.0.0.1:{port}"


def _load_human_config():
    try:
        with open(os.path.expanduser("~/.human/config.json")) as f:
            return json.load(f)
    except Exception:
        return None


GATEWAY_URL = gateway_url_from_config(_load_human_config(),
                                      os.environ.get("HU_GATEWAY_URL"))

# A gateway trial is a full agent turn: several :8741 generations, queued
# behind live service-loop traffic on the same serial server. It must never
# have a tighter deadline than the bare MLX completion (see MLX_TIMEOUT_S's
# comment — an early timeout still generates server-side, then BrokenPipes,
# degrading LIVE serving for the drain duration). At 60s a --gate run aborted
# after 8/50 trials on MAX_CONSECUTIVE_FAILURES while single turns measured
# ~21s idle.
GATEWAY_TIMEOUT_S = 240


def get_ai_response_gateway(message, context_turns=None):
    try:
        payload = json.dumps({
            "model": "default",
            "messages": build_gateway_messages(message, context_turns),
        }).encode()
        req = urllib.request.Request(
            f"{GATEWAY_URL}/v1/chat/completions",
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        resp = urllib.request.urlopen(req, timeout=GATEWAY_TIMEOUT_S)
        data = json.loads(resp.read())
        choices = data.get("choices", [])
        if choices:
            return choices[0].get("message", {}).get("content", "(empty)")
        return "(no choices)"
    except Exception as e:
        return f"(gateway error: {e})"


def blinded_judge(incoming, response_a, response_b):
    """Ask Gemini to judge which response is more human, without knowing which is AI."""
    prompt = f"""You are a forensic linguistic analyst determining which text message response was written by a real human and which by an AI.

CONTEXT: A friend sent this message to someone named Seth:
"{incoming}"

Two responses were generated. ONE is from the real Seth (human). The OTHER is from an AI trying to impersonate Seth. You must determine which is human.

RESPONSE A: "{response_a}"
RESPONSE B: "{response_b}"

Analyze each response for:
1. Natural language patterns (contractions, fragments, lowercase, typos)
2. Emotional authenticity (genuine vs performed warmth)
3. Length appropriateness (humans text brief)
4. AI tells (hedging, over-helpfulness, bullet points, "certainly", excessive empathy)
5. Personality (opinions, casual tone, humor)
6. Texting conventions (abbreviations, no punctuation, emojis)

Return ONLY valid JSON:
{{
  "choice": "A" or "B",
  "confidence": 1-10,
  "reasoning": "brief explanation",
  "a_analysis": "what makes A seem human or AI",
  "b_analysis": "what makes B seem human or AI"
}}"""

    raw = call_gemini(prompt, temperature=0.2,
                      response_schema=_BLINDED_AB_JUDGE_SCHEMA)
    # responseSchema yields bare JSON; keep the fence strip as a defensive
    # fallback for endpoints/models that ignore the schema.
    if "```json" in raw:
        raw = raw.split("```json")[1].split("```")[0].strip()
    elif "```" in raw:
        raw = raw.split("```")[1].split("```")[0].strip()
    return json.loads(raw)


def load_ground_truth():
    pairs = []
    if os.path.exists(GT_PATH):
        with open(GT_PATH) as f:
            for line in f:
                line = line.strip()
                if line:
                    pairs.append(json.loads(line))
    return pairs


BINOCULARS_SCRIPT = os.path.join(
    os.path.dirname(__file__), "blind_ab", "binoculars_score.py")


def _run_binoculars(results_path):
    """Compute the Binoculars AI-tell summary for the just-written results file
    and merge it under the "binoculars" key.

    Direction/thresholds calibrated 2026-07-25 on the 07-24 corpus against the
    seth-lora-v5 adapter (docs/research/2026-07-25-binoculars-discriminator.md).
    The 5%-FPR threshold is adapter-version-specific — override with
    HU_BINOCULARS_THR_FPR5 after an adapter promotion + recalibration.

    Never raises, never touches the gate file: on any failure the summary is
    {"error": ...} and the eval's outcome is unchanged.
    """
    started = time.time()
    thr_fpr5 = float(os.environ.get("HU_BINOCULARS_THR_FPR5", "0.9643"))
    try:
        py = os.environ.get("HU_BINOCULARS_PYTHON",
                            os.path.expanduser("~/.human/venv/bin/python"))
        if not os.path.exists(py):
            py = sys.executable
        binoc_out = results_path + ".binoculars.tmp.json"
        subprocess.run(
            [py, BINOCULARS_SCRIPT, "--pairs", results_path,
             "--windows", "5", "--out", binoc_out, "--quiet"],
            check=True, timeout=2400,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(binoc_out) as f:
            binoc = json.load(f)
        os.unlink(binoc_out)

        rows = binoc.get("results", [])
        real = [r["score_dirA"] for r in rows if r.get("label") == "real"]
        ai = [r["score_dirA"] for r in rows if r.get("label") == "ai"]
        dira = ((binoc.get("analysis") or {}).get("scores") or {}).get(
            "dirA (obs=base)") or {}
        summary = {
            "direction": "dirA (obs=base, performer=adapted)",
            "adapter": binoc.get("adapter"),
            "n_real": len(real), "n_ai": len(ai),
            "auc_per_message": dira.get("auc_oriented"),
            "auc_windowed_k5": (dira.get("windowed") or {}).get("k=5"),
            "mean_real": round(sum(real) / len(real), 4) if real else None,
            "mean_ai": round(sum(ai) / len(ai), 4) if ai else None,
            "fpr5_threshold": thr_fpr5,
            "ai_frac_below_fpr5_thr":
                round(sum(1 for s in ai if s < thr_fpr5) / len(ai), 4)
                if ai else None,
            "elapsed_s": round(time.time() - started, 1),
            # Per-trial AI scores so binoculars_to_dpo.py can mine from this
            # file alone — no second 12-minute scoring pass.
            "ai_by_trial": {
                str(r["trial"]): {"score": round(r["score_dirA"], 5),
                                  "n_tokens": r["n_tokens"]}
                for r in rows
                if r.get("label") == "ai" and r.get("trial") is not None},
        }
    except Exception as e:  # advisory metric — swallow everything
        summary = {"error": f"{type(e).__name__}: {e}",
                   "elapsed_s": round(time.time() - started, 1)}

    try:
        with open(results_path) as f:
            doc = json.load(f)
        doc["binoculars"] = summary
        with open(results_path, "w") as f:
            json.dump(doc, f, indent=2)
    except Exception as e:
        print(f"  BINOCULARS: could not merge summary: {e}")
        return summary

    if "error" in summary:
        print(f"\n  BINOCULARS: FAILED ({summary['error']}) — advisory only, run unaffected")
    else:
        print(f"\n  BINOCULARS (advisory AI-tell, {summary['elapsed_s']:.0f}s):")
        print(f"    per-message AUC:  {summary['auc_per_message']}")
        print(f"    windowed k=5 AUC: {summary['auc_windowed_k5']}  "
              "(0.5 = statistically indistinguishable, the goal)")
        print(f"    mean score real={summary['mean_real']} ai={summary['mean_ai']}")
        print(f"    AI below {thr_fpr5} (5%-FPR flag): {summary['ai_frac_below_fpr5_thr']}")
    return summary


def main():
    # Short-circuit for --gate --gate-dry-run (no creds, no data needed)
    if USE_GATE and GATE_DRY_RUN:
        _gate.write_proxy_half(_GATE_PATH, {
            "verdict": "ADVISORY", "mode": "ADVISORY", "fool_rate": None,
            "baseline_fool_rate": None, "n_trials": 0, "n_real_pairs": 0,
            "fail_under": _gate.DEFAULT_FAIL_UNDER,
            "max_regression": _gate.DEFAULT_MAX_REGRESSION,
        }, commit=_git_commit())
        print("GATE: ADVISORY (dry run / no data) — not blocking")
        sys.exit(0)

    if not API_KEY and not os.path.exists(os.path.expanduser("~/.config/gcloud/application_default_credentials.json")):
        print("ERROR: Set GEMINI_API_KEY or configure gcloud ADC")
        sys.exit(1)

    pairs = load_ground_truth()
    if USE_SYNTHETIC:
        pairs.extend(SYNTHETIC_SCENARIOS)

    if not pairs:
        print("ERROR: No ground truth data and --synthetic not specified")
        print(f"  Ground truth file: {GT_PATH}")
        print("  Run: python3 scripts/extract_imessage_pairs.py")
        print("  Or use: --synthetic flag for synthetic scenarios")
        sys.exit(1)

    total_available = len(pairs)
    if len(pairs) > MAX_TRIALS:
        random.shuffle(pairs)
        pairs = pairs[:MAX_TRIALS]

    mode = "MLX server" if USE_MLX else ("GATEWAY (live daemon)" if USE_GATEWAY else "CLI (hu agent -m)")
    print("=" * 70)
    print("  BLINDED A/B EVALUATION — AI vs Real Seth")
    print("=" * 70)
    print(f"  Mode: {mode}")
    print(f"  Pairs: {len(pairs)} (sampled from {total_available}"
          f"{f', +{len(SYNTHETIC_SCENARIOS)} synthetic' if USE_SYNTHETIC else ''})")
    print(f"  Judge: Gemini 3.1 Pro (independent, blinded)")
    print("=" * 70)

    results = []
    ai_detected_correctly = 0
    human_detected_correctly = 0
    total = 0

    # MLX generation holds the server's model_lock, so it CANNOT be parallelized
    # — it is the hard serial floor. The Gemini judge, by contrast, is an
    # independent network call per pair. So we generate serially (in order, to
    # show progress as the model grinds) and dispatch each judge to a thread pool
    # the instant its generation completes: judge(i) then overlaps gen(i+1..n)
    # and falls off the critical path. The A/B coin is seeded per pair index so
    # the human/AI assignment is reproducible and order-independent. Results are
    # collected in completion order (tally counts are order-independent) and
    # re-sorted by pair index before saving for stable output.
    # Consecutive transport failures mean the SERVER is unwell, not the pairs:
    # keep going and every remaining request piles onto the single-threaded
    # server's queue as an abandoned generation (2026-07-11: 50 stacked
    # timeouts degraded live :8741 for the whole drain). Abort early instead.
    MAX_CONSECUTIVE_FAILURES = 5
    consecutive_failures = 0
    with ThreadPoolExecutor(max_workers=JUDGE_WORKERS) as pool:
        futures = {}  # future -> meta dict
        for i, pair in enumerate(pairs):
            incoming = pair["incoming"]
            real_seth = pair["seth_reply"]

            print(f"\n{'─' * 70}")
            print(f"  [{i+1}/{len(pairs)}] Incoming: \"{incoming}\"")
            print(f"  Real Seth: \"{real_seth}\"")
            sys.stdout.flush()

            # Feed the SAME thread the human had — see get_ai_response_cli.
            ai_response = get_ai_response(
                incoming, context_turns=pair.get('context_turns'))  # serial — model_lock floor
            print(f"  AI Seth:   \"{ai_response}\"")

            if ai_response.startswith("("):
                print(f"  SKIP: AI response failed")
                consecutive_failures += 1
                if consecutive_failures >= MAX_CONSECUTIVE_FAILURES:
                    print(f"\n  ABORT: {consecutive_failures} consecutive AI-response failures — "
                          "server unhealthy; stopping to avoid stacking abandoned "
                          "generations on its queue. Fix serving, then re-run.")
                    break
                continue
            consecutive_failures = 0

            coin = random.Random(f"{SEED}:{i}").random() < 0.5
            if coin:
                response_a, response_b = real_seth, ai_response
                human_is = "A"
            else:
                response_a, response_b = ai_response, real_seth
                human_is = "B"

            fut = pool.submit(blinded_judge, incoming, response_a, response_b)
            futures[fut] = {
                "i": i, "incoming": incoming, "real_seth": real_seth,
                "ai_response": ai_response, "human_is": human_is, "pair": pair,
            }

        # Drain the judge pool in completion order. Runs in the main thread, so
        # the tally/append below need no locking.
        for fut in as_completed(futures):
            meta = futures[fut]
            try:
                judgment = fut.result()
                choice = judgment.get("choice", "?")
                confidence = judgment.get("confidence", 0)

                chose_human = (choice == meta["human_is"])
                if chose_human:
                    human_detected_correctly += 1
                else:
                    ai_detected_correctly += 1
                total += 1

                label = "CORRECT (detected human)" if chose_human else "FOOLED (picked AI as human)"
                print(f"  [{meta['i']+1}] Judge: picked {choice} as human "
                      f"(confidence {confidence}/10) — {label}")
                print(f"      Reasoning: {judgment.get('reasoning', '?')}")

                results.append({
                    "i": meta["i"],
                    "incoming": meta["incoming"],
                    "real_seth": meta["real_seth"],
                    "ai_response": meta["ai_response"],
                    "human_was": meta["human_is"],
                    "judge_choice": choice,
                    "judge_correct": chose_human,
                    "confidence": confidence,
                    "judgment": judgment,
                    "is_synthetic": "seth_reply" in meta["pair"] and "chat_id" not in meta["pair"],
                })
            except Exception as e:
                print(f"  [{meta['i']+1}] Judge error: {e}")

    # Stable, pair-index order for the saved artifact (judges finished out of order).
    results.sort(key=lambda r: r["i"])

    print(f"\n{'=' * 70}")
    print("  BLINDED A/B RESULTS")
    print(f"{'=' * 70}")

    if total > 0:
        detection_rate = human_detected_correctly / total * 100
        fool_rate = ai_detected_correctly / total * 100
        print(f"\n  Total trials:           {total}")
        print(f"  Judge detected human:   {human_detected_correctly}/{total} ({detection_rate:.0f}%)")
        print(f"  AI fooled judge:        {ai_detected_correctly}/{total} ({fool_rate:.0f}%)")
        print()

        if fool_rate >= 50:
            print("  VERDICT: AI PASSES TURING TEST")
            print("  The judge cannot reliably distinguish AI from human (fool rate >= 50%)")
        elif fool_rate >= 35:
            print("  VERDICT: BORDERLINE")
            print("  The judge sometimes confuses AI for human, but not consistently")
        else:
            print("  VERDICT: AI DETECTED")
            print("  The judge can reliably tell AI from human")

        target_met = fool_rate >= 45
        print(f"\n  Target (fool rate >= 45%): {'MET' if target_met else 'NOT MET'}")

    os.makedirs(os.path.dirname(RESULTS_PATH), exist_ok=True)
    with open(RESULTS_PATH, "w") as f:
        json.dump({
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "mode": run_mode(USE_GATEWAY, USE_MLX),
            "total_trials": total,
            "human_detected": human_detected_correctly,
            "ai_fooled": ai_detected_correctly,
            "detection_rate": human_detected_correctly / total * 100 if total else 0,
            "fool_rate": ai_detected_correctly / total * 100 if total else 0,
            "trials": results,
        }, f, indent=2)
    print(f"\n  Full results saved to {RESULTS_PATH}")

    # Advisory Binoculars metric: needs the results file on disk, and only a
    # run that counts as a real measurement (same >=50%-valid bar the gate
    # harness-fail check uses) is worth 12 min of GPU.
    if USE_BINOCULARS and total > 0 and (not pairs or total >= len(pairs) / 2):
        _run_binoculars(RESULTS_PATH)

    if USE_GATE:
        fool_rate = ai_detected_correctly / total * 100 if total else 0.0
        n_real_pairs = sum(1 for t in results if not t.get("is_synthetic"))
        baseline = _load_baseline()
        mode, verdict, should_fail = _gate.proxy_gate_decision(
            fool_rate=fool_rate, n_real_pairs=n_real_pairs, baseline=baseline)
        _gate.write_proxy_half(_GATE_PATH, {
            "verdict": verdict, "mode": mode,
            "fool_rate": fool_rate if total else None,
            "baseline_fool_rate": (baseline or {}).get("fool_rate"),
            "n_trials": total, "n_real_pairs": n_real_pairs,
            "fail_under": _gate.DEFAULT_FAIL_UNDER,
            "max_regression": _gate.DEFAULT_MAX_REGRESSION,
        }, commit=_git_commit())
        banner = ("ADVISORY (n_real_pairs<%d) — not blocking" % _gate.ENFORCE_MIN_PAIRS
                  if mode == "ADVISORY" else "%s (fool_rate=%.0f%%)" % (verdict, fool_rate))
        print(f"\n  GATE: {banner}")
        # A run where fewer than half the attempted trials produced a valid
        # judgment is a HARNESS failure, not a measurement — exit non-zero so
        # nightly logs record it as failed instead of a polite ADVISORY
        # (2026-07-11: 0/50 valid exited 0 and read like a completed run).
        if len(pairs) and total < len(pairs) / 2:
            print(f"  HARNESS FAIL: only {total}/{len(pairs)} trials valid (<50%) — "
                  "not a measurement; fix serving/judge and re-run.")
            sys.exit(1)
        sys.exit(1 if should_fail else 0)


if __name__ == "__main__":
    main()
